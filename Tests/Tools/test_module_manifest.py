#!/usr/bin/env python3
"""MOD-290: every discovered game module carries a truthful GameModules/<Name>/module.json.

The real tree must validate cleanly, and each mutation below must fail with the
named error that module_content.validate_module_manifests reports.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "site-data"))
import module_content  # noqa: E402


def _discovered(root: Path) -> dict[str, Path]:
    return {path.name: path for path in (root / "GameModules").iterdir() if path.is_dir() and path.name != "__pycache__"}


def _authoritative(root: Path) -> dict[str, dict]:
    evidence = json.loads((root / module_content.EVIDENCE_RELATIVE).read_text(encoding="utf-8"))
    return {module["name"]: module for module in evidence["modules"]}


class RealTreeManifestTests(unittest.TestCase):
    def test_every_discovered_module_has_a_valid_manifest(self) -> None:
        findings = module_content.validate_module_manifests(ROOT, _discovered(ROOT), _authoritative(ROOT))
        self.assertEqual([], findings)

    def test_manifest_count_matches_discovery_and_evidence(self) -> None:
        discovered = set(_discovered(ROOT))
        manifests = {path.parent.name for path in (ROOT / "GameModules").glob(f"*/{module_content.MODULE_MANIFEST_NAME}")}
        self.assertEqual(discovered, manifests)
        self.assertEqual(discovered, set(_authoritative(ROOT)))

    def test_manifests_carry_no_profile_policy(self) -> None:
        for path in (ROOT / "GameModules").glob(f"*/{module_content.MODULE_MANIFEST_NAME}"):
            manifest = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(module_content.MODULE_MANIFEST_KEYS, set(manifest), path)


def _link(link: Path, target: Path) -> None:
    """Mirror a read-only input without copying it where the platform allows.

    Windows only grants symlink creation to elevated shells or Developer Mode, so
    directories become junctions there (no privilege needed) and files are copied.
    """
    if sys.platform == "win32":
        if target.is_dir():
            subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)
        else:
            shutil.copy2(target, link)
        return
    link.symlink_to(target, target_is_directory=target.is_dir())


class ManifestMutationTests(unittest.TestCase):
    """Mutations run against a scratch mirror; the checked-in tree is never edited."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="module-manifest-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # Large read-only inputs are linked; every file a mutation edits is copied.
        _link(self.root / "Assets", ROOT / "Assets")
        tests = self.root / "Tests"
        tests.mkdir()
        for entry in (ROOT / "Tests").iterdir():
            if entry.name == "CMakeLists.txt":
                shutil.copy2(entry, tests / entry.name)
            else:
                _link(tests / entry.name, entry)
        shutil.copytree(ROOT / module_content.WORK_ITEMS_RELATIVE, self.root / module_content.WORK_ITEMS_RELATIVE)
        evidence = self.root / module_content.EVIDENCE_RELATIVE
        evidence.parent.mkdir(parents=True)
        shutil.copy2(ROOT / module_content.EVIDENCE_RELATIVE, evidence)
        for name, source_dir in _discovered(ROOT).items():
            module_dir = self.root / "GameModules" / name
            module_dir.mkdir(parents=True)
            for child in ("Source", "Assets"):
                if (source_dir / child).is_dir():
                    _link(module_dir / child, source_dir / child)
            shutil.copy2(source_dir / module_content.MODULE_MANIFEST_NAME, module_dir / module_content.MODULE_MANIFEST_NAME)
            # The docs rule is exercised by its own mutation; give every mirrored
            # module a README so the baseline isolates one failure per case.
            readme = module_dir / "README.md"
            if (source_dir / "README.md").is_file():
                shutil.copy2(source_dir / "README.md", readme)
            else:
                readme.write_text(f"# {name}\n", encoding="utf-8")

    def messages(self) -> list[str]:
        findings = module_content.validate_module_manifests(self.root, _discovered(self.root), _authoritative(self.root))
        return [f"{location}: {message}" for location, message in findings]

    def manifest_path(self, name: str) -> Path:
        return self.root / "GameModules" / name / module_content.MODULE_MANIFEST_NAME

    def edit(self, name: str, mutate) -> None:
        path = self.manifest_path(name)
        manifest = json.loads(path.read_text(encoding="utf-8"))
        mutate(manifest)
        path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    def assert_named_error(self, fragment: str) -> None:
        messages = self.messages()
        self.assertTrue(any(fragment in message for message in messages), messages)

    def test_mirror_baseline_is_clean(self) -> None:
        self.assertEqual([], self.messages())

    def test_removed_module_manifest_fails(self) -> None:
        self.manifest_path("SparkGameRTS").unlink()
        self.assert_named_error("module manifest is missing: SparkGameRTS")

    def test_missing_readme_fails(self) -> None:
        (self.root / "GameModules" / "SparkGameRTS" / "README.md").unlink()
        self.assert_named_error("module README is missing: GameModules/SparkGameRTS/README.md")

    def test_docs_pointing_elsewhere_fails(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest["docs"].update(readme="GameModules/SparkGameRTS/MISSING.md"))
        self.assert_named_error("docs must contain exactly readme: 'GameModules/SparkGameRTS/README.md'")

    def test_undeclared_not_applicable_cell_fails(self) -> None:
        for path in (self.root / module_content.WORK_ITEMS_RELATIVE).glob("*.json"):
            document = json.loads(path.read_text(encoding="utf-8"))
            if "parityDimensions" in document:
                parity = document["parityDimensions"]
                index = parity["dimensions"].index("networking")
                parity["currentScores"]["SparkGameFPS"][index] = "N/A"
                path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        self.assert_named_error("parity N/A cells disagree with declared notApplicable dimensions for SparkGameFPS")

    def test_declared_not_applicable_without_row_cell_fails(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest["parity"]["notApplicable"].append(
            {"dimension": "persistence", "reason": "Declared here only to prove the row disagreement is caught."}))
        self.assert_named_error("parity N/A cells disagree with declared notApplicable dimensions for SparkGameRTS")

    def test_renamed_test_prefix_fails(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest["tests"].update(
            prefixes=["RTSRenamed_" if prefix == "RTS_" else prefix for prefix in manifest["tests"]["prefixes"]]))
        self.assert_named_error("test prefix matches no TEST( definition in the listed files: RTSRenamed_")

    def test_missing_test_source_fails(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest["tests"]["files"].append("Tests/TestDoesNotExist.cpp"))
        self.assert_named_error("referenced file does not exist: Tests/TestDoesNotExist.cpp")

    def test_unregistered_test_source_fails(self) -> None:
        cmake = self.root / "Tests" / "CMakeLists.txt"
        text = cmake.read_text(encoding="utf-8")
        self.assertIn("TestGameModuleRTS.cpp", text)
        cmake.write_text(text.replace("TestGameModuleRTS.cpp", "TestGameModuleRTSRemoved.cpp"), encoding="utf-8")
        self.assert_named_error("test source is not registered in Tests/CMakeLists.txt: Tests/TestGameModuleRTS.cpp")

    def test_missing_source_directory_fails(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest.update(sourceDirectory="GameModules/SparkGameRTS/Missing"))
        messages = self.messages()
        self.assertTrue(any("sourceDirectory disagrees with" in message for message in messages), messages)
        self.assertTrue(any("referenced directory does not exist: GameModules/SparkGameRTS/Missing" in message for message in messages), messages)

    def test_cmake_target_drift_fails(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest.update(cmakeTarget="SparkGameRTSRenamed"))
        self.assert_named_error("cmakeTarget disagrees with tools/module-evidence/manifest.json")

    def test_profile_policy_key_is_rejected(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest.update(profileApplicability={"stable-v1": "required"}))
        self.assert_named_error("unknown=['profileApplicability']")

    def test_assets_none_for_module_that_ships_assets_fails(self) -> None:
        self.edit("SparkGameVisualScript", lambda manifest: manifest.update(
            assets={"state": "none", "reason": "Mutation: claims no assets although the module ships some."}))
        self.assert_named_error("assets declared 'none' but the module ships asset root GameModules/SparkGameVisualScript/Assets")

    def test_assets_none_for_module_with_shared_roots_fails(self) -> None:
        self.edit("SparkGameMMO", lambda manifest: manifest.update(
            assets={"state": "none", "reason": "Mutation: claims no assets although Assets/*/MMO roots exist."}))
        self.assert_named_error("assets declared 'none' but the module ships asset root Assets/Models/MMO")

    def test_undeclared_module_data_root_fails(self) -> None:
        self.edit("SparkGameMMOFPS", lambda manifest: manifest["assets"].update(
            roots=[root for root in manifest["assets"]["roots"] if root["directory"] != "Assets/MMOFPS/Data"]))
        self.assert_named_error("asset root shipped by the module is not declared: Assets/MMOFPS/Data")

    def test_missing_asset_root_fails(self) -> None:
        self.edit("SparkGameMMO", lambda manifest: manifest["assets"]["roots"].append(
            {"directory": "Assets/Models/MissingRoot", "manifest": "Assets/assets.integrity.json"}))
        self.assert_named_error("referenced directory does not exist: Assets/Models/MissingRoot")

    def test_asset_manifest_that_does_not_cover_root_fails(self) -> None:
        self.edit("SparkGameMMO", lambda manifest: manifest["assets"]["roots"].__setitem__(
            0, {"directory": "Assets/Models/MMO", "manifest": "GameModules/SparkGameVisualScript/Assets/manifest.json"}))
        self.assert_named_error("asset manifest does not list")

    def test_fps_shared_root_must_be_declared(self) -> None:
        self.edit("SparkGameFPS", lambda manifest: manifest["assets"].update(
            roots=[root for root in manifest["assets"]["roots"] if root["directory"] != "Assets/Scenes"]))
        self.assert_named_error("asset root shipped by the module is not declared: Assets/Scenes")

    def test_short_reason_is_rejected(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest["parity"]["notApplicable"][0].update(reason="n/a"))
        self.assert_named_error("reason must be a written explanation")


if __name__ == "__main__":
    unittest.main()
