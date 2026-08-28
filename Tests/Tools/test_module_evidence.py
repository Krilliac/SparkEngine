#!/usr/bin/env python3
"""Adversarial tests for the RDY-010 module evidence manifest and validator.

Tests operate on deep-copied manifest data.  No repository file is modified.

Rejection categories tested:
  - Copied mirror models (sourceDirectory pointing elsewhere)
  - Test-only source substitutions (test paths as source)
  - Missing lifecycle phases
  - Ambiguous/duplicate module identities (name, target, library)
  - Repository-only paths (absolute, home-dir)
  - Stale commit bindings (malformed SHA)
  - Unsupported profile promotion (outside module in included list)
  - Schema version mismatch
  - Unknown keys (fail-closed)
  - Invalid shared library naming
  - Experimental separation violations
  - Evidence binding validation
"""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "module-evidence"))

from validate_manifest import ManifestValidator, load_manifest  # noqa: E402


def _load_valid() -> dict[str, Any]:
    return load_manifest(REPO_ROOT / "tools" / "module-evidence" / "manifest.json")


def _validate(manifest: dict[str, Any]) -> list[str]:
    return ManifestValidator(manifest, REPO_ROOT).validate()


def _make_minimal_valid() -> dict[str, Any]:
    """Build a minimal valid manifest for mutation testing."""
    return {
        "schemaVersion": "stable-v1",
        "generatedAt": "2026-08-28T00:00:00Z",
        "commitSHA": "a" * 40,
        "profiles": [
            {
                "id": "stable-v1",
                "includedModules": ["SparkGameFPS"],
                "excludedModules": [],
            }
        ],
        "modules": [
            {
                "name": "SparkGameFPS",
                "cmakeTarget": "SparkGameFPS",
                "sharedLibrary": {
                    "windows": "SparkGameFPS.dll",
                    "linux": "libSparkGameFPS.so",
                },
                "sourceDirectory": "GameModules/SparkGameFPS/Source",
                "moduleKind": "Game",
                "lifecyclePhases": [
                    "SparkGetModuleCompatibility",
                    "CreateModule",
                    "GetModuleInfo",
                    "OnLoad",
                    "OnUpdate",
                    "OnFixedUpdate",
                    "OnRender",
                    "OnImGui",
                    "OnResize",
                    "OnPause",
                    "OnResume",
                    "CanUnload",
                    "OnUnload",
                    "DestroyModule",
                ],
                "profileApplicability": {"stable-v1": "required"},
            }
        ],
    }


class TestProductionManifestValid(unittest.TestCase):
    """The checked-in manifest must pass validation."""

    def test_production_manifest_validates(self) -> None:
        manifest = _load_valid()
        errors = _validate(manifest)
        self.assertEqual(errors, [], f"Production manifest has errors: {errors}")

    def test_production_manifest_has_all_11_modules(self) -> None:
        manifest = _load_valid()
        self.assertEqual(len(manifest["modules"]), 11)

    def test_production_manifest_stable_v1_includes_fps(self) -> None:
        manifest = _load_valid()
        profile = manifest["profiles"][0]
        self.assertEqual(profile["id"], "stable-v1")
        self.assertIn("SparkGameFPS", profile["includedModules"])

    def test_production_manifest_excludes_experimentals_from_stable(self) -> None:
        manifest = _load_valid()
        profile = manifest["profiles"][0]
        excluded = set(profile["excludedModules"])
        for mod in manifest["modules"]:
            if mod["name"] != "SparkGameFPS":
                self.assertIn(mod["name"], excluded)


class TestSchemaVersionReject(unittest.TestCase):
    """Wrong or missing schema version must fail."""

    def test_wrong_schema_version(self) -> None:
        m = _make_minimal_valid()
        m["schemaVersion"] = "unstable-v2"
        errors = _validate(m)
        self.assertTrue(any("schemaVersion" in e for e in errors))

    def test_missing_schema_version(self) -> None:
        m = _make_minimal_valid()
        del m["schemaVersion"]
        errors = _validate(m)
        self.assertTrue(any("missing required top-level" in e for e in errors))

    def test_numeric_schema_version(self) -> None:
        m = _make_minimal_valid()
        m["schemaVersion"] = 1
        errors = _validate(m)
        self.assertTrue(any("schemaVersion" in e for e in errors))


class TestCommitSHAReject(unittest.TestCase):
    """Stale or malformed commit SHA must fail."""

    def test_short_sha(self) -> None:
        m = _make_minimal_valid()
        m["commitSHA"] = "abc123"
        errors = _validate(m)
        self.assertTrue(any("commitSHA" in e for e in errors))

    def test_uppercase_sha(self) -> None:
        m = _make_minimal_valid()
        m["commitSHA"] = "A" * 40
        errors = _validate(m)
        self.assertTrue(any("commitSHA" in e for e in errors))

    def test_null_sha(self) -> None:
        m = _make_minimal_valid()
        m["commitSHA"] = None
        errors = _validate(m)
        self.assertTrue(any("commitSHA" in e for e in errors))

    def test_empty_sha(self) -> None:
        m = _make_minimal_valid()
        m["commitSHA"] = ""
        errors = _validate(m)
        self.assertTrue(any("commitSHA" in e for e in errors))


class TestUnknownKeysReject(unittest.TestCase):
    """Unknown keys at any level must fail (fail-closed)."""

    def test_unknown_top_level_key(self) -> None:
        m = _make_minimal_valid()
        m["extraField"] = "surprise"
        errors = _validate(m)
        self.assertTrue(any("unknown top-level" in e for e in errors))

    def test_unknown_module_key(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["hackField"] = True
        errors = _validate(m)
        self.assertTrue(any("unknown keys" in e for e in errors))


class TestDuplicateIdentityReject(unittest.TestCase):
    """Duplicate module names, targets, or library filenames must fail."""

    def test_duplicate_module_name(self) -> None:
        m = _make_minimal_valid()
        dupe = copy.deepcopy(m["modules"][0])
        dupe["cmakeTarget"] = "SparkGameFPS2"
        dupe["sharedLibrary"] = {"windows": "SparkGameFPS2.dll"}
        m["modules"].append(dupe)
        errors = _validate(m)
        self.assertTrue(any("duplicate module name" in e for e in errors))

    def test_duplicate_cmake_target(self) -> None:
        m = _make_minimal_valid()
        dupe = copy.deepcopy(m["modules"][0])
        dupe["name"] = "SparkGameFPS2"
        dupe["sharedLibrary"] = {"windows": "SparkGameFPS2.dll"}
        dupe["sourceDirectory"] = "GameModules/SparkGameFPS/Source"
        m["modules"].append(dupe)
        errors = _validate(m)
        self.assertTrue(any("duplicate cmakeTarget" in e for e in errors))

    def test_duplicate_shared_library(self) -> None:
        m = _make_minimal_valid()
        dupe = copy.deepcopy(m["modules"][0])
        dupe["name"] = "SparkGameFPS2"
        dupe["cmakeTarget"] = "SparkGameFPS2"
        m["modules"].append(dupe)
        errors = _validate(m)
        self.assertTrue(any("duplicate sharedLibrary" in e for e in errors))


class TestMissingLifecyclePhasesReject(unittest.TestCase):
    """Missing required lifecycle phases must fail."""

    def test_missing_onload(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["lifecyclePhases"].remove("OnLoad")
        errors = _validate(m)
        self.assertTrue(any("missing required lifecyclePhases" in e for e in errors))
        self.assertTrue(any("OnLoad" in e for e in errors))

    def test_missing_create_module(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["lifecyclePhases"].remove("CreateModule")
        errors = _validate(m)
        self.assertTrue(any("CreateModule" in e for e in errors))

    def test_missing_compatibility(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["lifecyclePhases"].remove("SparkGetModuleCompatibility")
        errors = _validate(m)
        self.assertTrue(any("SparkGetModuleCompatibility" in e for e in errors))

    def test_duplicate_lifecycle_phase(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["lifecyclePhases"].append("OnLoad")
        errors = _validate(m)
        self.assertTrue(any("duplicate lifecyclePhases" in e for e in errors))

    def test_unknown_lifecycle_phase(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["lifecyclePhases"].append("OnMagic")
        errors = _validate(m)
        self.assertTrue(any("unknown lifecyclePhases" in e for e in errors))


class TestSourceDirectoryReject(unittest.TestCase):
    """Invalid source directories must fail."""

    def test_absolute_path(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "/home/user/SparkGameFPS/Source"
        errors = _validate(m)
        self.assertTrue(any("repository-relative" in e for e in errors))

    def test_windows_absolute_path(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "C:\\Users\\dev\\GameModules"
        errors = _validate(m)
        self.assertTrue(any("repository-relative" in e for e in errors))

    def test_home_dir_reference(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "~/GameModules/SparkGameFPS/Source"
        errors = _validate(m)
        self.assertTrue(any("user-home" in e for e in errors))

    def test_test_path_substitution(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "Tests/GameModules/SparkGameFPS"
        errors = _validate(m)
        self.assertTrue(any("test path" in e for e in errors))

    def test_nonexistent_directory(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "GameModules/SparkGameFPS/DoesNotExist"
        errors = _validate(m)
        self.assertTrue(any("does not exist" in e for e in errors))


class TestSharedLibraryReject(unittest.TestCase):
    """Invalid shared library names must fail."""

    def test_wrong_windows_extension(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sharedLibrary"]["windows"] = "SparkGameFPS.so"
        errors = _validate(m)
        self.assertTrue(any("does not match" in e for e in errors))

    def test_wrong_linux_prefix(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sharedLibrary"]["linux"] = "SparkGameFPS.so"
        errors = _validate(m)
        self.assertTrue(any("does not match" in e for e in errors))

    def test_unknown_platform(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sharedLibrary"]["haiku"] = "libSparkGameFPS.so"
        errors = _validate(m)
        self.assertTrue(any("unknown sharedLibrary platform" in e for e in errors))

    def test_empty_library_dict(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sharedLibrary"] = {}
        errors = _validate(m)
        self.assertTrue(any("at least one platform" in e for e in errors))


class TestProfileApplicabilityReject(unittest.TestCase):
    """Invalid profile applicability values must fail."""

    def test_invalid_applicability(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["profileApplicability"]["stable-v1"] = "promoted"
        errors = _validate(m)
        self.assertTrue(any("profileApplicability" in e for e in errors))

    def test_empty_applicability(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["profileApplicability"] = {}
        errors = _validate(m)
        self.assertTrue(any("at least one profile" in e for e in errors))

    def test_outside_in_included(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["profileApplicability"]["stable-v1"] = "outside"
        errors = _validate(m)
        self.assertTrue(any("'outside' but is included" in e for e in errors))


class TestModuleKindReject(unittest.TestCase):
    """Invalid module kinds must fail."""

    def test_invalid_kind(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["moduleKind"] = "Plugin"
        errors = _validate(m)
        self.assertTrue(any("moduleKind" in e for e in errors))

    def test_empty_kind(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["moduleKind"] = ""
        errors = _validate(m)
        self.assertTrue(any("moduleKind" in e for e in errors))


class TestExperimentalSeparationReject(unittest.TestCase):
    """Experimental module in stable profile must fail."""

    def test_experimental_in_required(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["experimentalSeparation"] = {"trackedUnder": "RDY-015"}
        m["modules"][0]["profileApplicability"]["stable-v1"] = "required"
        errors = _validate(m)
        self.assertTrue(any("experimentalSeparation" in e for e in errors))

    def test_experimental_in_shared(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["experimentalSeparation"] = {"trackedUnder": "RDY-015"}
        m["modules"][0]["profileApplicability"]["stable-v1"] = "shared"
        errors = _validate(m)
        self.assertTrue(any("experimentalSeparation" in e for e in errors))

    def test_experimental_outside_is_ok(self) -> None:
        m = _make_minimal_valid()
        extra_mod = copy.deepcopy(m["modules"][0])
        extra_mod["name"] = "SparkGame"
        extra_mod["cmakeTarget"] = "SparkGame"
        extra_mod["sharedLibrary"] = {"windows": "SparkGame.dll"}
        extra_mod["sourceDirectory"] = "GameModules/SparkGame/Source"
        extra_mod["profileApplicability"] = {"stable-v1": "outside"}
        extra_mod["experimentalSeparation"] = {"trackedUnder": "RDY-015"}
        m["modules"].append(extra_mod)
        errors = _validate(m)
        exp_errors = [e for e in errors if "experimentalSeparation" in e]
        self.assertEqual(exp_errors, [])

    def test_missing_tracker(self) -> None:
        m = _make_minimal_valid()
        extra_mod = copy.deepcopy(m["modules"][0])
        extra_mod["name"] = "SparkGame"
        extra_mod["cmakeTarget"] = "SparkGame"
        extra_mod["sharedLibrary"] = {"windows": "SparkGame.dll"}
        extra_mod["sourceDirectory"] = "GameModules/SparkGame/Source"
        extra_mod["profileApplicability"] = {"stable-v1": "outside"}
        extra_mod["experimentalSeparation"] = {"trackedUnder": ""}
        m["modules"].append(extra_mod)
        errors = _validate(m)
        self.assertTrue(any("trackedUnder" in e for e in errors))


class TestEvidenceBindingsReject(unittest.TestCase):
    """Invalid evidence bindings must fail."""

    def test_unknown_evidence_type(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["evidenceBindings"] = [
            {"type": "magic-sauce", "artifactPattern": "build/magic.xml"}
        ]
        errors = _validate(m)
        self.assertTrue(any("type must be one of" in e for e in errors))

    def test_absolute_artifact_path(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["evidenceBindings"] = [
            {"type": "junit-xml", "artifactPattern": "/tmp/results.xml"}
        ]
        errors = _validate(m)
        self.assertTrue(any("must be relative" in e for e in errors))

    def test_dotdot_traversal_artifact(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["evidenceBindings"] = [
            {"type": "junit-xml", "artifactPattern": "../../etc/shadow"}
        ]
        errors = _validate(m)
        self.assertTrue(any("traversal" in e for e in errors))

    def test_dotdot_nested_traversal_artifact(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["evidenceBindings"] = [
            {"type": "junit-xml", "artifactPattern": "build/../../.git/config"}
        ]
        errors = _validate(m)
        self.assertTrue(any("traversal" in e for e in errors))

    def test_backslash_absolute_artifact(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["evidenceBindings"] = [
            {"type": "junit-xml", "artifactPattern": "\\\\server\\share\\results.xml"}
        ]
        errors = _validate(m)
        self.assertTrue(any("must be relative" in e for e in errors))

    def test_missing_artifact_pattern(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["evidenceBindings"] = [{"type": "junit-xml"}]
        errors = _validate(m)
        self.assertTrue(any("artifactPattern" in e for e in errors))


class TestCrossReferenceReject(unittest.TestCase):
    """Profile referencing non-existent module must fail."""

    def test_included_unknown_module(self) -> None:
        m = _make_minimal_valid()
        m["profiles"][0]["includedModules"].append("SparkGamePhantom")
        errors = _validate(m)
        self.assertTrue(any("unknown module" in e for e in errors))

    def test_excluded_unknown_module(self) -> None:
        m = _make_minimal_valid()
        m["profiles"][0]["excludedModules"].append("SparkGamePhantom")
        errors = _validate(m)
        self.assertTrue(any("unknown module" in e for e in errors))

    def test_module_in_both_included_and_excluded(self) -> None:
        m = _make_minimal_valid()
        m["profiles"][0]["excludedModules"].append("SparkGameFPS")
        errors = _validate(m)
        self.assertTrue(any("both included and excluded" in e for e in errors))


class TestSourceDirectoryTraversalReject(unittest.TestCase):
    """Dotdot traversal and non-GameModules source trees must be caught."""

    def test_dotdot_traversal(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "GameModules/../SparkEngine/Source"
        errors = _validate(m)
        self.assertTrue(any("traversal" in e for e in errors))

    def test_dotdot_backslash(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "GameModules\\..\\SparkEngine\\Source"
        errors = _validate(m)
        self.assertTrue(any("traversal" in e for e in errors))

    def test_non_gamemodules_source(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "SparkEngine/Source"
        errors = _validate(m)
        self.assertTrue(any("GameModules/" in e for e in errors))

    def test_non_gamemodules_editor(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "SparkEditor/Source"
        errors = _validate(m)
        self.assertTrue(any("GameModules/" in e for e in errors))


class TestCopiedMirrorModelReject(unittest.TestCase):
    """Source directory pointing to a different module's tree must be caught."""

    def test_source_mismatch_is_flagged_as_mirror(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["sourceDirectory"] = "GameModules/SparkGame/Source"
        errors = _validate(m)
        self.assertTrue(
            any("mirror" in e.lower() for e in errors),
            f"Expected copied mirror model error, got: {errors}",
        )


class TestModuleNameReject(unittest.TestCase):
    """Module names must be alphanumeric PascalCase."""

    def test_hyphenated_name(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["name"] = "Spark-Game-FPS"
        errors = _validate(m)
        self.assertTrue(any("alphanumeric PascalCase" in e for e in errors))

    def test_empty_name(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["name"] = ""
        errors = _validate(m)
        self.assertTrue(any("non-empty string" in e for e in errors))

    def test_numeric_start(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["name"] = "123Game"
        errors = _validate(m)
        self.assertTrue(any("alphanumeric" in e for e in errors))


class TestProfileReject(unittest.TestCase):
    """Profile-level validation."""

    def test_no_profiles(self) -> None:
        m = _make_minimal_valid()
        m["profiles"] = []
        errors = _validate(m)
        self.assertTrue(any("at least one profile" in e for e in errors))

    def test_duplicate_profile_id(self) -> None:
        m = _make_minimal_valid()
        m["profiles"].append(copy.deepcopy(m["profiles"][0]))
        errors = _validate(m)
        self.assertTrue(any("duplicate profile id" in e for e in errors))


class TestDependencyReject(unittest.TestCase):
    """Dependency validation."""

    def test_empty_dependency_string(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["dependencies"] = [""]
        errors = _validate(m)
        self.assertTrue(any("non-empty string" in e for e in errors))

    def test_non_string_dependency(self) -> None:
        m = _make_minimal_valid()
        m["modules"][0]["dependencies"] = [42]
        errors = _validate(m)
        self.assertTrue(any("non-empty string" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
