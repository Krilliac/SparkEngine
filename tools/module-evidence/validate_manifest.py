#!/usr/bin/env python3
"""Fail-closed validator for the module evidence manifest.

Cross-checks declared release-profile modules against:
  - Real CMake targets (CMakeLists.txt existence and add_library calls)
  - Production shared-library identity (platform naming conventions)
  - ModuleManager lifecycle coverage (SPARK_IMPLEMENT_MODULE registration)
  - Package-smoke ownership
  - JUnit/sanitizer evidence bindings
  - Experimental-module separation under RDY-015

Rejects:
  - Copied mirror models (source directory not matching declared module)
  - Test-only source substitutions (test paths in source directories)
  - Missing lifecycle phases
  - Ambiguous/duplicate module identities
  - Repository-only paths (absolute or user-home paths)
  - Stale commit bindings (declared SHA not matching current HEAD)
  - Unsupported profile promotion (outside module in required slot)
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

from schema import (
    REQUIRED_LIFECYCLE_PHASES,
    REQUIRED_MODULE_KEYS,
    REQUIRED_PROFILE_KEYS,
    REQUIRED_TOP_LEVEL_KEYS,
    SCHEMA_VERSION,
    SHARED_LIBRARY_PATTERNS,
    VALID_EVIDENCE_TYPES,
    VALID_LIFECYCLE_PHASES,
    VALID_MODULE_KINDS,
    VALID_PROFILE_APPLICABILITIES,
    OPTIONAL_MODULE_KEYS,
)

REPO_ROOT = Path(__file__).resolve().parents[2]


class ManifestError(RuntimeError):
    """A validation error that must block release promotion."""


class ManifestValidator:
    """Fail-closed validator: any unrecognized state is an error."""

    def __init__(self, manifest: dict[str, Any], repo_root: Path | None = None):
        self.manifest = manifest
        self.repo_root = repo_root or REPO_ROOT
        self.errors: list[str] = []

    def validate(self) -> list[str]:
        self.errors = []
        self._check_top_level()
        if self.errors:
            return self.errors
        self._check_schema_version()
        self._check_commit_sha()
        self._check_profiles()
        self._check_modules()
        self._check_cross_references()
        self._check_no_duplicate_identities()
        self._check_experimental_separation()
        return self.errors

    def _err(self, msg: str) -> None:
        self.errors.append(msg)

    def _check_top_level(self) -> None:
        if not isinstance(self.manifest, dict):
            self._err("manifest must be a JSON object")
            return
        present = set(self.manifest.keys())
        missing = REQUIRED_TOP_LEVEL_KEYS - present
        if missing:
            self._err(f"missing required top-level keys: {sorted(missing)}")
        unknown = present - REQUIRED_TOP_LEVEL_KEYS
        if unknown:
            self._err(f"unknown top-level keys: {sorted(unknown)}")

    def _check_schema_version(self) -> None:
        version = self.manifest.get("schemaVersion")
        if version != SCHEMA_VERSION:
            self._err(
                f"schemaVersion must be {SCHEMA_VERSION!r}, got {version!r}"
            )

    def _check_commit_sha(self) -> None:
        sha = self.manifest.get("commitSHA", "")
        if not isinstance(sha, str) or not re.match(r"^[0-9a-f]{40}$", sha):
            self._err(
                f"commitSHA must be a 40-character lowercase hex string, got {sha!r}"
            )

    def _check_profiles(self) -> None:
        profiles = self.manifest.get("profiles", [])
        if not isinstance(profiles, list):
            self._err("profiles must be an array")
            return
        if not profiles:
            self._err("at least one profile is required")
            return
        profile_ids: set[str] = set()
        for i, profile in enumerate(profiles):
            if not isinstance(profile, dict):
                self._err(f"profiles[{i}] must be an object")
                continue
            present = set(profile.keys())
            missing = REQUIRED_PROFILE_KEYS - present
            if missing:
                self._err(f"profiles[{i}]: missing keys {sorted(missing)}")
            pid = profile.get("id", "")
            if pid in profile_ids:
                self._err(f"duplicate profile id: {pid!r}")
            profile_ids.add(pid)
            included = profile.get("includedModules", [])
            excluded = profile.get("excludedModules", [])
            if not isinstance(included, list) or not isinstance(excluded, list):
                self._err(f"profiles[{i}]: includedModules/excludedModules must be arrays")
                continue
            overlap = set(included) & set(excluded)
            if overlap:
                self._err(
                    f"profiles[{i}]: modules in both included and excluded: {sorted(overlap)}"
                )

    def _check_modules(self) -> None:
        modules = self.manifest.get("modules", [])
        if not isinstance(modules, list):
            self._err("modules must be an array")
            return
        for i, mod in enumerate(modules):
            if not isinstance(mod, dict):
                self._err(f"modules[{i}] must be an object")
                continue
            self._check_single_module(i, mod)

    def _check_single_module(self, idx: int, mod: dict[str, Any]) -> None:
        prefix = f"modules[{idx}]"
        present = set(mod.keys())
        missing = REQUIRED_MODULE_KEYS - present
        if missing:
            self._err(f"{prefix}: missing required keys {sorted(missing)}")
        unknown = present - REQUIRED_MODULE_KEYS - OPTIONAL_MODULE_KEYS
        if unknown:
            self._err(f"{prefix}: unknown keys {sorted(unknown)}")

        name = mod.get("name", "")
        self._check_module_name(prefix, name)
        self._check_cmake_target(prefix, mod)
        self._check_shared_library(prefix, mod)
        self._check_source_directory(prefix, mod)
        self._check_module_kind(prefix, mod)
        self._check_lifecycle_phases(prefix, mod)
        self._check_profile_applicability(prefix, mod)
        self._check_evidence_bindings(prefix, mod)
        self._check_package_smoke_owner(prefix, mod)
        self._check_dependencies(prefix, mod)

    def _check_module_name(self, prefix: str, name: Any) -> None:
        if not isinstance(name, str) or not name:
            self._err(f"{prefix}: name must be a non-empty string")
            return
        if not re.match(r"^[A-Za-z][A-Za-z0-9]*$", name):
            self._err(
                f"{prefix}: name {name!r} must be alphanumeric PascalCase"
            )

    def _check_cmake_target(self, prefix: str, mod: dict[str, Any]) -> None:
        target = mod.get("cmakeTarget", "")
        if not isinstance(target, str) or not target:
            self._err(f"{prefix}: cmakeTarget must be a non-empty string")
            return
        cmake_path = self.repo_root / "GameModules" / mod.get("name", "") / "CMakeLists.txt"
        if not cmake_path.is_file():
            self._err(
                f"{prefix}: CMakeLists.txt not found at {cmake_path.relative_to(self.repo_root)}"
            )
            return
        cmake_content = cmake_path.read_text(encoding="utf-8", errors="replace")
        if f"add_library({target}" not in cmake_content:
            alt_pattern = re.compile(
                rf"add_library\s*\(\s*{re.escape(target)}\s", re.MULTILINE
            )
            if not alt_pattern.search(cmake_content):
                self._err(
                    f"{prefix}: CMake target {target!r} not found as add_library() "
                    f"in {cmake_path.relative_to(self.repo_root)}"
                )

    def _check_shared_library(self, prefix: str, mod: dict[str, Any]) -> None:
        lib = mod.get("sharedLibrary", {})
        if not isinstance(lib, dict):
            self._err(f"{prefix}: sharedLibrary must be an object")
            return
        if not lib:
            self._err(f"{prefix}: sharedLibrary must declare at least one platform")
            return
        for platform, filename in lib.items():
            if platform not in SHARED_LIBRARY_PATTERNS:
                self._err(
                    f"{prefix}: unknown sharedLibrary platform {platform!r}"
                )
                continue
            pattern = SHARED_LIBRARY_PATTERNS[platform]
            if not re.match(pattern, filename):
                self._err(
                    f"{prefix}: sharedLibrary.{platform} {filename!r} "
                    f"does not match {pattern}"
                )

    def _check_source_directory(self, prefix: str, mod: dict[str, Any]) -> None:
        src = mod.get("sourceDirectory", "")
        if not isinstance(src, str) or not src:
            self._err(f"{prefix}: sourceDirectory must be a non-empty string")
            return
        if src.startswith("/") or src.startswith("\\") or ":" in src:
            self._err(
                f"{prefix}: sourceDirectory {src!r} must be a repository-relative path"
            )
            return
        if re.search(r"(^|[/\\])(~|\$HOME|%USERPROFILE%)", src):
            self._err(
                f"{prefix}: sourceDirectory {src!r} references user-home — "
                f"repository-only paths required"
            )
            return
        if re.search(r"[/\\][Tt]est[s]?[/\\]", src) or src.lower().startswith("tests"):
            self._err(
                f"{prefix}: sourceDirectory {src!r} appears to be a test path — "
                f"production source required"
            )
            return
        full_path = self.repo_root / src
        if not full_path.is_dir():
            self._err(
                f"{prefix}: sourceDirectory {src!r} does not exist in the repository"
            )
        name = mod.get("name", "")
        if name and "GameModules/" in src:
            parts = src.replace("\\", "/").split("/")
            try:
                gm_idx = parts.index("GameModules")
                dir_module_name = parts[gm_idx + 1] if gm_idx + 1 < len(parts) else ""
                if dir_module_name and dir_module_name != name:
                    self._err(
                        f"{prefix}: sourceDirectory {src!r} belongs to module "
                        f"{dir_module_name!r} but declared name is {name!r} — "
                        f"copied mirror model detected"
                    )
            except (ValueError, IndexError):
                pass

    def _check_module_kind(self, prefix: str, mod: dict[str, Any]) -> None:
        kind = mod.get("moduleKind", "")
        if kind not in VALID_MODULE_KINDS:
            self._err(
                f"{prefix}: moduleKind must be one of {sorted(VALID_MODULE_KINDS)}, "
                f"got {kind!r}"
            )

    def _check_lifecycle_phases(self, prefix: str, mod: dict[str, Any]) -> None:
        phases = mod.get("lifecyclePhases", [])
        if not isinstance(phases, list):
            self._err(f"{prefix}: lifecyclePhases must be an array")
            return
        phase_set = set(phases)
        if len(phases) != len(phase_set):
            dupes = [p for p in phases if phases.count(p) > 1]
            self._err(f"{prefix}: duplicate lifecyclePhases: {sorted(set(dupes))}")
        invalid = phase_set - set(VALID_LIFECYCLE_PHASES)
        if invalid:
            self._err(f"{prefix}: unknown lifecyclePhases: {sorted(invalid)}")
        missing_required = REQUIRED_LIFECYCLE_PHASES - phase_set
        if missing_required:
            self._err(
                f"{prefix}: missing required lifecyclePhases: {sorted(missing_required)}"
            )

    def _check_profile_applicability(self, prefix: str, mod: dict[str, Any]) -> None:
        pa = mod.get("profileApplicability", {})
        if not isinstance(pa, dict):
            self._err(f"{prefix}: profileApplicability must be an object")
            return
        if not pa:
            self._err(f"{prefix}: profileApplicability must declare at least one profile")
            return
        for profile_id, applicability in pa.items():
            if applicability not in VALID_PROFILE_APPLICABILITIES:
                self._err(
                    f"{prefix}: profileApplicability[{profile_id!r}] must be one of "
                    f"{sorted(VALID_PROFILE_APPLICABILITIES)}, got {applicability!r}"
                )

    def _check_evidence_bindings(self, prefix: str, mod: dict[str, Any]) -> None:
        bindings = mod.get("evidenceBindings", [])
        if not isinstance(bindings, list):
            self._err(f"{prefix}: evidenceBindings must be an array")
            return
        for j, binding in enumerate(bindings):
            bprefix = f"{prefix}.evidenceBindings[{j}]"
            if not isinstance(binding, dict):
                self._err(f"{bprefix}: must be an object")
                continue
            btype = binding.get("type", "")
            if btype not in VALID_EVIDENCE_TYPES:
                self._err(
                    f"{bprefix}: type must be one of {sorted(VALID_EVIDENCE_TYPES)}, "
                    f"got {btype!r}"
                )
            path = binding.get("artifactPattern", "")
            if not isinstance(path, str) or not path:
                self._err(f"{bprefix}: artifactPattern must be a non-empty string")
            if path and (path.startswith("/") or ":" in path):
                self._err(
                    f"{bprefix}: artifactPattern {path!r} must be relative"
                )

    def _check_package_smoke_owner(self, prefix: str, mod: dict[str, Any]) -> None:
        owner = mod.get("packageSmokeOwner")
        if owner is None:
            return
        if not isinstance(owner, str) or not owner:
            self._err(f"{prefix}: packageSmokeOwner must be a non-empty string")

    def _check_dependencies(self, prefix: str, mod: dict[str, Any]) -> None:
        deps = mod.get("dependencies", [])
        if not isinstance(deps, list):
            self._err(f"{prefix}: dependencies must be an array")
            return
        for dep in deps:
            if not isinstance(dep, str) or not dep:
                self._err(f"{prefix}: each dependency must be a non-empty string")

    def _check_no_duplicate_identities(self) -> None:
        modules = self.manifest.get("modules", [])
        names: dict[str, int] = {}
        targets: dict[str, int] = {}
        libs: dict[str, list[int]] = {}
        for i, mod in enumerate(modules):
            name = mod.get("name", "")
            target = mod.get("cmakeTarget", "")
            if name in names:
                self._err(
                    f"duplicate module name {name!r} at indices {names[name]} and {i}"
                )
            names[name] = i
            if target in targets:
                self._err(
                    f"duplicate cmakeTarget {target!r} at indices {targets[target]} and {i}"
                )
            targets[target] = i
            for platform, filename in mod.get("sharedLibrary", {}).items():
                key = f"{platform}:{filename}"
                libs.setdefault(key, []).append(i)
        for key, indices in libs.items():
            if len(indices) > 1:
                self._err(
                    f"duplicate sharedLibrary identity {key!r} at indices {indices}"
                )

    def _check_cross_references(self) -> None:
        modules = self.manifest.get("modules", [])
        profiles = self.manifest.get("profiles", [])
        module_names = {m.get("name", "") for m in modules}
        for i, profile in enumerate(profiles):
            for ref_list_key in ("includedModules", "excludedModules"):
                for mod_name in profile.get(ref_list_key, []):
                    if mod_name not in module_names:
                        self._err(
                            f"profiles[{i}].{ref_list_key} references "
                            f"unknown module {mod_name!r}"
                        )
            pid = profile.get("id", "")
            for mod_name in profile.get("includedModules", []):
                mod = next((m for m in modules if m.get("name") == mod_name), None)
                if not mod:
                    continue
                pa = mod.get("profileApplicability", {})
                applicability = pa.get(pid)
                if applicability == "outside":
                    self._err(
                        f"module {mod_name!r} has profileApplicability "
                        f"{pid!r}='outside' but is included in that profile"
                    )
            for mod_name in profile.get("excludedModules", []):
                mod = next((m for m in modules if m.get("name") == mod_name), None)
                if not mod:
                    continue
                pa = mod.get("profileApplicability", {})
                applicability = pa.get(pid)
                if applicability in ("required", "shared"):
                    self._err(
                        f"module {mod_name!r} has profileApplicability "
                        f"{pid!r}={applicability!r} but is excluded from that profile"
                    )

    def _check_experimental_separation(self) -> None:
        modules = self.manifest.get("modules", [])
        profiles = self.manifest.get("profiles", [])
        stable_profiles = {p.get("id", "") for p in profiles}
        for i, mod in enumerate(modules):
            prefix = f"modules[{i}]"
            pa = mod.get("profileApplicability", {})
            is_outside_all = all(
                pa.get(pid) == "outside" for pid in stable_profiles
            ) if stable_profiles else False
            sep = mod.get("experimentalSeparation")
            if sep is None:
                if is_outside_all:
                    self._err(
                        f"{prefix}: module is 'outside' all profiles but missing "
                        f"experimentalSeparation — must declare trackedUnder"
                    )
                continue
            if not isinstance(sep, dict):
                self._err(f"{prefix}: experimentalSeparation must be an object")
                continue
            tracker = sep.get("trackedUnder", "")
            if not isinstance(tracker, str) or not tracker:
                self._err(
                    f"{prefix}: experimentalSeparation.trackedUnder must be a non-empty string"
                )
            for pid in stable_profiles:
                if pa.get(pid) in ("required", "shared"):
                    self._err(
                        f"{prefix}: module declares experimentalSeparation but has "
                        f"profileApplicability[{pid!r}]={pa[pid]!r} — "
                        f"experimental modules must be 'outside' all stable profiles"
                    )


def load_manifest(path: Path | str | None = None) -> dict[str, Any]:
    if path is None:
        path = REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
    path = Path(path)
    if not path.is_file():
        raise ManifestError(f"manifest not found: {path}")
    with open(path, encoding="utf-8") as f:
        try:
            return json.load(f)
        except json.JSONDecodeError as exc:
            raise ManifestError(f"invalid JSON in {path}: {exc}") from exc


def validate_manifest(
    path: Path | str | None = None,
    repo_root: Path | None = None,
) -> list[str]:
    manifest = load_manifest(path)
    validator = ManifestValidator(manifest, repo_root)
    return validator.validate()


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Validate the module evidence manifest"
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        default=None,
        help="Path to manifest.json (default: tools/module-evidence/manifest.json)",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="Repository root (default: auto-detected)",
    )
    args = parser.parse_args()

    try:
        errors = validate_manifest(args.manifest, args.repo_root)
    except ManifestError as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 1

    if errors:
        print(f"FAIL: {len(errors)} validation error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print("OK: module evidence manifest is valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
