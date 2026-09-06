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
    # A waived known-flaky test executed and failed; the failure was tolerated.
    # The runner used to emit it as <skipped>, which made a real tolerated
    # failure indistinguishable from an unavailable environment capability.
    flaky: bool = False
    # A test that registered no assertion at all: it passes because nothing
    # was checked, which is the shape of a check that stopped checking.
    empty: bool = False


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
        properties = {
            prop.attrib.get("name", ""): prop.attrib.get("value", "")
            for child in node
            if child.tag.rsplit("}", 1)[-1] == "properties"
            for prop in child
            if prop.tag.rsplit("}", 1)[-1] == "property"
        }
        cases.append(
            TestCase(
                name=node.attrib.get("name", "<unnamed>"),
                duration_seconds=_duration(node.attrib.get("time"), source=source),
                failed="failure" in child_tags,
                errored="error" in child_tags,
                skipped="skipped" in child_tags,
                flaky="flakyFailure" in child_tags or properties.get("flaky") == "true",
                empty=properties.get("empty") == "true",
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


def merge_duplicates(cases: list[TestCase]) -> tuple[list[TestCase], int]:
    """Collapse a test that ran in more than one lane into one population entry.

    The suite is split across ctest lanes (SparkEngineTests excludes the
    load/stress family, SparkEngineLoadTests runs that file on its own), and a
    handful of cases legitimately appear in both reports. Counting them twice
    would inflate the executed total against the registered-case floor, which is
    the one comparison that is supposed to notice a family disappearing.
    A case is skipped only if every lane skipped it; any lane's failure, waiver,
    or empty run is kept.
    """

    merged: dict[str, TestCase] = {}
    duplicates = 0
    for case in cases:
        existing = merged.get(case.name)
        if existing is None:
            merged[case.name] = case
            continue
        duplicates += 1
        merged[case.name] = TestCase(
            name=case.name,
            duration_seconds=existing.duration_seconds + case.duration_seconds,
            failed=existing.failed or case.failed,
            errored=existing.errored or case.errored,
            skipped=existing.skipped and case.skipped,
            flaky=existing.flaky or case.flaky,
            empty=existing.empty or case.empty,
        )
    return list(merged.values()), duplicates


def check_registration_floor(
    executed: int, skipped: int, registration_count: Path
) -> dict[str, object]:
    """Compare the executed population against the count CMake derived.

    A hand-written --min-tests literal cannot notice that a whole test family
    stopped being compiled in: the floor was written before the family vanished
    and the run stays green underneath it. testMacrosUnconditional is derived
    from the sources this build actually compiled, so it moves with the code.
    """

    try:
        payload = json.loads(registration_count.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ValueError(f"{registration_count}: cannot read registration count: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"{registration_count}: invalid registration count JSON: {exc}") from exc
    if not isinstance(payload, dict):
        raise ValueError(f"{registration_count}: expected a JSON object")
    unconditional = payload.get("testMacrosUnconditional")
    if isinstance(unconditional, bool) or not isinstance(unconditional, int) or unconditional < 1:
        raise ValueError(
            f"{registration_count}: testMacrosUnconditional must be a positive integer; "
            "an absent or zero registered count is not a passing floor"
        )
    floor = unconditional - skipped
    if executed < floor:
        raise ValueError(
            f"JUnit reports executed {executed} test cases with {skipped} skipped, but "
            f"{unconditional} TEST macros are unconditionally compiled into this build "
            f"({registration_count}). At least {floor} cases must run: a test family "
            "stopped executing, or a lane's report was left out of this summary."
        )
    return {
        "registeredUnconditional": unconditional,
        "registeredTotal": payload.get("testMacrosTotal"),
        "executedAboveRegisteredFloor": executed - floor,
    }


def summarize_cases(
    cases: list[TestCase],
    reports: list[Path],
    minimum_tests: int,
    registration_count: Path | None = None,
) -> dict[str, object]:
    cases, duplicates = merge_duplicates(cases)
    skipped = sum(case.skipped for case in cases)
    # `skipped` now means only a genuine SKIP_TEST/environment skip: a waived
    # known-flaky test is counted as executed, and separately as flaky.
    executed = len(cases) - skipped
    if executed < minimum_tests:
        raise ValueError(
            f"JUnit reports contain {executed} executed test cases and {skipped} skipped; "
            f"expected at least {minimum_tests} executed tests. "
            "The test executable may not have launched or registration may have regressed."
        )

    failures = sum(case.failed for case in cases)
    errors = sum(case.errored for case in cases)
    flaky = sum(case.flaky for case in cases)
    empty = sum(case.empty for case in cases)
    # A flaky case is a tolerated failure, not a pass; counting it as passed is
    # exactly the blend the JUnit shape was changed to stop.
    passed = len(cases) - failures - errors - skipped - flaky
    slowest = sorted(cases, key=lambda case: case.duration_seconds, reverse=True)[:10]
    registration: dict[str, object] = {}
    if registration_count is not None:
        registration = check_registration_floor(executed, skipped, registration_count)
    return {
        "schemaVersion": 1,
        "reports": [str(report) for report in reports],
        "duplicateCases": duplicates,
        **registration,
        "tests": len(cases),
        "executed": executed,
        "passed": passed,
        "failures": failures,
        "errors": errors,
        "skipped": skipped,
        "flaky": flaky,
        "empty": empty,
        "durationSeconds": round(sum(case.duration_seconds for case in cases), 6),
        "slowest": [
            {"name": case.name, "durationSeconds": round(case.duration_seconds, 6)}
            for case in slowest
        ],
    }


def summarize(
    reports: list[Path], minimum_tests: int, registration_count: Path | None = None
) -> dict[str, object]:
    cases: list[TestCase] = []
    for report in reports:
        cases.extend(read_cases(report))
    return summarize_cases(cases, reports, minimum_tests, registration_count)


def markdown(title: str, stats: dict[str, object]) -> str:
    lines = [
        f"### {title}",
        "",
        "| Tests | Executed | Passed | Failed | Errors | Flaky | Empty | Skipped | Test time |",
        "|------:|---------:|-------:|-------:|-------:|------:|------:|--------:|----------:|",
        (
            f"| {stats['tests']} | {stats['executed']} | {stats['passed']} | {stats['failures']} | "
            f"{stats['errors']} | {stats['flaky']} | {stats['empty']} | {stats['skipped']} | "
            f"{stats['durationSeconds']:.3f}s |"
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
    parser.add_argument(
        "--registration-count",
        type=Path,
        help=(
            "test-registration-count.json from the build; fails when the executed "
            "population drops below the unconditionally registered TEST count minus "
            "skipped, which a hand-written --min-tests literal cannot detect"
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.min_tests < 1:
        print("error: --min-tests must be at least 1", file=sys.stderr)
        return 2

    try:
        stats = summarize(args.reports, args.min_tests, args.registration_count)
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
