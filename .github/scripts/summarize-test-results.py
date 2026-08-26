#!/usr/bin/env python3
"""Validate SparkTests JUnit XML and publish compact CI statistics."""

from __future__ import annotations

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class TestCase:
    name: str
    duration_seconds: float
    failed: bool
    errored: bool
    skipped: bool


def _duration(value: str | None, *, source: Path) -> float:
    try:
        result = float(value or "0")
    except ValueError as exc:
        raise ValueError(f"{source}: invalid testcase duration {value!r}") from exc
    if result < 0:
        raise ValueError(f"{source}: testcase duration cannot be negative")
    return result


def parse_cases(xml_text: str, *, source: Path) -> list[TestCase]:
    try:
        root = ET.fromstring(xml_text)
    except ET.ParseError as exc:
        raise ValueError(f"{source}: cannot parse JUnit XML: {exc}") from exc

    root_tag = root.tag.rsplit("}", 1)[-1]
    if root_tag not in {"testsuite", "testsuites"}:
        raise ValueError(f"{source}: expected testsuite/testsuites root, got {root_tag!r}")

    cases: list[TestCase] = []
    for node in root.iter():
        if node.tag.rsplit("}", 1)[-1] != "testcase":
            continue
        child_tags = {child.tag.rsplit("}", 1)[-1] for child in node}
        cases.append(
            TestCase(
                name=node.attrib.get("name", "<unnamed>"),
                duration_seconds=_duration(node.attrib.get("time"), source=source),
                failed="failure" in child_tags,
                errored="error" in child_tags,
                skipped="skipped" in child_tags,
            )
        )
    return cases


def read_cases(report: Path) -> list[TestCase]:
    if not report.is_file():
        raise ValueError(f"{report}: JUnit report is missing")
    try:
        xml_text = report.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"{report}: cannot read JUnit report: {exc}") from exc
    if not xml_text:
        raise ValueError(f"{report}: JUnit report is empty")
    return parse_cases(xml_text, source=report)


def summarize_cases(cases: list[TestCase], reports: list[Path], minimum_tests: int) -> dict[str, object]:

    if len(cases) < minimum_tests:
        raise ValueError(
            f"JUnit reports contain {len(cases)} test cases; expected at least {minimum_tests}. "
            "The test executable may not have launched or registration may have regressed."
        )

    failures = sum(case.failed for case in cases)
    errors = sum(case.errored for case in cases)
    skipped = sum(case.skipped for case in cases)
    passed = len(cases) - failures - errors - skipped
    slowest = sorted(cases, key=lambda case: case.duration_seconds, reverse=True)[:10]
    return {
        "schemaVersion": 1,
        "reports": [str(report) for report in reports],
        "tests": len(cases),
        "passed": passed,
        "failures": failures,
        "errors": errors,
        "skipped": skipped,
        "durationSeconds": round(sum(case.duration_seconds for case in cases), 6),
        "slowest": [
            {"name": case.name, "durationSeconds": round(case.duration_seconds, 6)}
            for case in slowest
        ],
    }


def summarize(reports: list[Path], minimum_tests: int) -> dict[str, object]:
    cases: list[TestCase] = []
    for report in reports:
        cases.extend(read_cases(report))
    return summarize_cases(cases, reports, minimum_tests)


def markdown(title: str, stats: dict[str, object]) -> str:
    lines = [
        f"### {title}",
        "",
        "| Tests | Passed | Failed | Errors | Skipped | Test time |",
        "|------:|-------:|-------:|-------:|--------:|----------:|",
        (
            f"| {stats['tests']} | {stats['passed']} | {stats['failures']} | "
            f"{stats['errors']} | {stats['skipped']} | {stats['durationSeconds']:.3f}s |"
        ),
        "",
        "<details><summary>Slowest tests</summary>",
        "",
        "| Test | Time |",
        "|------|-----:|",
    ]
    for case in stats["slowest"]:
        safe_name = str(case["name"]).replace("|", "\\|")
        lines.append(f"| `{safe_name}` | {case['durationSeconds']:.3f}s |")
    lines.extend(["", "</details>", ""])
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path, help="JUnit XML report(s)")
    parser.add_argument("--json", dest="json_path", type=Path, help="write machine-readable statistics")
    parser.add_argument("--title", default="SparkTests results", help="job-summary heading")
    parser.add_argument(
        "--min-tests",
        type=int,
        default=1,
        help="fail if fewer test cases were recorded (default: 1)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.min_tests < 1:
        print("error: --min-tests must be at least 1", file=sys.stderr)
        return 2

    try:
        stats = summarize(args.reports, args.min_tests)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")

    report = markdown(args.title, stats)
    print(report)
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8", newline="\n") as stream:
            stream.write(report)

    if stats["failures"] or stats["errors"]:
        print("error: JUnit report contains failed or errored tests", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
