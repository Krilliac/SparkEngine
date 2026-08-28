#!/usr/bin/env python3
"""Compare one bounded benchmark result against governed performance budgets.

The blocking CLI requires ``--expected-sha`` from the workflow environment.
The SHA is never inferred from the result being checked, so a short or forged
self-asserted identifier cannot be reported as exact-commit evidence.
"""

from __future__ import annotations

import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from validate_budget import (
    load_bounded_json,
    validate_baselines,
    validate_budget,
    validate_hardware,
    validate_result,
)


@dataclass(frozen=True)
class ComparisonVerdict:
    metric_id: str
    budget_value: float
    measured_value: float
    unit: str
    direction: str
    passed: bool
    margin_percent: float | None
    margin_reason: str | None
    hardware_row_id: str
    commit_sha: str


@dataclass(frozen=True)
class SkippedMetric:
    metric_id: str
    status: str
    reason: str


@dataclass(frozen=True)
class ComparisonReport:
    passed: bool
    authoritative: bool
    commit_sha: str
    hardware_row_id: str
    total_metrics: int
    passed_count: int
    failed_count: int
    skipped_pending: int
    skipped_non_active: int
    skipped_by_status: dict[str, int]
    verdicts: list[ComparisonVerdict]
    skipped_metrics: list[SkippedMetric]
    errors: list[str]
    advisories: list[str]


def _empty_report(errors: list[str], result_data: Any = None) -> ComparisonReport:
    commit_sha = ""
    hardware_row_id = ""
    if isinstance(result_data, dict):
        if isinstance(result_data.get("commitSha"), str):
            commit_sha = result_data["commitSha"]
        if isinstance(result_data.get("hardwareRowId"), str):
            hardware_row_id = result_data["hardwareRowId"]
    return ComparisonReport(
        passed=False,
        authoritative=False,
        commit_sha=commit_sha,
        hardware_row_id=hardware_row_id,
        total_metrics=0,
        passed_count=0,
        failed_count=0,
        skipped_pending=0,
        skipped_non_active=0,
        skipped_by_status={},
        verdicts=[],
        skipped_metrics=[],
        errors=errors,
        advisories=[],
    )


def _passes_budget(measured: float, budget: float, direction: str) -> bool:
    if direction == "lower_is_better":
        return measured <= budget
    if direction == "higher_is_better":
        return measured >= budget
    return False


def _margin_percent(measured: float, budget: float,
                    direction: str) -> tuple[float | None, str | None]:
    if budget == 0:
        return None, "undefined because the budget denominator is zero"
    if direction == "lower_is_better":
        return ((budget - measured) / budget) * 100.0, None
    return ((measured - budget) / budget) * 100.0, None


def compare(budget_dir: Path, result_data: Any, *,
            expected_sha: str | None = None) -> ComparisonReport:
    """Compare a result set against a fully validated budget suite."""
    hardware_data, hardware_load_errors = load_bounded_json(
        budget_dir / "hardware.json", "hardware.json",
    )
    budget_data, budget_load_errors = load_bounded_json(
        budget_dir / "budget.json", "budget.json",
    )
    baselines_data, baseline_load_errors = load_bounded_json(
        budget_dir / "baselines.json", "baselines.json",
    )
    errors = hardware_load_errors + budget_load_errors + baseline_load_errors
    if errors:
        return _empty_report(errors, result_data)

    hardware_errors = validate_hardware(hardware_data)
    if hardware_errors:
        return _empty_report(hardware_errors, result_data)
    assert isinstance(hardware_data, dict)
    hardware_rows = {
        row["id"]: row for row in hardware_data["rows"]
        if isinstance(row, dict) and isinstance(row.get("id"), str)
    }
    hardware_ids = frozenset(hardware_rows)

    budget_errors = validate_budget(budget_data, hardware_ids)
    if budget_errors:
        return _empty_report(budget_errors, result_data)
    assert isinstance(budget_data, dict)
    metrics_by_id = {
        metric["id"]: metric for metric in budget_data["metrics"]
        if isinstance(metric, dict) and isinstance(metric.get("id"), str)
    }

    baseline_errors = validate_baselines(
        baselines_data, metrics_by_id, hardware_ids,
    )
    if baseline_errors:
        return _empty_report(baseline_errors, result_data)

    result_errors: list[str] = []
    if expected_sha is None:
        result_errors.append(
            "comparison requires an externally supplied full expected SHA"
        )
    result_errors.extend(validate_result(
        result_data,
        hardware_ids,
        expected_sha=expected_sha,
    ))
    if result_errors:
        return _empty_report(result_errors, result_data)
    assert isinstance(result_data, dict)

    result_hardware = result_data["hardwareRowId"]
    hardware_row = hardware_rows[result_hardware]
    authoritative = hardware_row["certified"] is True
    advisories: list[str] = []
    if not authoritative:
        advisories.append(
            f"result hardware {result_hardware!r} is not certified; "
            "the comparison is advisory and cannot be release evidence"
        )

    verdicts: list[ComparisonVerdict] = []
    skipped_metrics: list[SkippedMetric] = []
    skipped_by_status: dict[str, int] = {}
    measured_metric_ids: set[str] = set()

    for measurement in result_data["measurements"]:
        metric_id = measurement["metricId"]
        measured_metric_ids.add(metric_id)
        metric = metrics_by_id.get(metric_id)
        if metric is None:
            errors.append(f"measurement {metric_id!r} has no matching budget metric")
            continue

        metric_hardware = metric["hardwareRowId"]
        if metric_hardware is not None and metric_hardware != result_hardware:
            errors.append(
                f"hardware mismatch for {metric_id}: metric requires "
                f"{metric_hardware!r} but result measured on {result_hardware!r}"
            )
            continue

        status = metric["status"]
        if status != "active":
            reason_by_status = {
                "pending_measurement": "budget has not been measured or promoted",
                "suspended": "budget enforcement is explicitly suspended",
                "retired": "metric is retired and excluded from enforcement",
            }
            reason = reason_by_status[status]
            skipped_metrics.append(SkippedMetric(metric_id, status, reason))
            skipped_by_status[status] = skipped_by_status.get(status, 0) + 1
            continue

        if measurement["unit"] != metric["unit"]:
            errors.append(
                f"unit mismatch for {metric_id}: result={measurement['unit']!r}, "
                f"budget={metric['unit']!r}"
            )
            continue

        budget_value = metric["budget"]
        measured_value = measurement["value"]
        direction = metric["direction"]
        passed = _passes_budget(measured_value, budget_value, direction)
        margin, margin_reason = _margin_percent(
            measured_value, budget_value, direction,
        )
        verdicts.append(ComparisonVerdict(
            metric_id=metric_id,
            budget_value=budget_value,
            measured_value=measured_value,
            unit=metric["unit"],
            direction=direction,
            passed=passed,
            margin_percent=None if margin is None else round(margin, 2),
            margin_reason=margin_reason,
            hardware_row_id=result_hardware,
            commit_sha=result_data["commitSha"],
        ))

    active_ids_for_current_hardware = {
        metric["id"] for metric in budget_data["metrics"]
        if metric["status"] == "active"
        and (metric["hardwareRowId"] is None
             or metric["hardwareRowId"] == result_hardware)
    }
    for unmeasured in sorted(active_ids_for_current_hardware - measured_metric_ids):
        errors.append(
            f"unmeasured active metric {unmeasured!r} for hardware "
            f"{result_hardware!r}; missing coverage"
        )

    failed_count = sum(1 for verdict in verdicts if not verdict.passed)
    passed = not errors and failed_count == 0
    return ComparisonReport(
        passed=passed,
        authoritative=authoritative,
        commit_sha=result_data["commitSha"],
        hardware_row_id=result_hardware,
        total_metrics=len(result_data["measurements"]),
        passed_count=sum(1 for verdict in verdicts if verdict.passed),
        failed_count=failed_count,
        skipped_pending=skipped_by_status.get("pending_measurement", 0),
        skipped_non_active=len(skipped_metrics),
        skipped_by_status=skipped_by_status,
        verdicts=verdicts,
        skipped_metrics=skipped_metrics,
        errors=errors,
        advisories=advisories,
    )


def report_to_dict(report: ComparisonReport) -> dict[str, Any]:
    return asdict(report)


def _parse_cli(args: list[str]) -> tuple[Path | None, Path | None,
                                        str | None, bool, list[str]]:
    use_json = False
    expected_sha: str | None = None
    positional: list[str] = []
    errors: list[str] = []
    index = 0
    while index < len(args):
        argument = args[index]
        if argument == "--json":
            if use_json:
                errors.append("--json may be supplied only once")
            use_json = True
        elif argument == "--expected-sha":
            if expected_sha is not None:
                errors.append("--expected-sha may be supplied only once")
            elif index + 1 >= len(args):
                errors.append("--expected-sha requires a value")
            else:
                index += 1
                expected_sha = args[index]
        elif argument.startswith("-"):
            errors.append(f"unknown option {argument!r}")
        else:
            positional.append(argument)
        index += 1
    if len(positional) != 2:
        errors.append("expected <budget-dir> and <results.json>")
    if expected_sha is None:
        errors.append("--expected-sha is required")
    budget_dir = Path(positional[0]) if len(positional) >= 1 else None
    result_path = Path(positional[1]) if len(positional) >= 2 else None
    return budget_dir, result_path, expected_sha, use_json, errors


def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    budget_dir, result_path, expected_sha, use_json, parse_errors = _parse_cli(args)
    if parse_errors:
        print(
            "Usage: compare_results.py <budget-dir> <results.json> "
            "--expected-sha <full-sha> [--json]",
            file=sys.stderr,
        )
        for error in parse_errors:
            print(f"Error: {error}", file=sys.stderr)
        return 2
    assert budget_dir is not None and result_path is not None
    if not budget_dir.is_dir():
        print(f"Error: {budget_dir} is not a directory", file=sys.stderr)
        return 2

    result_data, result_load_errors = load_bounded_json(
        result_path, "results.json",
    )
    if result_load_errors:
        for error in result_load_errors:
            print(f"Error: {error}", file=sys.stderr)
        return 2

    report = compare(budget_dir, result_data, expected_sha=expected_sha)
    if use_json:
        print(json.dumps(report_to_dict(report), indent=2, allow_nan=False))
    else:
        qualifier = "" if report.authoritative else " (ADVISORY)"
        status = "PASS" if report.passed else "FAIL"
        print(
            f"{status}{qualifier}: {report.passed_count} passed, "
            f"{report.failed_count} failed, "
            f"{report.skipped_non_active} non-active"
        )
        for verdict in report.verdicts:
            mark = "OK" if verdict.passed else "REGRESSION"
            if verdict.margin_percent is None:
                margin_text = verdict.margin_reason or "undefined"
            else:
                margin_text = f"{verdict.margin_percent:+.1f}%"
            print(
                f"  [{mark}] {verdict.metric_id}: "
                f"{verdict.measured_value}{verdict.unit} vs budget "
                f"{verdict.budget_value}{verdict.unit} (margin {margin_text})"
            )
        for skipped in report.skipped_metrics:
            print(
                f"  [SKIP:{skipped.status}] {skipped.metric_id}: {skipped.reason}"
            )
        for advisory in report.advisories:
            print(f"  [ADVISORY] {advisory}")
        for error in report.errors:
            print(f"  [ERROR] {error}")
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
