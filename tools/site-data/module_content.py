#!/usr/bin/env python3
"""Fail-closed inventory of ``GameModules/*`` content boundaries."""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 4
INVENTORY_RELATIVE = Path("GameModules/module-content-inventory.json")
EVIDENCE_RELATIVE = Path("tools/module-evidence/manifest.json")
PROFILE_STATES = {"required", "shared", "outside"}
FPS_ROOT_DEPENDENCIES = ("Assets/Models", "Assets/Scenes")

# Per-module GameModules/<Name>/module.json holds per-module facts only. Profile
# policy (profileApplicability, evidenceBindings) stays in the authoritative
# evidence manifest, so an unknown key is rejected instead of silently becoming
# a second place where policy can be declared.
MODULE_MANIFEST_NAME = "module.json"
MODULE_MANIFEST_SCHEMA_VERSION = 1
MODULE_MANIFEST_KEYS = {"schemaVersion", "name", "cmakeTarget", "sourceDirectory", "assets", "tests", "docs", "parity"}
WORK_ITEMS_RELATIVE = Path("docs/readiness/work-items")
TESTS_CMAKE_RELATIVE = Path("Tests/CMakeLists.txt")
TEST_PREFIX_PATTERN = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")
TEST_DEFINITION_PATTERN = re.compile(r"\bTEST\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")


def _cmake_copy_declarations(text: str) -> tuple[set[str], bool]:
    """Return asset roots in actual add_custom_command(copy_directory ...) bodies."""
    # CMake bracket comments may use any number of '=' delimiters.
    bracket = re.compile(r"#\[(=*)\[.*?\]\1\]", re.DOTALL)
    text = bracket.sub(lambda match: "\n" * match.group(0).count("\n"), text)
    text = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
    declarations: set[str] = set()
    unbalanced = False
    for match in re.finditer(r"\badd_custom_command\s*\(", text, re.IGNORECASE):
        start, depth, quote = match.end(), 1, False
        index = start
        while index < len(text) and depth:
            char = text[index]
            if char == '"' and (index == 0 or text[index - 1] != "\\"):
                quote = not quote
            elif not quote:
                if char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
            index += 1
        if depth:
            unbalanced = True
            continue
        body = text[start:index - 1]
        if re.search(r"\bcopy_directory\b", body, re.IGNORECASE):
            for root in FPS_ROOT_DEPENDENCIES:
                if re.search(re.escape(root), body, re.IGNORECASE):
                    declarations.add(root)
    return declarations, unbalanced


def _read_evidence(repo_root: Path) -> tuple[dict[str, Any] | None, list[str]]:
    path = repo_root / EVIDENCE_RELATIVE
    if not path.is_file():
        return None, ["authoritative module-evidence manifest is missing"]
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, [f"manifest is unreadable or malformed: {error}"]
    errors: list[str] = []
    if not isinstance(payload, dict) or payload.get("schemaVersion") != "stable-v2":
        errors.append("schemaVersion must be 'stable-v2'")
        return None, errors
    profiles, modules = payload.get("profiles"), payload.get("modules")
    if not isinstance(profiles, list) or not profiles:
        errors.append("profiles must be a non-empty list")
    if not isinstance(modules, list) or not modules:
        errors.append("modules must be a non-empty list")
    names: list[str] = []
    for index, module in enumerate(modules if isinstance(modules, list) else []):
        location = f"modules[{index}]"
        if not isinstance(module, dict):
            errors.append(f"{location}: module entry must be an object")
            continue
        name, applicability = module.get("name"), module.get("profileApplicability")
        if not isinstance(name, str) or not name:
            errors.append(f"{location}: name must be a non-empty string")
        else:
            names.append(name)
        if not isinstance(applicability, dict) or not applicability:
            errors.append(f"{location}: profileApplicability must be a non-empty object")
        elif any(not isinstance(key, str) or state not in PROFILE_STATES for key, state in applicability.items()):
            errors.append(f"{location}: profileApplicability contains invalid profile/state")
    if len(names) != len(set(names)):
        errors.append("module names must be unique")
    for index, profile in enumerate(profiles if isinstance(profiles, list) else []):
        location = f"profiles[{index}]"
        if not isinstance(profile, dict) or not isinstance(profile.get("id"), str) or not profile["id"]:
            errors.append(f"{location}: profile must contain a string id")
            continue
        for key in ("includedModules", "excludedModules"):
            values = profile.get(key)
            if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
                errors.append(f"{location}.{key}: must be a list of non-empty strings")
            elif len(values) != len(set(values)):
                errors.append(f"{location}.{key}: entries must be unique")
    profile_ids = {profile["id"] for profile in profiles if isinstance(profile, dict) and isinstance(profile.get("id"), str)}
    for index, module in enumerate(modules if isinstance(modules, list) else []):
        if isinstance(module, dict) and isinstance(module.get("profileApplicability"), dict):
            if set(module["profileApplicability"]) != profile_ids:
                errors.append(f"modules[{index}]: profileApplicability keys must exactly match declared profile ids")
    return (payload if not errors else None), errors


def _integrity_entries(repo_root: Path) -> set[str]:
    try:
        payload = json.loads((repo_root / "Assets" / "assets.integrity.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return set()
    return {
        entry["path"] for entry in payload.get("entries", [])
        if isinstance(entry, dict) and isinstance(entry.get("path"), str)
    }


def _root_dependency(repo_root: Path, relative: str, integrity: set[str], declaration_present: bool) -> dict[str, Any]:
    root = repo_root / relative
    files = [path for path in root.rglob("*") if path.is_file()] if root.is_dir() else []
    paths = {path.relative_to(repo_root / "Assets").as_posix() for path in files}
    return {
        "path": relative,
        "buildDeclarationPresent": declaration_present,
        "exists": root.is_dir(),
        "fileCount": len(files),
        "integrityManifest": (repo_root / "Assets" / "assets.integrity.json").is_file(),
        "integrityCoveredFileCount": len(paths & integrity),
    }


# MOD-295 / MOD-310: the one include resolver shared by every module-boundary
# ratchet. It mirrors the include search order the module CMakeLists declare
# (target_include_directories: "Source", ENGINE_SOURCE_DIR, SPARK_SDK_INCLUDE_DIR):
# a quoted include is looked up next to the including file first, an angle
# include only on the declared directories. The first existing candidate wins and
# is classified by where the resolved file actually lives, so a "../" escape into
# SparkEngine/Source is still engine-private. Directives inside #if blocks are
# counted too: the ratchet is conservative, never optimistic.
INCLUDE_SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".inl"}
INCLUDE_DIRECTIVE_PATTERN = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(?:"([^"\n]+)"|<([^>\n]+)>)', re.MULTILINE)
ENGINE_PRIVATE_ROOT = Path("SparkEngine/Source")
SDK_INCLUDE_ROOT = Path("SparkSDK/Include")
COPIED_INFRASTRUCTURE_GLOB = "*EngineSystems.cpp"
PRIVATE_DEPENDENCY_KEYS = ("privateEngineHeaders", "privateEngineHeaderCount", "copiedInfrastructureFiles")


def _within(path: Path, root: Path) -> str | None:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return None


def classify_module_includes(repo_root: Path, module_dir: Path) -> dict[str, Any]:
    """Classify every include directive in ``module_dir/Source``.

    Returns ``{"module": set, "sdk": set, "engine": set, "unresolved": list}``.
    ``engine`` holds paths relative to SparkEngine/Source; ``unresolved`` holds
    ``(file, spelling)`` for quoted includes that resolve nowhere. Angle includes
    that resolve nowhere are toolchain or third-party headers and are ignored.
    """
    repo_root = repo_root.resolve()
    source = module_dir.resolve() / "Source"
    engine_root, sdk_root = repo_root / ENGINE_PRIVATE_ROOT, repo_root / SDK_INCLUDE_ROOT
    declared = [source, engine_root, sdk_root]
    result: dict[str, Any] = {"module": set(), "sdk": set(), "engine": set(), "unresolved": []}
    if not source.is_dir():
        return result
    for path in sorted(source.rglob("*")):
        if not path.is_file() or path.suffix not in INCLUDE_SOURCE_SUFFIXES:
            continue
        code, _ = _lex_cpp(path.read_text(encoding="utf-8", errors="replace"))
        for match in INCLUDE_DIRECTIVE_PATTERN.finditer(code):
            quoted, spelling = match.group(1) is not None, (match.group(1) or match.group(2)).strip()
            candidates = ([path.parent] if quoted else []) + declared
            resolved = next(
                (Path(os.path.normpath(base / spelling)) for base in candidates if (base / spelling).is_file()), None
            )
            if resolved is None:
                if quoted:
                    result["unresolved"].append((path.relative_to(repo_root).as_posix(), spelling))
                continue
            engine = _within(resolved, engine_root)
            if engine is not None:
                result["engine"].add(engine)
            elif _within(resolved, sdk_root) is not None:
                result["sdk"].add(_within(resolved, sdk_root))
            elif _within(resolved, module_dir.resolve()) is not None:
                result["module"].add(_within(resolved, module_dir.resolve()))
    return result


def _is_prototype(applicability: dict[str, str]) -> bool:
    """A prototype module is in no release profile (required or shared)."""
    return bool(applicability) and all(state == "outside" for state in applicability.values())


def _private_dependency_payload(module_dir: Path, repo_root: Path) -> dict[str, Any]:
    headers = sorted(classify_module_includes(repo_root, module_dir)["engine"])
    source = module_dir / "Source"
    copied = sorted(path.relative_to(repo_root).as_posix() for path in source.rglob(COPIED_INFRASTRUCTURE_GLOB) if path.is_file()) if source.is_dir() else []
    return {"privateEngineHeaders": headers, "privateEngineHeaderCount": len(headers), "copiedInfrastructureFiles": copied}


def _validate_private_dependencies(
    repo_root: Path, module_dir: Path, entry: dict[str, Any], location: str
) -> list[tuple[str, str]]:
    """Ratchet a prototype module's engine-private headers against its committed inventory entry."""
    name = module_dir.name
    findings: list[tuple[str, str]] = []
    classified = classify_module_includes(repo_root, module_dir)
    for relative, spelling in classified["unresolved"]:
        findings.append((location, f"unclassifiable include in {relative}: \"{spelling}\" resolves to no module, SDK or engine file"))
    measured = _private_dependency_payload(module_dir, repo_root)
    committed = entry.get("privateEngineHeaders")
    if not isinstance(committed, list) or not all(isinstance(value, str) for value in committed):
        findings.append((location, f"privateEngineHeaders must be a list of strings for prototype module {name}"))
        committed = []
    elif committed != sorted(set(committed)):
        findings.append((location, f"privateEngineHeaders must be sorted and unique for {name}"))
    for header in sorted(set(measured["privateEngineHeaders"]) - set(committed)):
        findings.append((location, f"{name} gained engine-private header not listed in its committed inventory: {header}"))
    for header in sorted(set(committed) - set(measured["privateEngineHeaders"])):
        findings.append((location, f"{name} no longer includes engine-private header {header}; shrink privateEngineHeaders"))
    if entry.get("privateEngineHeaderCount") != len(committed):
        findings.append((location, f"privateEngineHeaderCount for {name} must equal the listed header count {len(committed)}"))
    if entry.get("copiedInfrastructureFiles") != measured["copiedInfrastructureFiles"]:
        findings.append((location, f"copiedInfrastructureFiles drift for {name}: expected {measured['copiedInfrastructureFiles']!r}"))
    return findings


def _module_payload(module_dir: Path, repo_root: Path, applicability: dict[str, str]) -> dict[str, Any]:
    name = module_dir.name
    source, assets = module_dir / "Source", module_dir / "Assets"
    local_files = [path for path in assets.rglob("*") if path.is_file() and path.name not in {"manifest.json", "README.md"}] if assets.is_dir() else []
    declaration_parse_error = False
    if name == "SparkGameFPS":
        cmake = (module_dir / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
        roots = FPS_ROOT_DEPENDENCIES
        local_state = "root-dependent"
        declarations, declaration_parse_error = _cmake_copy_declarations(cmake)
    elif local_files:
        roots, local_state = [], "packaged"
    else:
        roots, local_state = [], "unclassified"
        declaration_parse_error = False
    integrity = _integrity_entries(repo_root)
    payload = {
        "name": name,
        "directory": module_dir.relative_to(repo_root).as_posix(),
        "cmakeLists": (module_dir / "CMakeLists.txt").is_file(),
        "sourceDirectory": source.relative_to(repo_root).as_posix(),
        "sourceFileCount": sum(1 for path in source.rglob("*") if path.is_file()) if source.is_dir() else 0,
        "moduleLocalContentState": local_state,
        "releaseClassification": "shipping" if applicability.get("stable-v1") == "required" else "unclassified",
        "cmakeDeclarationParseError": declaration_parse_error,
        "assetsDirectory": assets.relative_to(repo_root).as_posix() if assets.is_dir() else None,
        "assetManifest": (assets / "manifest.json").relative_to(repo_root).as_posix() if (assets / "manifest.json").is_file() else None,
        "assetFileCount": len(local_files),
        "profileApplicability": applicability,
        "sharedRootDependencies": [_root_dependency(repo_root, root, integrity, root in declarations if name == "SparkGameFPS" else False) for root in roots],
    }
    # MOD-295 measures, and does not reduce, what prototype modules take from
    # engine-private headers and copied *EngineSystems.cpp setup code.
    if _is_prototype(applicability):
        payload.update(_private_dependency_payload(module_dir, repo_root))
    return payload


def _contains_symlink(path: Path) -> bool:
    return any(candidate.is_symlink() for candidate in path.rglob("*")) if path.is_dir() else False


def generate(repo_root: Path) -> dict[str, Any]:
    evidence, errors = _read_evidence(repo_root)
    if errors or evidence is None:
        raise ValueError("; ".join(errors))
    applicability = {module["name"]: module["profileApplicability"] for module in evidence["modules"]}
    modules_root = repo_root / "GameModules"
    return {
        "schemaVersion": SCHEMA_VERSION,
        "modules": [_module_payload(path, repo_root, applicability.get(path.name, {})) for path in sorted(modules_root.iterdir(), key=lambda item: item.name.casefold()) if path.is_dir() and path.name != "__pycache__"],
        "fallbackSites": _regenerated_fallback_sites(repo_root, {module["name"]: module for module in evidence["modules"]}),
    }


def _regenerated_fallback_sites(repo_root: Path, authoritative: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    """Refresh detected site files while keeping the reviewed policy of each declared site.

    Policies are review input, not derived facts, so regeneration never invents
    one: an undeclared site stays absent and validation reports it.
    """
    try:
        existing = json.loads((repo_root / INVENTORY_RELATIVE).read_text(encoding="utf-8")).get("fallbackSites")
    except (OSError, json.JSONDecodeError, AttributeError):
        existing = None
    declared = {}
    for entry in existing if isinstance(existing, list) else []:
        key = (entry.get("module"), entry.get("kind"), entry.get("symbol")) if isinstance(entry, dict) else None
        # Malformed entries are dropped here and reported by validate_fallback_sites.
        if key is not None and all(isinstance(part, str) for part in key):
            declared[key] = entry
    sites = []
    for site in scan_fallback_sites(repo_root, _in_profile_modules(authoritative)):
        review = declared.get((site["module"], site["kind"], site["symbol"]))
        if review is not None:
            sites.append(site | {key: review[key] for key in ("policy", "owner", "reason") if key in review})
    return sites


def validate(repo_root: Path) -> list[tuple[str, str]]:
    evidence, errors = _read_evidence(repo_root)
    findings = [(EVIDENCE_RELATIVE.as_posix(), error) for error in errors]
    inventory_path = repo_root / INVENTORY_RELATIVE
    if not inventory_path.is_file():
        return findings + [(INVENTORY_RELATIVE.as_posix(), "module content inventory is missing")]
    try:
        payload = json.loads(inventory_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return findings + [(INVENTORY_RELATIVE.as_posix(), f"inventory is unreadable: {error}")]
    if not isinstance(payload, dict) or payload.get("schemaVersion") != SCHEMA_VERSION:
        findings.append((INVENTORY_RELATIVE.as_posix(), f"schemaVersion must be {SCHEMA_VERSION}"))
    entries = payload.get("modules") if isinstance(payload, dict) else None
    if not isinstance(entries, list):
        return findings + [(INVENTORY_RELATIVE.as_posix(), "modules must be a list")]
    if evidence is None:
        return findings
    authoritative = {module["name"]: module for module in evidence["modules"]}
    game_root = repo_root / "GameModules"
    actual = {path.name: path for path in game_root.iterdir() if path.is_dir() and path.name != "__pycache__"}
    seen: set[str] = set()
    folded: dict[str, str] = {}
    for index, entry in enumerate(entries):
        location = f"{INVENTORY_RELATIVE.as_posix()}.modules[{index}]"
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            findings.append((location, "module entry must contain a string name"))
            continue
        name = entry["name"]
        if name in seen:
            findings.append((location, f"duplicate module entry: {name}"))
        if name.casefold() in folded and folded[name.casefold()] != name:
            findings.append((location, f"module name differs only by case: {name}"))
        folded[name.casefold()] = name
        seen.add(name)
        if name not in actual:
            findings.append((location, f"module directory is missing: {name}"))
            continue
        if actual[name].is_symlink():
            findings.append((location, f"module directory is a symlink or reparse point: {name}"))
            continue
        if _contains_symlink(actual[name]):
            findings.append((location, f"module contains a symlink or reparse point: {name}"))
            continue
        if name not in authoritative:
            findings.append((location, f"module is absent from authoritative evidence: {name}"))
            continue
        expected = _module_payload(actual[name], repo_root, authoritative[name]["profileApplicability"])
        for key, value in expected.items():
            if key not in PRIVATE_DEPENDENCY_KEYS and entry.get(key) != value:
                findings.append((location, f"{key} drift for {name}: expected {value!r}"))
        if _is_prototype(authoritative[name]["profileApplicability"]):
            findings.extend(_validate_private_dependencies(repo_root, actual[name], entry, location))
        elif any(key in entry for key in PRIVATE_DEPENDENCY_KEYS):
            findings.append((location, f"private-dependency ratchet fields are published only for prototype modules: {name}"))
        if expected["assetsDirectory"] and expected["assetFileCount"] and not expected["assetManifest"]:
            findings.append((location, f"payload asset directory has no manifest for {name}"))
        if name == "SparkGameFPS":
            if expected["cmakeDeclarationParseError"]:
                findings.append((location, "FPS CMake add_custom_command has unbalanced parentheses"))
            for dependency in expected["sharedRootDependencies"]:
                if not dependency["buildDeclarationPresent"]:
                    findings.append((location, f"FPS shared root build declaration is missing: {dependency['path']}"))
                if not dependency["exists"]:
                    findings.append((location, f"FPS shared root dependency is missing: {dependency['path']}"))
                if dependency["integrityCoveredFileCount"] != dependency["fileCount"]:
                    findings.append((location, f"FPS shared root dependency lacks integrity coverage: {dependency['path']}"))
    for name in sorted(set(actual) - seen):
        findings.append((INVENTORY_RELATIVE.as_posix(), f"module directory is undeclared: {name}"))
    for name in sorted(set(authoritative) - set(actual)):
        findings.append((EVIDENCE_RELATIVE.as_posix(), f"authoritative module is absent from GameModules: {name}"))
    for profile in evidence["profiles"]:
        profile_id = profile["id"]
        included, excluded = set(profile["includedModules"]), set(profile["excludedModules"])
        if included & excluded or (included | excluded) != set(actual):
            findings.append((EVIDENCE_RELATIVE.as_posix(), f"profile {profile_id} does not partition discovered modules"))
        for name in actual:
            expected = "required" if name in included else "outside"
            if authoritative.get(name, {}).get("profileApplicability", {}).get(profile_id) != expected:
                findings.append((EVIDENCE_RELATIVE.as_posix(), f"profile {profile_id} applicability mismatch for {name}"))
        if profile_id == "stable-v1" and included != {"SparkGameFPS"}:
            findings.append((EVIDENCE_RELATIVE.as_posix(), "stable-v1 must include exactly SparkGameFPS"))
    findings.extend(validate_module_manifests(repo_root, actual, authoritative))
    findings.extend(validate_fallback_sites(repo_root, payload.get("fallbackSites"), authoritative))
    return findings


MIN_REASON_LENGTH = 20


def _parity_rows(repo_root: Path) -> tuple[list[str], dict[str, Any], list[tuple[str, str]]]:
    """Return (dimensions, currentScores) from the one work-item file that declares parity."""
    location = WORK_ITEMS_RELATIVE.as_posix()
    for path in sorted((repo_root / WORK_ITEMS_RELATIVE).glob("*.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            return [], {}, [(path.relative_to(repo_root).as_posix(), f"work-item file is unreadable: {error}")]
        if not isinstance(document, dict) or "parityDimensions" not in document:
            continue
        parity = document["parityDimensions"]
        dimensions = parity.get("dimensions") if isinstance(parity, dict) else None
        scores = parity.get("currentScores") if isinstance(parity, dict) else None
        if not isinstance(dimensions, list) or not isinstance(scores, dict):
            return [], {}, [(path.relative_to(repo_root).as_posix(), "parityDimensions needs dimensions and currentScores")]
        return dimensions, scores, []
    return [], {}, [(location, "no work-item file declares parityDimensions")]


def _strip_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def _strip_cmake_comments(text: str) -> str:
    text = re.sub(r"#\[(=*)\[.*?\]\1\]", "", text, flags=re.DOTALL)
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def _is_registered_test_source(cmake_text: str, relative: str) -> bool:
    """True when Tests/CMakeLists.txt names the Tests-relative source as its own token."""
    pattern = r'(?:^|[\s"(]|\$\{CMAKE_CURRENT_SOURCE_DIR\}/)' + re.escape(relative) + r'(?=$|[\s")])'
    return re.search(pattern, cmake_text, re.MULTILINE) is not None


# SparkTests builds on Windows and on the POSIX hosts ("other"). A TEST( inside
# a Windows-only preprocessor branch runs on one of them only, so a module
# selector's count may differ by platform. Any other condition is assumed true.
TEST_PLATFORMS = ("windows", "other")
# Optional selector "requires": build features without which the selected TEST(
# family is not compiled into SparkTests. Tests/CMakeLists.txt skips the generated
# ModuleManifest_ CTest when a listed feature is off (the count is the feature-on
# count), so an exact-count lane never runs an empty or shrunken family there.
SELECTOR_FEATURES = ("angelscript",)
_WINDOWS_MACROS = r"(?:_WIN32|SPARK_PLATFORM_WINDOWS)"
_IF_WINDOWS = re.compile(rf"^#\s*(?:ifdef\s+{_WINDOWS_MACROS}\b|if\s+defined\s*\(?\s*{_WINDOWS_MACROS}\s*\)?\s*$)")
_IF_NOT_WINDOWS = re.compile(rf"^#\s*(?:ifndef\s+{_WINDOWS_MACROS}\b|if\s+!\s*defined\s*\(?\s*{_WINDOWS_MACROS}\s*\)?\s*$)")
_IF_OTHER = re.compile(r"^#\s*if(?:n?def)?\b")


def _test_definitions_by_platform(text: str) -> list[tuple[str, frozenset[str]]]:
    """TEST( names in comment-stripped source, each with the platforms that compile it."""
    everywhere = frozenset(TEST_PLATFORMS)
    stack: list[frozenset[str]] = []
    definitions: list[tuple[str, frozenset[str]]] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            if _IF_WINDOWS.match(stripped):
                stack.append(frozenset({"windows"}))
            elif _IF_NOT_WINDOWS.match(stripped):
                stack.append(frozenset({"other"}))
            elif _IF_OTHER.match(stripped):
                stack.append(everywhere)
            elif re.match(r"^#\s*else\b", stripped) and stack:
                # Only a Windows split has a known complement; elif stays unknown.
                stack[-1] = everywhere - stack[-1] if stack[-1] != everywhere else everywhere
            elif re.match(r"^#\s*elif\b", stripped) and stack:
                stack[-1] = everywhere
            elif re.match(r"^#\s*endif\b", stripped) and stack:
                stack.pop()
            continue
        active = everywhere.intersection(*stack) if stack else everywhere
        for name in TEST_DEFINITION_PATTERN.findall(line):
            definitions.append((name, active))
    return definitions


def _registered_test_names(repo_root: Path, cmake_text: str) -> list[tuple[str, frozenset[str]]]:
    """Every TEST( defined by a Tests/ source that Tests/CMakeLists.txt registers, with its platforms."""
    tests_root = repo_root / "Tests"
    # Same token rule as _is_registered_test_source, tokenised once for ~700 sources.
    registered = {
        token.removeprefix("${CMAKE_CURRENT_SOURCE_DIR}/")
        for token in re.split(r'[\s"()]+', cmake_text)
        if token.endswith(".cpp")
    }
    names: list[tuple[str, frozenset[str]]] = []
    # followlinks: the mutation tests mirror Tests/ subdirectories as symlinks.
    for directory, subdirectories, files in os.walk(tests_root, followlinks=True):
        subdirectories.sort()
        for file_name in sorted(files):
            if not file_name.endswith(".cpp"):
                continue
            source = Path(directory) / file_name
            if source.relative_to(tests_root).as_posix() not in registered:
                continue
            text = _strip_cpp_comments(source.read_text(encoding="utf-8", errors="replace"))
            names.extend(_test_definitions_by_platform(text))
    return names


def _manifest_path(repo_root: Path, value: Any, location: str, findings: list[tuple[str, str]], *, kind: str) -> Path | None:
    if not isinstance(value, str) or not value:
        findings.append((location, "path must be a non-empty string"))
        return None
    candidate = Path(value)
    if candidate.is_absolute() or ".." in candidate.parts or "\\" in value:
        findings.append((location, f"path must be repository-relative without traversal: {value!r}"))
        return None
    resolved = repo_root / candidate
    exists = resolved.is_dir() if kind == "directory" else resolved.is_file()
    if not exists:
        findings.append((location, f"referenced {kind} does not exist: {value}"))
        return None
    return resolved


def _require_reason(value: Any, location: str, findings: list[tuple[str, str]]) -> None:
    if not isinstance(value, str) or len(value.strip()) < MIN_REASON_LENGTH:
        findings.append((location, f"reason must be a written explanation of at least {MIN_REASON_LENGTH} characters"))


def _asset_root_coverage(repo_root: Path, directory: Path, manifest: Path, location: str) -> list[tuple[str, str]]:
    """Require the named manifest to list every file under the asset root.

    Two manifest shapes can prove coverage: the repository integrity manifest
    (``root`` plus ``entries[].path``) and a module package manifest
    (``assets[].path`` relative to the manifest's directory).
    """
    try:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        return [(f"{location}.manifest", f"asset manifest is not readable JSON: {error}")]
    if isinstance(payload, dict) and isinstance(payload.get("root"), str) and isinstance(payload.get("entries"), list):
        base, listed = repo_root / payload["root"], payload["entries"]
    elif isinstance(payload, dict) and isinstance(payload.get("assets"), list):
        base, listed = manifest.parent, payload["assets"]
    else:
        return [(f"{location}.manifest", "asset manifest must be an integrity manifest (root, entries) or a package manifest (assets)")]
    covered = {
        (base / entry["path"]).resolve() for entry in listed
        if isinstance(entry, dict) and isinstance(entry.get("path"), str)
    }
    uncovered = sorted(
        path.relative_to(repo_root).as_posix() for path in directory.rglob("*")
        if path.is_file() and path != manifest and path.resolve() not in covered
    )
    if uncovered:
        return [(f"{location}.manifest", f"asset manifest does not list {len(uncovered)} file(s) under the root, first: {uncovered[0]}")]
    return []


def _module_shared_asset_roots(repo_root: Path, name: str) -> set[str]:
    """Return shared Assets/ roots named after the module (SparkGameMMO -> MMO) that contain files.

    Two layouts exist: Assets/<Kind>/<Suffix> (Assets/Models/MMO) and subdirectories of
    Assets/<Suffix> (Assets/MMOFPS/Data). Either one means the module ships content there.
    """
    suffix = name.removeprefix("SparkGame")
    assets_root = repo_root / "Assets"
    if not suffix or not assets_root.is_dir():
        return set()
    candidates = [kind / suffix for kind in assets_root.iterdir() if kind.is_dir() and kind.name != suffix]
    module_root = assets_root / suffix
    if module_root.is_dir():
        candidates.extend(child for child in module_root.iterdir() if child.is_dir())
    return {
        candidate.relative_to(repo_root).as_posix() for candidate in candidates
        if candidate.is_dir() and any(path.is_file() for path in candidate.rglob("*"))
    }


def _validate_manifest_assets(repo_root: Path, module_dir: Path, assets: Any, location: str) -> list[tuple[str, str]]:
    findings: list[tuple[str, str]] = []
    name = module_dir.name
    local_assets = module_dir / "Assets"
    local_payload = local_assets.is_dir() and any(
        path.is_file() and path.name not in {"manifest.json", "README.md"} for path in local_assets.rglob("*")
    )
    required_roots = set(FPS_ROOT_DEPENDENCIES) if name == "SparkGameFPS" else set()
    required_roots |= _module_shared_asset_roots(repo_root, name)
    if local_payload:
        required_roots.add(local_assets.relative_to(repo_root).as_posix())
    if not isinstance(assets, dict) or assets.get("state") not in {"none", "declared"}:
        return [(location, "assets.state must be 'none' or 'declared'")]
    if assets["state"] == "none":
        if set(assets) != {"state", "reason"}:
            findings.append((location, "assets with state 'none' must contain exactly state and reason"))
        _require_reason(assets.get("reason"), f"{location}.reason", findings)
        for root in sorted(required_roots):
            findings.append((location, f"assets declared 'none' but the module ships asset root {root}"))
        return findings
    roots = assets.get("roots")
    if set(assets) != {"state", "roots"} or not isinstance(roots, list) or not roots:
        return [(location, "assets with state 'declared' must contain exactly state and a non-empty roots list")]
    declared: set[str] = set()
    for index, root in enumerate(roots):
        root_location = f"{location}.roots[{index}]"
        if not isinstance(root, dict) or set(root) != {"directory", "manifest"}:
            findings.append((root_location, "asset root must contain exactly directory and manifest"))
            continue
        directory = _manifest_path(repo_root, root["directory"], f"{root_location}.directory", findings, kind="directory")
        if directory is not None:
            declared.add(root["directory"])
            if not any(path.is_file() for path in directory.rglob("*")):
                findings.append((f"{root_location}.directory", f"asset root contains no files: {root['directory']}"))
        manifest = _manifest_path(repo_root, root["manifest"], f"{root_location}.manifest", findings, kind="file")
        if directory is not None and manifest is not None:
            findings.extend(_asset_root_coverage(repo_root, directory, manifest, root_location))
    for root in sorted(required_roots - declared):
        findings.append((location, f"asset root shipped by the module is not declared: {root}"))
    return findings


def _validate_selector_count(
    prefix: str, count: Any, selected: dict[str, int], location: str
) -> list[tuple[str, str]]:
    """count is one positive integer, or {windows, other} when the platforms differ."""
    def positive(value: Any) -> bool:
        return isinstance(value, int) and not isinstance(value, bool) and value > 0

    if isinstance(count, dict):
        if set(count) != set(TEST_PLATFORMS) or not all(positive(value) for value in count.values()):
            return [(location, f"per-platform count must map exactly {list(TEST_PLATFORMS)} to positive integers")]
        if len(set(count.values())) == 1:
            return [(location, f"per-platform count for {prefix} is equal on every platform; declare one integer")]
        declared = dict(count)
    elif positive(count):
        declared = {platform: count for platform in TEST_PLATFORMS}
    else:
        return [(location, f"count must be a positive integer: {count!r}")]
    return [
        (
            location,
            f"declared count {declared[platform]} for {prefix} disagrees with {selected[platform]} registered TEST( "
            f"definitions whose name starts with it ({platform} build)",
        )
        for platform in TEST_PLATFORMS
        if declared[platform] != selected[platform]
    ]


def _validate_manifest_tests(
    repo_root: Path, tests: Any, location: str, registered_names: list[tuple[str, frozenset[str]]]
) -> list[tuple[str, str]]:
    if not isinstance(tests, dict) or set(tests) != {"files", "prefixes"}:
        return [(location, "tests must contain exactly files and prefixes")]
    files, prefixes = tests["files"], tests["prefixes"]
    findings: list[tuple[str, str]] = []
    if not isinstance(files, list) or not files or len(files) != len(set(map(str, files))):
        return [(f"{location}.files", "must be a non-empty list of unique test source paths")]
    if not isinstance(prefixes, list) or not prefixes:
        return [(f"{location}.prefixes", "must be a non-empty list of {prefix, count} selectors")]
    cmake_path = repo_root / TESTS_CMAKE_RELATIVE
    cmake_text = _strip_cmake_comments(cmake_path.read_text(encoding="utf-8")) if cmake_path.is_file() else ""
    names_by_file: dict[str, list[str]] = {}
    for index, value in enumerate(files):
        file_location = f"{location}.files[{index}]"
        source = _manifest_path(repo_root, value, file_location, findings, kind="file")
        if source is None:
            continue
        if not value.startswith("Tests/") or source.suffix != ".cpp":
            findings.append((file_location, f"test source must be a .cpp file under Tests/: {value}"))
            continue
        if not _is_registered_test_source(cmake_text, value[len("Tests/"):]):
            findings.append((file_location, f"test source is not registered in {TESTS_CMAKE_RELATIVE.as_posix()}: {value}"))
        text = _strip_cpp_comments(source.read_text(encoding="utf-8", errors="replace"))
        names_by_file[value] = TEST_DEFINITION_PATTERN.findall(text)
    valid_prefixes: list[str] = []
    for index, entry in enumerate(prefixes):
        entry_location = f"{location}.prefixes[{index}]"
        if not isinstance(entry, dict) or set(entry) - {"requires"} != {"prefix", "count"}:
            findings.append((entry_location, "selector must contain exactly prefix and count (and optional requires)"))
            continue
        if "requires" in entry:
            features = entry["requires"]
            if (
                not isinstance(features, list) or not features or len(features) != len(set(map(str, features)))
                or not all(feature in SELECTOR_FEATURES for feature in features)
            ):
                findings.append((
                    f"{entry_location}.requires",
                    f"requires must be a non-empty list of unique features from {list(SELECTOR_FEATURES)}",
                ))
                continue
        prefix, count = entry["prefix"], entry["count"]
        if not isinstance(prefix, str) or not TEST_PREFIX_PATTERN.match(prefix):
            findings.append((entry_location, f"invalid TEST-name prefix: {prefix!r}"))
            continue
        if prefix in valid_prefixes:
            findings.append((entry_location, f"duplicate TEST-name prefix: {prefix}"))
            continue
        valid_prefixes.append(prefix)
        if not any(name.startswith(prefix) for names in names_by_file.values() for name in names):
            findings.append((entry_location, f"test prefix matches no TEST( definition in the listed files: {prefix}"))
        # Tests/CMakeLists.txt turns each selector into a CTest that runs SparkTests
        # with SPARK_TEST_NAME_PREFIX=<prefix> (an anchored filter over every
        # compiled test) and SPARK_TEST_EXPECT_COUNT=<count>. Checking the same
        # count here catches drift without a build; the CTest re-checks it on
        # each platform.
        selected = {
            platform: sum(
                1 for name, platforms in registered_names if name.startswith(prefix) and platform in platforms
            )
            for platform in TEST_PLATFORMS
        }
        findings.extend(_validate_selector_count(prefix, count, selected, entry_location))
    for value, names in names_by_file.items():
        if not any(name.startswith(prefix) for name in names for prefix in valid_prefixes):
            findings.append((location, f"listed test source defines no TEST( matching a declared prefix: {value}"))
    return findings


def _validate_manifest_parity(
    module: str, parity: Any, dimensions: list[str], scores: dict[str, Any], location: str
) -> list[tuple[str, str]]:
    entries = parity.get("notApplicable") if isinstance(parity, dict) and set(parity) == {"notApplicable"} else None
    if not isinstance(entries, list):
        return [(location, "parity must contain exactly a notApplicable list")]
    findings: list[tuple[str, str]] = []
    declared: set[str] = set()
    for index, entry in enumerate(entries):
        entry_location = f"{location}.notApplicable[{index}]"
        if not isinstance(entry, dict) or set(entry) != {"dimension", "reason"}:
            findings.append((entry_location, "N/A entry must contain exactly dimension and reason"))
            continue
        dimension = entry["dimension"]
        if dimension not in dimensions:
            findings.append((entry_location, f"unknown parity dimension: {dimension!r}"))
        elif dimension in declared:
            findings.append((entry_location, f"duplicate parity dimension: {dimension}"))
        declared.add(dimension)
        _require_reason(entry["reason"], f"{entry_location}.reason", findings)
    row = scores.get(module)
    if not isinstance(row, list) or len(row) != len(dimensions):
        findings.append((location, f"parity row for {module} is missing or does not match the dimensions"))
        return findings
    marked = {dimension for dimension, value in zip(dimensions, row) if value == "N/A"}
    if marked != declared:
        findings.append((
            location,
            f"parity N/A cells disagree with declared notApplicable dimensions for {module}: "
            f"row={sorted(marked)} declared={sorted(declared)}",
        ))
    return findings


def validate_module_manifests(
    repo_root: Path, actual: dict[str, Path], authoritative: dict[str, dict[str, Any]]
) -> list[tuple[str, str]]:
    """Validate GameModules/<Name>/module.json for every discovered module directory."""
    dimensions, scores, findings = _parity_rows(repo_root)
    cmake_path = repo_root / TESTS_CMAKE_RELATIVE
    cmake_text = _strip_cmake_comments(cmake_path.read_text(encoding="utf-8")) if cmake_path.is_file() else ""
    registered_names = _registered_test_names(repo_root, cmake_text)
    for name in sorted(actual, key=str.casefold):
        module_dir = actual[name]
        path = module_dir / MODULE_MANIFEST_NAME
        location = path.relative_to(repo_root).as_posix()
        if not path.is_file():
            findings.append((location, f"module manifest is missing: {name}"))
            continue
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            findings.append((location, f"module manifest is unreadable or malformed: {error}"))
            continue
        if not isinstance(manifest, dict):
            findings.append((location, "module manifest must be a JSON object"))
            continue
        if set(manifest) != MODULE_MANIFEST_KEYS:
            missing, unknown = MODULE_MANIFEST_KEYS - set(manifest), set(manifest) - MODULE_MANIFEST_KEYS
            findings.append((location, f"module manifest keys differ: missing={sorted(missing)} unknown={sorted(unknown)}"))
            continue
        if manifest["schemaVersion"] != MODULE_MANIFEST_SCHEMA_VERSION:
            findings.append((location, f"schemaVersion must be {MODULE_MANIFEST_SCHEMA_VERSION}"))
        evidence = authoritative.get(name, {})
        if manifest["name"] != name:
            findings.append((location, f"name must equal the module directory: expected {name!r}"))
        for key in ("cmakeTarget", "sourceDirectory"):
            if manifest[key] != evidence.get(key):
                findings.append((location, f"{key} disagrees with {EVIDENCE_RELATIVE.as_posix()}: expected {evidence.get(key)!r}"))
        _manifest_path(repo_root, manifest["sourceDirectory"], f"{location}.sourceDirectory", findings, kind="directory")
        findings.extend(_validate_manifest_assets(repo_root, module_dir, manifest["assets"], f"{location}.assets"))
        findings.extend(_validate_manifest_tests(repo_root, manifest["tests"], f"{location}.tests", registered_names))
        docs = manifest["docs"]
        readme = docs.get("readme") if isinstance(docs, dict) and set(docs) == {"readme"} else None
        expected_readme = f"{module_dir.relative_to(repo_root).as_posix()}/README.md"
        if readme != expected_readme:
            findings.append((f"{location}.docs", f"docs must contain exactly readme: {expected_readme!r}"))
        elif not (repo_root / readme).is_file():
            findings.append((f"{location}.docs", f"module README is missing: {readme}"))
        if dimensions:
            findings.extend(_validate_manifest_parity(name, manifest["parity"], dimensions, scores, f"{location}.parity"))
    return findings


# RDY-020: asset-load fallback and procedural-substitution sites in in-profile
# modules must be declared with a reviewed policy. The pattern set is kept
# deliberately narrow (each pattern has mutation tests) because a broad
# "fallback" word search over-matches comments, log text and unrelated locals.
FALLBACK_SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".inl"}
FALLBACK_POLICIES = {"intentional", "gap", "tracked"}
FALLBACK_SITE_KEYS = {"module", "kind", "symbol", "files", "policy", "reason"}
FALLBACK_HELPER_PATTERN = re.compile(r"\b(\w*Fallback\w*)\s*\(")
PROCEDURAL_HELPER_PATTERN = re.compile(r"\b(\w*Procedural\w*)\s*\(")
# `m_shotgunModel->LoadObj(Resolve(L"Models/rifle.obj"), ...)`: the receiver
# names the asset it stands for, so a different .obj stem is a substitution.
# The lexer below rewrites every string literal (raw or prefixed) as a plain
# quoted literal, so the pattern needs no prefix or raw-string handling.
MODEL_SUBSTITUTION_PATTERN = re.compile(
    r"\b(?:m_)?([A-Za-z][A-Za-z0-9]*?)Model\s*(?:->|\.)\s*LoadObj\s*\([^;]*?\"(?:[^\"]*/)?([A-Za-z0-9_]+)\.obj\""
)
STRING_PREFIXES = {"L", "u8", "u", "U", "R", "LR", "u8R", "uR", "UR"}
RAW_STRING_PATTERN = re.compile(r'"([^()\\\s]{0,16})\(')
LITERAL_KEPT_CHARACTER = re.compile(r"[A-Za-z0-9_./\-]")
CPP_WORD_PATTERN = re.compile(r"\w+")
# pp-number, including C++14 digit separators such as 1'000'000.
CPP_NUMBER_PATTERN = re.compile(r"\d(?:'?[\w.]|[eEpP][+-])*")


def _lex_cpp(text: str) -> tuple[str, str]:
    """Remove comments and normalize literals in one pass that understands both.

    Returns ``(code, code_without_literals)``. In ``code`` each string literal,
    including raw and encoding-prefixed ones, becomes a plain ``"..."`` holding
    only path-safe characters, so literal text can neither open a comment nor
    close the literal early. In ``code_without_literals`` every literal is
    ``""``. Newlines are preserved so line structure survives.
    """
    code: list[str] = []
    bare: list[str] = []
    index, length = 0, len(text)

    def emit(both: str, *, literal: str | None = None) -> None:
        code.append(both if literal is None else literal)
        bare.append(both)

    while index < length:
        char = text[index]
        if text.startswith("//", index):
            end = text.find("\n", index)
            index = length if end < 0 else end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            emit(" " + "\n" * text.count("\n", index, end))
            index = end
        elif char.isalpha() or char == "_":
            match = CPP_WORD_PATTERN.match(text, index)
            word = match.group(0)
            index = match.end()
            if word in STRING_PREFIXES and index < length and text[index] == '"':
                index = _lex_string(text, index, word.endswith("R"), emit)
            else:
                emit(word)
        elif char.isdigit():
            match = CPP_NUMBER_PATTERN.match(text, index)
            emit(match.group(0))
            index = match.end()
        elif char == '"':
            index = _lex_string(text, index, False, emit)
        elif char == "'":
            end = index + 1
            while end < length and text[end] not in "'\n":
                end += 2 if text[end] == "\\" else 1
            emit("''")
            index = min(end + 1, length)
        else:
            emit(char)
            index += 1
    return "".join(code), "".join(bare)


def _lex_string(text: str, index: int, raw: bool, emit: Any) -> int:
    """Consume the string literal whose opening quote is at ``index``; return the index after it."""
    if raw:
        opener = RAW_STRING_PATTERN.match(text, index)
        if opener is not None:
            terminator = ")" + opener.group(1) + '"'
            end = text.find(terminator, opener.end())
            end = len(text) if end < 0 else end
            body = text[opener.end():end]
            newlines = "\n" * body.count("\n")
            emit('""' + newlines, literal='"' + "".join(LITERAL_KEPT_CHARACTER.findall(body)) + '"' + newlines)
            return min(end + len(terminator), len(text))
    end = index + 1
    while end < len(text) and text[end] not in '"\n':
        end += 2 if text[end] == "\\" else 1
    body = text[index + 1:end]
    emit('""', literal='"' + "".join(LITERAL_KEPT_CHARACTER.findall(body)) + '"')
    return min(end + 1, len(text))


def _in_profile_modules(authoritative: dict[str, dict[str, Any]]) -> list[str]:
    return sorted(
        name for name, module in authoritative.items()
        if any(state in {"required", "shared"} for state in module.get("profileApplicability", {}).values())
    )


def scan_fallback_sites(repo_root: Path, modules: list[str]) -> list[dict[str, Any]]:
    """Return detected fallback/procedural sites keyed by (module, kind, symbol)."""
    sites: dict[tuple[str, str, str], set[str]] = {}
    for module in modules:
        source = repo_root / "GameModules" / module / "Source"
        if not source.is_dir():
            continue
        for path in sorted(source.rglob("*")):
            if not path.is_file() or path.suffix not in FALLBACK_SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(repo_root).as_posix()
            code, code_without_literals = _lex_cpp(path.read_text(encoding="utf-8", errors="replace"))
            found = [("fallback-helper", match.group(1)) for match in FALLBACK_HELPER_PATTERN.finditer(code_without_literals)]
            found += [("procedural-helper", match.group(1)) for match in PROCEDURAL_HELPER_PATTERN.finditer(code_without_literals)]
            for match in MODEL_SUBSTITUTION_PATTERN.finditer(code):
                stand_in, loaded = match.group(1), match.group(2)
                if stand_in.casefold() != loaded.casefold():
                    found.append(("model-substitution", f"{stand_in[0].lower()}{stand_in[1:]} -> {loaded}.obj"))
            for kind, symbol in found:
                sites.setdefault((module, kind, symbol), set()).add(relative)
    return [
        {"module": module, "kind": kind, "symbol": symbol, "files": sorted(files)}
        for (module, kind, symbol), files in sorted(sites.items())
    ]


def _work_item_ids(repo_root: Path) -> set[str]:
    ids: set[str] = set()
    for path in sorted((repo_root / WORK_ITEMS_RELATIVE).glob("*.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        items = document.get("workItems") if isinstance(document, dict) else None
        ids.update(item["id"] for item in items or [] if isinstance(item, dict) and isinstance(item.get("id"), str))
    return ids


def validate_fallback_sites(
    repo_root: Path, declared: Any, authoritative: dict[str, dict[str, Any]]
) -> list[tuple[str, str]]:
    """Require every detected in-profile fallback site to carry exactly one reviewed policy."""
    location = f"{INVENTORY_RELATIVE.as_posix()}.fallbackSites"
    if not isinstance(declared, list):
        return [(location, "fallbackSites must be a list of declared fallback/procedural sites")]
    findings: list[tuple[str, str]] = []
    detected = {(site["module"], site["kind"], site["symbol"]): site for site in scan_fallback_sites(repo_root, _in_profile_modules(authoritative))}
    work_items = _work_item_ids(repo_root)
    seen: set[tuple[str, str, str]] = set()
    for index, entry in enumerate(declared):
        entry_location = f"{location}[{index}]"
        allowed = FALLBACK_SITE_KEYS | {"owner"}
        if not isinstance(entry, dict) or not FALLBACK_SITE_KEYS <= set(entry) or not set(entry) <= allowed:
            findings.append((entry_location, f"fallback site must contain {sorted(FALLBACK_SITE_KEYS)} and optionally owner"))
            continue
        scalars = ("module", "kind", "symbol", "policy", "reason") + (("owner",) if "owner" in entry else ())
        files = entry["files"]
        if not all(isinstance(entry[key], str) for key in scalars) or not isinstance(files, list) or not all(
            isinstance(value, str) for value in files
        ):
            findings.append((entry_location, f"fallback site fields {list(scalars)} must be strings and files a list of strings"))
            continue
        key = (entry["module"], entry["kind"], entry["symbol"])
        if key in seen:
            findings.append((entry_location, f"duplicate fallback site declaration: {key}"))
        seen.add(key)
        site = detected.get(key)
        if site is None:
            findings.append((entry_location, f"declared fallback site no longer exists in in-profile sources: {key}"))
        elif entry["files"] != site["files"]:
            findings.append((entry_location, f"files drift for fallback site {key}: expected {site['files']!r}"))
        policy, owner = entry["policy"], entry.get("owner")
        if policy not in FALLBACK_POLICIES:
            findings.append((entry_location, f"policy must be one of {sorted(FALLBACK_POLICIES)}: {policy!r}"))
        elif policy == "tracked" and owner not in work_items:
            findings.append((entry_location, f"tracked fallback site needs an owner naming an existing work item: {owner!r}"))
        elif policy != "tracked" and "owner" in entry:
            findings.append((entry_location, "only a tracked fallback site may name an owner work item"))
        _require_reason(entry["reason"], f"{entry_location}.reason", findings)
    for key in sorted(set(detected) - seen):
        findings.append((location, f"undeclared fallback site in {detected[key]['files'][0]}: {key}"))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        generated = generate(args.repo_root)
    except ValueError as error:
        print(error)
        return 1
    output = args.output or args.repo_root / INVENTORY_RELATIVE
    expected = json.dumps(generated, indent=2) + "\n"
    if args.check:
        findings = validate(args.repo_root)
        if not output.is_file() or output.read_text(encoding="utf-8") != expected:
            print(f"stale module content inventory: {output}")
            return 1
        for location, message in findings:
            print(f"{location}: {message}")
        return int(bool(findings))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(expected, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
