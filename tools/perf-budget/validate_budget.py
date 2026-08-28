#!/usr/bin/env python3
"""Fail-closed validator for PERF-100 performance-budget governance.

Validates budget definitions, hardware rows, and baseline approval files.
Every rejection is explicit; every unknown field or ambiguous value is an
error, not a warning.  A silent pass from this validator means the budget
data is structurally sound and governance-compliant.

Exit codes:
  0 — all validations pass
  1 — one or more validation errors
  2 — usage / file-not-found error
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any


class BudgetValidationError(Exception):
    """A structural or governance violation in budget data."""


# ── Canonical enumerations ────────────────────────────────────────────

VALID_UNITS = frozenset({
    "ms", "us", "ns", "s",
    "bytes", "kilobytes", "megabytes", "gigabytes",
    "bytes_per_hour",
    "count", "percent", "fps",
})

VALID_DIRECTIONS = frozenset({"lower_is_better", "higher_is_better"})

VALID_CATEGORIES = frozenset({
    "frame_time", "tick_time", "startup_time",
    "memory", "package_size", "soak", "visual", "throughput",
})

VALID_METRIC_STATUSES = frozenset({
    "pending_measurement", "active", "suspended", "retired",
})

VALID_BACKENDS = frozenset({
    "d3d11", "d3d12", "vulkan", "metal", "opengl", "nullrhi",
})

VALID_PERCENTILES = frozenset({
    "p50", "p90", "p95", "p99", "p999",
})

# ── Required keys per top-level object ────────────────────────────────

HARDWARE_ROW_REQUIRED = frozenset({
    "id", "certified", "os", "cpu", "cpuCores",
    "gpu", "gpuVramGb", "gpuDriverMinVersion", "ramGb",
    "certifiedAt", "certifiedBy", "certifiedCommit", "notes",
})

METRIC_REQUIRED = frozenset({
    "id", "category", "scene", "backend", "product",
    "unit", "direction", "percentile",
    "hardwareRowId", "budget", "status",
})

BASELINE_ENTRY_REQUIRED = frozenset({
    "metricId", "value", "unit", "commitSha", "hardwareRowId",
    "measuredAt", "sampleCount",
    "approvedBy", "approvedAt", "approvalCommit",
})

RESULT_MEASUREMENT_REQUIRED = frozenset({
    "metricId", "value", "unit", "sampleCount",
})


# ── Required keys per top-level schema ───────────────────────────────

HARDWARE_TOP_LEVEL_REQUIRED = frozenset({"schemaVersion", "rows"})
BUDGET_TOP_LEVEL_REQUIRED = frozenset({
    "schemaVersion", "budgetVersion", "metadata", "metrics",
})
BASELINES_TOP_LEVEL_REQUIRED = frozenset({
    "schemaVersion", "baselineVersion", "approvalPolicy", "baselines",
})
RESULT_TOP_LEVEL_REQUIRED = frozenset({
    "commitSha", "timestamp", "hardwareRowId", "measurements",
})


# ── Validation helpers ────────────────────────────────────────────────

def _is_strict_int(value: Any) -> bool:
    """True for int values that are not bool (Python bool is a subclass of int)."""
    return isinstance(value, int) and not isinstance(value, bool)


def _is_strict_number(value: Any) -> bool:
    """True for int/float values that are not bool."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _is_finite_number(value: Any) -> bool:
    """True for finite int/float values that are not bool, NaN, or infinity."""
    if not _is_strict_number(value):
        return False
    import math
    return math.isfinite(value)


def _check_keys(obj: dict, required: frozenset[str], context: str) -> list[str]:
    """Return error strings for missing or unknown keys."""
    errors: list[str] = []
    present = set(obj.keys())
    missing = required - present
    extra = present - required
    if missing:
        errors.append(f"{context}: missing required keys: {sorted(missing)}")
    if extra:
        errors.append(f"{context}: unknown keys (fail-closed): {sorted(extra)}")
    return errors


def _check_enum(value: Any, valid: frozenset[str], field: str,
                context: str, *, allow_null: bool = False) -> list[str]:
    if value is None and allow_null:
        return []
    if value not in valid:
        return [f"{context}: {field}={value!r} not in {sorted(valid)}"]
    return []


def _check_sha(value: Any, field: str, context: str,
               *, allow_null: bool = False) -> list[str]:
    if value is None and allow_null:
        return []
    if not isinstance(value, str) or len(value) < 7:
        return [f"{context}: {field} must be a hex SHA (>=7 chars), got {value!r}"]
    if not all(c in "0123456789abcdef" for c in value.lower()):
        return [f"{context}: {field} contains non-hex characters: {value!r}"]
    return []


def _check_iso8601(value: Any, field: str, context: str,
                   *, allow_null: bool = False) -> list[str]:
    if value is None and allow_null:
        return []
    if not isinstance(value, str):
        return [f"{context}: {field} must be an ISO-8601 string, got {type(value).__name__}"]
    if "T" not in value:
        return [f"{context}: {field} does not look like ISO-8601: {value!r}"]
    return []


# ── Hardware validation ───────────────────────────────────────────────

def validate_hardware(data: dict[str, Any]) -> list[str]:
    """Validate a hardware.json file. Returns list of error strings."""
    errors: list[str] = []

    errors.extend(_check_keys(data, HARDWARE_TOP_LEVEL_REQUIRED, "hardware"))

    if "schemaVersion" not in data:
        errors.append("hardware: missing 'schemaVersion'")
    if "rows" not in data:
        errors.append("hardware: missing 'rows'")
        return errors
    if not isinstance(data["rows"], list):
        errors.append("hardware: 'rows' must be a list")
        return errors

    seen_ids: set[str] = set()
    for i, row in enumerate(data["rows"]):
        ctx = f"hardware.rows[{i}]"
        if not isinstance(row, dict):
            errors.append(f"{ctx}: must be an object")
            continue

        errors.extend(_check_keys(row, HARDWARE_ROW_REQUIRED, ctx))

        row_id = row.get("id")
        if row_id is not None:
            if not isinstance(row_id, str) or not row_id:
                errors.append(f"{ctx}: 'id' must be a non-empty string")
            elif row_id in seen_ids:
                errors.append(f"{ctx}: duplicate hardware row id: {row_id!r}")
            else:
                seen_ids.add(row_id)

        certified = row.get("certified")
        if certified is True:
            errors.extend(_check_iso8601(row.get("certifiedAt"), "certifiedAt", ctx))
            if not row.get("certifiedBy"):
                errors.append(f"{ctx}: certified=true but certifiedBy is empty/null")
            errors.extend(_check_sha(row.get("certifiedCommit"), "certifiedCommit", ctx))
        elif certified is False:
            pass
        elif certified is not None:
            errors.append(f"{ctx}: 'certified' must be true or false")

        cores = row.get("cpuCores")
        if cores is not None and (not _is_strict_int(cores) or cores < 1):
            errors.append(f"{ctx}: cpuCores must be a positive integer")

        ram = row.get("ramGb")
        if ram is not None and not _is_finite_number(ram):
            errors.append(f"{ctx}: ramGb must be a finite positive number")
        elif ram is not None and ram <= 0:
            errors.append(f"{ctx}: ramGb must be a finite positive number")

        vram = row.get("gpuVramGb")
        if vram is not None and not _is_finite_number(vram):
            errors.append(f"{ctx}: gpuVramGb must be a finite non-negative number")
        elif vram is not None and vram < 0:
            errors.append(f"{ctx}: gpuVramGb must be a finite non-negative number")

    return errors


# ── Budget validation ─────────────────────────────────────────────────

def validate_budget(data: dict[str, Any],
                    hardware_ids: frozenset[str]) -> list[str]:
    """Validate a budget.json file. Returns list of error strings."""
    errors: list[str] = []

    errors.extend(_check_keys(data, BUDGET_TOP_LEVEL_REQUIRED, "budget"))

    for key in ("schemaVersion", "budgetVersion", "metadata", "metrics"):
        if key not in data:
            errors.append(f"budget: missing required key '{key}'")
    if any("missing required key" in e for e in errors):
        return errors

    meta = data["metadata"]
    if not isinstance(meta, dict):
        errors.append("budget.metadata: must be an object")
    else:
        for mk in ("createdAt", "createdBy", "commitSha", "description"):
            if mk not in meta:
                errors.append(f"budget.metadata: missing '{mk}'")

    metrics = data["metrics"]
    if not isinstance(metrics, list):
        errors.append("budget.metrics: must be a list")
        return errors

    seen_ids: set[str] = set()
    for i, m in enumerate(metrics):
        ctx = f"budget.metrics[{i}]"
        if not isinstance(m, dict):
            errors.append(f"{ctx}: must be an object")
            continue

        errors.extend(_check_keys(m, METRIC_REQUIRED, ctx))

        metric_id = m.get("id")
        if metric_id is not None:
            if not isinstance(metric_id, str) or not metric_id:
                errors.append(f"{ctx}: 'id' must be a non-empty string")
            elif metric_id in seen_ids:
                errors.append(f"{ctx}: duplicate metric id: {metric_id!r}")
            else:
                seen_ids.add(metric_id)

        errors.extend(_check_enum(m.get("category"), VALID_CATEGORIES,
                                  "category", ctx))
        errors.extend(_check_enum(m.get("unit"), VALID_UNITS, "unit", ctx))
        errors.extend(_check_enum(m.get("direction"), VALID_DIRECTIONS,
                                  "direction", ctx))
        errors.extend(_check_enum(m.get("status"), VALID_METRIC_STATUSES,
                                  "status", ctx))
        errors.extend(_check_enum(m.get("backend"), VALID_BACKENDS,
                                  "backend", ctx, allow_null=True))
        errors.extend(_check_enum(m.get("percentile"), VALID_PERCENTILES,
                                  "percentile", ctx, allow_null=True))

        hw_id = m.get("hardwareRowId")
        if hw_id is not None and hw_id not in hardware_ids:
            errors.append(
                f"{ctx}: hardwareRowId={hw_id!r} not found in hardware rows "
                f"{sorted(hardware_ids)}"
            )

        budget_val = m.get("budget")
        status = m.get("status")
        if status == "active" and budget_val is None:
            errors.append(f"{ctx}: status='active' but budget is null")
        if budget_val is not None:
            if not _is_finite_number(budget_val):
                errors.append(f"{ctx}: budget must be a finite number or null")
            elif budget_val < 0:
                errors.append(f"{ctx}: budget must be non-negative")

    return errors


# ── Baseline validation ──────────────────────────────────────────────

def validate_baselines(data: dict[str, Any],
                       metric_ids: frozenset[str],
                       hardware_ids: frozenset[str]) -> list[str]:
    """Validate a baselines.json file. Returns list of error strings."""
    errors: list[str] = []

    errors.extend(_check_keys(data, BASELINES_TOP_LEVEL_REQUIRED, "baselines"))

    for key in ("schemaVersion", "baselineVersion", "approvalPolicy", "baselines"):
        if key not in data:
            errors.append(f"baselines: missing required key '{key}'")
    if any("missing required key" in e for e in errors):
        return errors

    policy = data["approvalPolicy"]
    if not isinstance(policy, dict):
        errors.append("baselines.approvalPolicy: must be an object")

    baselines = data["baselines"]
    if not isinstance(baselines, list):
        errors.append("baselines.baselines: must be a list")
        return errors

    seen_metrics: set[str] = set()
    for i, b in enumerate(baselines):
        ctx = f"baselines.baselines[{i}]"
        if not isinstance(b, dict):
            errors.append(f"{ctx}: must be an object")
            continue

        errors.extend(_check_keys(b, BASELINE_ENTRY_REQUIRED, ctx))

        mid = b.get("metricId")
        if mid is not None:
            if mid not in metric_ids:
                errors.append(
                    f"{ctx}: metricId={mid!r} not found in budget metrics"
                )
            if mid in seen_metrics:
                errors.append(f"{ctx}: duplicate baseline for metric {mid!r}")
            else:
                seen_metrics.add(mid)

        hw_id = b.get("hardwareRowId")
        if hw_id is not None and hw_id not in hardware_ids:
            errors.append(
                f"{ctx}: hardwareRowId={hw_id!r} not in hardware rows"
            )

        val = b.get("value")
        if val is not None and not _is_finite_number(val):
            errors.append(f"{ctx}: value must be a finite number")

        errors.extend(_check_sha(b.get("commitSha"), "commitSha", ctx))
        errors.extend(_check_iso8601(b.get("measuredAt"), "measuredAt", ctx))

        sc = b.get("sampleCount")
        if sc is not None and (not _is_strict_int(sc) or sc < 1):
            errors.append(f"{ctx}: sampleCount must be a positive integer")

        errors.extend(_check_sha(b.get("approvalCommit"), "approvalCommit", ctx))
        errors.extend(_check_iso8601(b.get("approvedAt"), "approvedAt", ctx))

        approved_by = b.get("approvedBy")
        if not approved_by or not isinstance(approved_by, str):
            errors.append(f"{ctx}: approvedBy must be a non-empty string")

        commit_sha = b.get("commitSha")
        approval_commit = b.get("approvalCommit")
        if (commit_sha and approval_commit and
                isinstance(commit_sha, str) and isinstance(approval_commit, str) and
                commit_sha.lower() == approval_commit.lower()):
            self_allowed = policy.get("selfApprovalAllowed", False)
            if not self_allowed:
                errors.append(
                    f"{ctx}: approvalCommit equals commitSha — "
                    "self-approval is not allowed by policy"
                )

    return errors


# ── Result validation ─────────────────────────────────────────────────

def validate_result(data: dict[str, Any],
                    hardware_ids: frozenset[str]) -> list[str]:
    """Validate a benchmark result file for structural completeness."""
    errors: list[str] = []

    errors.extend(_check_keys(data, RESULT_TOP_LEVEL_REQUIRED, "result"))

    for key in ("commitSha", "timestamp", "hardwareRowId", "measurements"):
        if key not in data:
            errors.append(f"result: missing required key '{key}'")
    if any("missing required key" in e for e in errors):
        return errors

    errors.extend(_check_sha(data["commitSha"], "commitSha", "result"))
    errors.extend(_check_iso8601(data["timestamp"], "timestamp", "result"))

    hw_id = data["hardwareRowId"]
    if hw_id not in hardware_ids:
        errors.append(
            f"result: hardwareRowId={hw_id!r} not in certified hardware rows"
        )

    measurements = data["measurements"]
    if not isinstance(measurements, list):
        errors.append("result.measurements: must be a list")
        return errors

    seen_ids: set[str] = set()
    for i, m in enumerate(measurements):
        ctx = f"result.measurements[{i}]"
        if not isinstance(m, dict):
            errors.append(f"{ctx}: must be an object")
            continue

        errors.extend(_check_keys(m, RESULT_MEASUREMENT_REQUIRED, ctx))

        mid = m.get("metricId")
        if mid is not None:
            if mid in seen_ids:
                errors.append(f"{ctx}: duplicate measurement for {mid!r}")
            else:
                seen_ids.add(mid)

        val = m.get("value")
        if val is not None and not _is_finite_number(val):
            errors.append(f"{ctx}: value must be a finite number")

        errors.extend(_check_enum(m.get("unit"), VALID_UNITS, "unit", ctx))

        sc = m.get("sampleCount")
        if sc is not None and (not _is_strict_int(sc) or sc < 1):
            errors.append(f"{ctx}: sampleCount must be a positive integer")

    return errors


# ── Full-suite validation ─────────────────────────────────────────────

def validate_suite(budget_dir: Path) -> list[str]:
    """Validate the full budget suite in a versioned directory."""
    errors: list[str] = []

    hw_path = budget_dir / "hardware.json"
    budget_path = budget_dir / "budget.json"
    baselines_path = budget_dir / "baselines.json"

    for p in (hw_path, budget_path, baselines_path):
        if not p.exists():
            errors.append(f"missing required file: {p}")

    if errors:
        return errors

    try:
        hw_data = json.loads(hw_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        errors.append(f"hardware.json: malformed JSON: {e}")
        return errors

    try:
        budget_data = json.loads(budget_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        errors.append(f"budget.json: malformed JSON: {e}")
        return errors

    try:
        baselines_data = json.loads(baselines_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        errors.append(f"baselines.json: malformed JSON: {e}")
        return errors

    hw_errors = validate_hardware(hw_data)
    errors.extend(hw_errors)

    hardware_ids = frozenset(
        row["id"] for row in hw_data.get("rows", [])
        if isinstance(row, dict) and "id" in row
    )

    budget_errors = validate_budget(budget_data, hardware_ids)
    errors.extend(budget_errors)

    metric_ids = frozenset(
        m["id"] for m in budget_data.get("metrics", [])
        if isinstance(m, dict) and "id" in m
    )

    baseline_errors = validate_baselines(baselines_data, metric_ids, hardware_ids)
    errors.extend(baseline_errors)

    return errors


# ── CLI entry point ───────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    if not args:
        print("Usage: validate_budget.py <budget-dir>", file=sys.stderr)
        print("  e.g. validate_budget.py perf-budgets/v1", file=sys.stderr)
        return 2

    budget_dir = Path(args[0])
    if not budget_dir.is_dir():
        print(f"Error: {budget_dir} is not a directory", file=sys.stderr)
        return 2

    errors = validate_suite(budget_dir)

    if errors:
        print(f"FAIL: {len(errors)} validation error(s):", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(f"OK: {budget_dir} passes all validations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
