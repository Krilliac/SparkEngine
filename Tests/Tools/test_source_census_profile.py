#!/usr/bin/env python3
"""RDY-010: no copied model or tautology may satisfy a release-profile gate.

``Tools/test_source_census.py --profile-selectors`` resolves every CTest
labeled stable-v1 or module-profile that runs SparkTests to the TEST
definitions its SPARK_TEST_NAME / SPARK_TEST_FILE / SPARK_TEST_EXCLUDE filters
reach (with TestMain.cpp's strstr semantics) and fails when one of them lives in
a mirror file, asserts nothing, or the selector cannot meet its
SPARK_TEST_EXPECT_COUNT. These tests pin the guard on the real tree and prove
each mutation that would smuggle a mirror into the profile fails by name.

When SPARK_CENSUS_CTEST and SPARK_CENSUS_BUILD_DIR are set (the CTest
registration sets them), the configured tree's own 'ctest --show-only=json-v1'
inventory is checked as well.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CENSUS_SCRIPT = REPO_ROOT / "Tools" / "test_source_census.py"
TESTS_CMAKE = REPO_ROOT / "Tests" / "CMakeLists.txt"
MIRROR_FILE = "Tests/TestFPSComponents.cpp"
TAUTOLOGICAL_TEST = "SoftwareRender_DrawFrame"

# The Windows-only D3D11 golden selector: the static view must see it on every host.
GOLDEN_ENVIRONMENT = '"SPARK_TEST_NAME=D3D11_Golden_;SPARK_TEST_EXPECT_COUNT=2"'


def _load_census():
    spec = importlib.util.spec_from_file_location("spark_test_source_census_profile", CENSUS_SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot import {CENSUS_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _run_census(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(CENSUS_SCRIPT), *arguments],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
        timeout=240,
    )


def _ctest_entry(name: str, labels: list[str], environment: list[str], command: list[str] | None = None) -> dict:
    return {
        "name": name,
        "command": command if command is not None else ["/build/bin/SparkTests", "--warn-is-error"],
        "properties": [
            {"name": "ENVIRONMENT", "value": environment},
            {"name": "LABELS", "value": labels},
            {"name": "TIMEOUT", "value": 30.0},
        ],
    }


class ProfileSelectorGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.census = _load_census()
        cls.rows = cls.census.scan(REPO_ROOT)
        cls.definitions = cls.census.resolve_registered_tests(REPO_ROOT, cls.rows)
        cls.cmake_text = TESTS_CMAKE.read_text(encoding="utf-8")
        cls.temp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def _check_ctest(self, *entries: dict) -> list[str]:
        selectors = self.census.ctest_profile_selectors({"tests": list(entries)}, "fixture.json")
        failures, _ = self.census.check_profile_selectors(selectors, self.definitions, REPO_ROOT, "fixture.json")
        return failures

    def _mutated_cmake(self, replacement_environment: str) -> Path:
        self.assertEqual(self.cmake_text.count(GOLDEN_ENVIRONMENT), 1, "D3D11_Golden selector moved; update this test")
        path = Path(self.temp.name) / f"CMakeLists-{len(os.listdir(self.temp.name))}.txt"
        path.write_text(self.cmake_text.replace(GOLDEN_ENVIRONMENT, replacement_environment), encoding="utf-8")
        return path

    # -- the real tree -------------------------------------------------------

    def test_mirror_file_fixture_is_still_a_mirror(self) -> None:
        kinds = {row["path"]: row["kind"] for row in self.rows}
        self.assertEqual(kinds.get(MIRROR_FILE), "mirror", f"{MIRROR_FILE} is no longer a mirror; pick another")

    def test_real_static_view_passes_and_covers_windows_only_selectors(self) -> None:
        result = _run_census("--profile-selectors")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for selector in (
            "D3D11_Golden",
            "D3D11_Resource",
            "NullRHIResourceLifetime",
            "FPSSinglePlayerSlice_RespawnProductionSource",
            "ModuleDiscoveryProductionSource",
        ):
            self.assertIn(f": {selector}: ", result.stdout, f"static view no longer resolves {selector}")

    def test_configured_tree_inventory_passes(self) -> None:
        ctest = os.environ.get("SPARK_CENSUS_CTEST")
        build_dir = os.environ.get("SPARK_CENSUS_BUILD_DIR")
        if not ctest or not build_dir:
            self.skipTest("SPARK_CENSUS_CTEST/SPARK_CENSUS_BUILD_DIR unset; only the CTest registration sets them")
        command = [ctest, "--test-dir", build_dir, "--show-only=json-v1"]
        if os.environ.get("SPARK_CENSUS_CONFIG"):
            command += ["-C", os.environ["SPARK_CENSUS_CONFIG"]]
        inventory = subprocess.run(command, text=True, capture_output=True, check=False, timeout=120)
        self.assertEqual(inventory.returncode, 0, inventory.stderr)
        path = Path(self.temp.name) / "configured-ctest.json"
        path.write_text(inventory.stdout, encoding="utf-8")
        result = _run_census("--profile-selectors", str(path))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ModuleDiscoveryProductionSource", result.stdout)

    # -- mutations that must fail --------------------------------------------

    def test_static_selector_pointed_at_mirror_file_fails(self) -> None:
        path = self._mutated_cmake('"SPARK_TEST_FILE=TestFPSComponents.cpp;SPARK_TEST_EXPECT_COUNT=2"')
        result = _run_census("--profile-selectors", "--cmake-lists", str(path))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("D3D11_Golden: SPARK_TEST_EXPECT_COUNT=2, SPARK_TEST_FILE=TestFPSComponents.cpp", result.stderr)
        self.assertIn(f"mirror file {MIRROR_FILE}", result.stderr)

    def test_ctest_json_selector_pointed_at_mirror_file_fails(self) -> None:
        path = Path(self.temp.name) / "mirror-ctest.json"
        entry = _ctest_entry(
            "D3D11_Golden",
            ["d3d11", "stable-v1"],
            ["SPARK_TEST_FILE=TestFPSComponents.cpp", "SPARK_TEST_EXPECT_COUNT=2"],
            ["C:\\build\\bin\\Release\\SparkTests.exe", "--warn-is-error"],
        )
        path.write_text(json.dumps({"kind": "ctestInfo", "tests": [entry]}), encoding="utf-8")
        result = _run_census("--profile-selectors", str(path))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f"{path}: D3D11_Golden:", result.stderr)
        self.assertIn(f"mirror file {MIRROR_FILE}", result.stderr)

    def test_name_selector_reaching_a_mirror_test_fails(self) -> None:
        mirror_test = next(d.name for d in self.definitions if d.path == MIRROR_FILE)
        failures = self._check_ctest(
            _ctest_entry("Probe", ["module-profile"], [f"SPARK_TEST_NAME={mirror_test}", "SPARK_TEST_EXPECT_COUNT=1"])
        )
        self.assertTrue(any(MIRROR_FILE in failure for failure in failures), failures)

    def test_tautological_body_fails(self) -> None:
        failures = self._check_ctest(
            _ctest_entry(
                "Probe", ["stable-v1"], [f"SPARK_TEST_NAME={TAUTOLOGICAL_TEST}", "SPARK_TEST_EXPECT_COUNT=1"]
            )
        )
        self.assertTrue(any("EXPECT_TRUE(true)/EXPECT_NO_CRASH" in failure for failure in failures), failures)

    def test_unfiltered_whole_suite_fails(self) -> None:
        failures = self._check_ctest(_ctest_entry("Probe", ["stable-v1"], ["SPARK_TEST_EXPECT_COUNT=5"]))
        self.assertTrue(any("runs the whole SparkTests suite" in failure for failure in failures), failures)

    def test_missing_or_malformed_expect_count_fails(self) -> None:
        for count in (None, "0", "+2", " 2", "2x", "99999999999"):
            environment = ["SPARK_TEST_NAME=ModuleDiscovery_"]
            if count is not None:
                environment.append(f"SPARK_TEST_EXPECT_COUNT={count}")
            with self.subTest(count=count):
                failures = self._check_ctest(_ctest_entry("Probe", ["stable-v1"], environment))
                self.assertTrue(any("SPARK_TEST_EXPECT_COUNT" in failure for failure in failures), failures)

    def test_expect_count_above_visible_definitions_fails(self) -> None:
        failures = self._check_ctest(
            _ctest_entry("Probe", ["stable-v1"], ["SPARK_TEST_NAME=ModuleDiscovery_", "SPARK_TEST_EXPECT_COUNT=99"])
        )
        self.assertTrue(any("tests the guard cannot see" in failure for failure in failures), failures)

    def test_empty_selection_fails(self) -> None:
        failures = self._check_ctest(
            _ctest_entry("Probe", ["stable-v1"], ["SPARK_TEST_NAME=NoSuchTest_", "SPARK_TEST_EXPECT_COUNT=1"])
        )
        self.assertTrue(any("selects no TEST definition" in failure for failure in failures), failures)

    def test_command_level_env_assignment_is_resolved(self) -> None:
        command = ["cmake", "-E", "env", "SPARK_TEST_FILE=TestFPSComponents.cpp", "/b/SparkTests"]
        failures = self._check_ctest(
            _ctest_entry("Probe", ["stable-v1"], ["SPARK_TEST_EXPECT_COUNT=1"], command)
        )
        self.assertTrue(any(MIRROR_FILE in failure for failure in failures), failures)

    def test_unresolvable_static_selector_fails_closed(self) -> None:
        path = self._mutated_cmake('"SPARK_TEST_NAME=${_golden_prefix};SPARK_TEST_EXPECT_COUNT=2"')
        result = _run_census("--profile-selectors", "--cmake-lists", str(path))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("is not a literal the guard can resolve", result.stderr)

    def test_inventory_without_profile_sparktests_selector_fails(self) -> None:
        failures = self._check_ctest(
            _ctest_entry("Script", ["stable-v1"], [], ["/usr/bin/python3", "contract.py"])
        )
        self.assertTrue(any("no stable-v1/module-profile CTest runs SparkTests" in f for f in failures), failures)

    # -- what must not fail ----------------------------------------------------

    def test_non_profile_selector_may_reach_mirrors(self) -> None:
        entries = [
            _ctest_entry("Mirror", ["unit"], ["SPARK_TEST_FILE=TestFPSComponents.cpp"]),
            _ctest_entry("Probe", ["stable-v1"], ["SPARK_TEST_NAME=ModuleDiscovery_", "SPARK_TEST_EXPECT_COUNT=7"]),
        ]
        self.assertEqual(self._check_ctest(*entries), [])

    # -- TestMain.cpp filter semantics -----------------------------------------

    def test_filters_mirror_testmain_strstr_semantics(self) -> None:
        registered = self.census.RegisteredTest
        definitions = [
            registered("NullRHI_Lifetime_A", "Tests/TestNullRHI.cpp", 1, "production-source", False),
            registered("Fixture.NullRHI_Lifetime_B", "Tests/sub/TestNullRHI.cpp", 1, "production-source", False),
            registered("Other_Lifetime_C", "Tests/TestOther.cpp", 1, "production-source", False),
        ]

        def names(**environment: str) -> list[str]:
            selector = self.census.ProfileSelector("Probe", "fixture", True, dict(environment))
            return [d.name for d in self.census.select_definitions(selector, definitions, REPO_ROOT)]

        # Substring, not prefix: TestMain uses strstr on the registered name.
        self.assertEqual(names(SPARK_TEST_NAME="Lifetime_"), [d.name for d in definitions])
        self.assertEqual(
            names(SPARK_TEST_NAME="NullRHI_Lifetime_"), ["NullRHI_Lifetime_A", "Fixture.NullRHI_Lifetime_B"]
        )
        # An empty filter is still a filter, and strstr(x, "") matches everything.
        self.assertEqual(len(names(SPARK_TEST_NAME="")), 3)
        # The file filter sees __FILE__, which may be absolute or backslashed.
        self.assertEqual(names(SPARK_TEST_FILE="sub\\TestNullRHI"), ["Fixture.NullRHI_Lifetime_B"])
        self.assertEqual(len(names(SPARK_TEST_FILE=REPO_ROOT.as_posix())), 3)
        # Excludes are comma-separated name substrings; empty entries are ignored.
        self.assertEqual(names(SPARK_TEST_NAME="Lifetime_", SPARK_TEST_EXCLUDE=",_B,Other"), ["NullRHI_Lifetime_A"])

    def test_tautology_and_literal_parsing(self) -> None:
        code_only = self.census.cpp_code_only
        tautological = self.census.is_tautological
        self.assertTrue(tautological(code_only("\n    // why\n    EXPECT_TRUE(true);\n")))
        self.assertTrue(tautological(code_only('EXPECT_NO_CRASH("a; b"); { EXPECT_TRUE( true ); }')))
        self.assertTrue(tautological(""))
        self.assertFalse(tautological(code_only("EXPECT_TRUE(true); EXPECT_EQ(Add(1, 2), 3);")))
        self.assertFalse(tautological(code_only("EXPECT_TRUE(ready);")))
        # Digit separators, raw strings and comments never open a literal or a TEST.
        text = 'int n = 1\'000; auto s = R"j({"TEST(":1})j"; /* TEST(Ghost) */ char c = \'}\';\n'
        self.assertNotIn("TEST(", code_only(text))
        self.assertIn("1'000", code_only(text))
        self.assertEqual(code_only("a /* x\ny */ b").count("\n"), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
