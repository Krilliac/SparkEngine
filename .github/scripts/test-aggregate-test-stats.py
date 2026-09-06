#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("aggregate-test-stats.py")
SPEC = importlib.util.spec_from_file_location("aggregate_test_stats", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class AggregateTestStatsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.metrics = self.root / "repository-metrics.json"
        self.metrics.write_text(
            json.dumps(
                {
                    "total_lines": 100,
                    "file_count": 10,
                    "test_definitions": 7,
                    "test_files": 3,
                    "engine_lines": 20,
                    "editor_lines": 20,
                    "game_lines": 20,
                    "services_lines": 10,
                    "pipeline_lines": 10,
                    "test_lines": 10,
                    "tool_lines": 10,
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_lane(
        self,
        lane: str,
        *,
        tests: int = 7,
        passed: int = 7,
        failures: int = 0,
        errors: int = 0,
        skipped: int = 0,
        flaky: int = 0,
        empty: int = 0,
        directory: str = "results",
    ) -> Path:
        path = self.root / directory / f"test-stats-{lane}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "tests": tests,
                    "passed": passed,
                    "failures": failures,
                    "errors": errors,
                    "skipped": skipped,
                    "flaky": flaky,
                    "empty": empty,
                    "durationSeconds": 1.25,
                }
            ),
            encoding="utf-8",
        )
        return path

    def write_ratchet(self, lanes: dict[str, dict[str, object]]) -> Path:
        path = self.root / "test-count-ratchet.json"
        path.write_text(
            json.dumps({"schemaVersion": 1, "lanes": lanes}), encoding="utf-8"
        )
        return path

    @staticmethod
    def ratchet_entry(
        *,
        minimum_recorded: object = 7,
        minimum_executed: object = 5,
        maximum_skipped: object = 2,
        maximum_flaky: object = 2,
        maximum_empty: object = 2,
    ) -> dict[str, object]:
        return {
            "maximumFlaky": maximum_flaky,
            "maximumEmpty": maximum_empty,
            "minimumRecorded": minimum_recorded,
            "minimumExecuted": minimum_executed,
            "maximumSkipped": maximum_skipped,
        }

    def test_aggregates_repeated_executions_without_calling_them_unique_tests(self) -> None:
        files = [self.write_lane("linux-release"), self.write_lane("windows-release")]
        evidence = MODULE.aggregate(
            files,
            expected_lanes=["windows-release", "linux-release"],
            minimum_tests=5,
            repository_metrics=self.metrics,
            commit="abc123",
        )
        self.assertEqual(evidence["source"]["testDefinitions"], 7)
        self.assertEqual(evidence["testMatrix"]["recordedTestCases"], 14)
        self.assertEqual(evidence["testMatrix"]["executedCaseExecutions"], 14)
        self.assertEqual(evidence["testMatrix"]["laneCount"], 2)
        self.assertIn("test-case executions", MODULE.markdown(evidence))

    def test_distinguishes_recorded_executed_and_skipped_cases(self) -> None:
        files = [self.write_lane("linux-release", tests=7, passed=5, skipped=2)]
        evidence = MODULE.aggregate(
            files,
            expected_lanes=["linux-release"],
            minimum_tests=5,
            repository_metrics=self.metrics,
            commit="abc123",
        )
        matrix = evidence["testMatrix"]
        self.assertEqual(matrix["recordedTestCases"], 7)
        self.assertEqual(matrix["executedCaseExecutions"], 5)
        self.assertEqual(matrix["passedExecutions"], 5)
        self.assertEqual(matrix["skippedExecutions"], 2)
        report = MODULE.markdown(evidence)
        self.assertIn("5 test-case executions", report)
        self.assertIn("7 case records were present, including 2 skipped cases", report)

    def test_rejects_missing_required_lane(self) -> None:
        files = [self.write_lane("linux-release")]
        with self.assertRaisesRegex(ValueError, "missing required.*windows-release"):
            MODULE.aggregate(
                files,
                expected_lanes=["linux-release", "windows-release"],
                minimum_tests=1,
                repository_metrics=self.metrics,
                commit="abc123",
            )

    def test_rejects_duplicate_lane_evidence(self) -> None:
        files = [
            self.write_lane("linux-release", directory="first"),
            self.write_lane("linux-release", directory="second"),
        ]
        with self.assertRaisesRegex(ValueError, "duplicate"):
            MODULE.aggregate(
                files,
                expected_lanes=["linux-release"],
                minimum_tests=1,
                repository_metrics=self.metrics,
                commit="abc123",
            )

    def test_ignores_advisory_lane_evidence(self) -> None:
        files = [
            self.write_lane("linux-release"),
            self.write_lane("advisory", passed=6, failures=1),
        ]
        evidence = MODULE.aggregate(
            files,
            expected_lanes=["linux-release"],
            minimum_tests=1,
            repository_metrics=self.metrics,
            commit="abc123",
        )
        self.assertEqual(evidence["testMatrix"]["laneCount"], 1)
        self.assertNotIn("advisory", evidence["testMatrix"]["lanes"])

    def test_rejects_inconsistent_counts(self) -> None:
        path = self.write_lane("linux-release", tests=7, passed=6)
        with self.assertRaisesRegex(ValueError, "does not equal tests"):
            MODULE.validate_lane(path, 1)

    def test_rejects_failed_test_evidence(self) -> None:
        path = self.write_lane("linux-release", passed=6, failures=1)
        with self.assertRaisesRegex(ValueError, "contains 1 failures"):
            MODULE.validate_lane(path, 1)

    def test_rejects_all_skipped_lane(self) -> None:
        path = self.write_lane("linux-release", tests=7, passed=0, skipped=7)
        with self.assertRaisesRegex(ValueError, "0 executed tests and 7 skipped"):
            MODULE.validate_lane(path, 1)

    def test_rejects_repository_metric_total_mismatch(self) -> None:
        data = json.loads(self.metrics.read_text(encoding="utf-8"))
        data["total_lines"] = 101
        self.metrics.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "category LOC total"):
            MODULE.validate_repository_metrics(self.metrics)

    def test_ratchet_accepts_exact_limits_and_records_truthful_deltas(self) -> None:
        path = self.write_ratchet({"linux-release": self.ratchet_entry()})
        ratchet = MODULE.validate_ratchet(path, ["linux-release"])
        evidence = MODULE.aggregate(
            [self.write_lane("linux-release", tests=7, passed=5, skipped=2)],
            expected_lanes=["linux-release"],
            minimum_tests=5,
            repository_metrics=self.metrics,
            commit="abc123",
            ratchet=ratchet,
        )
        lane = evidence["testMatrix"]["lanes"]["linux-release"]
        self.assertEqual(lane["ratchetDelta"]["recordedAboveMinimum"], 0)
        self.assertEqual(lane["ratchetDelta"]["executedAboveMinimum"], 0)
        self.assertEqual(lane["ratchetDelta"]["skippedBelowMaximum"], 0)
        report = MODULE.markdown(evidence)
        self.assertIn("Recorded floor", report)
        self.assertIn("Skipped cap", report)

    def test_ratchet_rejects_more_waived_flaky_cases_than_the_ceiling(self) -> None:
        path = self.write_ratchet({"linux-release": self.ratchet_entry(maximum_flaky=1)})
        ratchet = MODULE.validate_ratchet(path, ["linux-release"])
        with self.assertRaisesRegex(ValueError, "2 waived flaky cases.*at most 1"):
            MODULE.aggregate(
                [self.write_lane("linux-release", tests=7, passed=5, flaky=2)],
                expected_lanes=["linux-release"],
                minimum_tests=5,
                repository_metrics=self.metrics,
                commit="abc123",
                ratchet=ratchet,
            )

    def test_ratchet_rejects_more_assertionless_cases_than_the_ceiling(self) -> None:
        path = self.write_ratchet({"linux-release": self.ratchet_entry(maximum_empty=1)})
        ratchet = MODULE.validate_ratchet(path, ["linux-release"])
        with self.assertRaisesRegex(ValueError, "3 cases ran zero assertions.*at most 1"):
            MODULE.aggregate(
                [self.write_lane("linux-release", tests=7, passed=7, empty=3)],
                expected_lanes=["linux-release"],
                minimum_tests=5,
                repository_metrics=self.metrics,
                commit="abc123",
                ratchet=ratchet,
            )

    def test_a_flaky_case_is_not_counted_as_passed(self) -> None:
        # tests = passed + failures + errors + skipped + flaky. A report that
        # still folds a waived failure into `passed` no longer balances.
        with self.assertRaisesRegex(ValueError, r"\+ flaky \(8\) does not equal tests \(7\)"):
            MODULE.aggregate(
                [self.write_lane("linux-release", tests=7, passed=7, flaky=1)],
                expected_lanes=["linux-release"],
                minimum_tests=5,
                repository_metrics=self.metrics,
                commit="abc123",
            )

    def test_ratchet_rejects_recorded_regression_above_generic_floor(self) -> None:
        path = self.write_ratchet(
            {"linux-release": self.ratchet_entry(minimum_recorded=7, minimum_executed=5)}
        )
        ratchet = MODULE.validate_ratchet(path, ["linux-release"])
        with self.assertRaisesRegex(ValueError, "recorded 6 cases.*at least 7 recorded"):
            MODULE.aggregate(
                [self.write_lane("linux-release", tests=6, passed=5, skipped=1)],
                expected_lanes=["linux-release"],
                minimum_tests=5,
                repository_metrics=self.metrics,
                commit="abc123",
                ratchet=ratchet,
            )

    def test_ratchet_rejects_executed_regression(self) -> None:
        path = self.write_ratchet(
            {"linux-release": self.ratchet_entry(minimum_recorded=7, minimum_executed=6, maximum_skipped=3)}
        )
        ratchet = MODULE.validate_ratchet(path, ["linux-release"])
        with self.assertRaisesRegex(ValueError, "executed 5 cases.*at least 6 executed"):
            MODULE.aggregate(
                [self.write_lane("linux-release", tests=7, passed=5, skipped=2)],
                expected_lanes=["linux-release"],
                minimum_tests=5,
                repository_metrics=self.metrics,
                commit="abc123",
                ratchet=ratchet,
            )

    def test_ratchet_rejects_skipped_increase_even_when_execution_floor_passes(self) -> None:
        path = self.write_ratchet(
            {"linux-release": self.ratchet_entry(minimum_recorded=8, minimum_executed=5, maximum_skipped=2)}
        )
        ratchet = MODULE.validate_ratchet(path, ["linux-release"])
        with self.assertRaisesRegex(ValueError, "skipped 3 cases.*at most 2 skipped"):
            MODULE.aggregate(
                [self.write_lane("linux-release", tests=8, passed=5, skipped=3)],
                expected_lanes=["linux-release"],
                minimum_tests=5,
                repository_metrics=self.metrics,
                commit="abc123",
                ratchet=ratchet,
            )

    def test_ratchet_allows_growth_and_fewer_skips(self) -> None:
        path = self.write_ratchet({"linux-release": self.ratchet_entry()})
        ratchet = MODULE.validate_ratchet(path, ["linux-release"])
        evidence = MODULE.aggregate(
            [self.write_lane("linux-release", tests=9, passed=8, skipped=1)],
            expected_lanes=["linux-release"],
            minimum_tests=5,
            repository_metrics=self.metrics,
            commit="abc123",
            ratchet=ratchet,
        )
        delta = evidence["testMatrix"]["lanes"]["linux-release"]["ratchetDelta"]
        self.assertEqual(delta["recordedAboveMinimum"], 2)
        self.assertEqual(delta["executedAboveMinimum"], 3)
        self.assertEqual(delta["skippedBelowMaximum"], 1)

    def test_ratchet_rejects_missing_and_unexpected_lanes(self) -> None:
        path = self.write_ratchet(
            {
                "linux-release": self.ratchet_entry(),
                "advisory": self.ratchet_entry(),
            }
        )
        with self.assertRaisesRegex(ValueError, r"missing lane\(s\): windows-release.*unexpected lane\(s\): advisory"):
            MODULE.validate_ratchet(path, ["linux-release", "windows-release"])

    def test_ratchet_rejects_boolean_negative_and_unknown_values(self) -> None:
        invalid_entries = (
            self.ratchet_entry(minimum_recorded=True),
            self.ratchet_entry(maximum_skipped=-1),
            {**self.ratchet_entry(), "extra": 1},
        )
        patterns = ("non-negative integer", "non-negative integer", "unexpected field")
        for entry, pattern in zip(invalid_entries, patterns, strict=True):
            with self.subTest(entry=entry):
                path = self.write_ratchet({"linux-release": entry})
                with self.assertRaisesRegex(ValueError, pattern):
                    MODULE.validate_ratchet(path, ["linux-release"])

    def test_ratchet_rejects_boolean_schema_version(self) -> None:
        path = self.root / "test-count-ratchet.json"
        path.write_text(
            json.dumps(
                {
                    "schemaVersion": True,
                    "lanes": {"linux-release": self.ratchet_entry()},
                }
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "schemaVersion"):
            MODULE.validate_ratchet(path, ["linux-release"])


if __name__ == "__main__":
    unittest.main()
