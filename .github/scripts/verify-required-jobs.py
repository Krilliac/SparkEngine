#!/usr/bin/env python3
"""Fail unless every GitHub Actions job supplied through needs succeeded."""

from __future__ import annotations

import json
import os
import sys
from typing import Any


def verify(needs: Any) -> tuple[list[str], list[tuple[str, str]]]:
    if not isinstance(needs, dict) or not needs:
        raise ValueError("needs JSON must be a non-empty object")
    passed: list[str] = []
    failed: list[tuple[str, str]] = []
    for job, metadata in sorted(needs.items()):
        if not isinstance(metadata, dict):
            raise ValueError(f"job {job!r} metadata must be an object")
        result = metadata.get("result")
        if result == "success":
            passed.append(job)
        else:
            failed.append((job, str(result or "missing")))
    return passed, failed


def markdown(passed: list[str], failed: list[tuple[str, str]]) -> str:
    lines = ["### Required CI gate", ""]
    if failed:
        lines.append(":x: One or more required jobs did not succeed.")
    else:
        lines.append(f":white_check_mark: All {len(passed)} required jobs succeeded.")
    lines.extend(["", "| Job | Result |", "|---|---|"])
    for job in passed:
        lines.append(f"| {job} | success |")
    for job, result in failed:
        lines.append(f"| {job} | **{result}** |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    raw = os.environ.get("NEEDS_JSON", "")
    try:
        needs = json.loads(raw)
        passed, failed = verify(needs)
    except (json.JSONDecodeError, ValueError) as exc:
        print(f"error: invalid required-job evidence: {exc}", file=sys.stderr)
        return 2

    report = markdown(passed, failed)
    print(report)
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8", newline="\n") as stream:
            stream.write(report)
    if failed:
        for job, result in failed:
            print(f"error: required job {job!r} ended as {result!r}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
