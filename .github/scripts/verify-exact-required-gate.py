#!/usr/bin/env python3
"""Verify the exact staged Build and trusted build-matrix evidence for publication."""

from __future__ import annotations

from dataclasses import dataclass, fields
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
import re
import sys
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


FetchJson = Callable[[str], Any]
ALLOWED_EVENTS = frozenset({"push", "workflow_dispatch"})
KNOWN_SOURCE_EVENTS = frozenset({"push", "workflow_dispatch", "pull_request", "schedule"})
SHA_PATTERN = re.compile(r"[0-9a-fA-F]{40}")
REPOSITORY_PATTERN = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
DIGEST_PATTERN = re.compile(r"sha256:[0-9a-fA-F]{64}")
MAX_API_ITEMS = 100
MAX_SOURCE_RUNS = 200
MAX_BUILD_MATRIX_SOURCE_ARTIFACT_BYTES = 4 * 1024 * 1024 * 1024
MAX_CODEQL_SOURCE_ARTIFACT_BYTES = 25 * 1024 * 1024
MAX_CODEQL_SOURCE_ARTIFACT_TOTAL_BYTES = 75 * 1024 * 1024
GITHUB_ACTIONS_BOT_ID = 41898282

SOURCE_WORKFLOW_NAME = "Build SparkEngine"
SOURCE_WORKFLOW_PATH = ".github/workflows/build.yml"
SOURCE_BRANCH = "Working"
SOURCE_JOB_NAME = "Windows Shipping build matrix"
SOURCE_FINAL_STEP = "Record build-matrix evidence"
REQUIRED_GATE_NAME = "Required CI Gate"
STATUS_CONTEXT = "Build Matrix Verifier / Exact Source"
VERIFIER_WORKFLOW_NAME = "Build Matrix Verifier"
VERIFIER_WORKFLOW_PATH = ".github/workflows/build-matrix-verifier.yml"
VERIFIER_JOB_NAME = "Verify and attest build-matrix evidence"
CODEQL_SOURCE_WORKFLOW_NAME = "CodeQL Advanced"
CODEQL_SOURCE_WORKFLOW_PATH = ".github/workflows/codeql.yml"
CODEQL_STATUS_CONTEXT = "CodeQL Trusted / Exact Source"
CODEQL_REPORTER_WORKFLOW_NAME = "CodeQL Trusted Reporter"
CODEQL_REPORTER_WORKFLOW_PATH = ".github/workflows/codeql-report.yml"
CODEQL_REPORTER_JOB_NAME = "Validate and report CodeQL evidence"

SOURCE_REQUIRED_STEPS = (
    "Checkout repository",
    "Setup MSVC",
    "Configure and build the Windows Shipping lane",
    "Capture Windows Shipping provenance",
    "Install the Windows Shipping SDK for the consumer profile",
    "Configure and build the Windows validation lane",
    "Capture Windows validation provenance",
    "Configure and build the installed SDK consumer lane",
    "Capture installed SDK consumer provenance",
    "Generate the configured build-matrix inventory",
    "Check the configured build matrix",
    "Upload build-matrix evidence",
)

VERIFIER_REQUIRED_STEPS = (
    "Checkout trusted default-branch verifier",
    "Resolve trusted verifier commit",
    "Attest exact default-branch verifier before execution",
    "Mark the exact source build-matrix status pending",
    "Test the trusted build-matrix verifier",
    "Authorize exact source run, job, and artifact",
    "Download the exact build-matrix source artifact",
    "Verify the build-matrix evidence as bounded data",
    "Attest the trusted build-matrix receipt",
    "Upload the trusted build-matrix receipt",
    "Publish the exact source build-matrix status",
)

CODEQL_SOURCE_JOB_NAMES = frozenset({"Analyze (actions)", "Analyze (c-cpp)", "Analyze (python)"})
CODEQL_SOURCE_REQUIRED_STEPS = (
    "Checkout repository",
    "Initialize CodeQL",
    "Perform CodeQL Analysis",
    "Bind raw CodeQL SARIF to exact source attempt",
    "Upload raw CodeQL SARIF",
)
CODEQL_REPORTER_REQUIRED_STEPS = (
    "Checkout trusted default-branch reporter",
    "Resolve trusted reporter commit",
    "Attest exact default-branch reporter before execution",
    "Mark exact source CodeQL status pending",
    "Test trusted CodeQL reporter",
    "Authorize source run and artifact inventory",
    "Download exact source-run SARIF artifacts",
    "Publish trusted CodeQL report",
    "Upload trusted CodeQL summary",
    "Publish exact source CodeQL status after durable summary",
)


@dataclass(frozen=True)
class GateEvidence:
    run_id: int
    run_url: str
    event: str
    run_attempt: int
    build_build_matrix_producer_job_id: int
    build_required_gate_job_id: int
    build_job_inventory_digest: str
    build_matrix_source_artifact_id: int
    build_matrix_source_artifact_digest: str
    build_matrix_source_artifact_bytes: int
    build_matrix_status_id: int
    build_matrix_status_target_url: str
    build_matrix_status_created_at: str
    build_matrix_status_updated_at: str
    verifier_run_id: int
    verifier_run_url: str
    verifier_run_attempt: int
    verifier_sha: str
    build_matrix_trusted_verifier_job_id: int
    build_matrix_verifier_job_inventory_digest: str
    build_matrix_status_publish_step_started_at: str
    build_matrix_status_publish_step_completed_at: str
    receipt_artifact_id: int
    receipt_artifact_digest: str
    receipt_artifact_bytes: int
    codeql_run_id: int
    codeql_run_attempt: int
    codeql_run_url: str
    codeql_actions_source_job_id: int
    codeql_c_cpp_source_job_id: int
    codeql_python_source_job_id: int
    codeql_source_job_inventory_digest: str
    codeql_actions_source_artifact_id: int
    codeql_actions_source_artifact_digest: str
    codeql_actions_source_artifact_bytes: int
    codeql_c_cpp_source_artifact_id: int
    codeql_c_cpp_source_artifact_digest: str
    codeql_c_cpp_source_artifact_bytes: int
    codeql_python_source_artifact_id: int
    codeql_python_source_artifact_digest: str
    codeql_python_source_artifact_bytes: int
    codeql_status_id: int
    codeql_status_target_url: str
    codeql_status_created_at: str
    codeql_status_updated_at: str
    codeql_reporter_run_id: int
    codeql_reporter_run_attempt: int
    codeql_reporter_run_url: str
    codeql_trusted_reporter_job_id: int
    codeql_reporter_job_inventory_digest: str
    codeql_status_publish_step_started_at: str
    codeql_status_publish_step_completed_at: str
    codeql_summary_artifact_id: int
    codeql_summary_artifact_digest: str
    codeql_summary_artifact_bytes: int


@dataclass(frozen=True)
class StagedBuildEvidence:
    run_id: int
    run_number: int
    run_attempt: int
    run_url: str
    event: str


@dataclass(frozen=True)
class CodeQLEvidence:
    source_run_id: int
    source_run_number: int
    source_run_attempt: int
    source_run_url: str
    actions_source_job_id: int
    c_cpp_source_job_id: int
    python_source_job_id: int
    source_job_inventory_digest: str
    source_artifacts: tuple["SourceArtifactEvidence", ...]
    status_id: int
    status_signature: tuple[Any, ...]
    status_target_url: str
    status_created_at: str
    status_updated_at: str
    reporter_run_id: int
    reporter_run_attempt: int
    reporter_run_url: str
    reporter_updated_at: str
    trusted_reporter_job_id: int
    reporter_job_inventory_digest: str
    status_publish_step_started_at: str
    status_publish_step_completed_at: str
    summary_artifact_id: int
    summary_artifact_digest: str
    summary_artifact_size: int
    summary_artifact_signature: tuple[Any, ...]


@dataclass(frozen=True)
class BuildJobEvidence:
    build_matrix_producer_job_id: int
    required_gate_job_id: int
    inventory_digest: str
    source_upload_step_started_at: str
    source_upload_step_completed_at: str


@dataclass(frozen=True)
class CodeQLSourceJobEvidence:
    actions_job_id: int
    c_cpp_job_id: int
    python_job_id: int
    inventory_digest: str
    actions_upload_step_started_at: str
    actions_upload_step_completed_at: str
    c_cpp_upload_step_started_at: str
    c_cpp_upload_step_completed_at: str
    python_upload_step_started_at: str
    python_upload_step_completed_at: str


@dataclass(frozen=True)
class TrustedJobEvidence:
    job_id: int
    inventory_digest: str
    artifact_upload_step_started_at: str
    artifact_upload_step_completed_at: str
    status_publish_step_started_at: str
    status_publish_step_completed_at: str


@dataclass(frozen=True)
class SourceArtifactEvidence:
    language: str
    artifact_id: int
    name: str
    size: int
    digest: str
    signature: tuple[Any, ...]


@dataclass(frozen=True)
class TrustedArtifactEvidence:
    artifact_id: int
    size: int
    digest: str
    signature: tuple[Any, ...]


def _object(payload: Any, label: str) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValueError(f"{label} response must be an object")
    return payload


def _bounded_collection(payload: Any, key: str, label: str) -> list[dict[str, Any]]:
    value = _object(payload, label)
    items = value.get(key)
    total_count = value.get("total_count")
    if (
        not isinstance(items, list)
        or not isinstance(total_count, int)
        or isinstance(total_count, bool)
        or total_count < 0
    ):
        raise ValueError(f"{label} response is missing exact {key}[]/total_count fields")
    if total_count != len(items) or total_count > MAX_API_ITEMS:
        raise ValueError(f"{label} inventory is incomplete or exceeds {MAX_API_ITEMS} items")
    if any(not isinstance(item, dict) for item in items):
        raise ValueError(f"{label} entries must be objects")
    return items


def _positive_int(value: Any, label: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 1:
        raise ValueError(f"{label} must be a positive integer")
    return value


def _timestamp(value: Any, label: str) -> datetime:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} must be an ISO-8601 timestamp")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError(f"{label} must be an ISO-8601 timestamp") from error
    if parsed.tzinfo is None:
        raise ValueError(f"{label} must include a timezone")
    return parsed


def _canonical_timestamp(value: Any, label: str) -> str:
    parsed = _timestamp(value, label).astimezone(timezone.utc)
    return parsed.isoformat().replace("+00:00", "Z")


def _run_window(run: dict[str, Any], label: str) -> tuple[datetime, datetime]:
    started = _timestamp(run.get("run_started_at"), f"{label} run_started_at")
    updated = _timestamp(run.get("updated_at"), f"{label} updated_at")
    if updated < started:
        raise ValueError(f"{label} has an invalid execution window")
    return started, updated


def _verify_job_window(
    job: dict[str, Any], run_started: datetime, run_updated: datetime, label: str
) -> None:
    started = _timestamp(job.get("started_at"), f"{label} started_at")
    completed = _timestamp(job.get("completed_at"), f"{label} completed_at")
    if completed < started:
        if (
            job.get("status") != "completed"
            or job.get("conclusion") != "skipped"
            or started - completed > timedelta(seconds=1)
            or not run_started <= completed <= started <= run_updated
        ):
            raise ValueError(f"{label} was not executed in the exact current run attempt")
        return
    if not run_started <= started <= completed <= run_updated:
        raise ValueError(f"{label} was not executed in the exact current run attempt")


def _canonical_step(step: dict[str, Any], label: str) -> dict[str, Any]:
    number = _positive_int(step.get("number"), f"{label} step number")
    name = step.get("name")
    status = step.get("status")
    conclusion = step.get("conclusion")
    if not isinstance(name, str) or not name:
        raise ValueError(f"{label} step {number} has no exact name")
    if not isinstance(status, str) or not status:
        raise ValueError(f"{label} step {number} has no exact status")
    if not isinstance(conclusion, str) or not conclusion:
        raise ValueError(f"{label} step {number} has no exact conclusion")

    raw_started = step.get("started_at")
    raw_completed = step.get("completed_at")
    if raw_started is None and raw_completed is None and conclusion == "skipped":
        started = None
        completed = None
    else:
        started_value = _timestamp(raw_started, f"{label} step {number} started_at")
        completed_value = _timestamp(raw_completed, f"{label} step {number} completed_at")
        if completed_value < started_value and not (
            conclusion == "skipped"
            and started_value - completed_value <= timedelta(seconds=1)
        ):
            raise ValueError(f"{label} step {number} has an invalid execution window")
        started = _canonical_timestamp(raw_started, f"{label} step {number} started_at")
        completed = _canonical_timestamp(raw_completed, f"{label} step {number} completed_at")
    return {
        "number": number,
        "name": name,
        "status": status,
        "conclusion": conclusion,
        "startedAt": started,
        "completedAt": completed,
    }


def _job_inventory_digest(jobs: list[dict[str, Any]], label: str) -> str:
    canonical_jobs: list[dict[str, Any]] = []
    for job in jobs:
        job_id = _positive_int(job.get("id"), f"{label} job id")
        name = job.get("name")
        if not isinstance(name, str) or not name:
            raise ValueError(f"{label} job {job_id} has no exact name")
        steps = job.get("steps")
        if not isinstance(steps, list) or any(not isinstance(step, dict) for step in steps):
            raise ValueError(f"{label} job '{name}' has a malformed step inventory")
        canonical_steps = [_canonical_step(step, f"{label} job '{name}'") for step in steps]
        step_numbers = [step["number"] for step in canonical_steps]
        if len(step_numbers) != len(set(step_numbers)):
            raise ValueError(f"{label} job '{name}' contains duplicate step numbers")
        canonical_steps.sort(key=lambda step: (step["number"], step["name"]))
        status = job.get("status")
        conclusion = job.get("conclusion")
        if not isinstance(status, str) or not status or not isinstance(conclusion, str) or not conclusion:
            raise ValueError(f"{label} job '{name}' has no exact terminal state")
        head_sha = str(job.get("head_sha", "")).lower()
        if not SHA_PATTERN.fullmatch(head_sha):
            raise ValueError(f"{label} job '{name}' has no exact head SHA")
        canonical_jobs.append(
            {
                "id": job_id,
                "runId": _positive_int(job.get("run_id"), f"{label} job '{name}' run id"),
                "runAttempt": _positive_int(
                    job.get("run_attempt"), f"{label} job '{name}' run attempt"
                ),
                "headSha": head_sha,
                "workflowName": str(job.get("workflow_name", "")),
                "headBranch": str(job.get("head_branch", "")),
                "runUrl": str(job.get("run_url", "")),
                "name": name,
                "status": status,
                "conclusion": conclusion,
                "startedAt": _canonical_timestamp(
                    job.get("started_at"), f"{label} job '{name}' started_at"
                ),
                "completedAt": _canonical_timestamp(
                    job.get("completed_at"), f"{label} job '{name}' completed_at"
                ),
                "steps": canonical_steps,
            }
        )
    canonical_jobs.sort(key=lambda job: (job["name"], job["id"]))
    payload = json.dumps(
        canonical_jobs, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def _normalize_workflow_path(value: Any) -> str:
    text = str(value or "").split("@", 1)[0].replace("\\", "/")
    marker = "/.github/workflows/"
    offset = text.find(marker)
    return text[offset + 1 :] if offset >= 0 else text.removeprefix("./")


def _exact_repository(candidate: Any, repository_id: int, full_name: str) -> bool:
    return (
        isinstance(candidate, dict)
        and candidate.get("id") == repository_id
        and candidate.get("full_name") == full_name
    )


def _source_identity(run: dict[str, Any]) -> tuple[int, int, int]:
    return (
        _positive_int(run.get("id"), "source run id"),
        _positive_int(run.get("run_number"), "source run number"),
        _positive_int(run.get("run_attempt"), "source run attempt"),
    )


def _validate_source_run(
    run: dict[str, Any], repository_id: int, repository: str, target_sha: str
) -> tuple[int, int, int]:
    identity = _source_identity(run)
    _positive_int(run.get("workflow_id"), "Build source workflow id")
    if (
        run.get("name") != SOURCE_WORKFLOW_NAME
        or _normalize_workflow_path(run.get("path")) != SOURCE_WORKFLOW_PATH
        or run.get("event") not in ALLOWED_EVENTS
        or run.get("head_branch") != SOURCE_BRANCH
        or str(run.get("head_sha", "")).lower() != target_sha
        or not _exact_repository(run.get("repository"), repository_id, repository)
        or not _exact_repository(run.get("head_repository"), repository_id, repository)
        or run.get("html_url") != f"https://github.com/{repository}/actions/runs/{identity[0]}"
    ):
        raise ValueError("Build run identity is not the exact base-repository Working workflow")
    return identity


def _workflow_run_inventory(
    fetch_json: FetchJson,
    repository: str,
    target_sha: str,
    workflow_file: str,
    label: str,
) -> list[dict[str, Any]]:
    runs: list[dict[str, Any]] = []
    expected_total: int | None = None
    max_pages = MAX_SOURCE_RUNS // MAX_API_ITEMS
    for page in range(1, max_pages + 1):
        query = urlencode({"head_sha": target_sha, "per_page": MAX_API_ITEMS, "page": page})
        payload = _object(
            fetch_json(f"/repos/{repository}/actions/workflows/{workflow_file}/runs?{query}"),
            f"{label} workflow runs page {page}",
        )
        page_runs = payload.get("workflow_runs")
        total_count = payload.get("total_count")
        if (
            not isinstance(page_runs, list)
            or any(not isinstance(run, dict) for run in page_runs)
            or not isinstance(total_count, int)
            or isinstance(total_count, bool)
            or total_count < 0
            or total_count > MAX_SOURCE_RUNS
            or len(page_runs) > MAX_API_ITEMS
        ):
            raise ValueError(
                f"{label} workflow run inventory is malformed or exceeds {MAX_SOURCE_RUNS} items"
            )
        if expected_total is None:
            expected_total = total_count
        elif total_count != expected_total:
            raise ValueError(f"{label} workflow run total_count changed during pagination")
        if len(runs) + len(page_runs) > expected_total:
            raise ValueError(f"{label} workflow run pages exceed the declared inventory")
        runs.extend(page_runs)
        if len(runs) == expected_total:
            break
        if len(page_runs) != MAX_API_ITEMS:
            raise ValueError(f"{label} workflow run inventory is incomplete: page ended before total_count")
    if expected_total is None or len(runs) != expected_total:
        raise ValueError(f"{label} workflow run inventory is incomplete or exceeds {MAX_SOURCE_RUNS} items")
    return runs


def _latest_source_run(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
) -> dict[str, Any]:
    runs = _workflow_run_inventory(
        fetch_json, repository, target_sha, "build.yml", "Build"
    )

    candidates: list[dict[str, Any]] = []
    run_ids: set[int] = set()
    execution_keys: set[tuple[int, int]] = set()
    for run in runs:
        if str(run.get("head_sha", "")).lower() != target_sha:
            raise ValueError("same-commit Build inventory contains a different commit")
        identity = _source_identity(run)
        if identity[0] in run_ids or identity[1:] in execution_keys:
            raise ValueError("same-commit Build inventory contains a duplicate source identity")
        run_ids.add(identity[0])
        execution_keys.add(identity[1:])
        if run.get("event") not in KNOWN_SOURCE_EVENTS:
            raise ValueError("same-commit Build inventory contains an unsupported event")
        if run.get("event") not in ALLOWED_EVENTS:
            continue
        _validate_source_run(run, repository_id, repository, target_sha)
        candidates.append(run)
    if not candidates:
        raise ValueError(f"no exact push/workflow_dispatch Build run exists for {target_sha}")
    latest = max(candidates, key=lambda item: (_source_identity(item)[1], _source_identity(item)[2]))
    if latest.get("status") != "completed" or latest.get("conclusion") != "success":
        raise ValueError("newest exact Build attempt is not a completed successful run")
    return latest


def _live_source_run(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
    selected: dict[str, Any],
) -> dict[str, Any]:
    selected_identity = _source_identity(selected)
    live = _object(
        fetch_json(f"/repos/{repository}/actions/runs/{selected_identity[0]}"),
        "exact Build run",
    )
    if _source_identity(live) != selected_identity:
        raise ValueError("exact Build run changed after inventory selection")
    _validate_source_run(live, repository_id, repository, target_sha)
    if _run_window(live, "live Build source run") != _run_window(
        selected, "selected Build source run"
    ):
        raise ValueError("exact Build run execution window changed after inventory selection")
    if live.get("status") != "completed" or live.get("conclusion") != "success":
        raise ValueError("exact Build run is no longer a completed successful run")
    return live


def _exact_step(job: dict[str, Any], name: str, conclusion: str) -> dict[str, Any]:
    steps = job.get("steps")
    if not isinstance(steps, list) or any(not isinstance(step, dict) for step in steps):
        raise ValueError(f"job '{job.get('name', '<unknown>')}' has a malformed step inventory")
    matches = [step for step in steps if step.get("name") == name]
    if len(matches) != 1 or matches[0].get("status") != "completed" or matches[0].get("conclusion") != conclusion:
        raise ValueError(f"step '{name}' did not conclude '{conclusion}' exactly once")
    return matches[0]


def _verified_step_window(
    job: dict[str, Any], step: dict[str, Any], label: str
) -> tuple[str, str]:
    """Return canonical step times only when the step is inside its exact job."""

    job_started = _timestamp(job.get("started_at"), f"{label} job started_at")
    job_completed = _timestamp(job.get("completed_at"), f"{label} job completed_at")
    step_started = _timestamp(step.get("started_at"), f"{label} step started_at")
    step_completed = _timestamp(step.get("completed_at"), f"{label} step completed_at")
    if not job_started <= step_started <= step_completed <= job_completed:
        raise ValueError(f"{label} step is outside its exact job window")
    return (
        _canonical_timestamp(step.get("started_at"), f"{label} step started_at"),
        _canonical_timestamp(step.get("completed_at"), f"{label} step completed_at"),
    )


def _verify_status_publish_window(
    status: dict[str, Any],
    job: dict[str, Any],
    upload_step_name: str,
    step_name: str,
    label: str,
) -> tuple[str, str, str, str]:
    """Bind an immutable commit status to the trusted step that published it."""

    created = _timestamp(status.get("created_at"), f"{label} created_at")
    updated = _timestamp(status.get("updated_at"), f"{label} updated_at")
    if updated < created:
        raise ValueError(f"{label} has an invalid status timestamp window")

    upload_step = _exact_step(job, upload_step_name, "success")
    step = _exact_step(job, step_name, "success")
    job_started = _timestamp(job.get("started_at"), f"{label} job started_at")
    job_completed = _timestamp(job.get("completed_at"), f"{label} job completed_at")
    upload_started = _timestamp(
        upload_step.get("started_at"), f"{label} durable upload step started_at"
    )
    upload_completed = _timestamp(
        upload_step.get("completed_at"), f"{label} durable upload step completed_at"
    )
    step_started = _timestamp(step.get("started_at"), f"{label} publish step started_at")
    step_completed = _timestamp(step.get("completed_at"), f"{label} publish step completed_at")
    if not (
        job_started <= upload_started <= upload_completed <= job_completed
        and job_started <= step_started <= step_completed <= job_completed
    ):
        raise ValueError(f"{label} durable upload or publish step is outside the trusted reporter job")

    tolerance = timedelta(seconds=1)
    if upload_completed > step_started + tolerance:
        raise ValueError(f"{label} was published before its durable evidence upload completed")
    if not (
        job_started - tolerance <= created <= updated <= job_completed + tolerance
        and step_started - tolerance <= created <= updated <= step_completed + tolerance
    ):
        raise ValueError(f"{label} was not created by the exact trusted publish step")
    return (
        _canonical_timestamp(
            upload_step.get("started_at"), f"{label} durable upload step started_at"
        ),
        _canonical_timestamp(
            upload_step.get("completed_at"), f"{label} durable upload step completed_at"
        ),
        _canonical_timestamp(step.get("started_at"), f"{label} publish step started_at"),
        _canonical_timestamp(step.get("completed_at"), f"{label} publish step completed_at"),
    )


def _validate_source_artifact(
    artifact: dict[str, Any],
    *,
    repository: str,
    repository_id: int,
    source: dict[str, Any],
    expected_name: str,
    language: str,
    upload_step_started_at: str,
    upload_step_completed_at: str,
    max_size: int,
    label: str,
) -> SourceArtifactEvidence:
    artifact_id = _positive_int(artifact.get("id"), f"{label} id")
    size = _positive_int(artifact.get("size_in_bytes"), f"{label} size")
    digest = str(artifact.get("digest", "")).lower()
    created = _timestamp(artifact.get("created_at"), f"{label} created_at")
    updated = _timestamp(artifact.get("updated_at"), f"{label} updated_at")
    expires = _timestamp(artifact.get("expires_at"), f"{label} expires_at")
    run_started, run_updated = _run_window(source, f"{label} source run")
    upload_started = _timestamp(upload_step_started_at, f"{label} upload step started_at")
    upload_completed = _timestamp(
        upload_step_completed_at, f"{label} upload step completed_at"
    )
    source_id, _, _ = _source_identity(source)
    source_sha = str(source.get("head_sha", "")).lower()
    provenance = artifact.get("workflow_run")
    expected_url = f"https://api.github.com/repos/{repository}/actions/artifacts/{artifact_id}"
    if (
        artifact.get("name") != expected_name
        or artifact.get("expired") is not False
        or size > max_size
        or not DIGEST_PATTERN.fullmatch(digest)
        or artifact.get("url") != expected_url
        or artifact.get("archive_download_url") != f"{expected_url}/zip"
        or not isinstance(artifact.get("node_id"), str)
        or not artifact["node_id"]
        or not run_started <= created <= updated <= run_updated
        or not upload_started - timedelta(seconds=1)
        <= created
        <= updated
        <= upload_completed + timedelta(seconds=1)
        or expires <= updated
        or not isinstance(provenance, dict)
        or provenance.get("id") != source_id
        or provenance.get("repository_id") != repository_id
        or provenance.get("head_repository_id") != repository_id
        or provenance.get("head_branch") != SOURCE_BRANCH
        or str(provenance.get("head_sha", "")).lower() != source_sha
    ):
        raise ValueError(f"{label} identity, digest, retention, timing, or provenance is invalid")
    signature = (
        artifact_id,
        artifact.get("node_id"),
        expected_name,
        size,
        expected_url,
        f"{expected_url}/zip",
        False,
        _canonical_timestamp(artifact.get("created_at"), f"{label} created_at"),
        _canonical_timestamp(artifact.get("updated_at"), f"{label} updated_at"),
        _canonical_timestamp(artifact.get("expires_at"), f"{label} expires_at"),
        digest,
        source_id,
        repository_id,
        repository_id,
        SOURCE_BRANCH,
        source_sha,
    )
    return SourceArtifactEvidence(
        language=language,
        artifact_id=artifact_id,
        name=expected_name,
        size=size,
        digest=digest,
        signature=signature,
    )


def _source_artifact_inventory(
    fetch_json: FetchJson, repository: str, source: dict[str, Any], label: str
) -> list[dict[str, Any]]:
    source_id, _, _ = _source_identity(source)
    artifacts = _bounded_collection(
        fetch_json(
            f"/repos/{repository}/actions/runs/{source_id}/artifacts"
            f"?per_page={MAX_API_ITEMS}"
        ),
        "artifacts",
        label,
    )
    artifact_ids: set[int] = set()
    artifact_names: set[str] = set()
    for artifact in artifacts:
        artifact_id = _positive_int(artifact.get("id"), f"{label} artifact id")
        artifact_name = str(artifact.get("name", ""))
        if not artifact_name or artifact_id in artifact_ids or artifact_name in artifact_names:
            raise ValueError(f"{label} contains a duplicate or unnamed artifact")
        artifact_ids.add(artifact_id)
        artifact_names.add(artifact_name)
    return artifacts


def _verify_build_matrix_source_artifact(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    source: dict[str, Any],
    jobs: BuildJobEvidence,
) -> SourceArtifactEvidence:
    _, _, attempt = _source_identity(source)
    source_sha = str(source.get("head_sha", "")).lower()
    expected_name = f"build-matrix-stable-v1-{source_sha}-{attempt}"
    artifacts = _source_artifact_inventory(
        fetch_json, repository, source, "Build source artifacts"
    )
    matches = [artifact for artifact in artifacts if artifact.get("name") == expected_name]
    if len(matches) != 1:
        raise ValueError("Build source run must retain one exact build-matrix source artifact")
    return _validate_source_artifact(
        matches[0],
        repository=repository,
        repository_id=repository_id,
        source=source,
        expected_name=expected_name,
        language="build-matrix",
        upload_step_started_at=jobs.source_upload_step_started_at,
        upload_step_completed_at=jobs.source_upload_step_completed_at,
        max_size=MAX_BUILD_MATRIX_SOURCE_ARTIFACT_BYTES,
        label="build-matrix source artifact",
    )


def _verify_codeql_source_artifacts(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    source: dict[str, Any],
    jobs: CodeQLSourceJobEvidence,
) -> tuple[SourceArtifactEvidence, ...]:
    _, _, attempt = _source_identity(source)
    artifacts = _source_artifact_inventory(
        fetch_json, repository, source, "CodeQL source artifacts"
    )
    languages = ("actions", "c-cpp", "python")
    expected_names = {
        language: f"codeql-{language}-attempt-{attempt}.sarif" for language in languages
    }
    if len(artifacts) != len(expected_names) or {
        str(artifact.get("name", "")) for artifact in artifacts
    } != set(expected_names.values()):
        raise ValueError("CodeQL source run must retain exactly three expected SARIF artifacts")
    evidence: list[SourceArtifactEvidence] = []
    upload_windows = {
        "actions": (
            jobs.actions_upload_step_started_at,
            jobs.actions_upload_step_completed_at,
        ),
        "c-cpp": (
            jobs.c_cpp_upload_step_started_at,
            jobs.c_cpp_upload_step_completed_at,
        ),
        "python": (
            jobs.python_upload_step_started_at,
            jobs.python_upload_step_completed_at,
        ),
    }
    for language in languages:
        name = expected_names[language]
        matches = [artifact for artifact in artifacts if artifact.get("name") == name]
        if len(matches) != 1:
            raise ValueError(f"CodeQL source run must retain one exact {language} SARIF artifact")
        evidence.append(
            _validate_source_artifact(
                matches[0],
                repository=repository,
                repository_id=repository_id,
                source=source,
                expected_name=name,
                language=language,
                upload_step_started_at=upload_windows[language][0],
                upload_step_completed_at=upload_windows[language][1],
                max_size=MAX_CODEQL_SOURCE_ARTIFACT_BYTES,
                label=f"CodeQL {language} source artifact",
            )
        )
    if sum(item.size for item in evidence) > MAX_CODEQL_SOURCE_ARTIFACT_TOTAL_BYTES:
        raise ValueError("CodeQL source artifacts exceed the total size limit")
    return tuple(evidence)


def _verify_source_jobs(
    fetch_json: FetchJson, repository: str, source: dict[str, Any]
) -> BuildJobEvidence:
    run_id, _, attempt = _source_identity(source)
    source_sha = str(source.get("head_sha", "")).lower()
    run_started, run_updated = _run_window(source, "Build source run")
    expected_run_url = f"https://api.github.com/repos/{repository}/actions/runs/{run_id}"
    jobs = _bounded_collection(
        fetch_json(
            f"/repos/{repository}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page={MAX_API_ITEMS}"
        ),
        "jobs",
        "exact Build attempt jobs",
    )
    job_ids: set[int] = set()
    job_names: set[str] = set()
    for job in jobs:
        job_id = _positive_int(job.get("id"), "Build job id")
        job_name = str(job.get("name", ""))
        if not job_name or job_id in job_ids or job_name in job_names:
            raise ValueError("exact Build attempt contains a duplicate or unnamed job")
        if (
            job.get("run_id") != run_id
            or job.get("run_attempt") != attempt
            or str(job.get("head_sha", "")).lower() != source_sha
            or job.get("workflow_name") != SOURCE_WORKFLOW_NAME
            or job.get("head_branch") != SOURCE_BRANCH
            or job.get("run_url") != expected_run_url
        ):
            raise ValueError("exact Build job provenance does not match the source run and commit")
        _verify_job_window(job, run_started, run_updated, f"Build job '{job_name}'")
        job_ids.add(job_id)
        job_names.add(job_name)

    source_jobs = [job for job in jobs if job.get("name") == SOURCE_JOB_NAME]
    gates = [job for job in jobs if job.get("name") == REQUIRED_GATE_NAME]
    if len(source_jobs) != 1 or len(gates) != 1:
        raise ValueError("exact Build attempt must contain one build-matrix producer and one Required CI Gate")
    source_job = source_jobs[0]
    gate = gates[0]
    if source_job.get("status") != "completed" or source_job.get("conclusion") != "success":
        raise ValueError("build-matrix producer is not the exact completed successful job")
    if gate.get("status") != "completed" or gate.get("conclusion") != "success":
        raise ValueError("Required CI Gate did not succeed in the exact Build attempt")
    for job in jobs:
        if job.get("status") != "completed" or job.get("conclusion") not in {"success", "skipped"}:
            raise ValueError(f"unexpected non-success Build job: {job.get('name', '<unknown>')}")
    for name in SOURCE_REQUIRED_STEPS:
        _exact_step(source_job, name, "success")
    _exact_step(source_job, SOURCE_FINAL_STEP, "success")
    unexpected = []
    seen_step_names: set[str] = set()
    for step in source_job["steps"]:
        step_name = str(step.get("name", ""))
        if not step_name or step_name in seen_step_names:
            raise ValueError("build-matrix producer contains a duplicate or unnamed step")
        seen_step_names.add(step_name)
        if step.get("status") != "completed":
            unexpected.append(step_name)
        elif step.get("conclusion") != "success":
            unexpected.append(step_name)
    if unexpected:
        raise ValueError(f"build-matrix producer has unexpected non-success steps: {unexpected}")
    source_upload = _exact_step(source_job, SOURCE_REQUIRED_STEPS[-1], "success")
    upload_started, upload_completed = _verified_step_window(
        source_job, source_upload, "build-matrix source upload"
    )
    return BuildJobEvidence(
        build_matrix_producer_job_id=int(source_job["id"]),
        required_gate_job_id=int(gate["id"]),
        inventory_digest=_job_inventory_digest(jobs, "Build attempt"),
        source_upload_step_started_at=upload_started,
        source_upload_step_completed_at=upload_completed,
    )


def verify_exact_staged_build(
    fetch_json: FetchJson,
    repository: str,
    target_sha: str,
    requested_run_id: int,
    requested_run_attempt: int,
) -> StagedBuildEvidence:
    """Verify the exact staged Build shape without trusting reporter state yet."""
    if not REPOSITORY_PATTERN.fullmatch(repository):
        raise ValueError("repository must have the form owner/name")
    if not SHA_PATTERN.fullmatch(target_sha):
        raise ValueError("target SHA must be a full 40-character hexadecimal commit ID")
    target_sha = target_sha.lower()
    requested_run_id = _positive_int(requested_run_id, "requested Build run id")
    requested_run_attempt = _positive_int(requested_run_attempt, "requested Build run attempt")

    repository_payload = _object(fetch_json(f"/repos/{repository}"), "repository")
    repository_id = _positive_int(repository_payload.get("id"), "repository id")
    if repository_payload.get("full_name") != repository or repository_payload.get("default_branch") != SOURCE_BRANCH:
        raise ValueError("repository identity or Working default branch is not exact")
    commit = _object(fetch_json(f"/repos/{repository}/commits/{target_sha}"), "target commit")
    if str(commit.get("sha", "")).lower() != target_sha:
        raise ValueError("target commit does not resolve exactly in the base repository")

    selected = _latest_source_run(fetch_json, repository, repository_id, target_sha)
    identity = _source_identity(selected)
    if identity[0] != requested_run_id or identity[2] != requested_run_attempt:
        raise ValueError("requested Build run id or attempt changed during resolution")
    source = _live_source_run(
        fetch_json, repository, repository_id, target_sha, selected
    )
    source_jobs = _verify_source_jobs(fetch_json, repository, source)
    source_artifact = _verify_build_matrix_source_artifact(
        fetch_json, repository, repository_id, source, source_jobs
    )

    final_selected = _latest_source_run(fetch_json, repository, repository_id, target_sha)
    final_source = _live_source_run(
        fetch_json, repository, repository_id, target_sha, final_selected
    )
    if _source_identity(final_source) != identity:
        raise ValueError("exact staged Build evidence changed during final revalidation")
    final_jobs = _verify_source_jobs(fetch_json, repository, final_source)
    final_source_artifact = _verify_build_matrix_source_artifact(
        fetch_json, repository, repository_id, final_source, final_jobs
    )
    if final_jobs != source_jobs or final_source_artifact.signature != source_artifact.signature:
        raise ValueError("exact staged Build job or artifact evidence changed during replay")

    return StagedBuildEvidence(
        run_id=identity[0],
        run_number=identity[1],
        run_attempt=identity[2],
        run_url=str(source.get("html_url", "")),
        event=str(source["event"]),
    )


def _trusted_status(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
    source: dict[str, Any],
) -> tuple[dict[str, Any], int, int]:
    payload = _object(
        fetch_json(f"/repos/{repository}/commits/{target_sha}/status?per_page={MAX_API_ITEMS}"),
        "combined commit status",
    )
    if str(payload.get("sha", "")).lower() != target_sha:
        raise ValueError("combined commit status does not identify the exact target commit")
    if not _exact_repository(payload.get("repository"), repository_id, repository):
        raise ValueError("combined commit status does not identify the exact repository")
    statuses = _bounded_collection(payload, "statuses", "combined commit status")
    matches = [status for status in statuses if status.get("context") == STATUS_CONTEXT]
    if len(matches) != 1:
        raise ValueError("combined commit status must contain one latest trusted build-matrix context")
    status = matches[0]
    run_id, _, source_attempt = _source_identity(source)
    expected_description = f"Trusted build-matrix verified for Build run {run_id}, attempt {source_attempt}."
    creator = status.get("creator")
    if (
        status.get("state") != "success"
        or status.get("description") != expected_description
        or status.get("url")
        != f"https://api.github.com/repos/{repository}/statuses/{target_sha}"
        or not isinstance(creator, dict)
        or creator.get("id") != GITHUB_ACTIONS_BOT_ID
        or creator.get("login") != "github-actions[bot]"
        or creator.get("type") != "Bot"
    ):
        raise ValueError("latest build-matrix status is not the exact trusted successful source binding")
    _positive_int(status.get("id"), "build-matrix status id")
    target_pattern = re.compile(
        rf"https://github\.com/{re.escape(repository)}/actions/runs/([1-9][0-9]*)/attempts/([1-9][0-9]*)"
    )
    target = target_pattern.fullmatch(str(status.get("target_url", "")))
    if not target:
        raise ValueError("trusted build-matrix status target is not an exact verifier run attempt")
    return status, int(target.group(1)), int(target.group(2))


def _status_signature(status: dict[str, Any]) -> tuple[Any, ...]:
    creator = status.get("creator")
    return (
        status.get("id"),
        status.get("context"),
        status.get("state"),
        status.get("description"),
        status.get("url"),
        status.get("target_url"),
        _canonical_timestamp(status.get("created_at"), "trusted status created_at"),
        _canonical_timestamp(status.get("updated_at"), "trusted status updated_at"),
        creator.get("id") if isinstance(creator, dict) else None,
        creator.get("login") if isinstance(creator, dict) else None,
        creator.get("type") if isinstance(creator, dict) else None,
    )


def _verify_verifier_run(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    verifier_sha: str,
    verifier_run_id: int,
    verifier_attempt: int,
) -> dict[str, Any]:
    run = _object(
        fetch_json(f"/repos/{repository}/actions/runs/{verifier_run_id}"),
        "build-matrix verifier run",
    )
    if (
        run.get("id") != verifier_run_id
        or run.get("run_attempt") != verifier_attempt
        or not isinstance(run.get("run_number"), int)
        or run["run_number"] < 1
        or run.get("name") != VERIFIER_WORKFLOW_NAME
        or _normalize_workflow_path(run.get("path")) != VERIFIER_WORKFLOW_PATH
        or run.get("event") != "workflow_run"
        or run.get("status") != "completed"
        or run.get("conclusion") != "success"
        or run.get("head_branch") != SOURCE_BRANCH
        or str(run.get("head_sha", "")).lower() != verifier_sha
        or not _exact_repository(run.get("repository"), repository_id, repository)
        or not _exact_repository(run.get("head_repository"), repository_id, repository)
    ):
        raise ValueError("status target is not the exact successful trusted build-matrix verifier run")
    expected_url = f"https://github.com/{repository}/actions/runs/{verifier_run_id}"
    if run.get("html_url") != expected_url:
        raise ValueError("trusted build-matrix verifier run URL is not exact")
    return run


def _verify_verifier_jobs(
    fetch_json: FetchJson,
    repository: str,
    verifier_sha: str,
    verifier_run_id: int,
    verifier_attempt: int,
    verifier_run: dict[str, Any],
    status: dict[str, Any],
) -> TrustedJobEvidence:
    run_started, run_updated = _run_window(verifier_run, "build-matrix verifier run")
    jobs = _bounded_collection(
        fetch_json(
            f"/repos/{repository}/actions/runs/{verifier_run_id}/attempts/{verifier_attempt}/jobs"
            f"?per_page={MAX_API_ITEMS}"
        ),
        "jobs",
        "exact build-matrix verifier attempt jobs",
    )
    expected_run_url = f"https://api.github.com/repos/{repository}/actions/runs/{verifier_run_id}"
    job_ids: set[int] = set()
    job_names: set[str] = set()
    for job in jobs:
        job_id = _positive_int(job.get("id"), "build-matrix verifier job id")
        job_name = str(job.get("name", ""))
        if not job_name or job_id in job_ids or job_name in job_names:
            raise ValueError("build-matrix verifier attempt contains a duplicate or unnamed job")
        if (
            job.get("run_id") != verifier_run_id
            or job.get("run_attempt") != verifier_attempt
            or str(job.get("head_sha", "")).lower() != verifier_sha
            or job.get("workflow_name") != VERIFIER_WORKFLOW_NAME
            or job.get("head_branch") != SOURCE_BRANCH
            or job.get("run_url") != expected_run_url
        ):
            raise ValueError("build-matrix verifier job provenance does not match the trusted run")
        _verify_job_window(job, run_started, run_updated, f"build-matrix verifier job '{job_name}'")
        job_ids.add(job_id)
        job_names.add(job_name)

    verify_jobs = [job for job in jobs if job.get("name") == VERIFIER_JOB_NAME]
    if len(verify_jobs) != 1:
        raise ValueError("trusted build-matrix verifier attempt must contain exactly one verification job")
    verify_job = verify_jobs[0]
    if verify_job.get("status") != "completed" or verify_job.get("conclusion") != "success":
        raise ValueError("trusted build-matrix verification job did not succeed")
    for job in jobs:
        if job.get("status") != "completed" or job.get("conclusion") not in {"success", "skipped"}:
            raise ValueError(f"unexpected non-success verifier job: {job.get('name', '<unknown>')}")
    for name in VERIFIER_REQUIRED_STEPS:
        _exact_step(verify_job, name, "success")
    seen_step_names: set[str] = set()
    unexpected_steps = []
    for step in verify_job.get("steps", []):
        step_name = str(step.get("name", ""))
        if not step_name or step_name in seen_step_names:
            raise ValueError("build-matrix verifier contains a duplicate or unnamed step")
        seen_step_names.add(step_name)
        if step.get("status") != "completed" or step.get("conclusion") != "success":
            unexpected_steps.append(step_name)
    if unexpected_steps:
        raise ValueError(f"build-matrix verifier has unexpected non-success steps: {unexpected_steps}")
    upload_started, upload_completed, publish_started, publish_completed = (
        _verify_status_publish_window(
            status,
            verify_job,
            VERIFIER_REQUIRED_STEPS[-2],
            VERIFIER_REQUIRED_STEPS[-1],
            "trusted build-matrix status",
        )
    )
    return TrustedJobEvidence(
        job_id=int(verify_job["id"]),
        inventory_digest=_job_inventory_digest(jobs, "build-matrix verifier attempt"),
        artifact_upload_step_started_at=upload_started,
        artifact_upload_step_completed_at=upload_completed,
        status_publish_step_started_at=publish_started,
        status_publish_step_completed_at=publish_completed,
    )


def _verify_artifact_inventory_identities(
    artifacts: list[dict[str, Any]], label: str
) -> None:
    artifact_ids: set[int] = set()
    artifact_names: set[str] = set()
    for artifact in artifacts:
        artifact_id = _positive_int(artifact.get("id"), f"{label} artifact id")
        artifact_name = artifact.get("name")
        if (
            not isinstance(artifact_name, str)
            or not artifact_name
            or artifact_id in artifact_ids
            or artifact_name in artifact_names
        ):
            raise ValueError(f"{label} contains a duplicate or unnamed artifact")
        artifact_ids.add(artifact_id)
        artifact_names.add(artifact_name)


def _validate_trusted_artifact(
    artifact: dict[str, Any],
    *,
    repository: str,
    repository_id: int,
    reporter_sha: str,
    reporter_run: dict[str, Any],
    reporter_run_id: int,
    reporter_attempt: int,
    reporter_jobs: TrustedJobEvidence,
    expected_name: str,
    max_size: int,
    label: str,
) -> TrustedArtifactEvidence:
    artifact_id = _positive_int(artifact.get("id"), f"{label} id")
    size = _positive_int(artifact.get("size_in_bytes"), f"{label} size")
    digest = str(artifact.get("digest", "")).lower()
    created = _timestamp(artifact.get("created_at"), f"{label} created_at")
    updated = _timestamp(artifact.get("updated_at"), f"{label} updated_at")
    expires = _timestamp(artifact.get("expires_at"), f"{label} expires_at")
    run_started, run_updated = _run_window(reporter_run, f"{label} reporter run")
    upload_started = _timestamp(
        reporter_jobs.artifact_upload_step_started_at, f"{label} upload step started_at"
    )
    upload_completed = _timestamp(
        reporter_jobs.artifact_upload_step_completed_at, f"{label} upload step completed_at"
    )
    provenance = artifact.get("workflow_run")
    expected_url = f"https://api.github.com/repos/{repository}/actions/artifacts/{artifact_id}"
    tolerance = timedelta(seconds=1)
    if (
        artifact.get("name") != expected_name
        or artifact.get("expired") is not False
        or size > max_size
        or not DIGEST_PATTERN.fullmatch(digest)
        or artifact.get("url") != expected_url
        or artifact.get("archive_download_url") != f"{expected_url}/zip"
        or not isinstance(artifact.get("node_id"), str)
        or not artifact["node_id"]
        or not run_started <= created <= updated <= run_updated
        or not upload_started - tolerance <= created <= updated <= upload_completed + tolerance
        or expires <= updated
        or not isinstance(provenance, dict)
        or provenance.get("id") != reporter_run_id
        or provenance.get("repository_id") != repository_id
        or provenance.get("head_repository_id") != repository_id
        or provenance.get("head_branch") != SOURCE_BRANCH
        or str(provenance.get("head_sha", "")).lower() != reporter_sha
    ):
        raise ValueError(f"{label} identity, digest, retention, timing, or provenance is invalid")
    signature = (
        artifact_id,
        artifact.get("node_id"),
        expected_name,
        size,
        expected_url,
        f"{expected_url}/zip",
        False,
        _canonical_timestamp(artifact.get("created_at"), f"{label} created_at"),
        _canonical_timestamp(artifact.get("updated_at"), f"{label} updated_at"),
        _canonical_timestamp(artifact.get("expires_at"), f"{label} expires_at"),
        digest,
        reporter_run_id,
        reporter_attempt,
        repository_id,
        repository_id,
        SOURCE_BRANCH,
        reporter_sha,
    )
    return TrustedArtifactEvidence(
        artifact_id=artifact_id,
        size=size,
        digest=digest,
        signature=signature,
    )


def _verify_receipt_artifact(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
    verifier_sha: str,
    source: dict[str, Any],
    verifier_run_id: int,
    verifier_attempt: int,
    verifier_run: dict[str, Any],
    verifier_jobs: TrustedJobEvidence,
) -> TrustedArtifactEvidence:
    artifacts = _bounded_collection(
        fetch_json(
            f"/repos/{repository}/actions/runs/{verifier_run_id}/artifacts?per_page={MAX_API_ITEMS}"
        ),
        "artifacts",
        "build-matrix verifier artifacts",
    )
    _verify_artifact_inventory_identities(artifacts, "build-matrix verifier artifacts")
    source_run_id, _, source_attempt = _source_identity(source)
    expected_name = (
        f"build-matrix-trusted-receipt-{target_sha}-{source_run_id}-{source_attempt}-{verifier_attempt}"
    )
    matches = [artifact for artifact in artifacts if artifact.get("name") == expected_name]
    if len(matches) != 1:
        raise ValueError("trusted verifier must retain one exact source-bound build-matrix receipt artifact")
    return _validate_trusted_artifact(
        matches[0],
        repository=repository,
        repository_id=repository_id,
        reporter_sha=verifier_sha,
        reporter_run=verifier_run,
        reporter_run_id=verifier_run_id,
        reporter_attempt=verifier_attempt,
        reporter_jobs=verifier_jobs,
        expected_name=expected_name,
        max_size=16 * 1024 * 1024,
        label="trusted build-matrix receipt artifact",
    )


def _validate_codeql_source_run(
    run: dict[str, Any], repository_id: int, repository: str, target_sha: str
) -> tuple[int, int, int]:
    identity = _source_identity(run)
    _positive_int(run.get("workflow_id"), "CodeQL source workflow id")
    if (
        run.get("name") != CODEQL_SOURCE_WORKFLOW_NAME
        or _normalize_workflow_path(run.get("path")) != CODEQL_SOURCE_WORKFLOW_PATH
        or run.get("event") not in ALLOWED_EVENTS
        or run.get("head_branch") != SOURCE_BRANCH
        or str(run.get("head_sha", "")).lower() != target_sha
        or not _exact_repository(run.get("repository"), repository_id, repository)
        or not _exact_repository(run.get("head_repository"), repository_id, repository)
        or run.get("html_url") != f"https://github.com/{repository}/actions/runs/{identity[0]}"
    ):
        raise ValueError("CodeQL source run is not the exact base-repository Working workflow")
    return identity


def _latest_codeql_source_run(
    fetch_json: FetchJson, repository: str, repository_id: int, target_sha: str
) -> dict[str, Any]:
    runs = _workflow_run_inventory(
        fetch_json, repository, target_sha, "codeql.yml", "CodeQL"
    )
    candidates: list[dict[str, Any]] = []
    run_ids: set[int] = set()
    execution_keys: set[tuple[int, int]] = set()
    for run in runs:
        if str(run.get("head_sha", "")).lower() != target_sha:
            raise ValueError("same-commit CodeQL inventory contains a different commit")
        identity = _source_identity(run)
        if identity[0] in run_ids or identity[1:] in execution_keys:
            raise ValueError("same-commit CodeQL inventory contains a duplicate source identity")
        run_ids.add(identity[0])
        execution_keys.add(identity[1:])
        if run.get("event") not in KNOWN_SOURCE_EVENTS:
            raise ValueError("same-commit CodeQL inventory contains an unsupported event")
        if run.get("event") not in ALLOWED_EVENTS:
            continue
        _validate_codeql_source_run(run, repository_id, repository, target_sha)
        candidates.append(run)
    if not candidates:
        raise ValueError(f"no exact push/workflow_dispatch CodeQL run exists for {target_sha}")
    latest = max(candidates, key=lambda item: (_source_identity(item)[1], _source_identity(item)[2]))
    if latest.get("status") != "completed" or latest.get("conclusion") != "success":
        raise ValueError("newest exact CodeQL attempt is not completed successfully")
    return latest


def _live_codeql_source_run(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
    selected: dict[str, Any],
) -> dict[str, Any]:
    selected_identity = _source_identity(selected)
    live = _object(
        fetch_json(f"/repos/{repository}/actions/runs/{selected_identity[0]}"),
        "exact CodeQL source run",
    )
    if _source_identity(live) != selected_identity:
        raise ValueError("exact CodeQL source run changed after inventory selection")
    _validate_codeql_source_run(live, repository_id, repository, target_sha)
    if _run_window(live, "live CodeQL source run") != _run_window(
        selected, "selected CodeQL source run"
    ):
        raise ValueError("exact CodeQL source run execution window changed after inventory selection")
    if live.get("status") != "completed" or live.get("conclusion") != "success":
        raise ValueError("exact CodeQL source run is no longer completed successfully")
    return live


def _all_steps_success(job: dict[str, Any], label: str) -> None:
    steps = job.get("steps")
    if not isinstance(steps, list) or any(not isinstance(step, dict) for step in steps):
        raise ValueError(f"{label} has a malformed step inventory")
    names: set[str] = set()
    for step in steps:
        name = str(step.get("name", ""))
        if not name or name in names:
            raise ValueError(f"{label} contains a duplicate or unnamed step")
        names.add(name)
        if step.get("status") != "completed" or step.get("conclusion") != "success":
            raise ValueError(f"{label} contains an unexpected non-success step '{name}'")


def _verify_codeql_source_jobs(
    fetch_json: FetchJson, repository: str, source: dict[str, Any]
) -> CodeQLSourceJobEvidence:
    run_id, _, attempt = _source_identity(source)
    source_sha = str(source.get("head_sha", "")).lower()
    run_started, run_updated = _run_window(source, "CodeQL source run")
    jobs = _bounded_collection(
        fetch_json(
            f"/repos/{repository}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page={MAX_API_ITEMS}"
        ),
        "jobs",
        "exact CodeQL source attempt jobs",
    )
    expected_run_url = f"https://api.github.com/repos/{repository}/actions/runs/{run_id}"
    job_ids: set[int] = set()
    job_names: set[str] = set()
    for job in jobs:
        job_id = _positive_int(job.get("id"), "CodeQL source job id")
        job_name = str(job.get("name", ""))
        if not job_name or job_id in job_ids or job_name in job_names:
            raise ValueError("CodeQL source attempt contains a duplicate or unnamed job")
        if (
            job.get("run_id") != run_id
            or job.get("run_attempt") != attempt
            or str(job.get("head_sha", "")).lower() != source_sha
            or job.get("workflow_name") != CODEQL_SOURCE_WORKFLOW_NAME
            or job.get("head_branch") != SOURCE_BRANCH
            or job.get("run_url") != expected_run_url
        ):
            raise ValueError("CodeQL source job provenance does not match the trusted source run")
        _verify_job_window(job, run_started, run_updated, f"CodeQL source job '{job_name}'")
        if job.get("status") != "completed" or job.get("conclusion") != "success":
            raise ValueError(f"CodeQL source job did not succeed: {job_name}")
        for step_name in CODEQL_SOURCE_REQUIRED_STEPS:
            _exact_step(job, step_name, "success")
        _all_steps_success(job, f"CodeQL source job '{job_name}'")
        job_ids.add(job_id)
        job_names.add(job_name)
    if job_names != CODEQL_SOURCE_JOB_NAMES:
        raise ValueError("CodeQL source attempt does not contain the exact three analyzed languages")
    ids = {str(job["name"]): int(job["id"]) for job in jobs}
    uploads = {
        str(job["name"]): _exact_step(job, CODEQL_SOURCE_REQUIRED_STEPS[-1], "success")
        for job in jobs
    }
    jobs_by_name = {str(job["name"]): job for job in jobs}
    def upload_time(job_name: str, key: str) -> str:
        window = _verified_step_window(
            jobs_by_name[job_name], uploads[job_name], f"CodeQL {job_name} upload"
        )
        return window[0 if key == "started_at" else 1]
    return CodeQLSourceJobEvidence(
        actions_job_id=ids["Analyze (actions)"],
        c_cpp_job_id=ids["Analyze (c-cpp)"],
        python_job_id=ids["Analyze (python)"],
        inventory_digest=_job_inventory_digest(jobs, "CodeQL source attempt"),
        actions_upload_step_started_at=upload_time("Analyze (actions)", "started_at"),
        actions_upload_step_completed_at=upload_time("Analyze (actions)", "completed_at"),
        c_cpp_upload_step_started_at=upload_time("Analyze (c-cpp)", "started_at"),
        c_cpp_upload_step_completed_at=upload_time("Analyze (c-cpp)", "completed_at"),
        python_upload_step_started_at=upload_time("Analyze (python)", "started_at"),
        python_upload_step_completed_at=upload_time("Analyze (python)", "completed_at"),
    )


def _codeql_trusted_status(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
    source: dict[str, Any],
) -> tuple[dict[str, Any], int, int]:
    payload = _object(
        fetch_json(f"/repos/{repository}/commits/{target_sha}/status?per_page={MAX_API_ITEMS}"),
        "combined commit status",
    )
    if str(payload.get("sha", "")).lower() != target_sha or not _exact_repository(
        payload.get("repository"), repository_id, repository
    ):
        raise ValueError("combined commit status does not identify the exact CodeQL target")
    statuses = _bounded_collection(payload, "statuses", "combined commit status")
    matches = [status for status in statuses if status.get("context") == CODEQL_STATUS_CONTEXT]
    if len(matches) != 1:
        raise ValueError("combined commit status must contain one latest trusted CodeQL context")
    status = matches[0]
    source_run_id, _, source_attempt = _source_identity(source)
    expected_description = (
        f"Trusted CodeQL verified for CodeQL run {source_run_id}, attempt {source_attempt}."
    )
    creator = status.get("creator")
    if (
        status.get("state") != "success"
        or status.get("description") != expected_description
        or status.get("url")
        != f"https://api.github.com/repos/{repository}/statuses/{target_sha}"
        or not isinstance(creator, dict)
        or creator.get("id") != GITHUB_ACTIONS_BOT_ID
        or creator.get("login") != "github-actions[bot]"
        or creator.get("type") != "Bot"
    ):
        raise ValueError("latest CodeQL status is not the exact trusted successful source binding")
    _positive_int(status.get("id"), "CodeQL status id")
    target_pattern = re.compile(
        rf"https://github\.com/{re.escape(repository)}/actions/runs/([1-9][0-9]*)/attempts/([1-9][0-9]*)"
    )
    target = target_pattern.fullmatch(str(status.get("target_url", "")))
    if not target:
        raise ValueError("trusted CodeQL status target is not an exact reporter run attempt")
    return status, int(target.group(1)), int(target.group(2))


def _verify_codeql_reporter_run(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    reporter_sha: str,
    reporter_run_id: int,
    reporter_attempt: int,
) -> dict[str, Any]:
    run = _object(
        fetch_json(f"/repos/{repository}/actions/runs/{reporter_run_id}"),
        "CodeQL reporter run",
    )
    if (
        run.get("id") != reporter_run_id
        or run.get("run_attempt") != reporter_attempt
        or not isinstance(run.get("run_number"), int)
        or isinstance(run.get("run_number"), bool)
        or run["run_number"] < 1
        or run.get("name") != CODEQL_REPORTER_WORKFLOW_NAME
        or _normalize_workflow_path(run.get("path")) != CODEQL_REPORTER_WORKFLOW_PATH
        or run.get("event") != "workflow_run"
        or run.get("status") != "completed"
        or run.get("conclusion") != "success"
        or run.get("head_branch") != SOURCE_BRANCH
        or str(run.get("head_sha", "")).lower() != reporter_sha
        or not _exact_repository(run.get("repository"), repository_id, repository)
        or not _exact_repository(run.get("head_repository"), repository_id, repository)
        or run.get("html_url") != f"https://github.com/{repository}/actions/runs/{reporter_run_id}"
    ):
        raise ValueError("status target is not the exact successful trusted CodeQL reporter run")
    return run


def _verify_codeql_reporter_jobs(
    fetch_json: FetchJson,
    repository: str,
    reporter_sha: str,
    reporter_run_id: int,
    reporter_attempt: int,
    reporter_run: dict[str, Any],
    status: dict[str, Any],
) -> TrustedJobEvidence:
    run_started, run_updated = _run_window(reporter_run, "CodeQL reporter run")
    jobs = _bounded_collection(
        fetch_json(
            f"/repos/{repository}/actions/runs/{reporter_run_id}/attempts/{reporter_attempt}/jobs"
            f"?per_page={MAX_API_ITEMS}"
        ),
        "jobs",
        "exact CodeQL reporter attempt jobs",
    )
    expected_run_url = f"https://api.github.com/repos/{repository}/actions/runs/{reporter_run_id}"
    job_ids: set[int] = set()
    job_names: set[str] = set()
    for job in jobs:
        job_id = _positive_int(job.get("id"), "CodeQL reporter job id")
        job_name = str(job.get("name", ""))
        if not job_name or job_id in job_ids or job_name in job_names:
            raise ValueError("CodeQL reporter attempt contains a duplicate or unnamed job")
        if (
            job.get("run_id") != reporter_run_id
            or job.get("run_attempt") != reporter_attempt
            or str(job.get("head_sha", "")).lower() != reporter_sha
            or job.get("workflow_name") != CODEQL_REPORTER_WORKFLOW_NAME
            or job.get("head_branch") != SOURCE_BRANCH
            or job.get("run_url") != expected_run_url
        ):
            raise ValueError("CodeQL reporter job provenance does not match the trusted reporter run")
        _verify_job_window(job, run_started, run_updated, f"CodeQL reporter job '{job_name}'")
        if job.get("status") != "completed" or job.get("conclusion") not in {"success", "skipped"}:
            raise ValueError(f"unexpected non-success CodeQL reporter job: {job_name}")
        job_ids.add(job_id)
        job_names.add(job_name)
    report_jobs = [job for job in jobs if job.get("name") == CODEQL_REPORTER_JOB_NAME]
    if len(report_jobs) != 1 or report_jobs[0].get("conclusion") != "success":
        raise ValueError("trusted CodeQL reporter attempt must contain one successful report job")
    report_job = report_jobs[0]
    for step_name in CODEQL_REPORTER_REQUIRED_STEPS:
        _exact_step(report_job, step_name, "success")
    _all_steps_success(report_job, "trusted CodeQL reporter job")
    upload_started, upload_completed, publish_started, publish_completed = (
        _verify_status_publish_window(
            status,
            report_job,
            CODEQL_REPORTER_REQUIRED_STEPS[-2],
            CODEQL_REPORTER_REQUIRED_STEPS[-1],
            "trusted CodeQL status",
        )
    )
    return TrustedJobEvidence(
        job_id=int(report_job["id"]),
        inventory_digest=_job_inventory_digest(jobs, "CodeQL reporter attempt"),
        artifact_upload_step_started_at=upload_started,
        artifact_upload_step_completed_at=upload_completed,
        status_publish_step_started_at=publish_started,
        status_publish_step_completed_at=publish_completed,
    )


def _verify_codeql_summary_artifact(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
    reporter_sha: str,
    source: dict[str, Any],
    reporter_run_id: int,
    reporter_attempt: int,
    reporter_run: dict[str, Any],
    reporter_jobs: TrustedJobEvidence,
) -> TrustedArtifactEvidence:
    artifacts = _bounded_collection(
        fetch_json(
            f"/repos/{repository}/actions/runs/{reporter_run_id}/artifacts?per_page={MAX_API_ITEMS}"
        ),
        "artifacts",
        "CodeQL reporter artifacts",
    )
    _verify_artifact_inventory_identities(artifacts, "CodeQL reporter artifacts")
    source_run_id, _, source_attempt = _source_identity(source)
    expected_name = (
        f"codeql-trusted-summary-{target_sha}-{source_run_id}-{source_attempt}-{reporter_attempt}"
    )
    matches = [artifact for artifact in artifacts if artifact.get("name") == expected_name]
    if len(matches) != 1:
        raise ValueError("trusted reporter must retain one exact source-bound CodeQL summary artifact")
    return _validate_trusted_artifact(
        matches[0],
        repository=repository,
        repository_id=repository_id,
        reporter_sha=reporter_sha,
        reporter_run=reporter_run,
        reporter_run_id=reporter_run_id,
        reporter_attempt=reporter_attempt,
        reporter_jobs=reporter_jobs,
        expected_name=expected_name,
        max_size=16 * 1024 * 1024,
        label="trusted CodeQL summary artifact",
    )


def _verify_codeql_evidence(
    fetch_json: FetchJson,
    repository: str,
    repository_id: int,
    target_sha: str,
    reporter_sha: str,
) -> CodeQLEvidence:
    selected_source = _latest_codeql_source_run(
        fetch_json, repository, repository_id, target_sha
    )
    source = _live_codeql_source_run(
        fetch_json, repository, repository_id, target_sha, selected_source
    )
    source_id, source_number, source_attempt = _source_identity(source)
    source_jobs = _verify_codeql_source_jobs(fetch_json, repository, source)
    source_artifacts = _verify_codeql_source_artifacts(
        fetch_json, repository, repository_id, source, source_jobs
    )

    status, reporter_run_id, reporter_attempt = _codeql_trusted_status(
        fetch_json, repository, repository_id, target_sha, source
    )
    reporter = _verify_codeql_reporter_run(
        fetch_json, repository, repository_id, reporter_sha, reporter_run_id, reporter_attempt
    )
    reporter_jobs = _verify_codeql_reporter_jobs(
        fetch_json, repository, reporter_sha, reporter_run_id, reporter_attempt, reporter,
        status
    )
    artifact = _verify_codeql_summary_artifact(
        fetch_json, repository, repository_id, target_sha, reporter_sha, source,
        reporter_run_id, reporter_attempt, reporter, reporter_jobs
    )

    final_selected_source = _latest_codeql_source_run(
        fetch_json, repository, repository_id, target_sha
    )
    final_source = _live_codeql_source_run(
        fetch_json, repository, repository_id, target_sha, final_selected_source
    )
    final_status, final_reporter_id, final_reporter_attempt = _codeql_trusted_status(
        fetch_json, repository, repository_id, target_sha, final_source
    )
    final_reporter = _verify_codeql_reporter_run(
        fetch_json, repository, repository_id, reporter_sha,
        final_reporter_id, final_reporter_attempt
    )
    final_source_jobs = _verify_codeql_source_jobs(fetch_json, repository, final_source)
    final_source_artifacts = _verify_codeql_source_artifacts(
        fetch_json, repository, repository_id, final_source, final_source_jobs
    )
    final_reporter_jobs = _verify_codeql_reporter_jobs(
        fetch_json, repository, reporter_sha, final_reporter_id, final_reporter_attempt,
        final_reporter, final_status
    )
    final_artifact = _verify_codeql_summary_artifact(
        fetch_json, repository, repository_id, target_sha, reporter_sha, final_source,
        final_reporter_id, final_reporter_attempt, final_reporter, final_reporter_jobs
    )
    final_default = _object(
        fetch_json(f"/repos/{repository}/commits/{SOURCE_BRANCH}"), "final CodeQL Working commit"
    )
    if (
        _source_identity(final_source) != _source_identity(source)
        or final_source_jobs != source_jobs
        or tuple(item.signature for item in final_source_artifacts)
        != tuple(item.signature for item in source_artifacts)
        or _status_signature(final_status) != _status_signature(status)
        or final_reporter_id != reporter_run_id
        or final_reporter_attempt != reporter_attempt
        or final_reporter.get("updated_at") != reporter.get("updated_at")
        or final_reporter_jobs != reporter_jobs
        or final_artifact.signature != artifact.signature
        or str(final_default.get("sha", "")).lower() != reporter_sha
    ):
        raise ValueError("exact CodeQL source/reporter evidence changed during final revalidation")

    return CodeQLEvidence(
        source_run_id=source_id,
        source_run_number=source_number,
        source_run_attempt=source_attempt,
        source_run_url=str(source.get("html_url", "")),
        actions_source_job_id=source_jobs.actions_job_id,
        c_cpp_source_job_id=source_jobs.c_cpp_job_id,
        python_source_job_id=source_jobs.python_job_id,
        source_job_inventory_digest=source_jobs.inventory_digest,
        source_artifacts=source_artifacts,
        status_id=int(status["id"]),
        status_signature=_status_signature(status),
        status_target_url=str(status["target_url"]),
        status_created_at=_canonical_timestamp(
            status.get("created_at"), "CodeQL status created_at"
        ),
        status_updated_at=_canonical_timestamp(
            status.get("updated_at"), "CodeQL status updated_at"
        ),
        reporter_run_id=reporter_run_id,
        reporter_run_attempt=reporter_attempt,
        reporter_run_url=str(reporter.get("html_url", "")),
        reporter_updated_at=str(reporter.get("updated_at", "")),
        trusted_reporter_job_id=reporter_jobs.job_id,
        reporter_job_inventory_digest=reporter_jobs.inventory_digest,
        status_publish_step_started_at=reporter_jobs.status_publish_step_started_at,
        status_publish_step_completed_at=reporter_jobs.status_publish_step_completed_at,
        summary_artifact_id=artifact.artifact_id,
        summary_artifact_digest=artifact.digest,
        summary_artifact_size=artifact.size,
        summary_artifact_signature=artifact.signature,
    )


def verify_exact_gate(fetch_json: FetchJson, repository: str, target_sha: str) -> GateEvidence:
    if not REPOSITORY_PATTERN.fullmatch(repository):
        raise ValueError("repository must have the form owner/name")
    if not SHA_PATTERN.fullmatch(target_sha):
        raise ValueError("target SHA must be a full 40-character hexadecimal commit ID")
    target_sha = target_sha.lower()

    repository_payload = _object(fetch_json(f"/repos/{repository}"), "repository")
    repository_id = _positive_int(repository_payload.get("id"), "repository id")
    if repository_payload.get("full_name") != repository or repository_payload.get("default_branch") != SOURCE_BRANCH:
        raise ValueError("repository identity or Working default branch is not exact")
    commit = _object(fetch_json(f"/repos/{repository}/commits/{target_sha}"), "target commit")
    if str(commit.get("sha", "")).lower() != target_sha:
        raise ValueError("target commit does not resolve exactly in the base repository")
    default_commit = _object(
        fetch_json(f"/repos/{repository}/commits/{SOURCE_BRANCH}"), "Working commit"
    )
    verifier_sha = str(default_commit.get("sha", "")).lower()
    if not SHA_PATTERN.fullmatch(verifier_sha):
        raise ValueError("current Working commit does not resolve exactly")

    selected_source = _latest_source_run(fetch_json, repository, repository_id, target_sha)
    source = _live_source_run(
        fetch_json, repository, repository_id, target_sha, selected_source
    )
    source_id, _, source_attempt = _source_identity(source)
    build_jobs = _verify_source_jobs(fetch_json, repository, source)
    build_matrix_source_artifact = _verify_build_matrix_source_artifact(
        fetch_json, repository, repository_id, source, build_jobs
    )

    status, verifier_run_id, verifier_attempt = _trusted_status(
        fetch_json, repository, repository_id, target_sha, source
    )
    verifier = _verify_verifier_run(
        fetch_json, repository, repository_id, verifier_sha, verifier_run_id, verifier_attempt
    )
    verifier_jobs = _verify_verifier_jobs(
        fetch_json, repository, verifier_sha, verifier_run_id, verifier_attempt, verifier,
        status
    )
    artifact = _verify_receipt_artifact(
        fetch_json, repository, repository_id, target_sha, verifier_sha, source,
        verifier_run_id, verifier_attempt, verifier, verifier_jobs
    )

    # Narrow the final mutation window: a new same-SHA attempt or replaced
    # reporter status invalidates this evidence before publication can proceed.
    final_selected_source = _latest_source_run(
        fetch_json, repository, repository_id, target_sha
    )
    final_source = _live_source_run(
        fetch_json, repository, repository_id, target_sha, final_selected_source
    )
    final_status, final_verifier_id, final_verifier_attempt = _trusted_status(
        fetch_json, repository, repository_id, target_sha, final_source
    )
    final_verifier = _verify_verifier_run(
        fetch_json, repository, repository_id, verifier_sha, final_verifier_id, final_verifier_attempt
    )
    final_build_jobs = _verify_source_jobs(fetch_json, repository, final_source)
    final_build_matrix_source_artifact = _verify_build_matrix_source_artifact(
        fetch_json, repository, repository_id, final_source, final_build_jobs
    )
    final_verifier_jobs = _verify_verifier_jobs(
        fetch_json, repository, verifier_sha, final_verifier_id, final_verifier_attempt,
        final_verifier, final_status
    )
    final_artifact = _verify_receipt_artifact(
        fetch_json, repository, repository_id, target_sha, verifier_sha, final_source,
        final_verifier_id, final_verifier_attempt, final_verifier, final_verifier_jobs
    )
    final_default = _object(
        fetch_json(f"/repos/{repository}/commits/{SOURCE_BRANCH}"), "final Working commit"
    )
    if (
        _source_identity(final_source) != _source_identity(source)
        or final_build_jobs != build_jobs
        or final_build_matrix_source_artifact.signature != build_matrix_source_artifact.signature
        or _status_signature(final_status) != _status_signature(status)
        or final_verifier_id != verifier_run_id
        or final_verifier_attempt != verifier_attempt
        or final_verifier.get("updated_at") != verifier.get("updated_at")
        or final_verifier_jobs != verifier_jobs
        or final_artifact.signature != artifact.signature
        or str(final_default.get("sha", "")).lower() != verifier_sha
    ):
        raise ValueError("exact CI/reporter evidence changed during final revalidation")

    codeql = _verify_codeql_evidence(
        fetch_json, repository, repository_id, target_sha, verifier_sha
    )

    # Cross-gate replay: neither evidence family may change while the other is
    # being checked. Re-read both sources, statuses, trusted runs, jobs,
    # artifacts, and Working before authorizing a publication boundary.
    cross_selected_source = _latest_source_run(
        fetch_json, repository, repository_id, target_sha
    )
    cross_source = _live_source_run(
        fetch_json, repository, repository_id, target_sha, cross_selected_source
    )
    cross_build_jobs = _verify_source_jobs(fetch_json, repository, cross_source)
    cross_build_matrix_source_artifact = _verify_build_matrix_source_artifact(
        fetch_json, repository, repository_id, cross_source, cross_build_jobs
    )
    cross_status, cross_verifier_id, cross_verifier_attempt = _trusted_status(
        fetch_json, repository, repository_id, target_sha, cross_source
    )
    cross_verifier = _verify_verifier_run(
        fetch_json, repository, repository_id, verifier_sha,
        cross_verifier_id, cross_verifier_attempt
    )
    cross_verifier_jobs = _verify_verifier_jobs(
        fetch_json, repository, verifier_sha, cross_verifier_id, cross_verifier_attempt,
        cross_verifier, cross_status
    )
    cross_artifact = _verify_receipt_artifact(
        fetch_json, repository, repository_id, target_sha, verifier_sha, cross_source,
        cross_verifier_id, cross_verifier_attempt, cross_verifier, cross_verifier_jobs
    )

    cross_selected_codeql_source = _latest_codeql_source_run(
        fetch_json, repository, repository_id, target_sha
    )
    cross_codeql_source = _live_codeql_source_run(
        fetch_json, repository, repository_id, target_sha, cross_selected_codeql_source
    )
    cross_codeql_source_jobs = _verify_codeql_source_jobs(
        fetch_json, repository, cross_codeql_source
    )
    cross_codeql_source_artifacts = _verify_codeql_source_artifacts(
        fetch_json, repository, repository_id, cross_codeql_source,
        cross_codeql_source_jobs
    )
    cross_codeql_status, cross_reporter_id, cross_reporter_attempt = _codeql_trusted_status(
        fetch_json, repository, repository_id, target_sha, cross_codeql_source
    )
    cross_reporter = _verify_codeql_reporter_run(
        fetch_json, repository, repository_id, verifier_sha,
        cross_reporter_id, cross_reporter_attempt
    )
    cross_codeql_reporter_jobs = _verify_codeql_reporter_jobs(
        fetch_json, repository, verifier_sha, cross_reporter_id, cross_reporter_attempt,
        cross_reporter, cross_codeql_status
    )
    cross_codeql_artifact = _verify_codeql_summary_artifact(
        fetch_json, repository, repository_id, target_sha, verifier_sha, cross_codeql_source,
        cross_reporter_id, cross_reporter_attempt, cross_reporter, cross_codeql_reporter_jobs
    )
    terminal_status_payload = _object(
        fetch_json(f"/repos/{repository}/commits/{target_sha}/status?per_page={MAX_API_ITEMS}"),
        "terminal combined commit status",
    )
    if (
        str(terminal_status_payload.get("sha", "")).lower() != target_sha
        or not _exact_repository(
            terminal_status_payload.get("repository"), repository_id, repository
        )
    ):
        raise ValueError("terminal combined status does not identify the exact repository commit")
    terminal_statuses = _bounded_collection(
        terminal_status_payload, "statuses", "terminal combined commit status"
    )
    terminal_ci = [item for item in terminal_statuses if item.get("context") == STATUS_CONTEXT]
    terminal_codeql = [
        item for item in terminal_statuses if item.get("context") == CODEQL_STATUS_CONTEXT
    ]
    if (
        len(terminal_ci) != 1
        or len(terminal_codeql) != 1
        or _status_signature(terminal_ci[0]) != _status_signature(status)
        or _status_signature(terminal_codeql[0]) != codeql.status_signature
    ):
        raise ValueError("trusted build-matrix or CodeQL status changed in the terminal shared snapshot")
    cross_default = _object(
        fetch_json(f"/repos/{repository}/commits/{SOURCE_BRANCH}"),
        "cross-gate final Working commit",
    )
    if (
        _source_identity(cross_source) != _source_identity(source)
        or cross_build_jobs != build_jobs
        or cross_build_matrix_source_artifact.signature != build_matrix_source_artifact.signature
        or _status_signature(cross_status) != _status_signature(status)
        or cross_verifier_id != verifier_run_id
        or cross_verifier_attempt != verifier_attempt
        or cross_verifier.get("updated_at") != verifier.get("updated_at")
        or cross_verifier_jobs != verifier_jobs
        or cross_artifact.signature != artifact.signature
        or _source_identity(cross_codeql_source)
        != (codeql.source_run_id, codeql.source_run_number, codeql.source_run_attempt)
        or cross_codeql_source_jobs.inventory_digest != codeql.source_job_inventory_digest
        or cross_codeql_source_jobs.actions_job_id != codeql.actions_source_job_id
        or cross_codeql_source_jobs.c_cpp_job_id != codeql.c_cpp_source_job_id
        or cross_codeql_source_jobs.python_job_id != codeql.python_source_job_id
        or tuple(item.signature for item in cross_codeql_source_artifacts)
        != tuple(item.signature for item in codeql.source_artifacts)
        or _status_signature(cross_codeql_status) != codeql.status_signature
        or cross_reporter_id != codeql.reporter_run_id
        or cross_reporter_attempt != codeql.reporter_run_attempt
        or str(cross_reporter.get("updated_at", "")) != codeql.reporter_updated_at
        or cross_codeql_reporter_jobs.job_id != codeql.trusted_reporter_job_id
        or cross_codeql_reporter_jobs.inventory_digest
        != codeql.reporter_job_inventory_digest
        or cross_codeql_reporter_jobs.status_publish_step_started_at
        != codeql.status_publish_step_started_at
        or cross_codeql_reporter_jobs.status_publish_step_completed_at
        != codeql.status_publish_step_completed_at
        or cross_codeql_artifact.signature != codeql.summary_artifact_signature
        or str(cross_default.get("sha", "")).lower() != verifier_sha
    ):
        raise ValueError("exact cross-gate evidence changed during final publication replay")

    evidence = GateEvidence(
        run_id=source_id,
        run_url=str(source.get("html_url", "")),
        event=str(source["event"]),
        run_attempt=source_attempt,
        build_build_matrix_producer_job_id=build_jobs.build_matrix_producer_job_id,
        build_required_gate_job_id=build_jobs.required_gate_job_id,
        build_job_inventory_digest=build_jobs.inventory_digest,
        build_matrix_source_artifact_id=build_matrix_source_artifact.artifact_id,
        build_matrix_source_artifact_digest=build_matrix_source_artifact.digest,
        build_matrix_source_artifact_bytes=build_matrix_source_artifact.size,
        build_matrix_status_id=int(status["id"]),
        build_matrix_status_target_url=str(status["target_url"]),
        build_matrix_status_created_at=_canonical_timestamp(
            status.get("created_at"), "build-matrix status created_at"
        ),
        build_matrix_status_updated_at=_canonical_timestamp(
            status.get("updated_at"), "build-matrix status updated_at"
        ),
        verifier_run_id=verifier_run_id,
        verifier_run_url=str(verifier.get("html_url", "")),
        verifier_run_attempt=verifier_attempt,
        verifier_sha=verifier_sha,
        build_matrix_trusted_verifier_job_id=verifier_jobs.job_id,
        build_matrix_verifier_job_inventory_digest=verifier_jobs.inventory_digest,
        build_matrix_status_publish_step_started_at=verifier_jobs.status_publish_step_started_at,
        build_matrix_status_publish_step_completed_at=verifier_jobs.status_publish_step_completed_at,
        receipt_artifact_id=artifact.artifact_id,
        receipt_artifact_digest=artifact.digest,
        receipt_artifact_bytes=artifact.size,
        codeql_run_id=codeql.source_run_id,
        codeql_run_attempt=codeql.source_run_attempt,
        codeql_run_url=codeql.source_run_url,
        codeql_actions_source_job_id=codeql.actions_source_job_id,
        codeql_c_cpp_source_job_id=codeql.c_cpp_source_job_id,
        codeql_python_source_job_id=codeql.python_source_job_id,
        codeql_source_job_inventory_digest=codeql.source_job_inventory_digest,
        codeql_actions_source_artifact_id=codeql.source_artifacts[0].artifact_id,
        codeql_actions_source_artifact_digest=codeql.source_artifacts[0].digest,
        codeql_actions_source_artifact_bytes=codeql.source_artifacts[0].size,
        codeql_c_cpp_source_artifact_id=codeql.source_artifacts[1].artifact_id,
        codeql_c_cpp_source_artifact_digest=codeql.source_artifacts[1].digest,
        codeql_c_cpp_source_artifact_bytes=codeql.source_artifacts[1].size,
        codeql_python_source_artifact_id=codeql.source_artifacts[2].artifact_id,
        codeql_python_source_artifact_digest=codeql.source_artifacts[2].digest,
        codeql_python_source_artifact_bytes=codeql.source_artifacts[2].size,
        codeql_status_id=codeql.status_id,
        codeql_status_target_url=codeql.status_target_url,
        codeql_status_created_at=codeql.status_created_at,
        codeql_status_updated_at=codeql.status_updated_at,
        codeql_reporter_run_id=codeql.reporter_run_id,
        codeql_reporter_run_attempt=codeql.reporter_run_attempt,
        codeql_reporter_run_url=codeql.reporter_run_url,
        codeql_trusted_reporter_job_id=codeql.trusted_reporter_job_id,
        codeql_reporter_job_inventory_digest=codeql.reporter_job_inventory_digest,
        codeql_status_publish_step_started_at=codeql.status_publish_step_started_at,
        codeql_status_publish_step_completed_at=codeql.status_publish_step_completed_at,
        codeql_summary_artifact_id=codeql.summary_artifact_id,
        codeql_summary_artifact_digest=codeql.summary_artifact_digest,
        codeql_summary_artifact_bytes=codeql.summary_artifact_size,
    )
    job_ids = (
        evidence.build_build_matrix_producer_job_id,
        evidence.build_required_gate_job_id,
        evidence.build_matrix_trusted_verifier_job_id,
        evidence.codeql_actions_source_job_id,
        evidence.codeql_c_cpp_source_job_id,
        evidence.codeql_python_source_job_id,
        evidence.codeql_trusted_reporter_job_id,
    )
    artifact_ids = (
        evidence.build_matrix_source_artifact_id,
        evidence.receipt_artifact_id,
        evidence.codeql_actions_source_artifact_id,
        evidence.codeql_c_cpp_source_artifact_id,
        evidence.codeql_python_source_artifact_id,
        evidence.codeql_summary_artifact_id,
    )
    run_ids = (
        evidence.run_id,
        evidence.verifier_run_id,
        evidence.codeql_run_id,
        evidence.codeql_reporter_run_id,
    )
    if len(set(run_ids)) != len(run_ids):
        raise ValueError("exact gate workflow run IDs are not globally unique")
    if len(set(job_ids)) != len(job_ids):
        raise ValueError("exact gate job IDs are not globally unique")
    if len(set(artifact_ids)) != len(artifact_ids):
        raise ValueError("exact gate artifact IDs are not globally unique")
    if evidence.build_matrix_status_id == evidence.codeql_status_id:
        raise ValueError("exact gate commit status IDs are not unique")
    return evidence


def github_fetcher(api_url: str, token: str) -> FetchJson:
    if not token:
        raise ValueError("GH_TOKEN is required")

    def fetch(path: str) -> Any:
        request = Request(
            api_url.rstrip("/") + path,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {token}",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "SparkEngine-exact-required-gate",
            },
        )
        try:
            with urlopen(request, timeout=30) as response:  # noqa: S310 - fixed GitHub API base.
                return json.load(response)
        except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
            raise RuntimeError(f"GitHub API request failed for {path}: {error}") from error

    return fetch


def main() -> int:
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    target_sha = os.environ.get("TARGET_SHA", "")
    staged_build_only = sys.argv[1:] == ["--staged-build-only"]
    if sys.argv[1:] and not staged_build_only:
        print("usage: verify-exact-required-gate.py [--staged-build-only]", file=sys.stderr)
        return 2
    try:
        fetch_json = github_fetcher(
            os.environ.get("GITHUB_API_URL", "https://api.github.com"),
            os.environ.get("GH_TOKEN", ""),
        )
        if staged_build_only:
            raw_run_id = os.environ.get("SOURCE_RUN_ID", "")
            raw_run_attempt = os.environ.get("SOURCE_RUN_ATTEMPT", "")
            if not raw_run_id.isdigit() or not raw_run_attempt.isdigit():
                raise ValueError("SOURCE_RUN_ID and SOURCE_RUN_ATTEMPT must be positive integers")
            staged_evidence = verify_exact_staged_build(
                fetch_json,
                repository,
                target_sha,
                int(raw_run_id),
                int(raw_run_attempt),
            )
        else:
            evidence = verify_exact_gate(fetch_json, repository, target_sha)
    except (RuntimeError, ValueError) as error:
        print(f"error: exact-SHA publication gate verification failed: {error}", file=sys.stderr)
        return 1

    output_path = os.environ.get("GITHUB_OUTPUT")
    if staged_build_only:
        print(
            f"Exact staged Build evidence certified {target_sha}: run {staged_evidence.run_id} "
            f"attempt {staged_evidence.run_attempt} ({staged_evidence.event})."
        )
        if output_path:
            with open(output_path, "a", encoding="utf-8", newline="\n") as stream:
                stream.write("conclusion=success\n")
                stream.write(f"run_id={staged_evidence.run_id}\n")
                stream.write(f"run_url={staged_evidence.run_url}\n")
                stream.write(f"run_attempt={staged_evidence.run_attempt}\n")
        return 0

    print(
        f"Exact publication evidence certified {target_sha}: Build run {evidence.run_id} "
        f"attempt {evidence.run_attempt} ({evidence.event}), build-matrix verifier run "
        f"{evidence.verifier_run_id} attempt {evidence.verifier_run_attempt}, receipt artifact "
        f"{evidence.receipt_artifact_id}; CodeQL run {evidence.codeql_run_id} attempt "
        f"{evidence.codeql_run_attempt}, reporter {evidence.codeql_reporter_run_id} attempt "
        f"{evidence.codeql_reporter_run_attempt}, summary artifact "
        f"{evidence.codeql_summary_artifact_id}."
    )
    if output_path:
        with open(output_path, "a", encoding="utf-8", newline="\n") as stream:
            for field in fields(evidence):
                value = getattr(evidence, field.name)
                if "\n" in str(value) or "\r" in str(value):
                    raise ValueError(f"exact evidence field {field.name} contains a line break")
                stream.write(f"{field.name}={value}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
