#!/usr/bin/env python3
"""Regression cases from the second adversarial PERF-100 audit."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "perf-budget"
sys.path.insert(0, str(TOOL_DIR))

from compare_results import compare  # noqa: E402
from validate_budget import (  # noqa: E402
    MAX_JSON_BYTES,
    load_bounded_json,
    validate_baselines,
    validate_budget,
    validate_hardware,
    validate_result,
    validate_suite,
)

RESULT_SHA = "c" * 40
BASELINE_SHA = "a" * 40
APPROVAL_SHA = "b" * 40
HARDWARE_IDS = frozenset({"test-row-1", "test-row-2"})


def _hardware(*, two_rows: bool = False, certified: bool = True) -> dict[str, Any]:
    def row(row_id: str) -> dict[str, Any]:
        return {
            "id": row_id,
            "certified": certified,
            "os": "Windows 11",
            "cpu": "TestCPU",
            "cpuCores": 8,
            "gpu": "TestGPU",
            "gpuVramGb": 8,
            "gpuDriverMinVersion": "500.0",
            "ramGb": 16,
            "certifiedAt": "2026-08-28T00:00:00Z" if certified else None,
            "certifiedBy": "reviewer" if certified else None,
            "certifiedCommit": BASELINE_SHA if certified else None,
            "notes": "test hardware",
        }

    rows = [row("test-row-1")]
    if two_rows:
        rows.append(row("test-row-2"))
    return {"schemaVersion": "1.0.0", "rows": rows}


def _metric(metric_id: str = "test.frame_time.p50", *,
            hardware_id: str = "test-row-1",
            status: str = "active", budget: float | None = 16.0) -> dict[str, Any]:
    return {
        "id": metric_id,
        "category": "frame_time",
        "scene": "test-scene",
        "backend": "d3d11",
        "product": "TestProduct",
        "unit": "ms",
        "direction": "lower_is_better",
        "percentile": "p50",
        "hardwareRowId": hardware_id,
        "budget": budget,
        "status": status,
    }


def _budget(metrics: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    return {
        "schemaVersion": "1.0.0",
        "budgetVersion": "v1",
        "metadata": {
            "createdAt": "2026-08-28T00:00:00Z",
            "createdBy": "test",
            "commitSha": None,
            "description": "test budget",
        },
        "metrics": metrics if metrics is not None else [_metric()],
    }


def _baseline(metric_id: str = "test.frame_time.p50", *,
              hardware_id: str = "test-row-1") -> dict[str, Any]:
    return {
        "metricId": metric_id,
        "value": 12.0,
        "unit": "ms",
        "commitSha": BASELINE_SHA,
        "hardwareRowId": hardware_id,
        "measuredAt": "2026-08-28T00:30:00Z",
        "sampleCount": 100,
        "approvedBy": "reviewer",
        "approvedAt": "2026-08-28T01:00:00Z",
        "approvalCommit": APPROVAL_SHA,
    }


def _baselines(entries: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    return {
        "schemaVersion": "1.0.0",
        "baselineVersion": "v1",
        "approvalPolicy": {
            "description": "independent review is required",
            "requiredFields": ["approvedBy", "approvedAt", "approvalCommit"],
            "selfApprovalAllowed": False,
        },
        "baselines": entries if entries is not None else [],
    }


def _result(measurements: list[dict[str, Any]] | None = None, *,
            hardware_id: str = "test-row-1") -> dict[str, Any]:
    return {
        "commitSha": RESULT_SHA,
        "timestamp": "2026-08-28T02:00:00Z",
        "hardwareRowId": hardware_id,
        "measurements": measurements if measurements is not None else [
            {
                "metricId": "test.frame_time.p50",
                "value": 14.0,
                "unit": "ms",
                "sampleCount": 100,
            }
        ],
    }


def _write_suite(root: Path, *, hardware: dict[str, Any] | None = None,
                 budget: dict[str, Any] | None = None,
                 baselines: dict[str, Any] | None = None) -> None:
    (root / "hardware.json").write_text(
        json.dumps(hardware if hardware is not None else _hardware()),
        encoding="utf-8",
    )
    (root / "budget.json").write_text(
        json.dumps(budget if budget is not None else _budget()),
        encoding="utf-8",
    )
    (root / "baselines.json").write_text(
        json.dumps(baselines if baselines is not None else _baselines()),
        encoding="utf-8",
    )


class TestSecondAuditReproductions(unittest.TestCase):
    def test_01_other_hardware_active_metric_is_not_required(self) -> None:
        metrics = [
            _metric(),
            _metric("test.other.frame", hardware_id="test-row-2"),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(
                root,
                hardware=_hardware(two_rows=True),
                budget=_budget(metrics),
            )
            report = compare(root, _result(), expected_sha=RESULT_SHA)
        self.assertTrue(report.passed, report.errors)
        self.assertFalse(any("test.other.frame" in error for error in report.errors))

    def test_02_budget_metadata_unknown_key_is_rejected(self) -> None:
        budget = _budget()
        budget["metadata"]["surprise"] = True
        errors = validate_budget(budget, HARDWARE_IDS)
        self.assertTrue(any("budget.metadata" in error and "unknown" in error
                            for error in errors))

    def test_03_budget_metadata_wrong_types_are_rejected(self) -> None:
        mutations = {
            "createdAt": 123,
            "createdBy": [],
            "commitSha": {},
            "description": None,
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                budget = _budget()
                budget["metadata"][field] = value
                self.assertTrue(validate_budget(budget, HARDWARE_IDS))

    def test_04_approval_policy_unknown_key_is_rejected(self) -> None:
        baselines = _baselines()
        baselines["approvalPolicy"]["escapeHatch"] = True
        errors = validate_baselines(baselines, {}, HARDWARE_IDS)
        self.assertTrue(any("approvalPolicy" in error and "unknown" in error
                            for error in errors))

    def test_05_approval_policy_wrong_types_are_rejected(self) -> None:
        mutations = {
            "description": None,
            "requiredFields": "approvedBy",
            "selfApprovalAllowed": 0,
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                baselines = _baselines()
                baselines["approvalPolicy"][field] = value
                self.assertTrue(validate_baselines(
                    baselines, {}, HARDWARE_IDS,
                ))

    def test_06_string_false_cannot_bypass_self_approval(self) -> None:
        entry = _baseline()
        entry["approvalCommit"] = entry["commitSha"]
        baselines = _baselines([entry])
        baselines["approvalPolicy"]["selfApprovalAllowed"] = "false"
        errors = validate_baselines(
            baselines, {"test.frame_time.p50": _metric()}, HARDWARE_IDS,
        )
        self.assertTrue(any("must be a boolean" in error for error in errors))
        self.assertTrue(any("self-approval" in error for error in errors))

    def test_07_impossible_or_naive_timestamps_are_rejected(self) -> None:
        hardware = _hardware()
        hardware["rows"][0]["certifiedAt"] = "2026-02-30T00:00:00Z"
        self.assertTrue(any("real RFC3339" in error
                            for error in validate_hardware(hardware)))
        result = _result()
        result["timestamp"] = "2026-08-28T02:00:00"
        self.assertTrue(any("RFC3339" in error
                            for error in validate_result(result, HARDWARE_IDS)))

    def test_08_extremely_long_sha_is_rejected_with_bounded_error(self) -> None:
        result = _result()
        result["commitSha"] = "a" * 100_000
        errors = validate_result(result, HARDWARE_IDS)
        self.assertTrue(errors)
        self.assertLess(sum(len(error) for error in errors), 1000)

    def test_09_null_result_returns_errors_instead_of_throwing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root)
            report = compare(root, None, expected_sha=RESULT_SHA)
        self.assertFalse(report.passed)
        self.assertTrue(any("must be an object" in error for error in report.errors))

    def test_10_unhashable_enum_value_returns_error(self) -> None:
        budget = _budget()
        budget["metrics"][0]["category"] = {"frame_time": True}
        errors = validate_budget(budget, HARDWARE_IDS)
        self.assertTrue(any("category must be a string" in error for error in errors))

    def test_11_top_level_arrays_return_errors(self) -> None:
        self.assertTrue(validate_hardware([]))
        self.assertTrue(validate_budget([], HARDWARE_IDS))
        self.assertTrue(validate_baselines([], {}, HARDWARE_IDS))
        self.assertTrue(validate_result([], HARDWARE_IDS))

    def test_12_non_object_approval_policy_does_not_crash(self) -> None:
        baselines = _baselines([_baseline()])
        baselines["approvalPolicy"] = []
        errors = validate_baselines(
            baselines, {"test.frame_time.p50": _metric()}, HARDWARE_IDS,
        )
        self.assertTrue(any("approvalPolicy" in error for error in errors))

    def test_13_list_hardware_id_does_not_crash_suite(self) -> None:
        hardware = _hardware()
        hardware["rows"][0]["id"] = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, hardware=hardware)
            errors = validate_suite(root)
        self.assertTrue(any("id must be a string" in error for error in errors))

    def test_14_zero_budget_margin_is_null_with_reason(self) -> None:
        budget = _budget([_metric(budget=0.0)])
        result = _result()
        result["measurements"][0]["value"] = 10.0
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, budget=budget)
            report = compare(root, result, expected_sha=RESULT_SHA)
        self.assertEqual(report.verdicts[0].margin_percent, None)
        self.assertIn("denominator is zero", report.verdicts[0].margin_reason)

    def test_15_missing_baselines_file_fails_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "hardware.json").write_text(
                json.dumps(_hardware()), encoding="utf-8",
            )
            (root / "budget.json").write_text(
                json.dumps(_budget()), encoding="utf-8",
            )
            report = compare(root, _result(), expected_sha=RESULT_SHA)
        self.assertFalse(report.passed)
        self.assertTrue(any("baselines.json" in error and "missing" in error
                            for error in report.errors))

    def test_16_short_self_asserted_sha_is_not_exact_evidence(self) -> None:
        result = _result()
        result["commitSha"] = "ccccccc"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root)
            report = compare(root, result, expected_sha=RESULT_SHA)
            missing_external = compare(root, _result())
        self.assertFalse(report.passed)
        self.assertTrue(any("full 40- or 64-character" in error
                            or "exactly match" in error
                            for error in report.errors))
        self.assertFalse(missing_external.passed)
        self.assertTrue(any("externally supplied" in error
                            for error in missing_external.errors))

    def test_17_five_thousand_measurements_are_rejected(self) -> None:
        measurement = _result()["measurements"][0]
        result = _result([copy.deepcopy(measurement) for _ in range(5000)])
        errors = validate_result(result, HARDWARE_IDS, expected_sha=RESULT_SHA)
        self.assertTrue(any("maximum count" in error for error in errors))

    def test_18_duplicate_json_object_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root)
            budget_text = (root / "budget.json").read_text(encoding="utf-8")
            budget_text = budget_text.replace(
                '{"schemaVersion": "1.0.0",',
                '{"schemaVersion": "1.0.0", "schemaVersion": "1.0.0",',
                1,
            )
            (root / "budget.json").write_text(budget_text, encoding="utf-8")
            errors = validate_suite(root)
        self.assertTrue(any("duplicate JSON object key" in error for error in errors))

    def test_19_suspended_metric_is_explicitly_skipped(self) -> None:
        budget = _budget([_metric(status="suspended", budget=16.0)])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, budget=budget)
            report = compare(root, _result(), expected_sha=RESULT_SHA)
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(report.skipped_by_status, {"suspended": 1})
        self.assertEqual(report.verdicts, [])

    def test_20_uncertified_hardware_is_advisory_not_failed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, hardware=_hardware(certified=False))
            report = compare(root, _result(), expected_sha=RESULT_SHA)
        self.assertTrue(report.passed, report.errors)
        self.assertFalse(report.authoritative)
        self.assertTrue(any("not certified" in advisory
                            for advisory in report.advisories))

    def test_21_blocking_ci_job_directly_covers_perf_paths(self) -> None:
        workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(
            encoding="utf-8",
        )
        triggers, jobs = workflow.split("jobs:", 1)
        self.assertNotIn("paths:", triggers)
        self.assertNotIn("paths-ignore:", triggers)
        self.assertIn("performance-budget-governance:", jobs)
        self.assertIn("Tests.Tools.test_perf_budget_hardening", jobs)
        self.assertIn("tools/perf-budget/validate_budget.py perf-budgets/v1", jobs)
        gate = jobs.split("required-ci-gate:", 1)[1]
        self.assertIn("- performance-budget-governance", gate)


class TestAdditionalGovernanceClosure(unittest.TestCase):
    def test_baseline_null_value_is_rejected(self) -> None:
        entry = _baseline()
        entry["value"] = None
        errors = validate_baselines(
            _baselines([entry]),
            {"test.frame_time.p50": _metric()},
            HARDWARE_IDS,
        )
        self.assertTrue(any("value must be a finite number" in error
                            for error in errors))

    def test_baseline_unit_and_hardware_must_match_metric(self) -> None:
        entry = _baseline(hardware_id="test-row-2")
        entry["unit"] = "us"
        errors = validate_baselines(
            _baselines([entry]),
            {"test.frame_time.p50": _metric()},
            HARDWARE_IDS,
        )
        self.assertTrue(any("does not match budget unit" in error for error in errors))
        self.assertTrue(any("does not match budget hardwareRowId" in error
                            for error in errors))

    def test_baseline_approval_cannot_precede_measurement(self) -> None:
        entry = _baseline()
        entry["approvedAt"] = "2026-08-27T23:00:00-01:00"
        errors = validate_baselines(
            _baselines([entry]),
            {"test.frame_time.p50": _metric()},
            HARDWARE_IDS,
        )
        self.assertTrue(any("must not precede" in error for error in errors))

    def test_comparator_rejects_invalid_baseline_governance(self) -> None:
        baselines = _baselines()
        baselines["approvalPolicy"]["selfApprovalAllowed"] = "false"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, baselines=baselines)
            report = compare(root, _result(), expected_sha=RESULT_SHA)
        self.assertFalse(report.passed)
        self.assertTrue(any("selfApprovalAllowed" in error for error in report.errors))

    def test_bounded_loader_rejects_oversized_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "large.json"
            path.write_bytes(b" " * (MAX_JSON_BYTES + 1))
            data, errors = load_bounded_json(path, "large.json")
        self.assertIsNone(data)
        self.assertTrue(any("maximum size" in error for error in errors))

    def test_schema_versions_are_exact_strings(self) -> None:
        hardware = _hardware()
        hardware["schemaVersion"] = 1.0
        self.assertTrue(any("schemaVersion" in error
                            for error in validate_hardware(hardware)))
        budget = _budget()
        budget["budgetVersion"] = "v2"
        self.assertTrue(any("budgetVersion" in error
                            for error in validate_budget(budget, HARDWARE_IDS)))


if __name__ == "__main__":
    unittest.main()
