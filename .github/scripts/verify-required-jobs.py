#!/usr/bin/env python3
"""Fail unless every GitHub Actions job supplied through needs succeeded."""

from __future__ import annotations

import json
import os
import sys
from typing import Any


def verify(needs: Any) -> tuple[list[str], list[tuple[str, str]]]:
    passed, _deferred, failed = verify_with_policy(needs)
    return passed, failed


def verify_with_policy(
    needs: Any,
    *,
    deferred_failures: Any = None,
    expected_jobs: Any = None,
) -> tuple[list[str], list[tuple[str, str]], list[tuple[str, str]]]:
    if not isinstance(needs, dict) or not needs:
        raise ValueError("needs JSON must be a non-empty object")
    if deferred_failures is None:
        deferred_failures = {}
    if not isinstance(deferred_failures, dict) or any(
        not isinstance(job, str)
        or not job
        or result != "failure"
        for job, result in deferred_failures.items()
    ):
        raise ValueError("deferred failures must map exact job names to 'failure'")
    if expected_jobs is not None:
        if (
            not isinstance(expected_jobs, list)
            or not expected_jobs
            or any(not isinstance(job, str) or not job for job in expected_jobs)
            or len(set(expected_jobs)) != len(expected_jobs)
        ):
            raise ValueError("expected jobs must be a non-empty unique string array")
        expected = set(expected_jobs)
        actual = set(needs)
        if actual != expected:
            missing = sorted(expected - actual)
            extra = sorted(actual - expected)
            raise ValueError(f"required-job inventory mismatch: missing={missing}, extra={extra}")
    missing_deferred = sorted(set(deferred_failures) - set(needs))
    if missing_deferred:
        raise ValueError(f"deferred jobs are absent from needs: {missing_deferred}")

    passed: list[str] = []
    deferred: list[tuple[str, str]] = []
    failed: list[tuple[str, str]] = []
    for job, metadata in sorted(needs.items()):
        if not isinstance(metadata, dict):
            raise ValueError(f"job {job!r} metadata must be an object")
        result = metadata.get("result")
        if job in deferred_failures and result == deferred_failures[job]:
            deferred.append((job, result))
        elif job in deferred_failures:
            failed.append((job, f"{str(result or 'missing')} (expected failure)"))
        elif result == "success":
            passed.append(job)
        else:
            failed.append((job, str(result or "missing")))
    return passed, deferred, failed


def markdown(
    passed: list[str],
    failed: list[tuple[str, str]],
    deferred: list[tuple[str, str]] | None = None,
) -> str:
    deferred = deferred or []
    lines = ["### Required CI gate", ""]
    if failed:
        lines.append(":x: One or more required jobs did not succeed.")
    elif deferred:
        lines.append(
            f":white_check_mark: {len(passed)} ordinary required jobs succeeded; "
            f"{len(deferred)} exact failure is deferred to a protected external status."
        )
    else:
        lines.append(f":white_check_mark: All {len(passed)} required jobs succeeded.")
    lines.extend(["", "| Job | Result |", "|---|---|"])
    for job in passed:
        lines.append(f"| {job} | success |")
    for job, result in deferred:
        lines.append(f"| {job} | **{result} — deferred to exact external gate** |")
    for job, result in failed:
        lines.append(f"| {job} | **{result}** |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    raw = os.environ.get("NEEDS_JSON", "")
    try:
        needs = json.loads(raw)
        deferred_failures = json.loads(
            os.environ.get("DEFERRED_REQUIRED_FAILURES_JSON", "{}")
        )
        expected_jobs_raw = os.environ.get("EXPECTED_REQUIRED_JOBS_JSON", "")
        expected_jobs = json.loads(expected_jobs_raw) if expected_jobs_raw else None
        passed, deferred, failed = verify_with_policy(
            needs,
            deferred_failures=deferred_failures,
            expected_jobs=expected_jobs,
        )
    except (json.JSONDecodeError, ValueError) as exc:
        print(f"error: invalid required-job evidence: {exc}", file=sys.stderr)
        return 2

    report = markdown(passed, failed, deferred)
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
