#!/usr/bin/env python3
"""Aggregate per-lane SparkTests statistics into exact-commit CI evidence."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


REQUIRED_STAT_FIELDS = ("tests", "passed", "failures", "errors", "skipped")
REQUIRED_REPO_FIELDS = ("total_lines", "file_count", "test_definitions", "test_files")
REQUIRED_RATCHET_FIELDS = ("minimumRecorded", "minimumExecuted", "maximumSkipped")


def lane_name(path: Path) -> str:
    prefix = "test-stats-"
    if not path.name.startswith(prefix) or path.suffix != ".json":
        raise ValueError(f"{path}: expected a test-stats-*.json file")
    return path.stem[len(prefix) :]


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ValueError(f"{path}: cannot read JSON: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def nonnegative_int(value: Any, *, field: str, source: Path) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{source}: {field} must be a non-negative integer")
    return value


def validate_lane(path: Path, minimum_tests: int) -> tuple[str, dict[str, Any]]:
    name = lane_name(path)
    stats = read_json(path)
    if stats.get("schemaVersion") != 1:
        raise ValueError(f"{path}: unsupported or missing schemaVersion")

    counts = {
        field: nonnegative_int(stats.get(field), field=field, source=path)
        for field in REQUIRED_STAT_FIELDS
    }
    classified = (
        counts["passed"] + counts["failures"] + counts["errors"] + counts["skipped"]
    )
    if classified != counts["tests"]:
        raise ValueError(
            f"{path}: passed + failures + errors + skipped ({classified}) "
            f"does not equal tests ({counts['tests']})"
        )
    executed = counts["tests"] - counts["skipped"]
    if executed < minimum_tests:
        raise ValueError(
            f"{path}: recorded {executed} executed tests and {counts['skipped']} skipped; "
            f"expected at least {minimum_tests} executed tests"
        )
    if counts["failures"] or counts["errors"]:
        raise ValueError(
            f"{path}: contains {counts['failures']} failures and {counts['errors']} errors"
        )

    duration = stats.get("durationSeconds")
    if (
        isinstance(duration, bool)
        or not isinstance(duration, (int, float))
        or duration < 0
    ):
        raise ValueError(f"{path}: durationSeconds must be a non-negative number")

    return name, {**counts, "executed": executed, "durationSeconds": float(duration)}


def validate_repository_metrics(path: Path) -> dict[str, int]:
    metrics = read_json(path)
    validated = {
        field: nonnegative_int(metrics.get(field), field=field, source=path)
        for field in REQUIRED_REPO_FIELDS
    }
    category_fields = (
        "engine_lines",
        "editor_lines",
        "game_lines",
        "services_lines",
        "pipeline_lines",
        "test_lines",
        "tool_lines",
    )
    category_total = sum(
        nonnegative_int(metrics.get(field), field=field, source=path)
        for field in category_fields
    )
    if category_total != validated["total_lines"]:
        raise ValueError(
            f"{path}: category LOC total {category_total} does not equal "
            f"total_lines {validated['total_lines']}"
        )
    return validated


def validate_ratchet(path: Path, expected_lanes: list[str]) -> dict[str, dict[str, int]]:
    value = read_json(path)
    schema_version = value.get("schemaVersion")
    if isinstance(schema_version, bool) or schema_version != 1:
        raise ValueError(f"{path}: unsupported or missing schemaVersion")
    lanes = value.get("lanes")
    if not isinstance(lanes, dict):
        raise ValueError(f"{path}: lanes must be a JSON object")

    expected = set(expected_lanes)
    actual = set(lanes)
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        details: list[str] = []
        if missing:
            details.append(f"missing lane(s): {', '.join(missing)}")
        if unexpected:
            details.append(f"unexpected lane(s): {', '.join(unexpected)}")
        raise ValueError(f"{path}: ratchet lane mismatch ({'; '.join(details)})")

    validated: dict[str, dict[str, int]] = {}
    required_fields = set(REQUIRED_RATCHET_FIELDS)
    for lane in sorted(lanes):
        entry = lanes[lane]
        if not isinstance(entry, dict):
            raise ValueError(f"{path}: ratchet lane {lane!r} must be a JSON object")
        entry_fields = set(entry)
        if entry_fields != required_fields:
            missing_fields = sorted(required_fields - entry_fields)
            unexpected_fields = sorted(entry_fields - required_fields)
            details = []
            if missing_fields:
                details.append(f"missing field(s): {', '.join(missing_fields)}")
            if unexpected_fields:
                details.append(f"unexpected field(s): {', '.join(unexpected_fields)}")
            raise ValueError(f"{path}: ratchet lane {lane!r} has invalid fields ({'; '.join(details)})")
        counts = {
            field: nonnegative_int(entry[field], field=f"lanes.{lane}.{field}", source=path)
            for field in REQUIRED_RATCHET_FIELDS
        }
        if counts["minimumRecorded"] < 1 or counts["minimumExecuted"] < 1:
            raise ValueError(f"{path}: ratchet lane {lane!r} minimums must be at least 1")
        if counts["minimumExecuted"] > counts["minimumRecorded"]:
            raise ValueError(
                f"{path}: ratchet lane {lane!r} minimumExecuted cannot exceed minimumRecorded"
            )
        validated[lane] = counts
    return validated


def enforce_ratchet(
    lanes: dict[str, dict[str, Any]], ratchet: dict[str, dict[str, int]]
) -> None:
    for name in sorted(lanes):
        stats = lanes[name]
        limits = ratchet[name]
        if stats["tests"] < limits["minimumRecorded"]:
            raise ValueError(
                f"lane {name!r}: recorded {stats['tests']} cases; ratchet requires at least "
                f"{limits['minimumRecorded']} recorded cases"
            )
        if stats["executed"] < limits["minimumExecuted"]:
            raise ValueError(
                f"lane {name!r}: executed {stats['executed']} cases; ratchet requires at least "
                f"{limits['minimumExecuted']} executed cases"
            )
        if stats["skipped"] > limits["maximumSkipped"]:
            raise ValueError(
                f"lane {name!r}: skipped {stats['skipped']} cases; ratchet permits at most "
                f"{limits['maximumSkipped']} skipped cases"
            )


def aggregate(
    files: list[Path],
    *,
    expected_lanes: list[str],
    minimum_tests: int,
    repository_metrics: Path,
    commit: str,
    ratchet: dict[str, dict[str, int]] | None = None,
) -> dict[str, Any]:
    expected = set(expected_lanes)
    if len(expected) != len(expected_lanes):
        raise ValueError("expected test statistics lanes must be unique")

    lanes: dict[str, dict[str, Any]] = {}
    for path in sorted(files, key=lambda item: str(item).lower()):
        name = lane_name(path)
        if name not in expected:
            continue
        name, stats = validate_lane(path, minimum_tests)
        if name in lanes:
            raise ValueError(f"duplicate test statistics for lane {name!r}")
        lanes[name] = stats

    missing = sorted(expected - set(lanes))
    if missing:
        raise ValueError(f"missing required test statistics lane(s): {', '.join(missing)}")

    if ratchet is not None:
        enforce_ratchet(lanes, ratchet)

    source = validate_repository_metrics(repository_metrics)
    ordered_lanes: dict[str, dict[str, Any]] = {}
    for name in sorted(lanes):
        stats = dict(lanes[name])
        if ratchet is not None:
            limits = ratchet[name]
            stats["ratchet"] = dict(limits)
            stats["ratchetDelta"] = {
                "recordedAboveMinimum": stats["tests"] - limits["minimumRecorded"],
                "executedAboveMinimum": stats["executed"] - limits["minimumExecuted"],
                "skippedBelowMaximum": limits["maximumSkipped"] - stats["skipped"],
            }
        ordered_lanes[name] = stats
    return {
        "schemaVersion": 1,
        "commit": commit,
        "source": {
            "cppLines": source["total_lines"],
            "sourceFiles": source["file_count"],
            "testDefinitions": source["test_definitions"],
            "testFiles": source["test_files"],
        },
        "testMatrix": {
            "laneCount": len(ordered_lanes),
            "recordedTestCases": sum(item["tests"] for item in ordered_lanes.values()),
            "executedCaseExecutions": sum(
                item["executed"] for item in ordered_lanes.values()
            ),
            "passedExecutions": sum(item["passed"] for item in ordered_lanes.values()),
            "skippedExecutions": sum(item["skipped"] for item in ordered_lanes.values()),
            "durationSeconds": round(
                sum(item["durationSeconds"] for item in ordered_lanes.values()), 6
            ),
            "lanes": ordered_lanes,
        },
    }


def markdown(evidence: dict[str, Any]) -> str:
    source = evidence["source"]
    matrix = evidence["testMatrix"]
    lines = [
        "### Exact-commit test and source statistics",
        "",
        f"Commit: `{evidence['commit'] or '<not supplied>'}`",
        "",
        "| Source metric | Count |",
        "|---|---:|",
        f"| C++ lines | {source['cppLines']:,} |",
        f"| C++ source files | {source['sourceFiles']:,} |",
        f"| Test definitions | {source['testDefinitions']:,} |",
        f"| Test source files | {source['testFiles']:,} |",
        "",
        "| Required lane | Recorded | Recorded floor | Executed | Executed floor | Passed | Skipped | Skipped cap | Time |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name, stats in matrix["lanes"].items():
        ratchet = stats.get("ratchet", {})
        lines.append(
            f"| `{name}` | {stats['tests']:,} | {ratchet.get('minimumRecorded', 'n/a')} | "
            f"{stats['executed']:,} | {ratchet.get('minimumExecuted', 'n/a')} | {stats['passed']:,} | "
            f"{stats['skipped']:,} | {ratchet.get('maximumSkipped', 'n/a')} | "
            f"{stats['durationSeconds']:.3f}s |"
        )
    lines.extend(
        [
            "",
            f"**{matrix['executedCaseExecutions']:,} test-case executions across "
            f"{matrix['laneCount']} lanes; all executed cases passed.**",
            "",
            f"{matrix['recordedTestCases']:,} case records were present, including "
            f"{matrix['skippedExecutions']:,} skipped cases.",
            "Executed test-case counts intentionally include the same definition once per compiler/configuration lane.",
            "",
        ]
    )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", required=True, type=Path)
    parser.add_argument("--repository-metrics", required=True, type=Path)
    parser.add_argument("--ratchet", required=True, type=Path)
    parser.add_argument("--expected-lane", action="append", default=[])
    parser.add_argument("--minimum-tests", type=int, default=1)
    parser.add_argument("--commit", default="")
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def append_summary(content: str) -> None:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8", newline="\n") as stream:
            stream.write(content)


def main() -> int:
    args = parse_args()
    if args.minimum_tests < 1:
        print("error: --minimum-tests must be at least 1", file=sys.stderr)
        return 2
    if not args.expected_lane:
        print("error: at least one --expected-lane is required", file=sys.stderr)
        return 2

    files = list(args.input_root.rglob("test-stats-*.json"))
    try:
        ratchet = validate_ratchet(args.ratchet, args.expected_lane)
        evidence = aggregate(
            files,
            expected_lanes=args.expected_lane,
            minimum_tests=args.minimum_tests,
            repository_metrics=args.repository_metrics,
            commit=args.commit,
            ratchet=ratchet,
        )
    except ValueError as exc:
        error_evidence = {
            "schemaVersion": 1,
            "commit": args.commit,
            "error": str(exc),
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(error_evidence, indent=2) + "\n", encoding="utf-8"
        )
        append_summary(f"### Exact-commit test and source statistics\n\n:x: {exc}\n")
        print(f"error: {exc}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    report = markdown(evidence)
    print(report)
    append_summary(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
