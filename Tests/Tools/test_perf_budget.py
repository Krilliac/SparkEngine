#!/usr/bin/env python3
"""Adversarial fail-closed tests for PERF-100 performance-budget governance.

Every test operates on deep-copied or synthetic data — this suite never
writes to tracked repository files.  Mutation tests prove that the
validator rejects every structural violation, governance gap, and
ambiguous input.
"""

from __future__ import annotations

import ast
import copy
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "perf-budget"
sys.path.insert(0, str(TOOL_DIR))

from validate_budget import (  # noqa: E402
    budget_definition_digest,
    validate_hardware,
    validate_budget,
    validate_baselines,
    validate_result,
    validate_suite,
    BudgetValidationError,
    VALID_UNITS,
    VALID_DIRECTIONS,
    VALID_CATEGORIES,
    VALID_METRIC_STATUSES,
    VALID_BACKENDS,
    VALID_PERCENTILES,
    HARDWARE_ROW_REQUIRED,
    METRIC_REQUIRED,
    BASELINE_ENTRY_REQUIRED,
    RESULT_MEASUREMENT_REQUIRED,
)
from compare_results import compare as compare_results, ComparisonReport  # noqa: E402


BUDGET_DIR = REPO_ROOT / "perf-budgets" / "v1"
EXPECTED_RESULT_SHA = "c" * 40


def compare(budget_dir: Path, result_data: Any) -> ComparisonReport:
    """Exercise the comparator with an independently supplied workflow SHA."""
    return compare_results(
        budget_dir, result_data, expected_sha=EXPECTED_RESULT_SHA,
    )


# ── Fixtures ──────────────────────────────────────────────────────────

def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


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


def _make_baselines_for_budget(
        budget: dict[str, Any], hardware_ids: list[str]) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
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
    baselines = _make_baselines()
    baselines["baselines"] = entries
    return baselines


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


# ── Self-integrity ────────────────────────────────────────────────────

class TestSelfIntegrity(unittest.TestCase):
    """Verify this test file never writes to repo files.

    Tempdir helpers that write to ``tempfile.TemporaryDirectory()`` paths
    are safe — the allowlist covers methods whose writes are scoped to a
    temporary directory that is cleaned up automatically.
    """

    TEMPDIR_HELPER_METHODS = frozenset({
        "_write_suite",
        "test_valid_tempdir_suite",
        "test_missing_file_detected",
        "test_malformed_json_detected",
    })

    def test_no_write_calls(self) -> None:
        source = Path(__file__).read_text(encoding="utf-8")
        tree = ast.parse(source)
        forbidden_attrs = {"write_text", "write_bytes", "unlink", "rmdir", "remove"}

        allowlisted_ranges: list[tuple[int, int]] = []
        for node in ast.walk(tree):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name in self.TEMPDIR_HELPER_METHODS:
                    allowlisted_ranges.append(
                        (node.lineno, node.end_lineno or node.lineno)
                    )

        def _is_allowed(lineno: int) -> bool:
            return any(lo <= lineno <= hi for lo, hi in allowlisted_ranges)

        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and node.attr in forbidden_attrs:
                if not _is_allowed(node.lineno):
                    self.fail(
                        f"forbidden call .{node.attr}() at line {node.lineno}"
                    )
            if isinstance(node, ast.Call):
                func = node.func
                if isinstance(func, ast.Name) and func.id == "open":
                    if not _is_allowed(node.lineno):
                        for kw in node.keywords:
                            if kw.arg == "mode" and isinstance(kw.value, ast.Constant):
                                if "w" in str(kw.value.value):
                                    self.fail(
                                        f"open(..., mode='w') at line {node.lineno}"
                                    )


# ── Committed data validation ─────────────────────────────────────────

class TestCommittedDataValid(unittest.TestCase):
    """The committed budget suite must pass validation."""

    def test_suite_validates(self) -> None:
        errors = validate_suite(BUDGET_DIR)
        self.assertEqual(errors, [], f"Committed suite has errors:\n" +
                         "\n".join(f"  - {e}" for e in errors))


# ── Hardware validation ───────────────────────────────────────────────

class TestHardwareValidation(unittest.TestCase):

    def test_valid_hardware(self) -> None:
        self.assertEqual(validate_hardware(_make_hardware()), [])

    def test_missing_schema_version(self) -> None:
        hw = _make_hardware()
        del hw["schemaVersion"]
        errors = validate_hardware(hw)
        self.assertTrue(any("schemaVersion" in e for e in errors))

    def test_missing_rows(self) -> None:
        hw = _make_hardware()
        del hw["rows"]
        errors = validate_hardware(hw)
        self.assertTrue(any("rows" in e for e in errors))

    def test_duplicate_row_id(self) -> None:
        hw = _make_hardware()
        hw["rows"].append(copy.deepcopy(hw["rows"][0]))
        errors = validate_hardware(hw)
        self.assertTrue(any("duplicate" in e for e in errors))

    def test_missing_required_key(self) -> None:
        for key in HARDWARE_ROW_REQUIRED:
            with self.subTest(key=key):
                hw = _make_hardware()
                del hw["rows"][0][key]
                errors = validate_hardware(hw)
                self.assertTrue(
                    any("missing" in e for e in errors),
                    f"removing '{key}' should produce a missing-key error",
                )

    def test_unknown_key_rejected(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["surpriseField"] = "oops"
        errors = validate_hardware(hw)
        self.assertTrue(any("unknown" in e.lower() or "fail-closed" in e.lower()
                            for e in errors))

    def test_certified_without_evidence(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["certified"] = True
        hw["rows"][0]["certifiedAt"] = None
        errors = validate_hardware(hw)
        self.assertTrue(len(errors) > 0)

    def test_certified_without_commit(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["certified"] = True
        hw["rows"][0]["certifiedCommit"] = None
        errors = validate_hardware(hw)
        self.assertTrue(any("certifiedCommit" in e for e in errors))

    def test_certified_without_reviewer(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["certified"] = True
        hw["rows"][0]["certifiedBy"] = None
        errors = validate_hardware(hw)
        self.assertTrue(any("certifiedBy" in e for e in errors))

    def test_negative_cores(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["cpuCores"] = -1
        errors = validate_hardware(hw)
        self.assertTrue(any("cpuCores" in e for e in errors))

    def test_zero_ram(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["ramGb"] = 0
        errors = validate_hardware(hw)
        self.assertTrue(any("ramGb" in e for e in errors))

    def test_empty_id(self) -> None:
        hw = _make_hardware()
        hw["rows"][0]["id"] = ""
        errors = validate_hardware(hw)
        self.assertTrue(any("id" in e for e in errors))

    def test_non_dict_row(self) -> None:
        hw = _make_hardware()
        hw["rows"][0] = "not a dict"
        errors = validate_hardware(hw)
        self.assertTrue(any("object" in e for e in errors))


# ── Budget validation ─────────────────────────────────────────────────

class TestBudgetValidation(unittest.TestCase):

    def setUp(self) -> None:
        self.hw_ids = frozenset({"test-row-1"})

    def test_valid_budget(self) -> None:
        self.assertEqual(validate_budget(_make_budget(), self.hw_ids), [])

    def test_missing_top_level_keys(self) -> None:
        for key in ("schemaVersion", "budgetVersion", "metadata", "metrics"):
            with self.subTest(key=key):
                b = _make_budget()
                del b[key]
                errors = validate_budget(b, self.hw_ids)
                self.assertTrue(any(key in e for e in errors))

    def test_duplicate_metric_id(self) -> None:
        b = _make_budget()
        b["metrics"].append(copy.deepcopy(b["metrics"][0]))
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("duplicate" in e for e in errors))

    def test_invalid_category(self) -> None:
        b = _make_budget()
        b["metrics"][0]["category"] = "bogus_category"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("category" in e for e in errors))

    def test_invalid_unit(self) -> None:
        b = _make_budget()
        b["metrics"][0]["unit"] = "furlongs"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("unit" in e for e in errors))

    def test_invalid_direction(self) -> None:
        b = _make_budget()
        b["metrics"][0]["direction"] = "sideways"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("direction" in e for e in errors))

    def test_invalid_status(self) -> None:
        b = _make_budget()
        b["metrics"][0]["status"] = "maybe_active"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("status" in e for e in errors))

    def test_invalid_backend(self) -> None:
        b = _make_budget()
        b["metrics"][0]["backend"] = "webgpu"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("backend" in e for e in errors))

    def test_invalid_percentile(self) -> None:
        b = _make_budget()
        b["metrics"][0]["percentile"] = "p42"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("percentile" in e for e in errors))

    def test_null_backend_allowed(self) -> None:
        b = _make_budget()
        b["metrics"][0]["backend"] = None
        errors = validate_budget(b, self.hw_ids)
        self.assertEqual(errors, [])

    def test_null_percentile_allowed(self) -> None:
        b = _make_budget()
        b["metrics"][0]["percentile"] = None
        errors = validate_budget(b, self.hw_ids)
        self.assertEqual(errors, [])

    def test_hardware_ref_to_nonexistent(self) -> None:
        b = _make_budget()
        b["metrics"][0]["hardwareRowId"] = "ghost-hardware"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("ghost-hardware" in e for e in errors))

    def test_active_with_null_budget(self) -> None:
        b = _make_budget()
        b["metrics"][0]["status"] = "active"
        b["metrics"][0]["budget"] = None
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("active" in e and "null" in e for e in errors))

    def test_negative_budget(self) -> None:
        b = _make_budget()
        b["metrics"][0]["budget"] = -5.0
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("non-negative" in e for e in errors))

    def test_string_budget_rejected(self) -> None:
        b = _make_budget()
        b["metrics"][0]["budget"] = "fast"
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("number" in e for e in errors))

    def test_unknown_metric_key_rejected(self) -> None:
        b = _make_budget()
        b["metrics"][0]["extraField"] = True
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("unknown" in e.lower() or "fail-closed" in e.lower()
                            for e in errors))

    def test_missing_metric_key(self) -> None:
        for key in METRIC_REQUIRED:
            with self.subTest(key=key):
                b = _make_budget()
                del b["metrics"][0][key]
                errors = validate_budget(b, self.hw_ids)
                self.assertTrue(
                    any("missing" in e for e in errors),
                    f"removing '{key}' should produce a missing-key error",
                )

    def test_empty_metric_id(self) -> None:
        b = _make_budget()
        b["metrics"][0]["id"] = ""
        errors = validate_budget(b, self.hw_ids)
        self.assertTrue(any("id" in e for e in errors))

    def test_metadata_missing_fields(self) -> None:
        for key in ("createdAt", "createdBy", "commitSha", "description"):
            with self.subTest(key=key):
                b = _make_budget()
                del b["metadata"][key]
                errors = validate_budget(b, self.hw_ids)
                self.assertTrue(any(key in e for e in errors))


# ── Baseline validation ──────────────────────────────────────────────

class TestBaselineValidation(unittest.TestCase):

    def setUp(self) -> None:
        self.metric_ids = frozenset({"test.frame_time.p50", "test.memory.peak_rss"})
        self.hw_ids = frozenset({"test-row-1"})

    def test_valid_baseline(self) -> None:
        self.assertEqual(
            validate_baselines(_make_baselines(), self.metric_ids, self.hw_ids),
            [],
        )

    def test_empty_baselines_valid(self) -> None:
        bl = _make_baselines()
        bl["baselines"] = []
        self.assertEqual(
            validate_baselines(bl, self.metric_ids, self.hw_ids), []
        )

    def test_missing_approval_fields(self) -> None:
        for field in ("approvedBy", "approvedAt", "approvalCommit"):
            with self.subTest(field=field):
                bl = _make_baselines()
                bl["baselines"][0][field] = None
                errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
                self.assertTrue(
                    len(errors) > 0,
                    f"null {field} should be rejected",
                )

    def test_self_approval_rejected(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["commitSha"] = "d" * 40
        bl["baselines"][0]["approvalCommit"] = "d" * 40
        bl["approvalPolicy"]["selfApprovalAllowed"] = False
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("self-approval" in e for e in errors))

    def test_self_approval_allowed_when_policy_permits(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["commitSha"] = "d" * 40
        bl["baselines"][0]["approvalCommit"] = "d" * 40
        bl["approvalPolicy"]["selfApprovalAllowed"] = True
        blocking_errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("forbidden" in error for error in blocking_errors))
        errors = validate_baselines(
            bl, self.metric_ids, self.hw_ids,
            allow_bootstrap_self_approval=True,
        )
        self.assertFalse(any("self-approval" in e for e in errors))

    def test_metric_ref_to_nonexistent(self) -> None:
        bl = _make_baselines(metric_id="ghost.metric")
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("ghost.metric" in e for e in errors))

    def test_hardware_ref_to_nonexistent(self) -> None:
        bl = _make_baselines(hw_id="ghost-hardware")
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("ghost-hardware" in e for e in errors))

    def test_duplicate_baseline_metric(self) -> None:
        bl = _make_baselines()
        bl["baselines"].append(copy.deepcopy(bl["baselines"][0]))
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("duplicate" in e for e in errors))

    def test_non_numeric_value(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["value"] = "twelve"
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("number" in e for e in errors))

    def test_invalid_commit_sha(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["commitSha"] = "xyz"
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("SHA" in e or "hex" in e for e in errors))

    def test_missing_measured_at(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["measuredAt"] = 12345
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("ISO-8601" in e for e in errors))

    def test_zero_sample_count(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["sampleCount"] = 0
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("sampleCount" in e for e in errors))

    def test_unknown_baseline_key_rejected(self) -> None:
        bl = _make_baselines()
        bl["baselines"][0]["sneakyField"] = True
        errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
        self.assertTrue(any("unknown" in e.lower() or "fail-closed" in e.lower()
                            for e in errors))

    def test_missing_required_baseline_key(self) -> None:
        for key in BASELINE_ENTRY_REQUIRED:
            with self.subTest(key=key):
                bl = _make_baselines()
                del bl["baselines"][0][key]
                errors = validate_baselines(bl, self.metric_ids, self.hw_ids)
                self.assertTrue(
                    any("missing" in e for e in errors),
                    f"removing '{key}' should produce a missing-key error",
                )


# ── Result validation ─────────────────────────────────────────────────

class TestResultValidation(unittest.TestCase):

    def setUp(self) -> None:
        self.hw_ids = frozenset({"test-row-1"})

    def test_valid_result(self) -> None:
        self.assertEqual(validate_result(_make_result(), self.hw_ids), [])

    def test_missing_top_level_keys(self) -> None:
        for key in ("commitSha", "timestamp", "hardwareRowId", "measurements"):
            with self.subTest(key=key):
                r = _make_result()
                del r[key]
                errors = validate_result(r, self.hw_ids)
                self.assertTrue(any(key in e for e in errors))

    def test_unknown_hardware(self) -> None:
        r = _make_result(hw_id="phantom-box")
        errors = validate_result(r, self.hw_ids)
        self.assertTrue(any("phantom-box" in e for e in errors))

    def test_duplicate_measurement(self) -> None:
        r = _make_result()
        r["measurements"].append(copy.deepcopy(r["measurements"][0]))
        errors = validate_result(r, self.hw_ids)
        self.assertTrue(any("duplicate" in e for e in errors))

    def test_invalid_commit_sha(self) -> None:
        r = _make_result()
        r["commitSha"] = "gg"
        errors = validate_result(r, self.hw_ids)
        self.assertTrue(len(errors) > 0)

    def test_non_numeric_value(self) -> None:
        r = _make_result()
        r["measurements"][0]["value"] = "fast"
        errors = validate_result(r, self.hw_ids)
        self.assertTrue(any("number" in e for e in errors))

    def test_invalid_unit(self) -> None:
        r = _make_result()
        r["measurements"][0]["unit"] = "parsecs"
        errors = validate_result(r, self.hw_ids)
        self.assertTrue(any("unit" in e for e in errors))

    def test_zero_sample_count(self) -> None:
        r = _make_result()
        r["measurements"][0]["sampleCount"] = 0
        errors = validate_result(r, self.hw_ids)
        self.assertTrue(any("sampleCount" in e for e in errors))

    def test_missing_measurement_key(self) -> None:
        for key in RESULT_MEASUREMENT_REQUIRED:
            with self.subTest(key=key):
                r = _make_result()
                del r["measurements"][0][key]
                errors = validate_result(r, self.hw_ids)
                self.assertTrue(
                    any("missing" in e for e in errors),
                    f"removing '{key}' should produce a missing-key error",
                )


# ── Comparator ────────────────────────────────────────────────────────

class TestComparator(unittest.TestCase):

    def _write_suite(self, tmpdir: Path, hw: dict, budget: dict) -> Path:
        """Write hw + budget to a temp dir and return the dir path."""
        (tmpdir / "hardware.json").write_text(
            json.dumps(hw), encoding="utf-8"
        )
        (tmpdir / "budget.json").write_text(
            json.dumps(budget), encoding="utf-8"
        )
        hardware_ids = [row["id"] for row in hw["rows"]]
        baselines = _make_baselines_for_budget(budget, hardware_ids)
        (tmpdir / "baselines.json").write_text(
            json.dumps(baselines), encoding="utf-8"
        )
        return tmpdir

    def test_all_passing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            report = compare(d, result)
            self.assertTrue(report.passed)
            self.assertEqual(report.failed_count, 0)
            self.assertEqual(report.passed_count, 2)

    def test_regression_detected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            result["measurements"][0]["value"] = 20.0
            report = compare(d, result)
            self.assertFalse(report.passed)
            self.assertEqual(report.failed_count, 1)
            failed = [v for v in report.verdicts if not v.passed]
            self.assertEqual(failed[0].metric_id, "test.frame_time.p50")
            self.assertLess(failed[0].margin_percent, 0)

    def test_exact_budget_passes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            result["measurements"][0]["value"] = 16.67
            report = compare(d, result)
            self.assertTrue(report.passed)

    def test_unit_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            result["measurements"][0]["unit"] = "us"
            report = compare(d, result)
            self.assertFalse(report.passed)
            self.assertTrue(any("mismatch" in e for e in report.errors))

    def test_unknown_metric_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            result["measurements"][0]["metricId"] = "ghost.metric.p50"
            report = compare(d, result)
            self.assertFalse(report.passed)
            self.assertTrue(any("ghost.metric" in e for e in report.errors))

    def test_pending_metrics_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            budget = _make_budget()
            budget["metrics"][0]["status"] = "pending_measurement"
            budget["metrics"][0]["budget"] = None
            self._write_suite(d, _make_hardware(), budget)
            result = _make_result()
            report = compare(d, result)
            self.assertTrue(report.passed)
            self.assertGreater(report.skipped_pending, 0)

    def test_uncertified_hardware_flagged(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            hw = _make_hardware()
            hw["rows"][0]["certified"] = False
            hw["rows"][0]["certifiedAt"] = None
            hw["rows"][0]["certifiedBy"] = None
            hw["rows"][0]["certifiedCommit"] = None
            self._write_suite(d, hw, _make_budget())
            result = _make_result()
            report = compare(d, result)
            self.assertTrue(report.passed)
            self.assertFalse(report.authoritative)
            self.assertTrue(any("not certified" in e for e in report.advisories))

    def test_higher_is_better_direction(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            budget = _make_budget()
            budget["metrics"][0]["direction"] = "higher_is_better"
            budget["metrics"][0]["unit"] = "fps"
            budget["metrics"][0]["budget"] = 60.0
            self._write_suite(d, _make_hardware(), budget)
            result = _make_result()
            result["measurements"][0]["value"] = 75.0
            result["measurements"][0]["unit"] = "fps"
            report = compare(d, result)
            passing = [v for v in report.verdicts
                       if v.metric_id == "test.frame_time.p50"]
            self.assertTrue(passing[0].passed)

    def test_higher_is_better_regression(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            budget = _make_budget()
            budget["metrics"][0]["direction"] = "higher_is_better"
            budget["metrics"][0]["unit"] = "fps"
            budget["metrics"][0]["budget"] = 60.0
            self._write_suite(d, _make_hardware(), budget)
            result = _make_result()
            result["measurements"][0]["value"] = 45.0
            result["measurements"][0]["unit"] = "fps"
            report = compare(d, result)
            failing = [v for v in report.verdicts
                       if v.metric_id == "test.frame_time.p50"]
            self.assertFalse(failing[0].passed)

    def test_malformed_result_json_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = {"not": "a valid result"}
            report = compare(d, result)
            self.assertFalse(report.passed)
            self.assertTrue(len(report.errors) > 0)

    def test_commit_sha_in_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            report = compare(d, result)
            self.assertEqual(report.commit_sha, EXPECTED_RESULT_SHA)
            for v in report.verdicts:
                self.assertEqual(v.commit_sha, EXPECTED_RESULT_SHA)

    def test_hardware_row_in_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            self._write_suite(d, _make_hardware(), _make_budget())
            result = _make_result()
            report = compare(d, result)
            self.assertEqual(report.hardware_row_id, "test-row-1")
            for v in report.verdicts:
                self.assertEqual(v.hardware_row_id, "test-row-1")


# ── Full-suite integration ────────────────────────────────────────────

class TestFullSuiteValidation(unittest.TestCase):

    def test_valid_tempdir_suite(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "hardware.json").write_text(
                json.dumps(_make_hardware()), encoding="utf-8"
            )
            (d / "budget.json").write_text(
                json.dumps(_make_budget()), encoding="utf-8"
            )
            (d / "baselines.json").write_text(
                json.dumps(_make_baselines_for_budget(
                    _make_budget(), ["test-row-1"],
                )), encoding="utf-8"
            )
            errors = validate_suite(d)
            self.assertEqual(errors, [])

    def test_missing_file_detected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "hardware.json").write_text(
                json.dumps(_make_hardware()), encoding="utf-8"
            )
            errors = validate_suite(d)
            self.assertTrue(any("missing" in e for e in errors))

    def test_malformed_json_detected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "hardware.json").write_text("{invalid", encoding="utf-8")
            (d / "budget.json").write_text(
                json.dumps(_make_budget()), encoding="utf-8"
            )
            (d / "baselines.json").write_text(
                json.dumps(_make_baselines()), encoding="utf-8"
            )
            errors = validate_suite(d)
            self.assertTrue(any("malformed" in e for e in errors))


# ── Enum completeness ─────────────────────────────────────────────────

class TestEnumCompleteness(unittest.TestCase):
    """Committed budget data only uses values from the canonical enums."""

    def test_all_committed_units_valid(self) -> None:
        budget = _load_json(BUDGET_DIR / "budget.json")
        for m in budget["metrics"]:
            with self.subTest(metric=m["id"]):
                self.assertIn(m["unit"], VALID_UNITS)

    def test_all_committed_directions_valid(self) -> None:
        budget = _load_json(BUDGET_DIR / "budget.json")
        for m in budget["metrics"]:
            with self.subTest(metric=m["id"]):
                self.assertIn(m["direction"], VALID_DIRECTIONS)

    def test_all_committed_categories_valid(self) -> None:
        budget = _load_json(BUDGET_DIR / "budget.json")
        for m in budget["metrics"]:
            with self.subTest(metric=m["id"]):
                self.assertIn(m["category"], VALID_CATEGORIES)

    def test_all_committed_statuses_valid(self) -> None:
        budget = _load_json(BUDGET_DIR / "budget.json")
        for m in budget["metrics"]:
            with self.subTest(metric=m["id"]):
                self.assertIn(m["status"], VALID_METRIC_STATUSES)

    def test_all_committed_backends_valid(self) -> None:
        budget = _load_json(BUDGET_DIR / "budget.json")
        for m in budget["metrics"]:
            with self.subTest(metric=m["id"]):
                if m["backend"] is not None:
                    self.assertIn(m["backend"], VALID_BACKENDS)

    def test_all_committed_hw_refs_valid(self) -> None:
        hw = _load_json(BUDGET_DIR / "hardware.json")
        hw_ids = {r["id"] for r in hw["rows"]}
        budget = _load_json(BUDGET_DIR / "budget.json")
        for m in budget["metrics"]:
            with self.subTest(metric=m["id"]):
                if m["hardwareRowId"] is not None:
                    self.assertIn(m["hardwareRowId"], hw_ids)


if __name__ == "__main__":
    unittest.main()
