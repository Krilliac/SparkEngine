#!/usr/bin/env python3
"""Adversarial tests for the CI-110 CTest timeout/label policy validator."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPO_ROOT / "Tools" / "validate_ctest_policy.py"
TESTS_CMAKE = REPO_ROOT / "Tests" / "CMakeLists.txt"

GOOD_CMAKE = """\
add_test(NAME Alpha COMMAND alpha)
set_tests_properties(Alpha PROPERTIES
    LABELS "unit"
    TIMEOUT 30)
foreach(_selector IN ITEMS A B)
    add_test(NAME ${_selector} COMMAND runner --filter "${_selector}")
    set_tests_properties(${_selector} PROPERTIES LABELS "security;unit" TIMEOUT 30)
endforeach()
add_test(
    NAME Beta
    COMMAND ${CMAKE_COMMAND} "-DVALUE=$<IF:$<CONFIG:Debug>,a,b>" -P script.cmake
)
set_tests_properties(Beta PROPERTIES
    ENVIRONMENT "X=1"
    LABELS "integration"
    TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})
"""


def ctest_json(*tests: dict) -> str:
    return json.dumps({"kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": list(tests)})


def ctest_entry(name: str, **props: object) -> dict:
    return {
        "name": name,
        "command": ["x"],
        "properties": [{"name": key, "value": value} for key, value in props.items()],
    }


class CTestPolicyValidator(unittest.TestCase):
    def run_validator(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VALIDATOR), *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def check_cmake(self, text: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "CMakeLists.txt"
            path.write_text(text, encoding="utf-8")
            return self.run_validator("--cmake-lists", str(path))

    def check_json(self, text: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ctest.json"
            path.write_text(text, encoding="utf-8")
            return self.run_validator("--ctest-json", str(path))

    # -- static CMake view ---------------------------------------------------

    def test_well_formed_registrations_pass(self) -> None:
        result = self.check_cmake(GOOD_CMAKE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (3 test registration(s)", result.stdout)

    def test_test_without_timeout_fails_by_name(self) -> None:
        result = self.check_cmake(GOOD_CMAKE + "add_test(NAME Hangs COMMAND hang)\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Hangs: no TIMEOUT", result.stderr)
        self.assertIn("Hangs: no LABELS", result.stderr)

    def test_timeout_zero_is_rejected_because_ctest_treats_it_as_unbounded(self) -> None:
        result = self.check_cmake(GOOD_CMAKE.replace("TIMEOUT 30)\nforeach", "TIMEOUT 0)\nforeach"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("Alpha: TIMEOUT 0 disables", result.stderr)

    def test_timeout_above_ceiling_and_non_numeric_are_rejected(self) -> None:
        over = self.check_cmake(GOOD_CMAKE.replace("TIMEOUT 30)\nforeach", "TIMEOUT 99999)\nforeach"))
        self.assertEqual(over.returncode, 1)
        self.assertIn("exceeds", over.stderr)
        junk = self.check_cmake(GOOD_CMAKE.replace("TIMEOUT 30)\nforeach", "TIMEOUT soon)\nforeach"))
        self.assertEqual(junk.returncode, 1)
        self.assertIn("not a positive integer", junk.stderr)

    def test_commented_out_timeout_does_not_count(self) -> None:
        text = (
            "add_test(NAME Gamma COMMAND gamma)\n"
            "set_tests_properties(Gamma PROPERTIES LABELS \"unit\")\n"
            "# set_tests_properties(Gamma PROPERTIES TIMEOUT 30)\n"
        )
        result = self.check_cmake(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Gamma: no TIMEOUT", result.stderr)

    def test_commented_out_add_test_is_not_counted_and_hash_in_quotes_survives(self) -> None:
        text = (
            "# add_test(NAME Ghost COMMAND ghost)\n"
            "add_test(NAME Delta COMMAND delta \"--tag=#1\")\n"
            "set_tests_properties(Delta PROPERTIES LABELS \"unit\" TIMEOUT 5)\n"
        )
        result = self.check_cmake(text)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (1 test registration(s)", result.stdout)

    def test_timeout_on_a_different_test_does_not_cover_this_one(self) -> None:
        text = (
            "add_test(NAME One COMMAND one)\n"
            "add_test(NAME Two COMMAND two)\n"
            "set_tests_properties(One PROPERTIES LABELS \"unit\" TIMEOUT 5)\n"
        )
        result = self.check_cmake(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Two: no TIMEOUT", result.stderr)
        self.assertNotIn("One:", result.stderr)

    def test_file_that_registers_nothing_fails(self) -> None:
        result = self.check_cmake("enable_testing()\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("registers no tests", result.stderr)

    # -- configured-tree view (ctest --show-only=json-v1) ----------------------

    def test_json_inventory_passes_when_every_test_is_bounded_and_labelled(self) -> None:
        result = self.check_json(ctest_json(ctest_entry("A", TIMEOUT=30.0, LABELS=["unit"])))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_json_inventory_rejects_missing_or_zero_timeout_and_missing_labels(self) -> None:
        result = self.check_json(
            ctest_json(
                ctest_entry("NoTimeout", LABELS=["unit"]),
                ctest_entry("ZeroTimeout", TIMEOUT=0.0, LABELS=["unit"]),
                ctest_entry("NoLabels", TIMEOUT=30.0),
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("NoTimeout: no TIMEOUT", result.stderr)
        self.assertIn("ZeroTimeout: TIMEOUT 0.0", result.stderr)
        self.assertIn("NoLabels: no LABELS", result.stderr)

    def test_json_inventory_with_zero_tests_fails(self) -> None:
        result = self.check_json(ctest_json())
        self.assertEqual(result.returncode, 1)
        self.assertIn("inventory is empty", result.stderr)

    def test_json_that_is_not_ctest_output_fails(self) -> None:
        result = self.check_json(json.dumps({"kind": "somethingElse"}))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing 'tests' list", result.stderr)

    # -- the real tree ----------------------------------------------------------

    def test_repository_tests_cmakelists_satisfies_policy(self) -> None:
        result = self.run_validator("--cmake-lists", str(TESTS_CMAKE))
        self.assertEqual(result.returncode, 0, result.stderr)
        # Guard against a parser regression that "passes" by seeing nothing.
        count = int(result.stdout.split("OK (", 1)[1].split(" ", 1)[0])
        self.assertGreaterEqual(count, 50, result.stdout)

    def test_main_suite_is_selectable_by_documented_unit_and_integration_labels(self) -> None:
        sys.path.insert(0, str(REPO_ROOT / "Tools"))
        try:
            import validate_ctest_policy as policy
        finally:
            sys.path.pop(0)
        _, policies = policy.parse_cmake(TESTS_CMAKE.read_text(encoding="utf-8"))
        labels = ";".join(policies["SparkEngineTests"].labels).split(";")
        self.assertIn("unit", labels)
        self.assertIn("integration", labels)

    def test_policy_validator_is_registered_as_a_bounded_ctest(self) -> None:
        text = TESTS_CMAKE.read_text(encoding="utf-8")
        self.assertTrue("NAME SparkCTestTimeoutPolicy" in text, "policy CTest is not registered")
        self.assertTrue(
            "Tests/Tools/test_validate_ctest_policy.py" in text, "policy CTest does not run this suite"
        )
        _, policies = self._policy_module().parse_cmake(text)
        self.assertTrue(policies["SparkCTestTimeoutPolicy"].timeouts, "policy CTest has no TIMEOUT")

    @staticmethod
    def _policy_module():
        sys.path.insert(0, str(REPO_ROOT / "Tools"))
        try:
            import validate_ctest_policy as policy
        finally:
            sys.path.pop(0)
        return policy


if __name__ == "__main__":
    unittest.main()
