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
