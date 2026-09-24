#!/usr/bin/env python3
"""Fail-closed inventory of ``GameModules/*`` content boundaries."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 2
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
    return {
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
    }


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
            if entry.get(key) != value:
                findings.append((location, f"{key} drift for {name}: expected {value!r}"))
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


def _validate_manifest_tests(repo_root: Path, tests: Any, location: str) -> list[tuple[str, str]]:
    if not isinstance(tests, dict) or set(tests) != {"files", "prefixes"}:
        return [(location, "tests must contain exactly files and prefixes")]
    files, prefixes = tests["files"], tests["prefixes"]
    findings: list[tuple[str, str]] = []
    if not isinstance(files, list) or not files or len(files) != len(set(map(str, files))):
        return [(f"{location}.files", "must be a non-empty list of unique test source paths")]
    if not isinstance(prefixes, list) or not prefixes or len(prefixes) != len(set(map(str, prefixes))):
        return [(f"{location}.prefixes", "must be a non-empty list of unique TEST-name prefixes")]
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
    valid_prefixes = []
    for index, prefix in enumerate(prefixes):
        if not isinstance(prefix, str) or not TEST_PREFIX_PATTERN.match(prefix):
            findings.append((f"{location}.prefixes[{index}]", f"invalid TEST-name prefix: {prefix!r}"))
            continue
        valid_prefixes.append(prefix)
        if not any(name.startswith(prefix) for names in names_by_file.values() for name in names):
            findings.append((f"{location}.prefixes[{index}]", f"test prefix matches no TEST( definition in the listed files: {prefix}"))
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
        findings.extend(_validate_manifest_tests(repo_root, manifest["tests"], f"{location}.tests"))
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
