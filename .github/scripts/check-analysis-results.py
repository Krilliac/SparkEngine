#!/usr/bin/env python3
"""Fail the required analysis lane on CodeQL findings or incomplete SARIF.

There is deliberately no local alert baseline: existing findings also block.
The read-only scan job supplies fresh SARIF directly, never an uploaded report.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def reject_duplicates(pairs: list[tuple]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key: {key}")
        result[key] = value
    return result


def check_report(report: object) -> int:
    """Validate scanner completion and return the number of blocking findings."""
    if not isinstance(report, dict) or report.get("version") != "2.1.0":
        raise ValueError("expected SARIF 2.1.0")
    runs = report.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("analysis has no runs")
    findings = 0
    for run in runs:
        if not isinstance(run, dict) or run.get("tool", {}).get("driver", {}).get("name") != "CodeQL":
            raise ValueError("expected a CodeQL run")
        invocations = run.get("invocations")
        if not isinstance(invocations, list) or not invocations:
            raise ValueError("missing scanner completion evidence")
        for invocation in invocations:
            if not isinstance(invocation, dict) or invocation.get("executionSuccessful") is not True:
                raise ValueError("scanner did not complete successfully")
            if invocation.get("toolExecutionNotifications") or invocation.get("toolConfigurationNotifications"):
                raise ValueError("scanner emitted execution/configuration notifications; review required")
        results = run.get("results")
        if not isinstance(results, list):
            raise ValueError("missing results array")
        for result in results:
            if not isinstance(result, dict) or not isinstance(result.get("ruleId"), str) or not result["ruleId"]:
                raise ValueError("malformed analysis result")
            # No severity, suppression, baselineState or accepted flag can hide
            # an alert. Query-suite changes need ordinary source review.
            findings += 1
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        if args.report.is_symlink() or not args.report.is_file() or args.report.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("report must be one bounded regular file")
        report = json.loads(args.report.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicates)
        count = check_report(report)
    except (OSError, ValueError, TypeError, AttributeError) as error:
        print(f"analysis: error: {error}", file=sys.stderr)
        return 2
    print(f"analysis: {count} blocking CodeQL findings")
    return 1 if count else 0


if __name__ == "__main__":
    raise SystemExit(main())
