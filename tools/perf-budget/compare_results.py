#!/usr/bin/env python3
"""Compare benchmark results against performance budgets.

Fail-closed: rejects malformed inputs, unknown metrics, uncertified
hardware, and unit mismatches before any comparison begins.  A clean
exit means every measured metric passed its budget.

Exit codes:
  0 — all comparisons pass (or no active budgets exist to compare)
  1 — one or more regressions or validation errors
  2 — usage / file-not-found / structural error

Produces a structured JSON verdict on stdout when --json is passed.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any

from validate_budget import (
    validate_result,
    validate_budget,
    validate_hardware,
    VALID_DIRECTIONS,
)


@dataclass(frozen=True)
class ComparisonVerdict:
    metric_id: str
    budget_value: float
    measured_value: float
    unit: str
    direction: str
    passed: bool
    margin_percent: float
    hardware_row_id: str
    commit_sha: str


@dataclass(frozen=True)
class ComparisonReport:
    passed: bool
    commit_sha: str
    hardware_row_id: str
    total_metrics: int
    passed_count: int
    failed_count: int
    skipped_pending: int
    verdicts: list[ComparisonVerdict]
    errors: list[str]


def _passes_budget(measured: float, budget: float, direction: str) -> bool:
    if direction == "lower_is_better":
        return measured <= budget
    elif direction == "higher_is_better":
        return measured >= budget
    return False


def _margin_percent(measured: float, budget: float, direction: str) -> float:
    if budget == 0:
        return 0.0 if measured == 0 else float("inf")
    if direction == "lower_is_better":
        return ((budget - measured) / budget) * 100.0
    else:
        return ((measured - budget) / budget) * 100.0


def compare(budget_dir: Path, result_data: dict[str, Any]) -> ComparisonReport:
    """Compare a result set against the budget suite in budget_dir."""
    errors: list[str] = []

    hw_path = budget_dir / "hardware.json"
    budget_path = budget_dir / "budget.json"

    for p in (hw_path, budget_path):
        if not p.exists():
            errors.append(f"missing required file: {p}")
    if errors:
        return ComparisonReport(
            passed=False, commit_sha="", hardware_row_id="",
            total_metrics=0, passed_count=0, failed_count=0,
            skipped_pending=0, verdicts=[], errors=errors,
        )

    try:
        hw_data = json.loads(hw_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        errors.append(f"hardware.json: malformed: {e}")
        return ComparisonReport(
            passed=False, commit_sha="", hardware_row_id="",
            total_metrics=0, passed_count=0, failed_count=0,
            skipped_pending=0, verdicts=[], errors=errors,
        )

    try:
        budget_data = json.loads(budget_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        errors.append(f"budget.json: malformed: {e}")
        return ComparisonReport(
            passed=False, commit_sha="", hardware_row_id="",
            total_metrics=0, passed_count=0, failed_count=0,
            skipped_pending=0, verdicts=[], errors=errors,
        )

    hw_errors = validate_hardware(hw_data)
    if hw_errors:
        errors.extend(hw_errors)
        return ComparisonReport(
            passed=False, commit_sha="", hardware_row_id="",
            total_metrics=0, passed_count=0, failed_count=0,
            skipped_pending=0, verdicts=[], errors=errors,
        )

    hardware_ids = frozenset(
        row["id"] for row in hw_data.get("rows", [])
        if isinstance(row, dict) and "id" in row
    )

    certified_ids = frozenset(
        row["id"] for row in hw_data.get("rows", [])
        if isinstance(row, dict) and row.get("certified") is True
    )

    budget_errors = validate_budget(budget_data, hardware_ids)
    if budget_errors:
        errors.extend(budget_errors)
        return ComparisonReport(
            passed=False, commit_sha="", hardware_row_id="",
            total_metrics=0, passed_count=0, failed_count=0,
            skipped_pending=0, verdicts=[], errors=errors,
        )

    result_errors = validate_result(result_data, hardware_ids)
    if result_errors:
        errors.extend(result_errors)
        return ComparisonReport(
            passed=False, commit_sha="", hardware_row_id="",
            total_metrics=0, passed_count=0, failed_count=0,
            skipped_pending=0, verdicts=[], errors=errors,
        )

    result_hw = result_data["hardwareRowId"]
    if result_hw not in certified_ids:
        errors.append(
            f"result hardware {result_hw!r} is not certified — "
            "comparison results are advisory only"
        )

    metrics_by_id = {
        m["id"]: m for m in budget_data["metrics"]
        if isinstance(m, dict) and "id" in m
    }

    verdicts: list[ComparisonVerdict] = []
    skipped = 0
    overall_pass = True

    for measurement in result_data["measurements"]:
        mid = measurement["metricId"]
        if mid not in metrics_by_id:
            errors.append(f"measurement {mid!r} has no matching budget metric")
            overall_pass = False
            continue

        metric = metrics_by_id[mid]

        if metric["status"] == "pending_measurement":
            skipped += 1
            continue

        if metric["budget"] is None:
            skipped += 1
            continue

        if measurement["unit"] != metric["unit"]:
            errors.append(
                f"unit mismatch for {mid}: result={measurement['unit']!r}, "
                f"budget={metric['unit']!r}"
            )
            overall_pass = False
            continue

        budget_val = metric["budget"]
        measured_val = measurement["value"]
        direction = metric["direction"]

        passed = _passes_budget(measured_val, budget_val, direction)
        margin = _margin_percent(measured_val, budget_val, direction)

        verdicts.append(ComparisonVerdict(
            metric_id=mid,
            budget_value=budget_val,
            measured_value=measured_val,
            unit=metric["unit"],
            direction=direction,
            passed=passed,
            margin_percent=round(margin, 2),
            hardware_row_id=result_hw,
            commit_sha=result_data["commitSha"],
        ))

        if not passed:
            overall_pass = False

    if errors:
        overall_pass = False

    return ComparisonReport(
        passed=overall_pass,
        commit_sha=result_data.get("commitSha", ""),
        hardware_row_id=result_data.get("hardwareRowId", ""),
        total_metrics=len(verdicts) + skipped,
        passed_count=sum(1 for v in verdicts if v.passed),
        failed_count=sum(1 for v in verdicts if not v.passed),
        skipped_pending=skipped,
        verdicts=verdicts,
        errors=errors,
    )


def report_to_dict(report: ComparisonReport) -> dict[str, Any]:
    d = asdict(report)
    d["verdicts"] = [asdict(v) for v in report.verdicts]
    return d


def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    use_json = "--json" in args
    args = [a for a in args if a != "--json"]

    if len(args) != 2:
        print(
            "Usage: compare_results.py <budget-dir> <results.json> [--json]",
            file=sys.stderr,
        )
        return 2

    budget_dir = Path(args[0])
    result_path = Path(args[1])

    if not budget_dir.is_dir():
        print(f"Error: {budget_dir} is not a directory", file=sys.stderr)
        return 2
    if not result_path.is_file():
        print(f"Error: {result_path} not found", file=sys.stderr)
        return 2

    try:
        result_data = json.loads(result_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        print(f"Error: malformed results JSON: {e}", file=sys.stderr)
        return 2

    report = compare(budget_dir, result_data)

    if use_json:
        print(json.dumps(report_to_dict(report), indent=2))
    else:
        status = "PASS" if report.passed else "FAIL"
        print(f"{status}: {report.passed_count} passed, "
              f"{report.failed_count} failed, "
              f"{report.skipped_pending} pending")
        for v in report.verdicts:
            mark = "OK" if v.passed else "REGRESSION"
            print(f"  [{mark}] {v.metric_id}: "
                  f"{v.measured_value}{v.unit} vs budget {v.budget_value}{v.unit} "
                  f"(margin {v.margin_percent:+.1f}%)")
        for e in report.errors:
            print(f"  [ERROR] {e}")

    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
