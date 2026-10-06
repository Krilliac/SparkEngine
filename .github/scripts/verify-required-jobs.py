#!/usr/bin/env python3
"""Fail unless every GitHub Actions job supplied through needs succeeded.

With ``--json-out PATH`` the verifier also writes a normalized, exact-SHA
record of the gate decision (schema ``REQUIRED_GATE_RECORD_SCHEMA``). The
record is written for both passing and failing verdicts so a red gate still
publishes machine-readable evidence; it is never written when the needs
evidence or run identity is invalid (exit 2). The exit code is unchanged by
the option: 0 pass, 1 a required job did not succeed, 2 invalid evidence.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

REQUIRED_GATE_RECORD_SCHEMA = "sparkengine.required-ci-gate.v1"
_SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
_POSITIVE_INTEGER_PATTERN = re.compile(r"[1-9][0-9]*")


def _reject_duplicate_json_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON object key: {key!r}")
        value[key] = item
    return value


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


def run_identity(environment: dict[str, str]) -> dict[str, Any]:
    """Return the exact GitHub Actions run identity the gate record is bound to."""

    sha = environment.get("GITHUB_SHA", "")
    if not _SHA_PATTERN.fullmatch(sha):
        raise ValueError("GITHUB_SHA must be a 40-character lowercase hex commit SHA")
    identity: dict[str, Any] = {"sha": sha}
    for key, variable in (("run_id", "GITHUB_RUN_ID"), ("run_attempt", "GITHUB_RUN_ATTEMPT")):
        value = environment.get(variable, "")
        if not _POSITIVE_INTEGER_PATTERN.fullmatch(value):
            raise ValueError(f"{variable} must be a positive integer")
        identity[key] = int(value)
    for key, variable in (
        ("repository", "GITHUB_REPOSITORY"),
        ("event", "GITHUB_EVENT_NAME"),
        ("ref", "GITHUB_REF"),
    ):
        value = environment.get(variable, "")
        if not value or value != value.strip():
            raise ValueError(f"{variable} must be a non-empty string")
        identity[key] = value
    return identity


def gate_record(
    identity: dict[str, Any],
    needs: dict[str, Any],
    expected_jobs: list[str],
    passed: list[str],
    deferred: list[tuple[str, str]],
    failed: list[tuple[str, str]],
) -> dict[str, Any]:
    """Build the normalized gate record from an already-validated verification."""

    status_by_job = {job: "success" for job in passed}
    status_by_job.update({job: "deferred" for job, _result in deferred})
    status_by_job.update({job: "failed" for job, _result in failed})
    jobs = []
    for job in sorted(needs):
        raw_result = needs[job].get("result")
        jobs.append(
            {
                "job": job,
                "result": None if raw_result is None else str(raw_result),
                "status": status_by_job[job],
            }
        )
    return {
        "schema": REQUIRED_GATE_RECORD_SCHEMA,
        **identity,
        "expected_jobs": list(expected_jobs),
        "jobs": jobs,
        "deferred": [{"job": job, "result": result} for job, result in deferred],
        "failed": [{"job": job, "reason": reason} for job, reason in failed],
        "counts": {"success": len(passed), "deferred": len(deferred), "failed": len(failed)},
        "verdict": "fail" if failed else "pass",
    }


def write_record(path: Path, record: dict[str, Any]) -> None:
    """Atomically write the canonical JSON record (sorted keys, LF, trailing newline)."""

    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(record, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--json-out",
        type=Path,
        help="write the normalized exact-SHA gate record to this path",
    )
    arguments = parser.parse_args(argv)
    if arguments.json_out is not None:
        # A stale record from an earlier invocation must never be published
        # as this run's evidence when the current evidence turns out invalid.
        arguments.json_out.unlink(missing_ok=True)

    raw = os.environ.get("NEEDS_JSON", "")
    try:
        needs = json.loads(raw, object_pairs_hook=_reject_duplicate_json_keys)
        deferred_failures = json.loads(
            os.environ.get("DEFERRED_REQUIRED_FAILURES_JSON", "{}"),
            object_pairs_hook=_reject_duplicate_json_keys,
        )
        expected_jobs_raw = os.environ.get("EXPECTED_REQUIRED_JOBS_JSON")
        if not expected_jobs_raw:
            raise ValueError("EXPECTED_REQUIRED_JOBS_JSON is required")
        expected_jobs = json.loads(
            expected_jobs_raw,
            object_pairs_hook=_reject_duplicate_json_keys,
        )
        passed, deferred, failed = verify_with_policy(
            needs,
            deferred_failures=deferred_failures,
            expected_jobs=expected_jobs,
        )
        identity = run_identity(dict(os.environ)) if arguments.json_out is not None else None
    except (json.JSONDecodeError, ValueError) as exc:
        print(f"error: invalid required-job evidence: {exc}", file=sys.stderr)
        return 2

    if identity is not None:
        write_record(
            arguments.json_out,
            gate_record(identity, needs, expected_jobs, passed, deferred, failed),
        )

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
