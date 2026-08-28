#!/usr/bin/env python3
"""Adversarial audit tests for PERF-100 budget governance control plane.

Targets defects found in commit 3cf86ca1: bool-as-int type confusion,
NaN/infinity acceptance, JSON output crash, fail-open top-level keys,
missing-measurement pass-through, SHA case self-approval bypass,
per-metric hardware binding, and gpuVramGb validation gap.
"""

from __future__ import annotations

import copy
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "perf-budget"
sys.path.insert(0, str(TOOL_DIR))

from validate_budget import (
    budget_definition_digest,
    validate_hardware,
    validate_budget,
    validate_baselines,
    validate_result,
    VALID_UNITS,
)
from compare_results import (
    compare as compare_results,
    report_to_dict,
    ComparisonReport,
)


EXPECTED_RESULT_SHA = "c" * 40


def compare(budget_dir: Path, result_data: Any) -> ComparisonReport:
    """Exercise the comparator with an independently supplied workflow SHA."""
    return compare_results(
        budget_dir, result_data, expected_sha=EXPECTED_RESULT_SHA,
    )


# ── Fixtures ──────────────────────────────────────────────────────────

def _make_hardware() -> dict[str, Any]:
    return {
        "schemaVersion": "1.0.0",
        "rows": [
            {
                "id": "test-row-1",
                "certified": True,
                "os": "Windows 11",
                "cpu": "TestCPU",
                "cpuCores": 8,
                "gpu": "TestGPU",
                "gpuVramGb": 8,
                "gpuDriverMinVersion": "500.0",
                "ramGb": 16,
                "certifiedAt": "2026-08-28T00:00:00Z",
                "certifiedBy": "test-reviewer",
                "certifiedCommit": "a" * 40,
                "notes": "test hardware row",
            }
        ],
    }


def _make_budget(hw_id: str = "test-row-1") -> dict[str, Any]:
    return {
        "schemaVersion": "1.0.0",
        "budgetVersion": "v1",
        "metadata": {
            "createdAt": "2026-08-28T00:00:00Z",
            "createdBy": "test",
            "commitSha": None,
            "description": "test budget",
        },
        "metrics": [
            {
                "id": "test.frame_time.p50",
                "category": "frame_time",
                "scene": "test-scene",
                "backend": "d3d11",
                "product": "TestProduct",
                "unit": "ms",
                "direction": "lower_is_better",
                "percentile": "p50",
                "hardwareRowId": hw_id,
                "budget": 16.67,
                "status": "active",
            },
            {
                "id": "test.memory.peak_rss",
                "category": "memory",
                "scene": "test-scene",
                "backend": "d3d11",
                "product": "TestProduct",
                "unit": "megabytes",
                "direction": "lower_is_better",
                "percentile": None,
                "hardwareRowId": hw_id,
                "budget": 512.0,
                "status": "active",
            },
        ],
    }


def _make_baselines(metric_id: str = "test.frame_time.p50",
                    hw_id: str = "test-row-1") -> dict[str, Any]:
    metrics = _make_budget(hw_id)["metrics"]
    metric = next((item for item in metrics if item["id"] == metric_id), metrics[0])
    return {
        "schemaVersion": "1.0.0",
        "baselineVersion": "v1",
        "approvalPolicy": {
            "description": "test policy",
            "requiredFields": [
                "approvedBy", "approvedAt", "approvalCommit",
                "budgetDefinitionDigest",
            ],
            "selfApprovalAllowed": False,
        },
        "baselines": [
            {
                "metricId": metric_id,
                "value": 12.5,
                "unit": "ms",
                "commitSha": "a" * 40,
                "hardwareRowId": hw_id,
                "measuredAt": "2026-08-28T00:00:00Z",
                "sampleCount": 1000,
                "approvedBy": "reviewer-name",
                "approvedAt": "2026-08-28T01:00:00Z",
                "approvalCommit": "b" * 40,
                "budgetDefinitionDigest": budget_definition_digest(metric),
            },
        ],
    }


def _make_result(hw_id: str = "test-row-1") -> dict[str, Any]:
    return {
        "commitSha": EXPECTED_RESULT_SHA,
        "timestamp": "2026-08-28T02:00:00Z",
        "hardwareRowId": hw_id,
        "measurements": [
            {
                "metricId": "test.frame_time.p50",
                "value": 14.0,
                "unit": "ms",
                "sampleCount": 1000,
            },
            {
                "metricId": "test.memory.peak_rss",
                "value": 480.0,
                "unit": "megabytes",
                "sampleCount": 1,
            },
        ],
    }


def _hw_ids(hw: dict[str, Any]) -> frozenset[str]:
    return frozenset(r["id"] for r in hw.get("rows", []))


def _metric_ids(budget: dict[str, Any]) -> frozenset[str]:
    return frozenset(m["id"] for m in budget.get("metrics", []))


def _write_suite(tmpdir: Path, hw: dict, budget: dict) -> Path:
    (tmpdir / "hardware.json").write_text(json.dumps(hw), encoding="utf-8")
    (tmpdir / "budget.json").write_text(json.dumps(budget), encoding="utf-8")
    baselines = _make_baselines()
    entries: list[dict[str, Any]] = []
    hardware_ids = [row["id"] for row in hw["rows"]]
    for metric in budget["metrics"]:
        if metric["status"] != "active":
            continue
        applicable = (
            hardware_ids if metric["hardwareRowId"] is None
            else [metric["hardwareRowId"]]
        )
        for hardware_id in applicable:
            entries.append({
                "metricId": metric["id"],
                "value": metric["budget"],
                "unit": metric["unit"],
                "commitSha": "a" * 40,
                "hardwareRowId": hardware_id,
                "measuredAt": "2026-08-28T00:00:00Z",
                "sampleCount": 1000,
                "approvedBy": "reviewer-name",
                "approvedAt": "2026-08-28T01:00:00Z",
                "approvalCommit": "b" * 40,
                "budgetDefinitionDigest": budget_definition_digest(metric),
            })
    baselines["baselines"] = entries
    (tmpdir / "baselines.json").write_text(json.dumps(baselines), encoding="utf-8")
    return tmpdir


# ── D1: bool-as-int type confusion ──────────────────────────────────

class TestBoolAsIntTypeConfusion(unittest.TestCase):
    """Python bool is a subclass of int — isinstance(True, int) is True.
    Booleans must be explicitly rejected in all numeric fields."""

    def test_cpuCores_true_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["cpuCores"] = True
        errors = validate_hardware(hw)
        self.assertTrue(
            any("cpuCores" in e for e in errors),
            "cpuCores=True must be rejected (bool is not a valid core count)",
        )

    def test_cpuCores_false_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["cpuCores"] = False
        errors = validate_hardware(hw)
        self.assertTrue(
            any("cpuCores" in e for e in errors),
            "cpuCores=False must be rejected",
        )

    def test_ramGb_true_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["ramGb"] = True
        errors = validate_hardware(hw)
        self.assertTrue(
            any("ramGb" in e for e in errors),
            "ramGb=True must be rejected (bool is not a valid RAM quantity)",
        )

    def test_budget_true_rejected(self) -> None:
        b = _make_budget()
        b["metrics"][0]["budget"] = True
        errors = validate_budget(b, frozenset({"test-row-1"}))
        self.assertTrue(
            any("budget" in e for e in errors),
            "budget=True must be rejected (bool is not a valid budget value)",
        )

    def test_budget_false_rejected(self) -> None:
        b = _make_budget()
        b["metrics"][0]["budget"] = False
        errors = validate_budget(b, frozenset({"test-row-1"}))
        self.assertTrue(
            any("budget" in e for e in errors),
            "budget=False must be rejected",
        )

    def test_baseline_value_true_rejected(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["value"] = True
        errors = validate_baselines(
            bl, frozenset({"test.frame_time.p50"}), frozenset({"test-row-1"})
        )
        self.assertTrue(
            any("value" in e for e in errors),
            "baseline value=True must be rejected",
        )

    def test_baseline_sampleCount_true_rejected(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["sampleCount"] = True
        errors = validate_baselines(
            bl, frozenset({"test.frame_time.p50"}), frozenset({"test-row-1"})
        )
        self.assertTrue(
            any("sampleCount" in e for e in errors),
            "baseline sampleCount=True must be rejected",
        )

    def test_result_value_true_rejected(self) -> None:
        r = _make_result()
        r["measurements"][0]["value"] = True
        errors = validate_result(r, frozenset({"test-row-1"}))
        self.assertTrue(
            any("value" in e for e in errors),
            "result value=True must be rejected",
        )

    def test_result_sampleCount_true_rejected(self) -> None:
        r = _make_result()
        r["measurements"][0]["sampleCount"] = True
        errors = validate_result(r, frozenset({"test-row-1"}))
        self.assertTrue(
            any("sampleCount" in e for e in errors),
            "result sampleCount=True must be rejected",
        )

    def test_gpuVramGb_true_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["gpuVramGb"] = True
        errors = validate_hardware(hw)
        self.assertTrue(
            any("gpuVramGb" in e for e in errors),
            "gpuVramGb=True must be rejected",
        )


# ── D2: NaN and infinity in numeric fields ──────────────────────────

class TestNaNInfinityRejection(unittest.TestCase):
    """NaN and infinity must be rejected in all numeric fields."""

    def test_budget_nan_rejected(self) -> None:
        b = _make_budget()
        b["metrics"][0]["budget"] = float("nan")
        errors = validate_budget(b, frozenset({"test-row-1"}))
        self.assertTrue(
            any("budget" in e for e in errors),
            "budget=NaN must be rejected (NaN poisons all comparisons)",
        )

    def test_budget_inf_rejected(self) -> None:
        b = _make_budget()
        b["metrics"][0]["budget"] = float("inf")
        errors = validate_budget(b, frozenset({"test-row-1"}))
        self.assertTrue(
            any("budget" in e for e in errors),
            "budget=inf must be rejected (infinite budget always passes)",
        )

    def test_budget_neg_inf_rejected(self) -> None:
        b = _make_budget()
        b["metrics"][0]["budget"] = float("-inf")
        errors = validate_budget(b, frozenset({"test-row-1"}))
        self.assertTrue(
            any("budget" in e for e in errors),
            "budget=-inf must be rejected",
        )

    def test_result_value_nan_rejected(self) -> None:
        r = _make_result()
        r["measurements"][0]["value"] = float("nan")
        errors = validate_result(r, frozenset({"test-row-1"}))
        self.assertTrue(
            any("value" in e for e in errors),
            "result value=NaN must be rejected",
        )

    def test_result_value_inf_rejected(self) -> None:
        r = _make_result()
        r["measurements"][0]["value"] = float("inf")
        errors = validate_result(r, frozenset({"test-row-1"}))
        self.assertTrue(
            any("value" in e for e in errors),
            "result value=inf must be rejected",
        )

    def test_baseline_value_nan_rejected(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["value"] = float("nan")
        errors = validate_baselines(
            bl, frozenset({"test.frame_time.p50"}), frozenset({"test-row-1"})
        )
        self.assertTrue(
            any("value" in e for e in errors),
            "baseline value=NaN must be rejected",
        )

    def test_ramGb_nan_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["ramGb"] = float("nan")
        errors = validate_hardware(hw)
        self.assertTrue(
            any("ramGb" in e for e in errors),
            "ramGb=NaN must be rejected",
        )

    def test_ramGb_inf_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["ramGb"] = float("inf")
        errors = validate_hardware(hw)
        self.assertTrue(
            any("ramGb" in e for e in errors),
            "ramGb=inf must be rejected",
        )


# ── D3: margin_percent infinity crashes JSON output ─────────────────

class TestMarginInfinityCrash(unittest.TestCase):
    """budget=0 with non-zero measurement produces inf margin,
    which crashes json.dumps in --json mode."""

    def test_zero_budget_json_serializable(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            budget = _make_budget()
            budget["metrics"][0]["budget"] = 0.0
            _write_suite(d, _make_hardware(), budget)
            result = _make_result()
            result["measurements"][0]["value"] = 10.0
            report = compare(d, result)
            report_dict = report_to_dict(report)
            try:
                serialized = json.dumps(report_dict)
            except (ValueError, OverflowError) as e:
                self.fail(
                    f"report_to_dict produces non-JSON-serializable values: {e}"
                )
            self.assertNotIn("Infinity", serialized)
            self.assertNotIn("NaN", serialized)

    def test_zero_budget_zero_measured_no_crash(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            budget = _make_budget()
            budget["metrics"][0]["budget"] = 0.0
            _write_suite(d, _make_hardware(), budget)
            result = _make_result()
            result["measurements"][0]["value"] = 0.0
            report = compare(d, result)
            report_dict = report_to_dict(report)
            serialized = json.dumps(report_dict)
            self.assertNotIn("Infinity", serialized)


# ── D4: Top-level unknown keys silently accepted ────────────────────

class TestTopLevelUnknownKeys(unittest.TestCase):
    """The validator claims fail-closed, but only checks keys at the
    row/metric/entry level, not at the top-level schema."""

    def test_hardware_top_level_extra_key_rejected(self) -> None:
        hw = _make_hardware()
        hw["sneakyTopLevel"] = "should fail"
        errors = validate_hardware(hw)
        self.assertTrue(
            any("sneaky" in e.lower() or "unknown" in e.lower()
                for e in errors),
            "Extra top-level key in hardware.json must be rejected",
        )

    def test_budget_top_level_extra_key_rejected(self) -> None:
        b = _make_budget()
        b["extraTopLevel"] = "should fail"
        errors = validate_budget(b, frozenset({"test-row-1"}))
        self.assertTrue(
            any("extra" in e.lower() or "unknown" in e.lower()
                for e in errors),
            "Extra top-level key in budget.json must be rejected",
        )

    def test_baselines_top_level_extra_key_rejected(self) -> None:
        bl = _make_baselines()
        bl["hiddenField"] = "should fail"
        errors = validate_baselines(
            bl, frozenset({"test.frame_time.p50"}), frozenset({"test-row-1"})
        )
        self.assertTrue(
            any("hidden" in e.lower() or "unknown" in e.lower()
                for e in errors),
            "Extra top-level key in baselines.json must be rejected",
        )

    def test_result_top_level_extra_key_rejected(self) -> None:
        r = _make_result()
        r["injectedField"] = "should fail"
        errors = validate_result(r, frozenset({"test-row-1"}))
        self.assertTrue(
            any("injected" in e.lower() or "unknown" in e.lower()
                for e in errors),
            "Extra top-level key in result must be rejected",
        )


# ── D5: Missing measurements for active budgets silently pass ───────

class TestMissingMeasurementCompleteness(unittest.TestCase):
    """A result set missing measurements for active budgets must fail."""

    def test_empty_measurements_with_active_budgets_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            _write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            result["measurements"] = []
            report = compare(d, result)
            self.assertFalse(
                report.passed,
                "Empty measurements against active budgets must fail — "
                "0 measurements is not 0 regressions, it's 0 coverage",
            )

    def test_partial_measurements_with_active_budgets_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            _write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            result["measurements"] = [result["measurements"][0]]
            report = compare(d, result)
            self.assertFalse(
                report.passed,
                "Partial measurements (1 of 2 active metrics) must fail",
            )
            self.assertTrue(
                any("missing" in e.lower() or "unmeasured" in e.lower()
                    or "coverage" in e.lower()
                    for e in report.errors),
                "Error message must flag unmeasured active metrics",
            )


# ── D6: SHA case-sensitivity self-approval bypass ───────────────────

class TestSHACaseSelfApproval(unittest.TestCase):
    """Self-approval check must be case-insensitive for hex SHAs."""

    def test_mixed_case_self_approval_rejected(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["commitSha"] = "abcdef12" * 5
        bl["baselines"][0]["approvalCommit"] = "ABCDEF12" * 5
        bl["approvalPolicy"]["selfApprovalAllowed"] = False
        errors = validate_baselines(
            bl, frozenset({"test.frame_time.p50"}), frozenset({"test-row-1"})
        )
        self.assertTrue(
            any("self-approval" in e for e in errors),
            "Case-different SHAs 'abcdef1' vs 'ABCDEF1' must still trigger "
            "self-approval rejection (hex SHAs are case-insensitive)",
        )


# ── D7: Per-metric hardware binding not enforced ────────────────────

class TestPerMetricHardwareBinding(unittest.TestCase):
    """Comparator must check that result hardware matches the metric's
    hardwareRowId when the metric specifies one."""

    def test_wrong_hardware_for_metric_flagged(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            hw = _make_hardware()
            hw["rows"].append({
                "id": "other-row",
                "certified": True,
                "os": "Linux",
                "cpu": "TestCPU2",
                "cpuCores": 4,
                "gpu": "none",
                "gpuVramGb": 0,
                "gpuDriverMinVersion": None,
                "ramGb": 8,
                "certifiedAt": "2026-08-28T00:00:00Z",
                "certifiedBy": "test-reviewer",
                "certifiedCommit": "a" * 40,
                "notes": "other row",
            })
            budget = _make_budget(hw_id="test-row-1")
            _write_suite(d, hw, budget)
            result = _make_result(hw_id="other-row")
            report = compare(d, result)
            self.assertTrue(
                any("hardware" in e.lower() and "mismatch" in e.lower()
                    for e in report.errors)
                or any("hardware" in e.lower() and "match" in e.lower()
                       for e in report.errors),
                "Measuring on 'other-row' when metric requires 'test-row-1' "
                "must produce a hardware mismatch warning or error",
            )


# ── D8: gpuVramGb not validated ─────────────────────────────────────

class TestGpuVramGbValidation(unittest.TestCase):
    """gpuVramGb should be validated like cpuCores and ramGb."""

    def test_negative_gpuVramGb_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["gpuVramGb"] = -1
        errors = validate_hardware(hw)
        self.assertTrue(
            any("gpuVramGb" in e for e in errors),
            "gpuVramGb=-1 must be rejected",
        )

    def test_string_gpuVramGb_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["gpuVramGb"] = "sixteen"
        errors = validate_hardware(hw)
        self.assertTrue(
            any("gpuVramGb" in e for e in errors),
            "gpuVramGb='sixteen' must be rejected",
        )


# ── Bonus: certified field type confusion ───────────────────────────

class TestCertifiedFieldTypeConfusion(unittest.TestCase):
    """The `certified` field check uses `is True` / `is False`, which
    correctly rejects non-boolean values. Verify truthy non-booleans."""

    def test_certified_int_one_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["certified"] = 1
        errors = validate_hardware(hw)
        self.assertTrue(
            any("certified" in e for e in errors),
            "certified=1 (int) must be rejected — only true/false accepted",
        )

    def test_certified_string_true_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["certified"] = "true"
        errors = validate_hardware(hw)
        self.assertTrue(
            any("certified" in e for e in errors),
            "certified='true' (string) must be rejected",
        )


if __name__ == "__main__":
    unittest.main()
