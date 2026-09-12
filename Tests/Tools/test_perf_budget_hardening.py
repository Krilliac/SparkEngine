#!/usr/bin/env python3
"""Regression cases from the second adversarial PERF-100 audit."""

from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "perf-budget"
sys.path.insert(0, str(TOOL_DIR))

from compare_results import compare, report_to_dict  # noqa: E402
from validate_budget import (  # noqa: E402
    MAX_JSON_BYTES,
    budget_definition_digest,
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
              hardware_id: str = "test-row-1",
              metric_definition: dict[str, Any] | None = None) -> dict[str, Any]:
    metric = metric_definition or _metric(metric_id, hardware_id=hardware_id)
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
        "budgetDefinitionDigest": budget_definition_digest(metric),
    }


def _baselines(entries: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    return {
        "schemaVersion": "1.0.0",
        "baselineVersion": "v1",
        "approvalPolicy": {
            "description": "independent review is required",
            "requiredFields": [
                "approvedBy", "approvedAt", "approvalCommit",
                "budgetDefinitionDigest",
            ],
            "selfApprovalAllowed": False,
        },
        "baselines": entries if entries is not None else [],
    }


def _baselines_for_budget(
        budget: dict[str, Any], hardware: dict[str, Any]) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    hardware_ids = [row["id"] for row in hardware["rows"]]
    for metric in budget["metrics"]:
        if metric["status"] != "active":
            continue
        applicable = (
            hardware_ids if metric["hardwareRowId"] is None
            else [metric["hardwareRowId"]]
        )
        for hardware_id in applicable:
            entry = _baseline(
                metric["id"],
                hardware_id=hardware_id,
                metric_definition=metric,
            )
            entry["unit"] = metric["unit"]
            entry["value"] = metric["budget"]
            entries.append(entry)
    return _baselines(entries)


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
    hardware_data = hardware if hardware is not None else _hardware()
    budget_data = budget if budget is not None else _budget()
    (root / "hardware.json").write_text(
        json.dumps(hardware_data),
        encoding="utf-8",
    )
    (root / "budget.json").write_text(
        json.dumps(budget_data),
        encoding="utf-8",
    )
    (root / "baselines.json").write_text(
        json.dumps(
            baselines if baselines is not None
            else _baselines_for_budget(budget_data, hardware_data)
        ),
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
        result["timestamp"] = "2026-08-28T02:00:00-00:00"
        self.assertTrue(any("unknown local offset" in error
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

    def test_13b_unhashable_metric_hardware_id_returns_errors(self) -> None:
        metric = _metric(hardware_id=[])
        errors = validate_baselines(
            _baselines(), {metric["id"]: metric}, HARDWARE_IDS,
        )
        self.assertTrue(any("hardwareRowId" in error for error in errors))

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


class TestFinalAuditClosure(unittest.TestCase):
    """Hostile cases from the final independent PERF-100 audit."""

    def test_active_metric_requires_reviewed_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, baselines=_baselines([]))
            report = compare(root, _result(), expected_sha=RESULT_SHA)
        self.assertFalse(report.passed)
        self.assertTrue(any("requires a reviewed baseline" in error
                            for error in report.errors))

    def test_threshold_weakening_invalidates_approval_digest(self) -> None:
        original = _budget([_metric(budget=10.0)])
        baselines = _baselines_for_budget(original, _hardware())
        weakened = copy.deepcopy(original)
        weakened["metrics"][0]["budget"] = 1000.0
        result = _result()
        result["measurements"][0]["value"] = 999.0
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, budget=weakened, baselines=baselines)
            report = compare(root, result, expected_sha=RESULT_SHA)
        self.assertFalse(report.passed)
        self.assertTrue(any("budgetDefinitionDigest" in error
                            for error in report.errors))

    def test_in_band_self_approval_is_rejected_by_blocking_path(self) -> None:
        budget = _budget()
        baselines = _baselines_for_budget(budget, _hardware())
        baselines["approvalPolicy"]["selfApprovalAllowed"] = True
        baselines["baselines"][0]["approvalCommit"] = \
            baselines["baselines"][0]["commitSha"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, budget=budget, baselines=baselines)
            report = compare(root, _result(), expected_sha=RESULT_SHA)
        self.assertFalse(report.passed)
        self.assertTrue(any("selfApprovalAllowed=true is forbidden" in error
                            for error in report.errors))

    def test_seven_and_eight_character_prefix_aliases_are_rejected(self) -> None:
        metric = _metric()
        baseline = _baseline(metric_definition=metric)
        baseline["commitSha"] = "abcdef1"
        baseline["approvalCommit"] = "abcdef12"
        errors = validate_baselines(
            _baselines([baseline]), {metric["id"]: metric}, HARDWARE_IDS,
        )
        self.assertGreaterEqual(
            sum("full 40- or 64-character" in error for error in errors), 2,
        )
        result = _result()
        result["commitSha"] = "abcdef1"
        result_errors = validate_result(
            result, HARDWARE_IDS, expected_sha="abcdef12",
        )
        self.assertGreaterEqual(
            sum("full 40- or 64-character" in error for error in result_errors), 2,
        )

    def test_full_64_character_provenance_is_supported(self) -> None:
        metric = _metric()
        baseline = _baseline(metric_definition=metric)
        baseline["commitSha"] = "a" * 64
        baseline["approvalCommit"] = "b" * 64
        errors = validate_baselines(
            _baselines([baseline]), {metric["id"]: metric}, HARDWARE_IDS,
        )
        self.assertEqual(errors, [])

        hardware = _hardware()
        hardware["rows"][0]["certifiedCommit"] = "a" * 64
        self.assertEqual(validate_hardware(hardware), [])

        budget = _budget()
        budget["metadata"]["commitSha"] = "b" * 64
        self.assertEqual(validate_budget(budget, HARDWARE_IDS), [])

        result = _result()
        result["commitSha"] = "c" * 64
        self.assertEqual(validate_result(
            result, HARDWARE_IDS, expected_sha="c" * 64,
        ), [])

    def test_huge_json_integers_fail_closed_without_exception(self) -> None:
        huge = 10 ** 400
        hardware = _hardware()
        hardware["rows"][0]["ramGb"] = huge
        self.assertTrue(validate_hardware(hardware))

        budget = _budget()
        budget["metrics"][0]["budget"] = huge
        self.assertTrue(validate_budget(budget, HARDWARE_IDS))

        metric = _metric()
        baseline = _baseline(metric_definition=metric)
        baseline["value"] = huge
        self.assertTrue(validate_baselines(
            _baselines([baseline]), {metric["id"]: metric}, HARDWARE_IDS,
        ))

        result = _result()
        result["measurements"][0]["value"] = huge
        self.assertTrue(validate_result(result, HARDWARE_IDS))

    def test_subnormal_denominator_emits_json_safe_null_margin(self) -> None:
        budget = _budget([_metric(budget=5e-324)])
        result = _result()
        result["measurements"][0]["value"] = 1.0
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, budget=budget)
            report = compare(root, result, expected_sha=RESULT_SHA)
        self.assertEqual(report.verdicts[0].margin_percent, None)
        self.assertIn("subnormal", report.verdicts[0].margin_reason)
        json.dumps(report_to_dict(report), allow_nan=False)

    def test_zero_derived_ratio_emits_json_safe_null_margin(self) -> None:
        budget = _budget([_metric(budget=16.0)])
        result = _result()
        result["measurements"][0]["value"] = 16.0
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, budget=budget)
            report = compare(root, result, expected_sha=RESULT_SHA)
        self.assertTrue(report.passed)
        self.assertEqual(report.verdicts[0].margin_percent, None)
        self.assertIn("ratio is zero", report.verdicts[0].margin_reason)
        json.dumps(report_to_dict(report), allow_nan=False)

        rounded_budget = _budget([_metric(budget=1.0e18)])
        rounded_result = _result()
        rounded_result["measurements"][0]["value"] = 1.0e18 - 128.0
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root, budget=rounded_budget)
            rounded_report = compare(
                root, rounded_result, expected_sha=RESULT_SHA,
            )
        self.assertTrue(rounded_report.passed)
        self.assertEqual(rounded_report.verdicts[0].margin_percent, None)
        self.assertIn("rounds to zero",
                      rounded_report.verdicts[0].margin_reason)
        json.dumps(report_to_dict(rounded_report), allow_nan=False)

    def test_wrong_case_required_filename_is_rejected(self) -> None:
        hardware = _hardware()
        budget = _budget()
        baselines = _baselines_for_budget(budget, hardware)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "hardware.json").write_text(json.dumps(hardware), encoding="utf-8")
            (root / "Budget.JSON").write_text(json.dumps(budget), encoding="utf-8")
            (root / "baselines.json").write_text(json.dumps(baselines), encoding="utf-8")
            errors = validate_suite(root)
        self.assertTrue(any("filename case" in error for error in errors))

        if os.path.normcase("Budget.JSON") != os.path.normcase("budget.json"):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                _write_suite(root)
                (root / "Budget.JSON").write_text(
                    json.dumps(budget), encoding="utf-8",
                )
                errors = validate_suite(root)
            self.assertTrue(any("ambiguous case alias" in error
                                for error in errors))

    def test_hard_linked_governance_file_is_rejected(self) -> None:
        hardware = _hardware()
        budget = _budget()
        baselines = _baselines_for_budget(budget, hardware)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            suite = root / "suite"
            suite.mkdir()
            external = root / "external-budget.json"
            external.write_text(json.dumps(budget), encoding="utf-8")
            (suite / "hardware.json").write_text(json.dumps(hardware), encoding="utf-8")
            (suite / "baselines.json").write_text(json.dumps(baselines), encoding="utf-8")
            os.link(external, suite / "budget.json")
            errors = validate_suite(suite)
        self.assertTrue(any("hard-linked" in error for error in errors))

    def test_directory_junction_or_symlink_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "external-suite"
            target.mkdir()
            _write_suite(target)
            alias = root / "governance-suite"
            if os.name == "nt":
                created = subprocess.run(
                    ["cmd", "/c", "mklink", "/J", str(alias), str(target)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(created.returncode, 0, created.stderr)
            else:
                os.symlink(target, alias, target_is_directory=True)
            try:
                errors = validate_suite(alias)
            finally:
                if os.name == "nt":
                    os.rmdir(alias)
                else:
                    alias.unlink()
        self.assertTrue(any("junction" in error or "reparse" in error
                            for error in errors))

    def test_file_swap_between_inspection_and_open_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _write_suite(root)
            target = root / "budget.json"
            replacement = root / "replacement.json"
            replacement.write_text(target.read_text(encoding="utf-8"),
                                   encoding="utf-8")
            real_open = os.open
            swapped = False

            def swapping_open(path: Any, flags: int, *args: Any,
                              **kwargs: Any) -> int:
                nonlocal swapped
                if not swapped and Path(path) == target:
                    swapped = True
                    os.replace(replacement, target)
                return real_open(path, flags, *args, **kwargs)

            with mock.patch.object(os, "open", side_effect=swapping_open):
                _, errors = load_bounded_json(
                    target, "budget.json", trusted_root=root,
                )
        self.assertTrue(swapped)
        self.assertTrue(any("identity changed" in error for error in errors))

    def test_deep_nesting_and_wide_key_diagnostics_are_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            nested = root / "nested.json"
            nested.write_text("[" * 5000 + "0" + "]" * 5000,
                              encoding="utf-8")
            _, nesting_errors = load_bounded_json(
                nested, "nested.json", trusted_root=root,
            )
        self.assertTrue(any("nesting/value is invalid" in error
                            for error in nesting_errors))

        wide = {
            "schemaVersion": "1.0.0",
            "rows": [],
            **{f"unknown{i:05d}": True for i in range(5000)},
        }
        wide_errors = validate_hardware(wide)
        self.assertTrue(any("showing 16 of 5000" in error
                            for error in wide_errors))
        self.assertLess(sum(len(error) for error in wide_errors), 2000)

    def test_case_aliased_identifiers_are_rejected(self) -> None:
        hardware = _hardware(two_rows=True)
        hardware["rows"][1]["id"] = "TEST-ROW-1"
        hardware_errors = validate_hardware(hardware)
        self.assertTrue(any("lowercase identifier" in error
                            for error in hardware_errors))
        self.assertTrue(any("duplicate hardware row" in error
                            for error in hardware_errors))

        metrics = [_metric(), _metric("TEST.FRAME_TIME.P50")]
        budget_errors = validate_budget(_budget(metrics), HARDWARE_IDS)
        self.assertTrue(any("lowercase identifier" in error
                            for error in budget_errors))
        self.assertTrue(any("duplicate metric" in error
                            for error in budget_errors))

    def test_hardware_independent_active_metric_requires_each_row(self) -> None:
        hardware = _hardware(two_rows=True)
        metric = _metric(hardware_id=None)
        budget = _budget([metric])
        baselines = _baselines([
            _baseline(
                metric["id"], hardware_id="test-row-1",
                metric_definition=metric,
            ),
        ])
        errors = validate_baselines(
            baselines, {metric["id"]: metric},
            {row["id"] for row in hardware["rows"]},
        )
        self.assertTrue(any("test-row-2" in error and "requires" in error
                            for error in errors))


if __name__ == "__main__":
    unittest.main()
