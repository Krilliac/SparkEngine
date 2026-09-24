#!/usr/bin/env python3
"""MOD-290: every discovered game module carries a truthful GameModules/<Name>/module.json.

The real tree must validate cleanly, and each mutation below must fail with the
named error that module_content.validate_module_manifests reports.
"""

from __future__ import annotations

import copy
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
import validate as site_data_validate  # noqa: E402
from common import load_contract  # noqa: E402
from contract_selectors import required_gate_jobs, workflow_job_ids  # noqa: E402


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

    def selector(self, name: str, prefix: str) -> dict:
        path = self.manifest_path(name)
        entries = json.loads(path.read_text(encoding="utf-8"))["tests"]["prefixes"]
        return next(entry for entry in entries if entry["prefix"] == prefix)

    def edit_selector(self, name: str, selected: str, /, **changes) -> None:
        def mutate(manifest: dict) -> None:
            for entry in manifest["tests"]["prefixes"]:
                if entry["prefix"] == selected:
                    entry.update(changes)
        self.edit(name, mutate)

    def test_renamed_test_prefix_fails(self) -> None:
        self.edit_selector("SparkGameRTS", "RTS_", prefix="RTSRenamed_")
        self.assert_named_error("test prefix matches no TEST( definition in the listed files: RTSRenamed_")

    def test_changed_declared_count_fails(self) -> None:
        # Tests/CMakeLists.txt feeds this count to SPARK_TEST_EXPECT_COUNT; a
        # manifest that drifts from the tree must fail without a build too.
        declared = self.selector("SparkGameRTS", "RTS_")["count"]
        self.edit_selector("SparkGameRTS", "RTS_", count=declared + 1)
        self.assert_named_error(f"declared count {declared + 1} for RTS_ disagrees with {declared} registered TEST(")

    def test_removed_test_definition_breaks_declared_count(self) -> None:
        source = self.root / "Tests" / "TestModuleABI.cpp"
        text = source.read_text(encoding="utf-8")
        source.unlink()
        name = "ModuleABI_AllValidationRuleOwnersReleaseCallbacksBeforeUnload"
        source.write_text(text.replace(f"TEST({name})", "TEST(ModuleABI_RenamedForMutation)"), encoding="utf-8")
        self.assert_named_error(f"declared count 1 for {name} disagrees with 0 registered TEST(")

    def test_non_positive_or_missing_count_fails(self) -> None:
        self.edit_selector("SparkGameRTS", "RTS_", count=0)
        self.assert_named_error("count must be a positive integer: 0")
        self.edit_selector("SparkGameRTS", "RTS_", count=True)
        self.assert_named_error("count must be a positive integer: True")
        self.edit("SparkGameRTS", lambda manifest: manifest["tests"].update(prefixes=["RTS_"]))
        self.assert_named_error("selector must contain exactly prefix and count")

    def test_selector_count_excludes_names_that_only_contain_the_prefix(self) -> None:
        # "ARPG_Hero_Initialize" contains "RPG_". The generated CTest runs with the
        # anchored SPARK_TEST_NAME_PREFIX, so a count that also includes the ARPG_
        # family (the substring count) must be rejected, or renaming an RPG_ test
        # while adding an ARPG_ test would leave the declared count unchanged.
        cmake = module_content._strip_cmake_comments((ROOT / "Tests" / "CMakeLists.txt").read_text(encoding="utf-8"))
        names = [name for name, _ in module_content._registered_test_names(ROOT, cmake)]
        anchored = sum(1 for name in names if name.startswith("RPG_"))
        substring = sum(1 for name in names if "RPG_" in name)
        self.assertGreater(substring, anchored, "the ARPG_ family must overlap RPG_ for this mutation to mean anything")
        self.assertEqual(anchored, self.selector("SparkGameRPG", "RPG_")["count"])
        self.edit_selector("SparkGameRPG", "RPG_", count=substring)
        self.assert_named_error(f"declared count {substring} for RPG_ disagrees with {anchored} registered TEST(")

    def test_generated_module_ctests_use_the_anchored_name_filter(self) -> None:
        cmake = (ROOT / "Tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('"SPARK_TEST_NAME_PREFIX=${_spark_module_kit_prefix};', cmake)
        self.assertNotIn('"SPARK_TEST_NAME=${_spark_module_kit_prefix};', cmake)
        runner = (ROOT / "Tests" / "TestMain.cpp").read_text(encoding="utf-8")
        self.assertIn('std::getenv("SPARK_TEST_NAME_PREFIX")', runner)

    def test_windows_only_test_needs_a_per_platform_count(self) -> None:
        # FPSScene_FailedReloadPreservesLiveObjectIdentityAndRuntimeState is
        # compiled only under SPARK_PLATFORM_WINDOWS, so one integer cannot hold.
        self.assertEqual({"windows": 5, "other": 4}, self.selector("SparkGameFPS", "FPSScene_")["count"])
        self.edit_selector("SparkGameFPS", "FPSScene_", count=5)
        self.assert_named_error("declared count 5 for FPSScene_ disagrees with 4 registered TEST( definitions "
                                "whose name starts with it (other build)")
        self.edit_selector("SparkGameFPS", "FPSScene_", count={"windows": 4, "other": 4})
        self.assert_named_error("per-platform count for FPSScene_ is equal on every platform; declare one integer")
        self.edit_selector("SparkGameFPS", "FPSScene_", count={"windows": 5, "linux": 4})
        self.assert_named_error("per-platform count must map exactly ['windows', 'other'] to positive integers")

    def test_preprocessor_platform_tracking(self) -> None:
        source = "\n".join([
            "#ifdef SPARK_PLATFORM_WINDOWS", "TEST(P_Windows)", "#else", "TEST(P_Other)", "#endif",
            "#if !defined(_WIN32)", "#if SOME_FEATURE", "TEST(P_OtherNested)", "#endif", "#endif",
            "#ifdef SOME_FEATURE", "TEST(P_Unknown)", "#else", "TEST(P_UnknownElse)", "#endif",
            "TEST(P_Everywhere)",
        ])
        platforms = {name: set(active) for name, active in module_content._test_definitions_by_platform(source)}
        self.assertEqual({
            "P_Windows": {"windows"},
            "P_Other": {"other"},
            "P_OtherNested": {"other"},
            "P_Unknown": {"windows", "other"},
            "P_UnknownElse": {"windows", "other"},
            "P_Everywhere": {"windows", "other"},
        }, platforms)

    def test_duplicate_selector_fails(self) -> None:
        self.edit("SparkGameRTS", lambda manifest: manifest["tests"]["prefixes"].append(
            dict(manifest["tests"]["prefixes"][0])))
        self.assert_named_error("duplicate TEST-name prefix: RTS_")

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


class ParityEvidenceTests(unittest.TestCase):
    """A parity score of 3 needs a release profile plus resolving, required evidence."""

    contract: dict

    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_contract()

    def setUp(self) -> None:
        self.mutable = copy.deepcopy(self.contract)
        self.parity = self.mutable["parityDimensions"]
        self.parity.setdefault("parityEvidence", {})

    def messages(self) -> list[str]:
        validator = site_data_validate.Validator(self.mutable)
        validator.validate_modules()
        return validator.errors

    def set_score(self, module: str, dimension: str, value) -> None:
        self.parity["currentScores"][module][self.parity["dimensions"].index(dimension)] = value

    def bind(self, module: str, dimension: str, *, selectors: list[str], jobs: list[str]) -> None:
        self.parity["parityEvidence"].setdefault(module, {})[dimension] = {
            "testSelectors": selectors,
            "requiredCiJobs": jobs,
        }

    def assert_named_error(self, fragment: str) -> None:
        messages = self.messages()
        self.assertTrue(any(fragment in message for message in messages), messages)

    def test_checked_in_scores_validate(self) -> None:
        self.assertEqual([], self.messages())

    def test_required_gate_jobs_are_parsed_from_the_aggregate(self) -> None:
        jobs = required_gate_jobs()
        self.assertIn("module-evidence", jobs)
        self.assertIn("module-profile-package-smoke", jobs)
        self.assertIn("build-macos", workflow_job_ids())
        self.assertNotIn("build-macos", jobs)

    def test_unevidenced_three_fails(self) -> None:
        self.set_score("SparkGameFPS", "lifecycle", 3)
        self.assert_named_error("parity.SparkGameFPS.lifecycle: score 3 (validated shipping candidate) requires parityEvidence")

    def test_reverted_mmofps_claim_fails(self) -> None:
        self.set_score("SparkGameMMOFPS", "ai", 3)
        messages = self.messages()
        self.assertTrue(any("parity.SparkGameMMOFPS.ai: score 3" in m and "release profile" in m for m in messages), messages)
        self.assertTrue(any("parity.SparkGameMMOFPS.ai: score 3" in m and "parityEvidence" in m for m in messages), messages)

    def test_evidence_does_not_excuse_a_module_outside_every_profile(self) -> None:
        self.set_score("SparkGameMMOFPS", "ai", 3)
        self.bind("SparkGameMMOFPS", "ai", selectors=["ModuleManifest_Contract"], jobs=["module-evidence"])
        messages = self.messages()
        self.assertEqual(1, len(messages), messages)
        self.assertIn("requires the module to be included in a release profile", messages[0])

    def test_evidenced_three_in_profile_passes(self) -> None:
        self.set_score("SparkGameFPS", "lifecycle", 3)
        self.bind("SparkGameFPS", "lifecycle", selectors=["ModuleManifest_Contract"], jobs=["module-evidence"])
        self.assertEqual([], self.messages())

    def test_selector_only_evidence_fails(self) -> None:
        self.set_score("SparkGameFPS", "lifecycle", 3)
        self.bind("SparkGameFPS", "lifecycle", selectors=["ModuleManifest_Contract"], jobs=[])
        self.assert_named_error("requires parityEvidence naming at least one resolving test selector and one required-ci-gate job")

    def test_job_outside_required_gate_fails(self) -> None:
        self.set_score("SparkGameFPS", "lifecycle", 3)
        self.bind("SparkGameFPS", "lifecycle", selectors=["ModuleManifest_Contract"], jobs=["build-macos"])
        self.assert_named_error("'build-macos' is not a need of required-ci-gate")
        self.assert_named_error("parity.SparkGameFPS.lifecycle: score 3")

    def test_unresolved_selector_fails_even_below_three(self) -> None:
        self.bind("SparkGameRTS", "ai", selectors=["RTSNoSuchSuite_*"], jobs=["module-evidence"])
        self.assert_named_error("'RTSNoSuchSuite_*' resolves to nothing")

    def test_unknown_job_fails(self) -> None:
        self.bind("SparkGameRTS", "ai", selectors=["ModuleManifest_Contract"], jobs=["no-such-parity-job"])
        self.assert_named_error("'no-such-parity-job' resolves to nothing: no workflow job is defined with this id")

    def test_unknown_module_and_dimension_fail(self) -> None:
        self.bind("SparkGameMissing", "ai", selectors=["ModuleManifest_Contract"], jobs=["module-evidence"])
        self.bind("SparkGameRTS", "graphics", selectors=["ModuleManifest_Contract"], jobs=["module-evidence"])
        self.assert_named_error("parityDimensions.parityEvidence.SparkGameMissing: names no scored module")
        self.assert_named_error("parityDimensions.parityEvidence.SparkGameRTS.graphics: names no parity dimension")

    def test_unknown_evidence_key_fails(self) -> None:
        self.parity["parityEvidence"]["SparkGameRTS"] = {"ai": {"testSelectors": [], "notes": "mutation"}}
        self.assert_named_error("must be an object with only testSelectors and requiredCiJobs")


if __name__ == "__main__":
    unittest.main()
