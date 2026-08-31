#!/usr/bin/env python3

from __future__ import annotations

import copy
import importlib.util
import sys
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlparse


SCRIPT = Path(__file__).with_name("verify-exact-required-gate.py")
SPEC = importlib.util.spec_from_file_location("verify_exact_required_gate", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

SHA = "a" * 40
OTHER_SHA = "b" * 40
REPOSITORY = "Krilliac/SparkEngine"
REPOSITORY_ID = 101
SOURCE_RUN_ID = 42
VERIFIER_RUN_ID = 84
DIGEST = "sha256:" + "c" * 64
CODEQL_SOURCE_RUN_ID = 43
CODEQL_REPORTER_RUN_ID = 85
CODEQL_DIGEST = "sha256:" + "d" * 64
CI120_SOURCE_DIGEST = "sha256:" + "e" * 64
CODEQL_SOURCE_DIGESTS = {
    "actions": "sha256:" + "f" * 64,
    "c-cpp": "sha256:" + "1" * 64,
    "python": "sha256:" + "2" * 64,
}
BUILD_RUN_STARTED_AT = "2026-08-30T04:00:00Z"
BUILD_RUN_UPDATED_AT = "2026-08-30T04:30:00Z"
CODEQL_RUN_STARTED_AT = "2026-08-30T03:00:00Z"
CODEQL_RUN_UPDATED_AT = "2026-08-30T03:30:00Z"
VERIFIER_RUN_STARTED_AT = "2026-08-30T04:31:00Z"
VERIFIER_RUN_UPDATED_AT = "2026-08-30T05:00:00Z"
REPORTER_RUN_STARTED_AT = "2026-08-30T05:01:00Z"
REPORTER_RUN_UPDATED_AT = "2026-08-30T05:05:00Z"
CI120_STATUS_AT = "2026-08-30T04:58:30Z"
CODEQL_STATUS_AT = "2026-08-30T05:03:30Z"


def repository():
    return {"id": REPOSITORY_ID, "full_name": REPOSITORY, "default_branch": "Working"}


def source_run(run_id=SOURCE_RUN_ID, **overrides):
    value = {
        "id": run_id,
        "workflow_id": 202,
        "run_number": 10,
        "run_attempt": 1,
        "name": "Build SparkEngine",
        "path": ".github/workflows/build.yml@refs/heads/Working",
        "head_sha": SHA,
        "head_branch": "Working",
        "event": "push",
        "status": "completed",
        "conclusion": "failure",
        "run_started_at": BUILD_RUN_STARTED_AT,
        "updated_at": BUILD_RUN_UPDATED_AT,
        "html_url": f"https://github.com/{REPOSITORY}/actions/runs/{run_id}",
        "repository": repository(),
        "head_repository": repository(),
    }
    value.update(overrides)
    return value


def completed_step(name, conclusion="success", **overrides):
    value = {"name": name, "status": "completed", "conclusion": conclusion}
    value.update(overrides)
    return value


def completed_steps_with_timed_terminal(
    names,
    upload_started_at,
    upload_completed_at,
    publish_started_at,
    publish_completed_at,
):
    steps = [
        completed_step(
            name,
            number=index,
            started_at=upload_started_at,
            completed_at=upload_started_at,
        )
        for index, name in enumerate(names, start=1)
    ]
    steps[-2].update(
        {"started_at": upload_started_at, "completed_at": upload_completed_at}
    )
    steps[-1].update(
        {"started_at": publish_started_at, "completed_at": publish_completed_at}
    )
    return steps


def source_job(**overrides):
    value = {
        "id": 501,
        "run_id": SOURCE_RUN_ID,
        "run_attempt": 1,
        "head_sha": SHA,
        "workflow_name": "Build SparkEngine",
        "head_branch": "Working",
        "run_url": f"https://api.github.com/repos/{REPOSITORY}/actions/runs/{SOURCE_RUN_ID}",
        "name": "Windows Shipping structural configured-evidence producer",
        "status": "completed",
        "conclusion": "failure",
        "started_at": "2026-08-30T04:01:00Z",
        "completed_at": "2026-08-30T04:20:00Z",
        "steps": [
            completed_step(
                name,
                number=index,
                started_at="2026-08-30T04:01:00Z",
                completed_at="2026-08-30T04:18:00Z",
            )
            for index, name in enumerate(MODULE.SOURCE_REQUIRED_STEPS, start=1)
        ]
        + [
            completed_step(
                MODULE.SOURCE_FINAL_STEP,
                "failure",
                number=len(MODULE.SOURCE_REQUIRED_STEPS) + 1,
                started_at="2026-08-30T04:19:00Z",
                completed_at="2026-08-30T04:20:00Z",
            )
        ],
    }
    value.update(overrides)
    return value


def required_gate(**overrides):
    value = {
        "id": 502,
        "run_id": SOURCE_RUN_ID,
        "run_attempt": 1,
        "head_sha": SHA,
        "workflow_name": "Build SparkEngine",
        "head_branch": "Working",
        "run_url": f"https://api.github.com/repos/{REPOSITORY}/actions/runs/{SOURCE_RUN_ID}",
        "name": "Required CI Gate",
        "status": "completed",
        "conclusion": "success",
        "started_at": "2026-08-30T04:21:00Z",
        "completed_at": "2026-08-30T04:22:00Z",
        "steps": [
            completed_step(
                "Aggregate exact required jobs",
                number=1,
                started_at="2026-08-30T04:21:00Z",
                completed_at="2026-08-30T04:22:00Z",
            )
        ],
    }
    value.update(overrides)
    return value


def ordinary_job(**overrides):
    value = {
        "id": 503,
        "run_id": SOURCE_RUN_ID,
        "run_attempt": 1,
        "head_sha": SHA,
        "workflow_name": "Build SparkEngine",
        "head_branch": "Working",
        "run_url": f"https://api.github.com/repos/{REPOSITORY}/actions/runs/{SOURCE_RUN_ID}",
        "name": "ordinary required job",
        "status": "completed",
        "conclusion": "success",
        "started_at": "2026-08-30T04:02:00Z",
        "completed_at": "2026-08-30T04:10:00Z",
        "steps": [
            completed_step(
                "ordinary required step",
                number=1,
                started_at="2026-08-30T04:02:00Z",
                completed_at="2026-08-30T04:10:00Z",
            )
        ],
    }
    value.update(overrides)
    return value


def trusted_status(**overrides):
    value = {
        "id": 601,
        "context": "CI-120 Trusted / Exact Source",
        "state": "success",
        "description": f"Trusted CI-120 verified for Build run {SOURCE_RUN_ID}, attempt 1.",
        "url": f"https://api.github.com/repos/{REPOSITORY}/statuses/{SHA}",
        "target_url": f"https://github.com/{REPOSITORY}/actions/runs/{VERIFIER_RUN_ID}/attempts/2",
        "created_at": CI120_STATUS_AT,
        "updated_at": CI120_STATUS_AT,
        "creator": {"id": 41898282, "login": "github-actions[bot]", "type": "Bot"},
    }
    value.update(overrides)
    return value


def verifier_run(**overrides):
    value = {
        "id": VERIFIER_RUN_ID,
        "workflow_id": 303,
        "run_number": 20,
        "run_attempt": 2,
        "name": "CI-120 Trusted Verifier",
        "path": ".github/workflows/ci120-report.yml@refs/heads/Working",
        "head_sha": SHA,
        "head_branch": "Working",
        "event": "workflow_run",
        "status": "completed",
        "conclusion": "success",
        "html_url": f"https://github.com/{REPOSITORY}/actions/runs/{VERIFIER_RUN_ID}",
        "run_started_at": VERIFIER_RUN_STARTED_AT,
        "updated_at": VERIFIER_RUN_UPDATED_AT,
        "repository": repository(),
        "head_repository": repository(),
    }
    value.update(overrides)
    return value


def verifier_job(**overrides):
    value = {
        "id": 701,
        "run_id": VERIFIER_RUN_ID,
        "run_attempt": 2,
        "head_sha": SHA,
        "workflow_name": "CI-120 Trusted Verifier",
        "head_branch": "Working",
        "run_url": f"https://api.github.com/repos/{REPOSITORY}/actions/runs/{VERIFIER_RUN_ID}",
        "name": "Verify and attest CI-120 evidence",
        "status": "completed",
        "conclusion": "success",
        "started_at": "2026-08-30T04:32:00Z",
        "completed_at": "2026-08-30T04:59:00Z",
        "steps": completed_steps_with_timed_terminal(
            MODULE.VERIFIER_REQUIRED_STEPS,
            "2026-08-30T04:57:00.500Z",
            "2026-08-30T04:57:59.500Z",
            "2026-08-30T04:58:00.500Z",
            "2026-08-30T04:58:59.250Z",
        ),
    }
    value.update(overrides)
    return value


def receipt_artifact(**overrides):
    artifact_id = overrides.pop("id", 801)
    value = {
        "id": artifact_id,
        "node_id": "ARTIFACT_CI120_RECEIPT",
        "name": f"ci120-trusted-receipt-{SHA}-{SOURCE_RUN_ID}-1-2",
        "expired": False,
        "size_in_bytes": 4096,
        "url": f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}",
        "archive_download_url": (
            f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip"
        ),
        "created_at": "2026-08-30T04:57:30Z",
        "updated_at": "2026-08-30T04:57:59Z",
        "expires_at": "2026-09-29T04:57:59Z",
        "digest": DIGEST,
        "workflow_run": {
            "id": VERIFIER_RUN_ID,
            "repository_id": REPOSITORY_ID,
            "head_repository_id": REPOSITORY_ID,
            "head_branch": "Working",
            "head_sha": SHA,
        },
    }
    value.update(overrides)
    return value


def ci120_source_artifact(**overrides):
    artifact_id = overrides.pop("id", 803)
    value = {
        "id": artifact_id,
        "node_id": "ARTIFACT_CI120_SOURCE",
        "name": f"ci120-untrusted-stable-v1-{SHA}-1",
        "size_in_bytes": 16384,
        "url": f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}",
        "archive_download_url": (
            f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip"
        ),
        "expired": False,
        "created_at": "2026-08-30T04:15:00Z",
        "updated_at": "2026-08-30T04:16:00Z",
        "expires_at": "2026-09-13T04:16:00Z",
        "digest": CI120_SOURCE_DIGEST,
        "workflow_run": {
            "id": SOURCE_RUN_ID,
            "repository_id": REPOSITORY_ID,
            "head_repository_id": REPOSITORY_ID,
            "head_branch": "Working",
            "head_sha": SHA,
        },
    }
    value.update(overrides)
    return value


def codeql_source_run(**overrides):
    value = {
        "id": CODEQL_SOURCE_RUN_ID,
        "workflow_id": 404,
        "run_number": 12,
        "run_attempt": 1,
        "name": "CodeQL Advanced",
        "path": ".github/workflows/codeql.yml@refs/heads/Working",
        "head_sha": SHA,
        "head_branch": "Working",
        "event": "push",
        "status": "completed",
        "conclusion": "success",
        "run_started_at": CODEQL_RUN_STARTED_AT,
        "updated_at": CODEQL_RUN_UPDATED_AT,
        "html_url": f"https://github.com/{REPOSITORY}/actions/runs/{CODEQL_SOURCE_RUN_ID}",
        "repository": repository(),
        "head_repository": repository(),
    }
    value.update(overrides)
    return value


def codeql_source_job(language="actions", index=0, **overrides):
    value = {
        "id": 900 + index,
        "run_id": CODEQL_SOURCE_RUN_ID,
        "run_attempt": 1,
        "head_sha": SHA,
        "workflow_name": "CodeQL Advanced",
        "head_branch": "Working",
        "run_url": f"https://api.github.com/repos/{REPOSITORY}/actions/runs/{CODEQL_SOURCE_RUN_ID}",
        "name": f"Analyze ({language})",
        "status": "completed",
        "conclusion": "success",
        "started_at": "2026-08-30T03:01:00Z",
        "completed_at": "2026-08-30T03:20:00Z",
        "steps": [
            completed_step(
                name,
                number=number,
                started_at="2026-08-30T03:01:00Z",
                completed_at="2026-08-30T03:19:00Z",
            )
            for number, name in enumerate(MODULE.CODEQL_SOURCE_REQUIRED_STEPS, start=1)
        ],
    }
    value.update(overrides)
    return value


def codeql_status(**overrides):
    value = {
        "id": 602,
        "context": "CodeQL Trusted / Exact Source",
        "state": "success",
        "description": f"Trusted CodeQL verified for CodeQL run {CODEQL_SOURCE_RUN_ID}, attempt 1.",
        "url": f"https://api.github.com/repos/{REPOSITORY}/statuses/{SHA}",
        "target_url": f"https://github.com/{REPOSITORY}/actions/runs/{CODEQL_REPORTER_RUN_ID}/attempts/3",
        "created_at": CODEQL_STATUS_AT,
        "updated_at": CODEQL_STATUS_AT,
        "creator": {"id": 41898282, "login": "github-actions[bot]", "type": "Bot"},
    }
    value.update(overrides)
    return value


def codeql_source_artifact(language="actions", index=0, **overrides):
    artifact_id = overrides.pop("id", 810 + index)
    value = {
        "id": artifact_id,
        "node_id": f"ARTIFACT_CODEQL_{language}",
        "name": f"codeql-{language}-attempt-1.sarif",
        "size_in_bytes": 4096 + index,
        "url": f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}",
        "archive_download_url": (
            f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip"
        ),
        "expired": False,
        "created_at": "2026-08-30T03:15:00Z",
        "updated_at": "2026-08-30T03:16:00Z",
        "expires_at": "2026-09-06T03:16:00Z",
        "digest": CODEQL_SOURCE_DIGESTS[language],
        "workflow_run": {
            "id": CODEQL_SOURCE_RUN_ID,
            "repository_id": REPOSITORY_ID,
            "head_repository_id": REPOSITORY_ID,
            "head_branch": "Working",
            "head_sha": SHA,
        },
    }
    value.update(overrides)
    return value


def codeql_reporter_run(**overrides):
    value = {
        "id": CODEQL_REPORTER_RUN_ID,
        "workflow_id": 505,
        "run_number": 21,
        "run_attempt": 3,
        "name": "CodeQL Trusted Reporter",
        "path": ".github/workflows/codeql-report.yml@refs/heads/Working",
        "head_sha": SHA,
        "head_branch": "Working",
        "event": "workflow_run",
        "status": "completed",
        "conclusion": "success",
        "html_url": f"https://github.com/{REPOSITORY}/actions/runs/{CODEQL_REPORTER_RUN_ID}",
        "run_started_at": REPORTER_RUN_STARTED_AT,
        "updated_at": REPORTER_RUN_UPDATED_AT,
        "repository": repository(),
        "head_repository": repository(),
    }
    value.update(overrides)
    return value


def codeql_reporter_job(**overrides):
    value = {
        "id": 950,
        "run_id": CODEQL_REPORTER_RUN_ID,
        "run_attempt": 3,
        "head_sha": SHA,
        "workflow_name": "CodeQL Trusted Reporter",
        "head_branch": "Working",
        "run_url": f"https://api.github.com/repos/{REPOSITORY}/actions/runs/{CODEQL_REPORTER_RUN_ID}",
        "name": "Validate and report CodeQL evidence",
        "status": "completed",
        "conclusion": "success",
        "started_at": "2026-08-30T05:01:10Z",
        "completed_at": "2026-08-30T05:04:00Z",
        "steps": completed_steps_with_timed_terminal(
            MODULE.CODEQL_REPORTER_REQUIRED_STEPS,
            "2026-08-30T05:02:00.500Z",
            "2026-08-30T05:02:59.500Z",
            "2026-08-30T05:03:00.500Z",
            "2026-08-30T05:03:59.250Z",
        ),
    }
    value.update(overrides)
    return value


def codeql_summary_artifact(**overrides):
    artifact_id = overrides.pop("id", 802)
    value = {
        "id": artifact_id,
        "node_id": "ARTIFACT_CODEQL_SUMMARY",
        "name": f"codeql-trusted-summary-{SHA}-{CODEQL_SOURCE_RUN_ID}-1-3",
        "expired": False,
        "size_in_bytes": 8192,
        "url": f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}",
        "archive_download_url": (
            f"https://api.github.com/repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip"
        ),
        "created_at": "2026-08-30T05:02:30Z",
        "updated_at": "2026-08-30T05:02:59Z",
        "expires_at": "2026-09-29T05:02:59Z",
        "digest": CODEQL_DIGEST,
        "workflow_run": {
            "id": CODEQL_REPORTER_RUN_ID,
            "repository_id": REPOSITORY_ID,
            "head_repository_id": REPOSITORY_ID,
            "head_branch": "Working",
            "head_sha": SHA,
        },
    }
    value.update(overrides)
    return value


class FakeApi:
    def __init__(self):
        self.repository = repository()
        self.commit = {"sha": SHA}
        self.default_sha = SHA
        self.runs_responses = [[source_run()]]
        self.source_live = source_run()
        self.source_jobs = [source_job(), required_gate(), ordinary_job()]
        self.source_job_responses = None
        self.source_artifact_responses = [[ci120_source_artifact()]]
        self.codeql_runs_responses = [[codeql_source_run()]]
        self.codeql_source_live = codeql_source_run()
        self.codeql_source_jobs = [
            codeql_source_job("actions", 0),
            codeql_source_job("c-cpp", 1),
            codeql_source_job("python", 2),
        ]
        self.codeql_source_job_responses = None
        self.codeql_source_artifact_responses = [[
            codeql_source_artifact("actions", 0),
            codeql_source_artifact("c-cpp", 1),
            codeql_source_artifact("python", 2),
        ]]
        self.status_responses = [[trusted_status(), codeql_status()]]
        self.verifier_responses = [verifier_run()]
        self.verifier_jobs = [verifier_job()]
        self.verifier_job_responses = None
        self.artifact_responses = [[receipt_artifact()]]
        self.codeql_reporter_responses = [codeql_reporter_run()]
        self.codeql_reporter_jobs = [codeql_reporter_job()]
        self.codeql_reporter_job_responses = None
        self.codeql_artifact_responses = [[codeql_summary_artifact()]]
        self.run_calls = 0
        self.source_job_calls = 0
        self.source_artifact_calls = 0
        self.codeql_run_calls = 0
        self.codeql_source_job_calls = 0
        self.codeql_source_artifact_calls = 0
        self.status_calls = 0
        self.verifier_calls = 0
        self.verifier_job_calls = 0
        self.artifact_calls = 0
        self.codeql_reporter_calls = 0
        self.codeql_reporter_job_calls = 0
        self.codeql_artifact_calls = 0
        self.run_paths = []
        self.run_page_overrides = {}

    @staticmethod
    def _next(values, index):
        return copy.deepcopy(values[min(index, len(values) - 1)])

    def __call__(self, path):
        if path == f"/repos/{REPOSITORY}":
            return copy.deepcopy(self.repository)
        if path == f"/repos/{REPOSITORY}/commits/{SHA}":
            return copy.deepcopy(self.commit)
        if path == f"/repos/{REPOSITORY}/commits/Working":
            return {"sha": self.default_sha}
        if "/actions/workflows/build.yml/runs?" in path:
            self.run_paths.append(path)
            query = parse_qs(urlparse(path).query)
            page = int(query.get("page", ["0"])[0])
            per_page = int(query.get("per_page", ["0"])[0])
            if page < 1 or per_page != MODULE.MAX_API_ITEMS:
                raise AssertionError(f"unexpected Build pagination request: {path}")
            result = self._next(self.runs_responses, self.run_calls)
            start = (page - 1) * per_page
            page_runs = result[start : start + per_page]
            override = self.run_page_overrides.get((self.run_calls, page))
            if override is not None:
                return copy.deepcopy(override)
            if start + len(page_runs) >= len(result):
                self.run_calls += 1
            return {"total_count": len(result), "workflow_runs": page_runs}
        if "/actions/workflows/codeql.yml/runs?" in path:
            query = parse_qs(urlparse(path).query)
            page = int(query.get("page", ["0"])[0])
            per_page = int(query.get("per_page", ["0"])[0])
            if page < 1 or per_page != MODULE.MAX_API_ITEMS:
                raise AssertionError(f"unexpected CodeQL pagination request: {path}")
            result = self._next(self.codeql_runs_responses, self.codeql_run_calls)
            start = (page - 1) * per_page
            page_runs = result[start : start + per_page]
            if start + len(page_runs) >= len(result):
                self.codeql_run_calls += 1
            return {"total_count": len(result), "workflow_runs": page_runs}
        if path == f"/repos/{REPOSITORY}/actions/runs/{SOURCE_RUN_ID}":
            return copy.deepcopy(self.source_live)
        if f"/actions/runs/{SOURCE_RUN_ID}/attempts/1/jobs?" in path:
            jobs = (
                self._next(self.source_job_responses, self.source_job_calls)
                if self.source_job_responses is not None
                else copy.deepcopy(self.source_jobs)
            )
            self.source_job_calls += 1
            return {"total_count": len(jobs), "jobs": jobs}
        if f"/actions/runs/{SOURCE_RUN_ID}/artifacts?" in path:
            artifacts = self._next(self.source_artifact_responses, self.source_artifact_calls)
            self.source_artifact_calls += 1
            return {"total_count": len(artifacts), "artifacts": artifacts}
        if path == f"/repos/{REPOSITORY}/actions/runs/{CODEQL_SOURCE_RUN_ID}":
            return copy.deepcopy(self.codeql_source_live)
        if f"/actions/runs/{CODEQL_SOURCE_RUN_ID}/attempts/1/jobs?" in path:
            jobs = (
                self._next(self.codeql_source_job_responses, self.codeql_source_job_calls)
                if self.codeql_source_job_responses is not None
                else copy.deepcopy(self.codeql_source_jobs)
            )
            self.codeql_source_job_calls += 1
            return {
                "total_count": len(jobs),
                "jobs": jobs,
            }
        if f"/actions/runs/{CODEQL_SOURCE_RUN_ID}/artifacts?" in path:
            artifacts = self._next(
                self.codeql_source_artifact_responses, self.codeql_source_artifact_calls
            )
            self.codeql_source_artifact_calls += 1
            return {"total_count": len(artifacts), "artifacts": artifacts}
        if path.startswith(f"/repos/{REPOSITORY}/commits/{SHA}/status?"):
            statuses = self._next(self.status_responses, self.status_calls)
            self.status_calls += 1
            return {
                "sha": SHA,
                "repository": repository(),
                "total_count": len(statuses),
                "statuses": statuses,
            }
        if path == f"/repos/{REPOSITORY}/actions/runs/{VERIFIER_RUN_ID}":
            result = self._next(self.verifier_responses, self.verifier_calls)
            self.verifier_calls += 1
            return result
        if f"/actions/runs/{VERIFIER_RUN_ID}/attempts/2/jobs?" in path:
            jobs = (
                self._next(self.verifier_job_responses, self.verifier_job_calls)
                if self.verifier_job_responses is not None
                else copy.deepcopy(self.verifier_jobs)
            )
            self.verifier_job_calls += 1
            return {"total_count": len(jobs), "jobs": jobs}
        if f"/actions/runs/{VERIFIER_RUN_ID}/artifacts?" in path:
            artifacts = self._next(self.artifact_responses, self.artifact_calls)
            self.artifact_calls += 1
            return {"total_count": len(artifacts), "artifacts": artifacts}
        if path == f"/repos/{REPOSITORY}/actions/runs/{CODEQL_REPORTER_RUN_ID}":
            result = self._next(self.codeql_reporter_responses, self.codeql_reporter_calls)
            self.codeql_reporter_calls += 1
            return result
        if f"/actions/runs/{CODEQL_REPORTER_RUN_ID}/attempts/3/jobs?" in path:
            jobs = (
                self._next(
                    self.codeql_reporter_job_responses, self.codeql_reporter_job_calls
                )
                if self.codeql_reporter_job_responses is not None
                else copy.deepcopy(self.codeql_reporter_jobs)
            )
            self.codeql_reporter_job_calls += 1
            return {
                "total_count": len(jobs),
                "jobs": jobs,
            }
        if f"/actions/runs/{CODEQL_REPORTER_RUN_ID}/artifacts?" in path:
            artifacts = self._next(
                self.codeql_artifact_responses, self.codeql_artifact_calls
            )
            self.codeql_artifact_calls += 1
            return {"total_count": len(artifacts), "artifacts": artifacts}
        raise AssertionError(f"unexpected API path: {path}")


class VerifyExactRequiredGateTests(unittest.TestCase):
    def test_staged_build_only_accepts_exact_reviewed_failure(self):
        evidence = MODULE.verify_exact_staged_build(
            FakeApi(), REPOSITORY, SHA.upper(), SOURCE_RUN_ID, 1
        )
        self.assertEqual(evidence.run_id, SOURCE_RUN_ID)
        self.assertEqual(evidence.run_attempt, 1)
        self.assertEqual(evidence.event, "push")

    def test_staged_build_only_rejects_attempt_mismatch(self):
        with self.assertRaisesRegex(ValueError, "id or attempt"):
            MODULE.verify_exact_staged_build(
                FakeApi(), REPOSITORY, SHA, SOURCE_RUN_ID, 2
            )

    def test_staged_build_only_rejects_newer_same_sha_run(self):
        api = FakeApi()
        newer = source_run(43, run_number=11, status="in_progress", conclusion=None)
        api.runs_responses = [[source_run(), newer]]
        with self.assertRaisesRegex(ValueError, "newest exact Build attempt"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_accepts_latest_source_on_second_bounded_page(self):
        api = FakeApi()
        scheduled = [
            source_run(1000 + index, run_number=1000 + index, event="schedule")
            for index in range(149)
        ]
        api.runs_responses = [scheduled + [source_run()]]
        evidence = MODULE.verify_exact_staged_build(
            api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
        )
        self.assertEqual(evidence.run_id, SOURCE_RUN_ID)
        self.assertEqual(len(api.run_paths), 4)
        self.assertTrue(all("page=1" in path or "page=2" in path for path in api.run_paths))

    def test_staged_build_only_rejects_newer_source_on_second_page(self):
        api = FakeApi()
        scheduled = [
            source_run(1000 + index, run_number=1000 + index, event="schedule")
            for index in range(149)
        ]
        newer = source_run(43, run_number=11, status="in_progress", conclusion=None)
        api.runs_responses = [scheduled + [source_run(), newer]]
        with self.assertRaisesRegex(ValueError, "newest exact Build attempt"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_duplicate_source_identity_across_pages(self):
        api = FakeApi()
        scheduled = [
            source_run(1000 + index, run_number=1000 + index, event="schedule")
            for index in range(100)
        ]
        api.runs_responses = [scheduled + [copy.deepcopy(scheduled[0]), source_run()]]
        with self.assertRaisesRegex(ValueError, "duplicate source identity"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_reused_run_id_with_changed_attempt(self):
        api = FakeApi()
        reused = source_run(run_attempt=2, event="schedule")
        api.runs_responses = [[source_run(), reused]]
        with self.assertRaisesRegex(ValueError, "duplicate source identity"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_reused_execution_key_with_changed_id(self):
        api = FakeApi()
        reused = source_run(SOURCE_RUN_ID + 1, event="schedule")
        api.runs_responses = [[source_run(), reused]]
        with self.assertRaisesRegex(ValueError, "duplicate source identity"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_more_than_two_hundred_source_runs(self):
        api = FakeApi()
        api.runs_responses = [[
            source_run(1000 + index, run_number=1000 + index, event="schedule")
            for index in range(MODULE.MAX_SOURCE_RUNS + 1)
        ]]
        with self.assertRaisesRegex(ValueError, "exceeds 200"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )
        self.assertEqual(len(api.run_paths), 1)

    def test_staged_build_only_rejects_total_count_drift_between_pages(self):
        api = FakeApi()
        inventory = [
            source_run(1000 + index, run_number=1000 + index, event="schedule")
            for index in range(149)
        ] + [source_run()]
        api.runs_responses = [inventory]
        api.run_page_overrides[(0, 2)] = {
            "total_count": 149,
            "workflow_runs": copy.deepcopy(inventory[100:]),
        }
        with self.assertRaisesRegex(ValueError, "total_count changed"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_short_nonterminal_page(self):
        api = FakeApi()
        inventory = [
            source_run(1000 + index, run_number=1000 + index, event="schedule")
            for index in range(149)
        ] + [source_run()]
        api.runs_responses = [inventory]
        api.run_page_overrides[(0, 1)] = {
            "total_count": len(inventory),
            "workflow_runs": copy.deepcopy(inventory[:99]),
        }
        with self.assertRaisesRegex(ValueError, "incomplete"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_unrelated_job_failure(self):
        api = FakeApi()
        api.source_jobs[2] = ordinary_job(conclusion="failure")
        with self.assertRaisesRegex(ValueError, "unexpected non-success Build job"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_duplicate_job_identity(self):
        api = FakeApi()
        api.source_jobs[2]["id"] = api.source_jobs[0]["id"]
        with self.assertRaisesRegex(ValueError, "duplicate or unnamed job"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_staged_build_only_rejects_wrong_job_provenance(self):
        mutations = (
            {"run_id": SOURCE_RUN_ID + 1},
            {"run_attempt": 99},
            {"head_sha": OTHER_SHA},
            {"workflow_name": "Untrusted Build"},
            {"head_branch": "other"},
            {"run_url": "https://api.github.com/repos/other/repository/actions/runs/1"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.source_jobs[2].update(mutation)
                with self.assertRaisesRegex(ValueError, "job provenance"):
                    MODULE.verify_exact_staged_build(
                        api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
                    )

    def test_staged_build_only_rejects_extra_cancelled_shipping_step(self):
        api = FakeApi()
        api.source_jobs[0]["steps"].append(completed_step("unexpected optional step", "cancelled"))
        with self.assertRaisesRegex(ValueError, "unexpected non-success steps"):
            MODULE.verify_exact_staged_build(
                api, REPOSITORY, SHA, SOURCE_RUN_ID, 1
            )

    def test_accepts_only_exact_staged_failure_and_trusted_receipt(self):
        evidence = MODULE.verify_exact_gate(FakeApi(), REPOSITORY, SHA.upper())
        self.assertEqual(evidence.run_id, SOURCE_RUN_ID)
        self.assertEqual(evidence.run_attempt, 1)
        self.assertEqual(evidence.event, "push")
        self.assertEqual(evidence.build_ci120_producer_job_id, 501)
        self.assertEqual(evidence.build_required_gate_job_id, 502)
        self.assertRegex(evidence.build_job_inventory_digest, r"^sha256:[0-9a-f]{64}$")
        self.assertEqual(evidence.ci120_source_artifact_id, 803)
        self.assertEqual(evidence.ci120_source_artifact_digest, CI120_SOURCE_DIGEST)
        self.assertEqual(evidence.ci120_status_id, 601)
        self.assertEqual(evidence.ci120_status_created_at, CI120_STATUS_AT)
        self.assertEqual(evidence.ci120_status_updated_at, CI120_STATUS_AT)
        self.assertEqual(evidence.verifier_run_id, VERIFIER_RUN_ID)
        self.assertEqual(evidence.verifier_run_attempt, 2)
        self.assertEqual(evidence.ci120_trusted_verifier_job_id, 701)
        self.assertEqual(evidence.receipt_artifact_id, 801)
        self.assertEqual(evidence.receipt_artifact_digest, DIGEST)
        self.assertEqual(evidence.codeql_run_id, CODEQL_SOURCE_RUN_ID)
        self.assertEqual(evidence.codeql_run_attempt, 1)
        self.assertEqual(evidence.codeql_actions_source_job_id, 900)
        self.assertEqual(evidence.codeql_c_cpp_source_job_id, 901)
        self.assertEqual(evidence.codeql_python_source_job_id, 902)
        self.assertEqual(evidence.codeql_actions_source_artifact_id, 810)
        self.assertEqual(evidence.codeql_c_cpp_source_artifact_id, 811)
        self.assertEqual(evidence.codeql_python_source_artifact_id, 812)
        self.assertEqual(evidence.codeql_status_id, 602)
        self.assertEqual(evidence.codeql_status_created_at, CODEQL_STATUS_AT)
        self.assertEqual(evidence.codeql_status_updated_at, CODEQL_STATUS_AT)
        self.assertEqual(evidence.codeql_reporter_run_id, CODEQL_REPORTER_RUN_ID)
        self.assertEqual(evidence.codeql_reporter_run_attempt, 3)
        self.assertEqual(evidence.codeql_trusted_reporter_job_id, 950)
        self.assertEqual(evidence.codeql_summary_artifact_id, 802)
        self.assertEqual(evidence.codeql_summary_artifact_digest, CODEQL_DIGEST)

    def test_accepts_workflow_dispatch_source(self):
        api = FakeApi()
        dispatched = source_run(event="workflow_dispatch")
        api.runs_responses = [[dispatched]]
        api.source_live = dispatched
        evidence = MODULE.verify_exact_gate(api, REPOSITORY, SHA)
        self.assertEqual(evidence.event, "workflow_dispatch")

    def test_job_inventory_reordering_is_canonical_but_id_replay_is_rejected(self):
        api = FakeApi()
        build = [source_job(), required_gate(), ordinary_job()]
        codeql = [
            codeql_source_job("actions", 0),
            codeql_source_job("c-cpp", 1),
            codeql_source_job("python", 2),
        ]
        api.source_job_responses = [build, list(reversed(build)), build]
        api.codeql_source_job_responses = [codeql, list(reversed(codeql)), codeql]
        api.codeql_source_artifact_responses = [
            [
                codeql_source_artifact("actions", 0),
                codeql_source_artifact("c-cpp", 1),
                codeql_source_artifact("python", 2),
            ],
            [
                codeql_source_artifact("python", 2),
                codeql_source_artifact("actions", 0),
                codeql_source_artifact("c-cpp", 1),
            ],
        ]
        MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        mutations = []
        changed_build = copy.deepcopy(build)
        changed_build[2]["id"] = 1503
        mutations.append(("build", "source_job_responses", [build, changed_build]))
        verifier = [verifier_job()]
        changed_verifier = copy.deepcopy(verifier)
        changed_verifier[0]["id"] = 1701
        mutations.append(("verifier", "verifier_job_responses", [verifier, changed_verifier]))
        changed_codeql = copy.deepcopy(codeql)
        changed_codeql[1]["id"] = 1901
        mutations.append(("codeql source", "codeql_source_job_responses", [codeql, changed_codeql]))
        reporter = [codeql_reporter_job()]
        changed_reporter = copy.deepcopy(reporter)
        changed_reporter[0]["id"] = 1950
        mutations.append(("codeql reporter", "codeql_reporter_job_responses", [reporter, changed_reporter]))
        for label, attribute, responses in mutations:
            with self.subTest(label=label):
                api = FakeApi()
                setattr(api, attribute, responses)
                with self.assertRaises(ValueError):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_job_inventory_digest_is_step_order_canonical_and_field_complete(self):
        jobs = [source_job(), required_gate(), ordinary_job()]
        baseline = MODULE._job_inventory_digest(jobs, "test inventory")
        reordered = copy.deepcopy(list(reversed(jobs)))
        for job in reordered:
            job["steps"].reverse()
        self.assertEqual(
            MODULE._job_inventory_digest(reordered, "test inventory"), baseline
        )

        changed = copy.deepcopy(jobs)
        changed[0]["steps"][0]["completed_at"] = "2026-08-30T04:17:59Z"
        self.assertNotEqual(
            MODULE._job_inventory_digest(changed, "test inventory"), baseline
        )

        duplicate_number = copy.deepcopy(jobs)
        duplicate_number[0]["steps"][1]["number"] = duplicate_number[0]["steps"][0][
            "number"
        ]
        with self.assertRaisesRegex(ValueError, "duplicate step numbers"):
            MODULE._job_inventory_digest(duplicate_number, "test inventory")

    def test_source_artifact_replay_and_invalid_inventory_fail_closed(self):
        api = FakeApi()
        api.source_artifact_responses = [
            [ci120_source_artifact()],
            [ci120_source_artifact(id=804)],
        ]
        with self.assertRaisesRegex(ValueError, "job or artifact evidence|CI/reporter evidence"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        base_codeql = [
            codeql_source_artifact("actions", 0),
            codeql_source_artifact("c-cpp", 1),
            codeql_source_artifact("python", 2),
        ]
        changed_codeql = copy.deepcopy(base_codeql)
        changed_codeql[2]["digest"] = "sha256:" + "3" * 64
        api = FakeApi()
        api.codeql_source_artifact_responses = [base_codeql, changed_codeql]
        with self.assertRaisesRegex(ValueError, "CodeQL source/reporter evidence changed"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        invalid_sets = (
            [ci120_source_artifact(expired=True)],
            [ci120_source_artifact(created_at="2026-08-30T03:59:00Z")],
            [ci120_source_artifact(workflow_run={})],
            [ci120_source_artifact(digest="sha256:bad")],
        )
        for artifacts in invalid_sets:
            with self.subTest(artifacts=artifacts[0].get("digest")):
                api = FakeApi()
                api.source_artifact_responses = [artifacts]
                with self.assertRaisesRegex(ValueError, "CI-120 source artifact"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.codeql_source_artifact_responses = [
            [*base_codeql, codeql_source_artifact("actions", 9, id=899)]
        ]
        with self.assertRaisesRegex(ValueError, "duplicate or unnamed|exactly three expected"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_source_upload_steps_outside_their_exact_jobs(self):
        api = FakeApi()
        source_upload = api.source_jobs[0]["steps"][-2]
        source_upload.update(
            started_at="2026-08-30T04:28:00Z",
            completed_at="2026-08-30T04:29:00Z",
        )
        api.source_artifact_responses = [[ci120_source_artifact(
            created_at="2026-08-30T04:28:30Z",
            updated_at="2026-08-30T04:29:00Z",
        )]]
        with self.assertRaisesRegex(ValueError, "outside its exact job window"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        codeql_upload = api.codeql_source_jobs[1]["steps"][-1]
        codeql_upload.update(
            started_at="2026-08-30T03:28:00Z",
            completed_at="2026-08-30T03:29:00Z",
        )
        api.codeql_source_artifact_responses = [[
            codeql_source_artifact("actions", 0),
            codeql_source_artifact(
                "c-cpp",
                1,
                created_at="2026-08-30T03:28:30Z",
                updated_at="2026-08-30T03:29:00Z",
            ),
            codeql_source_artifact("python", 2),
        ]]
        with self.assertRaisesRegex(ValueError, "outside its exact job window"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_duplicate_source_artifact_ids(self):
        api = FakeApi()
        api.codeql_source_artifact_responses = [[
            codeql_source_artifact("actions", 0, id=810),
            codeql_source_artifact("c-cpp", 1, id=810),
            codeql_source_artifact("python", 2, id=812),
        ]]
        with self.assertRaisesRegex(ValueError, "duplicate or unnamed artifact"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_cross_family_global_identity_reuse(self):
        api = FakeApi()
        api.verifier_jobs[0]["id"] = api.source_jobs[0]["id"]
        with self.assertRaisesRegex(ValueError, "job IDs are not globally unique"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.artifact_responses = [[receipt_artifact(id=803)]]
        with self.assertRaisesRegex(ValueError, "artifact IDs are not globally unique"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.status_responses = [[trusted_status(), codeql_status(id=601)]]
        with self.assertRaisesRegex(ValueError, "commit status IDs are not unique"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_source_artifacts_after_upload_even_inside_run(self):
        api = FakeApi()
        api.source_artifact_responses = [[ci120_source_artifact(
            created_at="2026-08-30T04:29:00Z",
            updated_at="2026-08-30T04:29:00Z",
        )]]
        with self.assertRaisesRegex(ValueError, "CI-120 source artifact"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.codeql_source_artifact_responses = [[
            codeql_source_artifact(
                language,
                index,
                created_at="2026-08-30T03:29:00Z",
                updated_at="2026-08-30T03:29:00Z",
            )
            for index, language in enumerate(("actions", "c-cpp", "python"))
        ]]
        with self.assertRaisesRegex(ValueError, "CodeQL actions source artifact"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_status_timestamps_urls_and_terminal_publish_order_fail_closed(self):
        mutations = (
            {"url": None},
            {"url": f"https://api.github.com/repos/{REPOSITORY}/statuses/{SHA}?replay=1"},
            {"created_at": None},
            {"created_at": "2026-08-30T04:58:30"},
            {"created_at": "2026-08-30T04:58:40Z", "updated_at": "2026-08-30T04:58:30Z"},
            {"created_at": "2026-08-30T04:57:58Z", "updated_at": "2026-08-30T04:57:58Z"},
            {"created_at": "2026-08-30T05:00:01Z", "updated_at": "2026-08-30T05:00:01Z"},
            {"target_url": f"https://github.com/{REPOSITORY}/actions/runs/{VERIFIER_RUN_ID}/attempts/2/"},
            {"target_url": f"https://github.com/{REPOSITORY}/actions/runs/{VERIFIER_RUN_ID}/attempts/2?replay=1"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.status_responses = [[trusted_status(**mutation), codeql_status()]]
                with self.assertRaises(ValueError):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        boundary = "2026-08-30T04:57:59.500Z"
        api.status_responses = [[
            trusted_status(created_at=boundary, updated_at=boundary), codeql_status()
        ]]
        MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.verifier_jobs[0]["steps"][-2]["completed_at"] = "2026-08-30T04:58:02Z"
        with self.assertRaisesRegex(ValueError, "before its durable evidence upload"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.verifier_jobs[0]["steps"][-1]["started_at"] = "2026-08-30T04:59:00Z"
        api.verifier_jobs[0]["steps"][-1]["completed_at"] = "2026-08-30T04:58:59Z"
        with self.assertRaisesRegex(ValueError, "outside the trusted reporter job"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_timestamp_only_status_replacement_is_rejected_at_each_replay(self):
        stable = [trusted_status(), codeql_status()]
        changed_ci = [
            trusted_status(
                created_at="2026-08-30T04:58:31Z",
                updated_at="2026-08-30T04:58:31Z",
            ),
            codeql_status(),
        ]
        api = FakeApi()
        api.status_responses = [stable, changed_ci]
        with self.assertRaisesRegex(ValueError, "changed during final revalidation"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        changed_codeql = [
            trusted_status(),
            codeql_status(
                created_at="2026-08-30T05:03:31Z",
                updated_at="2026-08-30T05:03:31Z",
            ),
        ]
        api = FakeApi()
        api.status_responses = [stable, stable, stable, changed_codeql]
        with self.assertRaisesRegex(ValueError, "CodeQL source/reporter evidence changed"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.status_responses = [stable] * 6 + [changed_ci]
        with self.assertRaisesRegex(ValueError, "terminal shared snapshot"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_one_second_skipped_job_clock_skew_is_the_only_accepted_inversion(self):
        api = FakeApi()
        api.source_jobs[2] = ordinary_job(
            conclusion="skipped",
            started_at="2026-08-30T04:10:01Z",
            completed_at="2026-08-30T04:10:00Z",
        )
        MODULE.verify_exact_staged_build(api, REPOSITORY, SHA, SOURCE_RUN_ID, 1)

        api = FakeApi()
        api.source_jobs[2] = ordinary_job(
            conclusion="skipped",
            started_at="2026-08-30T04:10:02Z",
            completed_at="2026-08-30T04:10:00Z",
        )
        with self.assertRaisesRegex(ValueError, "exact current run attempt"):
            MODULE.verify_exact_staged_build(api, REPOSITORY, SHA, SOURCE_RUN_ID, 1)

    def test_rejects_build_live_identity_fields_missing_from_inventory_binding(self):
        for mutation in (
            {"workflow_id": 0},
            {"html_url": "https://github.com/other/repository/actions/runs/42"},
        ):
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.source_live = source_run(**mutation)
                with self.assertRaises(ValueError):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_build_same_identity_live_window_divergence(self):
        api = FakeApi()
        api.source_live = source_run(
            run_started_at="2026-08-30T04:25:00Z",
            updated_at=BUILD_RUN_UPDATED_AT,
        )
        with self.assertRaisesRegex(ValueError, "execution window changed"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_same_identity_live_window_divergence(self):
        api = FakeApi()
        api.codeql_source_live = codeql_source_run(
            run_started_at="2026-08-30T03:25:00Z",
            updated_at=CODEQL_RUN_UPDATED_AT,
        )
        with self.assertRaisesRegex(ValueError, "execution window changed"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_build_attempt_transition_during_final_live_revalidation(self):
        api = FakeApi()
        live_calls = 0

        def changing_build_attempt(path):
            nonlocal live_calls
            if path == f"/repos/{REPOSITORY}/actions/runs/{SOURCE_RUN_ID}":
                live_calls += 1
                if live_calls >= 2:
                    return source_run(run_attempt=2)
            return api(path)

        with self.assertRaisesRegex(ValueError, "changed after inventory selection"):
            MODULE.verify_exact_gate(changing_build_attempt, REPOSITORY, SHA)
        self.assertEqual(live_calls, 2)

    def test_rejects_codeql_attempt_transition_during_cross_live_revalidation(self):
        api = FakeApi()
        live_calls = 0

        def changing_codeql_attempt(path):
            nonlocal live_calls
            if path == f"/repos/{REPOSITORY}/actions/runs/{CODEQL_SOURCE_RUN_ID}":
                live_calls += 1
                if live_calls >= 3:
                    return codeql_source_run(run_attempt=2)
            return api(path)

        with self.assertRaisesRegex(ValueError, "changed after inventory selection"):
            MODULE.verify_exact_gate(changing_codeql_attempt, REPOSITORY, SHA)
        self.assertEqual(live_calls, 3)

    def test_accepts_historical_source_with_current_trusted_verifier(self):
        api = FakeApi()
        api.default_sha = OTHER_SHA
        current_verifier = verifier_run(head_sha=OTHER_SHA)
        api.verifier_responses = [current_verifier]
        api.verifier_jobs = [verifier_job(head_sha=OTHER_SHA)]
        current_artifact = receipt_artifact()
        current_artifact["workflow_run"]["head_sha"] = OTHER_SHA
        api.artifact_responses = [[current_artifact]]
        api.codeql_reporter_responses = [codeql_reporter_run(head_sha=OTHER_SHA)]
        api.codeql_reporter_jobs = [codeql_reporter_job(head_sha=OTHER_SHA)]
        current_codeql_artifact = codeql_summary_artifact()
        current_codeql_artifact["workflow_run"]["head_sha"] = OTHER_SHA
        api.codeql_artifact_responses = [[current_codeql_artifact]]
        evidence = MODULE.verify_exact_gate(api, REPOSITORY, SHA)
        self.assertEqual(evidence.verifier_sha, OTHER_SHA)
        self.assertEqual(evidence.codeql_reporter_run_id, CODEQL_REPORTER_RUN_ID)

    def test_accepts_full_codeql_rerun_with_current_attempt_job_times(self):
        source = codeql_source_run(
            run_attempt=2,
            run_started_at="2026-08-30T06:00:00Z",
            updated_at="2026-08-30T06:30:00Z",
        )
        jobs = [
            codeql_source_job(
                language,
                index,
                run_attempt=2,
                started_at=f"2026-08-30T06:0{index + 1}:00Z",
                completed_at=f"2026-08-30T06:1{index + 1}:00Z",
            )
            for index, language in enumerate(("actions", "c-cpp", "python"))
        ]
        for job in jobs:
            for step in job["steps"]:
                step["started_at"] = job["started_at"]
                step["completed_at"] = job["completed_at"]

        def fetch(path):
            self.assertIn(f"/actions/runs/{CODEQL_SOURCE_RUN_ID}/attempts/2/jobs?", path)
            return {"total_count": len(jobs), "jobs": copy.deepcopy(jobs)}

        job_evidence = MODULE._verify_codeql_source_jobs(fetch, REPOSITORY, source)
        artifacts = [
            codeql_source_artifact(
                language,
                index,
                name=f"codeql-{language}-attempt-2.sarif",
                created_at=jobs[index]["started_at"],
                updated_at=jobs[index]["completed_at"],
            )
            for index, language in enumerate(("actions", "c-cpp", "python"))
        ]

        def fetch_artifacts(path):
            self.assertIn(f"/actions/runs/{CODEQL_SOURCE_RUN_ID}/artifacts?", path)
            return {"total_count": len(artifacts), "artifacts": copy.deepcopy(artifacts)}

        accepted = MODULE._verify_codeql_source_artifacts(
            fetch_artifacts, REPOSITORY, REPOSITORY_ID, source, job_evidence
        )
        self.assertEqual([item.artifact_id for item in accepted], [810, 811, 812])

        stale = copy.deepcopy(artifacts)
        stale[0]["name"] = "codeql-actions-attempt-1.sarif"

        def fetch_stale(_path):
            return {"total_count": len(stale), "artifacts": stale}

        with self.assertRaisesRegex(ValueError, "exactly three expected"):
            MODULE._verify_codeql_source_artifacts(
                fetch_stale, REPOSITORY, REPOSITORY_ID, source, job_evidence
            )

    def test_rejects_build_partial_rerun_with_copied_success_job(self):
        source = source_run(
            run_attempt=2,
            run_started_at="2026-08-30T06:00:00Z",
            updated_at="2026-08-30T06:30:00Z",
        )
        jobs = [
            source_job(
                run_attempt=2,
                started_at="2026-08-30T06:01:00Z",
                completed_at="2026-08-30T06:20:00Z",
            ),
            required_gate(
                run_attempt=2,
                started_at="2026-08-30T06:21:00Z",
                completed_at="2026-08-30T06:22:00Z",
            ),
            ordinary_job(
                run_attempt=2,
                started_at="2026-08-30T04:02:00Z",
                completed_at="2026-08-30T04:10:00Z",
            ),
        ]

        def fetch(path):
            self.assertIn(f"/actions/runs/{SOURCE_RUN_ID}/attempts/2/jobs?", path)
            return {"total_count": len(jobs), "jobs": copy.deepcopy(jobs)}

        with self.assertRaisesRegex(ValueError, "exact current run attempt"):
            MODULE._verify_source_jobs(fetch, REPOSITORY, source)

    def test_rejects_codeql_partial_rerun_with_copied_success_jobs(self):
        source = codeql_source_run(
            run_attempt=2,
            run_started_at="2026-08-30T06:00:00Z",
            updated_at="2026-08-30T06:30:00Z",
        )
        jobs = [
            codeql_source_job(
                "actions",
                0,
                run_attempt=2,
                started_at="2026-08-30T06:01:00Z",
                completed_at="2026-08-30T06:11:00Z",
            ),
            codeql_source_job(
                "c-cpp",
                1,
                run_attempt=2,
                started_at="2026-08-30T03:02:00Z",
                completed_at="2026-08-30T03:12:00Z",
            ),
            codeql_source_job(
                "python",
                2,
                run_attempt=2,
                started_at="2026-08-30T03:03:00Z",
                completed_at="2026-08-30T03:13:00Z",
            ),
        ]

        def fetch(path):
            self.assertIn(f"/actions/runs/{CODEQL_SOURCE_RUN_ID}/attempts/2/jobs?", path)
            return {"total_count": len(jobs), "jobs": copy.deepcopy(jobs)}

        with self.assertRaisesRegex(ValueError, "exact current run attempt"):
            MODULE._verify_codeql_source_jobs(fetch, REPOSITORY, source)

    def test_rejects_ci120_verifier_partial_rerun_with_copied_required_job(self):
        run = verifier_run(
            run_attempt=3,
            run_started_at="2026-08-30T06:00:00Z",
            updated_at="2026-08-30T06:30:00Z",
        )
        jobs = [
            verifier_job(
                id=702,
                name="Verifier retry trigger",
                run_attempt=3,
                started_at="2026-08-30T06:01:00Z",
                completed_at="2026-08-30T06:10:00Z",
                steps=[],
            ),
            verifier_job(
                run_attempt=3,
                started_at="2026-08-30T04:32:00Z",
                completed_at="2026-08-30T04:59:00Z",
            ),
        ]

        def fetch(path):
            self.assertIn(f"/actions/runs/{VERIFIER_RUN_ID}/attempts/3/jobs?", path)
            return {"total_count": len(jobs), "jobs": copy.deepcopy(jobs)}

        with self.assertRaisesRegex(ValueError, "exact current run attempt"):
            MODULE._verify_verifier_jobs(
                fetch, REPOSITORY, SHA, VERIFIER_RUN_ID, 3, run, trusted_status()
            )

    def test_rejects_codeql_reporter_partial_rerun_with_copied_required_job(self):
        run = codeql_reporter_run(
            run_attempt=4,
            run_started_at="2026-08-30T06:00:00Z",
            updated_at="2026-08-30T06:30:00Z",
        )
        jobs = [
            codeql_reporter_job(
                id=951,
                name="Reporter retry trigger",
                run_attempt=4,
                started_at="2026-08-30T06:01:00Z",
                completed_at="2026-08-30T06:10:00Z",
                steps=[],
            ),
            codeql_reporter_job(
                run_attempt=4,
                started_at="2026-08-30T05:01:10Z",
                completed_at="2026-08-30T05:04:00Z",
            ),
        ]

        def fetch(path):
            self.assertIn(f"/actions/runs/{CODEQL_REPORTER_RUN_ID}/attempts/4/jobs?", path)
            return {"total_count": len(jobs), "jobs": copy.deepcopy(jobs)}

        with self.assertRaisesRegex(ValueError, "exact current run attempt"):
            MODULE._verify_codeql_reporter_jobs(
                fetch, REPOSITORY, SHA, CODEQL_REPORTER_RUN_ID, 4, run,
                codeql_status()
            )

    def test_rejects_newer_non_successful_codeql_source(self):
        api = FakeApi()
        newer = codeql_source_run(
            id=CODEQL_SOURCE_RUN_ID + 1,
            run_number=13,
            status="in_progress",
            conclusion=None,
            html_url=f"https://github.com/{REPOSITORY}/actions/runs/{CODEQL_SOURCE_RUN_ID + 1}",
        )
        api.codeql_runs_responses = [[codeql_source_run(), newer]]
        with self.assertRaisesRegex(ValueError, "newest exact CodeQL attempt"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_source_job_provenance_mutations(self):
        mutations = (
            {"run_id": CODEQL_SOURCE_RUN_ID + 1},
            {"run_attempt": 2},
            {"head_sha": OTHER_SHA},
            {"workflow_name": "Untrusted CodeQL"},
            {"head_branch": "other"},
            {"run_url": "https://api.github.com/repos/other/repository/actions/runs/1"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.codeql_source_jobs[0].update(mutation)
                with self.assertRaisesRegex(ValueError, "CodeQL source job provenance"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_status_contract_mutations(self):
        mutations = (
            {"state": "pending"},
            {"description": "generic success"},
            {"creator": {"id": 1, "login": "attacker", "type": "User"}},
            {"target_url": "https://github.com/Krilliac/SparkEngine/actions/runs/85"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.status_responses = [[trusted_status(), codeql_status(**mutation)]]
                with self.assertRaisesRegex(ValueError, "CodeQL status"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_reporter_run_mutations(self):
        mutations = (
            {"run_attempt": 4},
            {"name": "Untrusted Reporter"},
            {"path": ".github/workflows/other.yml@refs/heads/Working"},
            {"event": "workflow_dispatch"},
            {"head_branch": "other"},
            {"head_sha": OTHER_SHA},
            {"conclusion": "failure"},
            {"html_url": "https://github.com/other/repository/actions/runs/85"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.codeql_reporter_responses = [codeql_reporter_run(**mutation)]
                with self.assertRaisesRegex(ValueError, "CodeQL reporter run"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_reporter_job_provenance_mutations(self):
        mutations = (
            {"run_id": CODEQL_REPORTER_RUN_ID + 1},
            {"run_attempt": 4},
            {"head_sha": OTHER_SHA},
            {"workflow_name": "Untrusted Reporter"},
            {"head_branch": "other"},
            {"run_url": "https://api.github.com/repos/other/repository/actions/runs/1"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.codeql_reporter_jobs[0].update(mutation)
                with self.assertRaisesRegex(ValueError, "CodeQL reporter job provenance"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_required_step_failure(self):
        api = FakeApi()
        api.codeql_reporter_jobs[0]["steps"][-1]["conclusion"] = "failure"
        with self.assertRaisesRegex(ValueError, "did not conclude 'success'"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_summary_artifact_mutations(self):
        mutations = (
            {"name": f"codeql-trusted-summary-{OTHER_SHA}-{CODEQL_SOURCE_RUN_ID}-1-3"},
            {"name": f"codeql-trusted-summary-{SHA}-{CODEQL_SOURCE_RUN_ID + 1}-1-3"},
            {"name": f"codeql-trusted-summary-{SHA}-{CODEQL_SOURCE_RUN_ID}-2-3"},
            {"name": f"codeql-trusted-summary-{SHA}-{CODEQL_SOURCE_RUN_ID}-1-4"},
            {"digest": "sha256:bad"},
            {"expired": True},
            {"size_in_bytes": True},
            {"node_id": ""},
            {"url": "https://api.github.com/repos/attacker/wrong/actions/artifacts/802"},
            {"archive_download_url": "https://attacker.invalid/summary.zip"},
            {"created_at": "2026-08-30T05:01:59Z"},
            {"updated_at": "2026-08-30T05:03:01Z"},
            {"expires_at": "2026-08-30T05:02:59Z"},
            {"expires_at": "invalid"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.codeql_artifact_responses = [[codeql_summary_artifact(**mutation)]]
                with self.assertRaisesRegex(ValueError, "CodeQL summary"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_duplicate_codeql_summary_artifact_identities(self):
        duplicate_id = codeql_summary_artifact(name="unrelated-summary")
        duplicate_name_left = codeql_summary_artifact(id=820, name="duplicate-summary")
        duplicate_name_right = codeql_summary_artifact(id=821, name="duplicate-summary")
        for artifacts in (
            [codeql_summary_artifact(), duplicate_id],
            [codeql_summary_artifact(), duplicate_name_left, duplicate_name_right],
        ):
            with self.subTest(artifacts=[item["id"] for item in artifacts]):
                api = FakeApi()
                api.codeql_artifact_responses = [artifacts]
                with self.assertRaisesRegex(ValueError, "duplicate or unnamed"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_summary_identity_or_timing_drift_during_replay(self):
        mutations = (
            {"node_id": "ARTIFACT_CODEQL_SUMMARY_REPLACED"},
            {"created_at": "2026-08-30T05:02:31Z"},
            {"updated_at": "2026-08-30T05:02:58Z"},
            {"expires_at": "2026-09-30T05:02:59Z"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.codeql_artifact_responses = [
                    [codeql_summary_artifact()],
                    [codeql_summary_artifact(**mutation)],
                ]
                with self.assertRaisesRegex(ValueError, "CodeQL source/reporter evidence changed"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_artifact_bound_to_source_instead_of_current_reporter(self):
        api = FakeApi()
        api.default_sha = OTHER_SHA
        api.verifier_responses = [verifier_run(head_sha=OTHER_SHA)]
        api.verifier_jobs = [verifier_job(head_sha=OTHER_SHA)]
        receipt = receipt_artifact()
        receipt["workflow_run"]["head_sha"] = OTHER_SHA
        api.artifact_responses = [[receipt]]
        api.codeql_reporter_responses = [codeql_reporter_run(head_sha=OTHER_SHA)]
        api.codeql_reporter_jobs = [codeql_reporter_job(head_sha=OTHER_SHA)]
        # Leave the CodeQL artifact provenance at T instead of current W.
        with self.assertRaisesRegex(ValueError, "CodeQL summary"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_codeql_evidence_changes_during_final_replay(self):
        api = FakeApi()
        stable = [trusted_status(), codeql_status()]
        changed = [trusted_status(), codeql_status(id=999)]
        api.status_responses = [stable, stable, stable, changed]
        with self.assertRaisesRegex(ValueError, "CodeQL source/reporter evidence changed"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        api.codeql_artifact_responses = [
            [codeql_summary_artifact()],
            [codeql_summary_artifact(digest="sha256:" + "e" * 64)],
        ]
        with self.assertRaisesRegex(ValueError, "CodeQL source/reporter evidence changed"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        working_calls = 0

        def changing_working(path):
            nonlocal working_calls
            if path == f"/repos/{REPOSITORY}/commits/Working":
                working_calls += 1
                return {"sha": OTHER_SHA if working_calls >= 3 else SHA}
            return api(path)

        with self.assertRaisesRegex(ValueError, "CodeQL source/reporter evidence changed"):
            MODULE.verify_exact_gate(changing_working, REPOSITORY, SHA)

    def test_rejects_terminal_shared_status_replacement(self):
        stable = [trusted_status(), codeql_status()]

        api = FakeApi()
        changed_ci = [trusted_status(id=999), codeql_status()]
        api.status_responses = [stable] * 6 + [changed_ci]
        with self.assertRaisesRegex(ValueError, "terminal shared snapshot"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        changed_codeql = [trusted_status(), codeql_status(id=999)]
        api.status_responses = [stable] * 6 + [changed_codeql]
        with self.assertRaisesRegex(ValueError, "terminal shared snapshot"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_newer_running_or_nonstaged_source(self):
        for status, conclusion in (("in_progress", None), ("completed", "success"), ("completed", "cancelled")):
            with self.subTest(status=status, conclusion=conclusion):
                api = FakeApi()
                newer = source_run(43, run_number=11, status=status, conclusion=conclusion)
                api.runs_responses = [[source_run(), newer]]
                with self.assertRaisesRegex(ValueError, "newest exact Build attempt"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_arbitrary_failed_job_or_failed_required_gate(self):
        for replacement in (
            ordinary_job(conclusion="failure"),
            required_gate(conclusion="failure"),
            required_gate(conclusion="skipped"),
        ):
            with self.subTest(replacement=replacement):
                api = FakeApi()
                api.source_jobs = [source_job(), required_gate(), ordinary_job()]
                index = 1 if replacement["name"] == "Required CI Gate" else 2
                api.source_jobs[index] = replacement
                with self.assertRaises(ValueError):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_wrong_source_failure_shape(self):
        api = FakeApi()
        broken = source_job()
        broken["steps"][-1]["conclusion"] = "success"
        api.source_jobs[0] = broken
        with self.assertRaisesRegex(ValueError, "Enforce reviewed CI-120 findings"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_stale_untrusted_or_unbound_status(self):
        mutations = (
            {"state": "pending"},
            {"description": "old source binding"},
            {"creator": {"id": 1, "login": "github-actions[bot]", "type": "Bot"}},
            {"target_url": f"https://github.com/{REPOSITORY}/actions/runs/1"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.status_responses = [[trusted_status(**mutation)]]
                with self.assertRaises(ValueError):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_wrong_verifier_identity_or_job(self):
        api = FakeApi()
        api.verifier_responses = [verifier_run(path=".github/workflows/evil.yml")]
        with self.assertRaisesRegex(ValueError, "verifier run"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

        api = FakeApi()
        broken = verifier_job()
        broken["steps"][-1]["conclusion"] = "failure"
        api.verifier_jobs = [broken]
        with self.assertRaisesRegex(ValueError, "Publish exact source CI-120 status"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_wrong_verifier_job_provenance(self):
        mutations = (
            {"run_id": VERIFIER_RUN_ID + 1},
            {"run_attempt": 1},
            {"head_sha": OTHER_SHA},
            {"workflow_name": "Untrusted Verifier"},
            {"head_branch": "other"},
            {"run_url": "https://api.github.com/repos/other/repository/actions/runs/1"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.verifier_jobs = [verifier_job(**mutation)]
                with self.assertRaisesRegex(ValueError, "verifier job provenance"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_extra_non_success_verifier_step(self):
        api = FakeApi()
        api.verifier_jobs[0]["steps"].append(
            completed_step("unexpected verifier cleanup", "cancelled")
        )
        with self.assertRaisesRegex(ValueError, "verifier has unexpected non-success steps"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_missing_expired_or_unbound_receipt_artifact(self):
        mutations = (
            {"name": "wrong"},
            {"expired": True},
            {"digest": "sha256:bad"},
            {"workflow_run": {"id": 1}},
            {"size_in_bytes": True},
            {"node_id": ""},
            {"url": "https://api.github.com/repos/attacker/wrong/actions/artifacts/801"},
            {"archive_download_url": "https://attacker.invalid/receipt.zip"},
            {"created_at": "2026-08-30T04:56:59Z"},
            {"updated_at": "2026-08-30T04:58:01Z"},
            {"expires_at": "2026-08-30T04:57:59Z"},
            {"expires_at": "invalid"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.artifact_responses = [[receipt_artifact(**mutation)]]
                with self.assertRaises(ValueError):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_duplicate_ci120_receipt_artifact_identities(self):
        duplicate_id = receipt_artifact(name="unrelated-receipt")
        duplicate_name_left = receipt_artifact(id=830, name="duplicate-receipt")
        duplicate_name_right = receipt_artifact(id=831, name="duplicate-receipt")
        for artifacts in (
            [receipt_artifact(), duplicate_id],
            [receipt_artifact(), duplicate_name_left, duplicate_name_right],
        ):
            with self.subTest(artifacts=[item["id"] for item in artifacts]):
                api = FakeApi()
                api.artifact_responses = [artifacts]
                with self.assertRaisesRegex(ValueError, "duplicate or unnamed"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_ci120_receipt_identity_or_timing_drift_during_replay(self):
        mutations = (
            {"node_id": "ARTIFACT_CI120_RECEIPT_REPLACED"},
            {"created_at": "2026-08-30T04:57:31Z"},
            {"updated_at": "2026-08-30T04:57:58Z"},
            {"expires_at": "2026-09-30T04:57:59Z"},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                api = FakeApi()
                api.artifact_responses = [
                    [receipt_artifact()],
                    [receipt_artifact(**mutation)],
                ]
                with self.assertRaisesRegex(ValueError, "CI/reporter evidence changed"):
                    MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_receipt_removal_during_final_revalidation(self):
        api = FakeApi()
        api.artifact_responses = [[receipt_artifact()], []]
        with self.assertRaisesRegex(ValueError, "retain one exact"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_rejects_status_replacement_during_final_revalidation(self):
        api = FakeApi()
        api.status_responses = [[trusted_status()], [trusted_status(id=999)]]
        with self.assertRaisesRegex(ValueError, "changed during final revalidation"):
            MODULE.verify_exact_gate(api, REPOSITORY, SHA)

    def test_fails_closed_on_api_error(self):
        def broken(_path):
            raise RuntimeError("network unavailable")

        with self.assertRaisesRegex(RuntimeError, "network unavailable"):
            MODULE.verify_exact_gate(broken, REPOSITORY, SHA)

    def test_rejects_malformed_input_and_truncated_inventory(self):
        with self.assertRaisesRegex(ValueError, "owner/name"):
            MODULE.verify_exact_gate(FakeApi(), "bad/repo/extra", SHA)
        with self.assertRaisesRegex(ValueError, "40-character"):
            MODULE.verify_exact_gate(FakeApi(), REPOSITORY, "abc")

        api = FakeApi()

        def truncated(path):
            if "/workflows/build.yml/runs?" in path:
                return {"total_count": 2, "workflow_runs": [source_run()]}
            return api(path)

        with self.assertRaisesRegex(ValueError, "incomplete"):
            MODULE.verify_exact_gate(truncated, REPOSITORY, SHA)

    def test_workflow_permissions_and_receipt_name_are_exact(self):
        release = (SCRIPT.parents[1] / "workflows" / "release.yml").read_text(encoding="utf-8")
        verifier = (SCRIPT.parents[1] / "workflows" / "ci120-report.yml").read_text(encoding="utf-8")
        build = (SCRIPT.parents[1] / "workflows" / "build.yml").read_text(encoding="utf-8")
        self.assertIn("      statuses: read", release)
        self.assertEqual(
            release.count("python3 .github/scripts/verify-exact-required-gate.py"),
            3,
            "publication must verify once at entry and immediately before both mutation boundaries",
        )
        self.assertIn(
            "ci120-trusted-receipt-${{ github.event.workflow_run.head_sha }}-"
            "${{ github.event.workflow_run.id }}-${{ github.event.workflow_run.run_attempt }}-"
            "${{ github.run_attempt }}",
            verifier,
        )
        self.assertIn("python3 .github/scripts/test-verify-exact-required-gate.py", build)


if __name__ == "__main__":
    unittest.main()
