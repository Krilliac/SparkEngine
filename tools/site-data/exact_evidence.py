#!/usr/bin/env python3
"""Build and verify a durable manifest for SparkEngine's exact CI evidence."""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Any, Mapping


SCHEMA_VERSION = 2
MAX_MANIFEST_BYTES = 32 * 1024
MAX_GATE_OUTPUT_BYTES = 16 * 1024
REPOSITORY_PATTERN = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
SHA_PATTERN = re.compile(r"[0-9a-f]{40}")
DIGEST_PATTERN = re.compile(r"sha256:[0-9a-f]{64}")

CI120_STATUS_CONTEXT = "CI-120 Trusted / Exact Source"
CODEQL_STATUS_CONTEXT = "CodeQL Trusted / Exact Source"

ENV_FROM_GATE_KEY = {
    "run_id": "EXACT_BUILD_RUN_ID",
    "run_url": "EXACT_BUILD_RUN_URL",
    "run_attempt": "EXACT_BUILD_RUN_ATTEMPT",
    "event": "EXACT_BUILD_EVENT",
    "build_ci120_producer_job_id": "EXACT_BUILD_CI120_PRODUCER_JOB_ID",
    "build_required_gate_job_id": "EXACT_BUILD_REQUIRED_GATE_JOB_ID",
    "build_job_inventory_digest": "EXACT_BUILD_JOB_INVENTORY_DIGEST",
    "ci120_source_artifact_id": "EXACT_CI120_SOURCE_ARTIFACT_ID",
    "ci120_source_artifact_digest": "EXACT_CI120_SOURCE_ARTIFACT_DIGEST",
    "ci120_source_artifact_bytes": "EXACT_CI120_SOURCE_ARTIFACT_BYTES",
    "ci120_status_id": "EXACT_CI120_STATUS_ID",
    "ci120_status_target_url": "EXACT_CI120_STATUS_TARGET_URL",
    "ci120_status_created_at": "EXACT_CI120_STATUS_CREATED_AT",
    "ci120_status_updated_at": "EXACT_CI120_STATUS_UPDATED_AT",
    "verifier_run_id": "EXACT_CI120_VERIFIER_RUN_ID",
    "verifier_run_url": "EXACT_CI120_VERIFIER_RUN_URL",
    "verifier_run_attempt": "EXACT_CI120_VERIFIER_RUN_ATTEMPT",
    "verifier_sha": "EXACT_VERIFIER_COMMIT",
    "ci120_trusted_verifier_job_id": "EXACT_CI120_TRUSTED_VERIFIER_JOB_ID",
    "ci120_verifier_job_inventory_digest": "EXACT_CI120_VERIFIER_JOB_INVENTORY_DIGEST",
    "ci120_status_publish_step_started_at": "EXACT_CI120_STATUS_PUBLISH_STEP_STARTED_AT",
    "ci120_status_publish_step_completed_at": "EXACT_CI120_STATUS_PUBLISH_STEP_COMPLETED_AT",
    "receipt_artifact_id": "EXACT_CI120_RECEIPT_ARTIFACT_ID",
    "receipt_artifact_digest": "EXACT_CI120_RECEIPT_ARTIFACT_DIGEST",
    "receipt_artifact_bytes": "EXACT_CI120_RECEIPT_ARTIFACT_BYTES",
    "codeql_run_id": "EXACT_CODEQL_RUN_ID",
    "codeql_run_attempt": "EXACT_CODEQL_RUN_ATTEMPT",
    "codeql_run_url": "EXACT_CODEQL_RUN_URL",
    "codeql_actions_source_job_id": "EXACT_CODEQL_ACTIONS_SOURCE_JOB_ID",
    "codeql_c_cpp_source_job_id": "EXACT_CODEQL_C_CPP_SOURCE_JOB_ID",
    "codeql_python_source_job_id": "EXACT_CODEQL_PYTHON_SOURCE_JOB_ID",
    "codeql_source_job_inventory_digest": "EXACT_CODEQL_SOURCE_JOB_INVENTORY_DIGEST",
    "codeql_actions_source_artifact_id": "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_ID",
    "codeql_actions_source_artifact_digest": "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_DIGEST",
    "codeql_actions_source_artifact_bytes": "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_BYTES",
    "codeql_c_cpp_source_artifact_id": "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_ID",
    "codeql_c_cpp_source_artifact_digest": "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_DIGEST",
    "codeql_c_cpp_source_artifact_bytes": "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_BYTES",
    "codeql_python_source_artifact_id": "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_ID",
    "codeql_python_source_artifact_digest": "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_DIGEST",
    "codeql_python_source_artifact_bytes": "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_BYTES",
    "codeql_status_id": "EXACT_CODEQL_STATUS_ID",
    "codeql_status_target_url": "EXACT_CODEQL_STATUS_TARGET_URL",
    "codeql_status_created_at": "EXACT_CODEQL_STATUS_CREATED_AT",
    "codeql_status_updated_at": "EXACT_CODEQL_STATUS_UPDATED_AT",
    "codeql_reporter_run_id": "EXACT_CODEQL_REPORTER_RUN_ID",
    "codeql_reporter_run_attempt": "EXACT_CODEQL_REPORTER_RUN_ATTEMPT",
    "codeql_reporter_run_url": "EXACT_CODEQL_REPORTER_RUN_URL",
    "codeql_trusted_reporter_job_id": "EXACT_CODEQL_TRUSTED_REPORTER_JOB_ID",
    "codeql_reporter_job_inventory_digest": "EXACT_CODEQL_REPORTER_JOB_INVENTORY_DIGEST",
    "codeql_status_publish_step_started_at": "EXACT_CODEQL_STATUS_PUBLISH_STEP_STARTED_AT",
    "codeql_status_publish_step_completed_at": "EXACT_CODEQL_STATUS_PUBLISH_STEP_COMPLETED_AT",
    "codeql_summary_artifact_id": "EXACT_CODEQL_SUMMARY_ARTIFACT_ID",
    "codeql_summary_artifact_digest": "EXACT_CODEQL_SUMMARY_ARTIFACT_DIGEST",
    "codeql_summary_artifact_bytes": "EXACT_CODEQL_SUMMARY_ARTIFACT_BYTES",
}

GATE_OUTPUT_KEYS = frozenset(ENV_FROM_GATE_KEY)


class ExactEvidenceError(ValueError):
    """The exact-evidence manifest or its inputs are invalid."""


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ExactEvidenceError(f"exact-evidence manifest contains duplicate key {key!r}")
        result[key] = value
    return result


def _required(values: Mapping[str, str], name: str) -> str:
    value = values.get(name, "")
    if not isinstance(value, str) or not value:
        raise ExactEvidenceError(f"{name} is required")
    if "\n" in value or "\r" in value:
        raise ExactEvidenceError(f"{name} contains a line break")
    return value


def _positive_int(values: Mapping[str, str], name: str) -> int:
    raw = _required(values, name)
    if not raw.isdigit() or int(raw) < 1:
        raise ExactEvidenceError(f"{name} must be a positive integer")
    return int(raw)


def _sha(values: Mapping[str, str], name: str) -> str:
    value = _required(values, name)
    if not SHA_PATTERN.fullmatch(value):
        raise ExactEvidenceError(f"{name} must be a lowercase 40-character commit SHA")
    return value


def _digest(values: Mapping[str, str], name: str) -> str:
    value = _required(values, name)
    if not DIGEST_PATTERN.fullmatch(value):
        raise ExactEvidenceError(f"{name} must be a lowercase SHA-256 digest")
    return value


def _timestamp(values: Mapping[str, str], name: str) -> tuple[str, datetime]:
    value = _required(values, name)
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ExactEvidenceError(f"{name} must be an ISO-8601 timestamp") from error
    if parsed.tzinfo is None:
        raise ExactEvidenceError(f"{name} must include a timezone")
    parsed = parsed.astimezone(timezone.utc)
    canonical = parsed.isoformat().replace("+00:00", "Z")
    if value != canonical:
        raise ExactEvidenceError(f"{name} must be canonical UTC")
    return value, parsed


def _status_window(
    values: Mapping[str, str],
    repository: str,
    *,
    prefix: str,
    run_id: int,
    run_attempt: int,
) -> tuple[str, str, str, str, str]:
    target_name = f"{prefix}_TARGET_URL"
    target_url = _required(values, target_name)
    expected_target = (
        f"https://github.com/{repository}/actions/runs/{run_id}/attempts/{run_attempt}"
    )
    if target_url != expected_target:
        raise ExactEvidenceError(f"{target_name} must equal {expected_target}")

    created, created_at = _timestamp(values, f"{prefix}_CREATED_AT")
    updated, updated_at = _timestamp(values, f"{prefix}_UPDATED_AT")
    step_started, step_started_at = _timestamp(
        values, f"{prefix}_PUBLISH_STEP_STARTED_AT"
    )
    step_completed, step_completed_at = _timestamp(
        values, f"{prefix}_PUBLISH_STEP_COMPLETED_AT"
    )
    if created_at > updated_at:
        raise ExactEvidenceError(f"{prefix} created_at exceeds updated_at")
    if step_started_at > step_completed_at:
        raise ExactEvidenceError(f"{prefix} publish step starts after it completes")
    tolerance = timedelta(seconds=1)
    if created_at < step_started_at - tolerance or updated_at > step_completed_at + tolerance:
        raise ExactEvidenceError(f"{prefix} timestamps escape the trusted publish step")
    return target_url, created, updated, step_started, step_completed


def _run(
    values: Mapping[str, str],
    repository: str,
    *,
    id_name: str,
    attempt_name: str,
    url_name: str,
) -> tuple[int, int, str]:
    run_id = _positive_int(values, id_name)
    run_attempt = _positive_int(values, attempt_name)
    run_url = _required(values, url_name)
    expected_url = f"https://github.com/{repository}/actions/runs/{run_id}"
    if run_url != expected_url:
        raise ExactEvidenceError(f"{url_name} must equal {expected_url}")
    return run_id, run_attempt, run_url


def build_manifest(values: Mapping[str, str]) -> dict[str, Any]:
    """Return the one canonical manifest described by validated environment values."""

    repository = _required(values, "GITHUB_REPOSITORY")
    if not REPOSITORY_PATTERN.fullmatch(repository):
        raise ExactEvidenceError("GITHUB_REPOSITORY must be an owner/name repository")
    source_commit = _sha(values, "EXACT_SOURCE_COMMIT")
    verifier_commit = _sha(values, "EXACT_VERIFIER_COMMIT")

    build_id, build_attempt, build_url = _run(
        values,
        repository,
        id_name="EXACT_BUILD_RUN_ID",
        attempt_name="EXACT_BUILD_RUN_ATTEMPT",
        url_name="EXACT_BUILD_RUN_URL",
    )
    build_event = _required(values, "EXACT_BUILD_EVENT")
    if build_event not in {"push", "workflow_dispatch"}:
        raise ExactEvidenceError("EXACT_BUILD_EVENT is not eligible publication evidence")
    ci120_id, ci120_attempt, ci120_url = _run(
        values,
        repository,
        id_name="EXACT_CI120_VERIFIER_RUN_ID",
        attempt_name="EXACT_CI120_VERIFIER_RUN_ATTEMPT",
        url_name="EXACT_CI120_VERIFIER_RUN_URL",
    )
    codeql_id, codeql_attempt, codeql_url = _run(
        values,
        repository,
        id_name="EXACT_CODEQL_RUN_ID",
        attempt_name="EXACT_CODEQL_RUN_ATTEMPT",
        url_name="EXACT_CODEQL_RUN_URL",
    )
    reporter_id, reporter_attempt, reporter_url = _run(
        values,
        repository,
        id_name="EXACT_CODEQL_REPORTER_RUN_ID",
        attempt_name="EXACT_CODEQL_REPORTER_RUN_ATTEMPT",
        url_name="EXACT_CODEQL_REPORTER_RUN_URL",
    )
    run_ids = (build_id, ci120_id, codeql_id, reporter_id)
    if len(set(run_ids)) != len(run_ids):
        raise ExactEvidenceError("exact-evidence workflow run IDs must be globally unique")

    ci120_status = _status_window(
        values,
        repository,
        prefix="EXACT_CI120_STATUS",
        run_id=ci120_id,
        run_attempt=ci120_attempt,
    )
    codeql_status = _status_window(
        values,
        repository,
        prefix="EXACT_CODEQL_STATUS",
        run_id=reporter_id,
        run_attempt=reporter_attempt,
    )

    build_ci120_job_id = _positive_int(values, "EXACT_BUILD_CI120_PRODUCER_JOB_ID")
    build_gate_job_id = _positive_int(values, "EXACT_BUILD_REQUIRED_GATE_JOB_ID")
    ci120_verifier_job_id = _positive_int(
        values, "EXACT_CI120_TRUSTED_VERIFIER_JOB_ID"
    )
    codeql_reporter_job_id = _positive_int(
        values, "EXACT_CODEQL_TRUSTED_REPORTER_JOB_ID"
    )
    ci120_source_id = _positive_int(values, "EXACT_CI120_SOURCE_ARTIFACT_ID")
    receipt_id = _positive_int(values, "EXACT_CI120_RECEIPT_ARTIFACT_ID")
    codeql_source_ids = {
        language: _positive_int(values, f"EXACT_CODEQL_{environment}_SOURCE_ARTIFACT_ID")
        for language, environment in (
            ("actions", "ACTIONS"),
            ("c-cpp", "C_CPP"),
            ("python", "PYTHON"),
        )
    }
    summary_id = _positive_int(values, "EXACT_CODEQL_SUMMARY_ARTIFACT_ID")
    artifact_ids = [ci120_source_id, receipt_id, *codeql_source_ids.values(), summary_id]
    if len(set(artifact_ids)) != len(artifact_ids):
        raise ExactEvidenceError("exact-evidence artifact IDs must be unique")

    codeql_job_ids = {
        language: _positive_int(values, f"EXACT_CODEQL_{environment}_SOURCE_JOB_ID")
        for language, environment in (
            ("actions", "ACTIONS"),
            ("c-cpp", "C_CPP"),
            ("python", "PYTHON"),
        )
    }
    if len(set(codeql_job_ids.values())) != len(codeql_job_ids):
        raise ExactEvidenceError("CodeQL source job IDs must be unique")
    job_ids = [
        build_ci120_job_id,
        build_gate_job_id,
        ci120_verifier_job_id,
        *codeql_job_ids.values(),
        codeql_reporter_job_id,
    ]
    if len(set(job_ids)) != len(job_ids):
        raise ExactEvidenceError("exact-evidence job IDs must be globally unique")

    ci120_status_id = _positive_int(values, "EXACT_CI120_STATUS_ID")
    codeql_status_id = _positive_int(values, "EXACT_CODEQL_STATUS_ID")
    if ci120_status_id == codeql_status_id:
        raise ExactEvidenceError("exact-evidence commit status IDs must be unique")

    codeql_source_jobs = [
        {
            "language": language,
            "name": f"Analyze ({language})",
            "id": codeql_job_ids[language],
        }
        for language in ("actions", "c-cpp", "python")
    ]
    codeql_source_artifacts = []
    for language, environment in (
        ("actions", "ACTIONS"),
        ("c-cpp", "C_CPP"),
        ("python", "PYTHON"),
    ):
        codeql_source_artifacts.append(
            {
                "language": language,
                "name": f"codeql-{language}-attempt-{codeql_attempt}.sarif",
                "id": codeql_source_ids[language],
                "bytes": _positive_int(
                    values, f"EXACT_CODEQL_{environment}_SOURCE_ARTIFACT_BYTES"
                ),
                "digest": _digest(
                    values, f"EXACT_CODEQL_{environment}_SOURCE_ARTIFACT_DIGEST"
                ),
            }
        )

    return {
        "schemaVersion": SCHEMA_VERSION,
        "repository": repository,
        "sourceCommit": source_commit,
        "build": {
            "workflow": "Build SparkEngine",
            "runId": build_id,
            "runAttempt": build_attempt,
            "runUrl": build_url,
            "event": build_event,
            "ci120ProducerJobId": build_ci120_job_id,
            "requiredGateJobId": build_gate_job_id,
            "jobInventoryDigest": _digest(
                values, "EXACT_BUILD_JOB_INVENTORY_DIGEST"
            ),
            "ci120SourceArtifact": {
                "id": ci120_source_id,
                "name": f"ci120-untrusted-stable-v1-{source_commit}-{build_attempt}",
                "bytes": _positive_int(values, "EXACT_CI120_SOURCE_ARTIFACT_BYTES"),
                "digest": _digest(values, "EXACT_CI120_SOURCE_ARTIFACT_DIGEST"),
            },
        },
        "ci120": {
            "statusContext": CI120_STATUS_CONTEXT,
            "statusId": ci120_status_id,
            "statusTargetUrl": ci120_status[0],
            "statusCreatedAt": ci120_status[1],
            "statusUpdatedAt": ci120_status[2],
            "statusPublishStepStartedAt": ci120_status[3],
            "statusPublishStepCompletedAt": ci120_status[4],
            "verifierWorkflow": "CI-120 Trusted Verifier",
            "verifierCommit": verifier_commit,
            "verifierRunId": ci120_id,
            "verifierRunAttempt": ci120_attempt,
            "verifierRunUrl": ci120_url,
            "verifierJobId": ci120_verifier_job_id,
            "verifierJobInventoryDigest": _digest(
                values, "EXACT_CI120_VERIFIER_JOB_INVENTORY_DIGEST"
            ),
            "receiptArtifact": {
                "id": receipt_id,
                "name": (
                    f"ci120-trusted-receipt-{source_commit}-{build_id}-"
                    f"{build_attempt}-{ci120_attempt}"
                ),
                "bytes": _positive_int(
                    values, "EXACT_CI120_RECEIPT_ARTIFACT_BYTES"
                ),
                "digest": _digest(values, "EXACT_CI120_RECEIPT_ARTIFACT_DIGEST"),
            },
        },
        "codeql": {
            "workflow": "CodeQL Advanced",
            "runId": codeql_id,
            "runAttempt": codeql_attempt,
            "runUrl": codeql_url,
            "sourceJobs": codeql_source_jobs,
            "sourceJobInventoryDigest": _digest(
                values, "EXACT_CODEQL_SOURCE_JOB_INVENTORY_DIGEST"
            ),
            "sourceArtifacts": codeql_source_artifacts,
            "statusContext": CODEQL_STATUS_CONTEXT,
            "statusId": codeql_status_id,
            "statusTargetUrl": codeql_status[0],
            "statusCreatedAt": codeql_status[1],
            "statusUpdatedAt": codeql_status[2],
            "statusPublishStepStartedAt": codeql_status[3],
            "statusPublishStepCompletedAt": codeql_status[4],
            "reporterWorkflow": "CodeQL Trusted Reporter",
            "reporterCommit": verifier_commit,
            "reporterRunId": reporter_id,
            "reporterRunAttempt": reporter_attempt,
            "reporterRunUrl": reporter_url,
            "reporterJobId": codeql_reporter_job_id,
            "reporterJobInventoryDigest": _digest(
                values, "EXACT_CODEQL_REPORTER_JOB_INVENTORY_DIGEST"
            ),
            "summaryArtifact": {
                "id": summary_id,
                "name": (
                    f"codeql-trusted-summary-{source_commit}-{codeql_id}-"
                    f"{codeql_attempt}-{reporter_attempt}"
                ),
                "bytes": _positive_int(
                    values, "EXACT_CODEQL_SUMMARY_ARTIFACT_BYTES"
                ),
                "digest": _digest(values, "EXACT_CODEQL_SUMMARY_ARTIFACT_DIGEST"),
            },
        },
    }


def canonical_bytes(manifest: Mapping[str, Any]) -> bytes:
    return (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        if path.is_symlink() or not path.is_file():
            raise ExactEvidenceError(f"exact-evidence manifest is not a regular file: {path}")
        payload = path.read_bytes()
    except OSError as error:
        raise ExactEvidenceError(f"cannot read exact-evidence manifest {path}: {error}") from error
    if not payload or len(payload) > MAX_MANIFEST_BYTES:
        raise ExactEvidenceError("exact-evidence manifest has an invalid byte size")
    try:
        decoded = json.loads(payload, object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ExactEvidenceError(f"exact-evidence manifest is invalid JSON: {error}") from error
    if not isinstance(decoded, dict):
        raise ExactEvidenceError("exact-evidence manifest must be a JSON object")
    return decoded


def values_from_manifest(manifest: Mapping[str, Any]) -> dict[str, str]:
    """Extract inputs, then let build_manifest enforce the complete exact shape."""

    try:
        build = manifest["build"]
        ci120 = manifest["ci120"]
        receipt = ci120["receiptArtifact"]
        codeql = manifest["codeql"]
        source_jobs = codeql["sourceJobs"]
        source_artifacts = codeql["sourceArtifacts"]
        summary = codeql["summaryArtifact"]
        return {
            "GITHUB_REPOSITORY": str(manifest["repository"]),
            "EXACT_SOURCE_COMMIT": str(manifest["sourceCommit"]),
            "EXACT_BUILD_RUN_ID": str(build["runId"]),
            "EXACT_BUILD_RUN_ATTEMPT": str(build["runAttempt"]),
            "EXACT_BUILD_RUN_URL": str(build["runUrl"]),
            "EXACT_BUILD_EVENT": str(build["event"]),
            "EXACT_BUILD_CI120_PRODUCER_JOB_ID": str(build["ci120ProducerJobId"]),
            "EXACT_BUILD_REQUIRED_GATE_JOB_ID": str(build["requiredGateJobId"]),
            "EXACT_BUILD_JOB_INVENTORY_DIGEST": str(build["jobInventoryDigest"]),
            "EXACT_CI120_SOURCE_ARTIFACT_ID": str(
                build["ci120SourceArtifact"]["id"]
            ),
            "EXACT_CI120_SOURCE_ARTIFACT_DIGEST": str(
                build["ci120SourceArtifact"]["digest"]
            ),
            "EXACT_CI120_SOURCE_ARTIFACT_BYTES": str(
                build["ci120SourceArtifact"]["bytes"]
            ),
            "EXACT_CI120_STATUS_ID": str(ci120["statusId"]),
            "EXACT_CI120_STATUS_TARGET_URL": str(ci120["statusTargetUrl"]),
            "EXACT_CI120_STATUS_CREATED_AT": str(ci120["statusCreatedAt"]),
            "EXACT_CI120_STATUS_UPDATED_AT": str(ci120["statusUpdatedAt"]),
            "EXACT_CI120_STATUS_PUBLISH_STEP_STARTED_AT": str(
                ci120["statusPublishStepStartedAt"]
            ),
            "EXACT_CI120_STATUS_PUBLISH_STEP_COMPLETED_AT": str(
                ci120["statusPublishStepCompletedAt"]
            ),
            "EXACT_CI120_VERIFIER_RUN_ID": str(ci120["verifierRunId"]),
            "EXACT_CI120_VERIFIER_RUN_ATTEMPT": str(ci120["verifierRunAttempt"]),
            "EXACT_CI120_VERIFIER_RUN_URL": str(ci120["verifierRunUrl"]),
            "EXACT_VERIFIER_COMMIT": str(ci120["verifierCommit"]),
            "EXACT_CI120_TRUSTED_VERIFIER_JOB_ID": str(ci120["verifierJobId"]),
            "EXACT_CI120_VERIFIER_JOB_INVENTORY_DIGEST": str(
                ci120["verifierJobInventoryDigest"]
            ),
            "EXACT_CI120_RECEIPT_ARTIFACT_ID": str(receipt["id"]),
            "EXACT_CI120_RECEIPT_ARTIFACT_DIGEST": str(receipt["digest"]),
            "EXACT_CI120_RECEIPT_ARTIFACT_BYTES": str(receipt["bytes"]),
            "EXACT_CODEQL_RUN_ID": str(codeql["runId"]),
            "EXACT_CODEQL_RUN_ATTEMPT": str(codeql["runAttempt"]),
            "EXACT_CODEQL_RUN_URL": str(codeql["runUrl"]),
            "EXACT_CODEQL_ACTIONS_SOURCE_JOB_ID": str(source_jobs[0]["id"]),
            "EXACT_CODEQL_C_CPP_SOURCE_JOB_ID": str(source_jobs[1]["id"]),
            "EXACT_CODEQL_PYTHON_SOURCE_JOB_ID": str(source_jobs[2]["id"]),
            "EXACT_CODEQL_SOURCE_JOB_INVENTORY_DIGEST": str(
                codeql["sourceJobInventoryDigest"]
            ),
            "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_ID": str(
                source_artifacts[0]["id"]
            ),
            "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_DIGEST": str(
                source_artifacts[0]["digest"]
            ),
            "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_BYTES": str(
                source_artifacts[0]["bytes"]
            ),
            "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_ID": str(
                source_artifacts[1]["id"]
            ),
            "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_DIGEST": str(
                source_artifacts[1]["digest"]
            ),
            "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_BYTES": str(
                source_artifacts[1]["bytes"]
            ),
            "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_ID": str(
                source_artifacts[2]["id"]
            ),
            "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_DIGEST": str(
                source_artifacts[2]["digest"]
            ),
            "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_BYTES": str(
                source_artifacts[2]["bytes"]
            ),
            "EXACT_CODEQL_STATUS_ID": str(codeql["statusId"]),
            "EXACT_CODEQL_STATUS_TARGET_URL": str(codeql["statusTargetUrl"]),
            "EXACT_CODEQL_STATUS_CREATED_AT": str(codeql["statusCreatedAt"]),
            "EXACT_CODEQL_STATUS_UPDATED_AT": str(codeql["statusUpdatedAt"]),
            "EXACT_CODEQL_STATUS_PUBLISH_STEP_STARTED_AT": str(
                codeql["statusPublishStepStartedAt"]
            ),
            "EXACT_CODEQL_STATUS_PUBLISH_STEP_COMPLETED_AT": str(
                codeql["statusPublishStepCompletedAt"]
            ),
            "EXACT_CODEQL_REPORTER_RUN_ID": str(codeql["reporterRunId"]),
            "EXACT_CODEQL_REPORTER_RUN_ATTEMPT": str(codeql["reporterRunAttempt"]),
            "EXACT_CODEQL_REPORTER_RUN_URL": str(codeql["reporterRunUrl"]),
            "EXACT_CODEQL_TRUSTED_REPORTER_JOB_ID": str(codeql["reporterJobId"]),
            "EXACT_CODEQL_REPORTER_JOB_INVENTORY_DIGEST": str(
                codeql["reporterJobInventoryDigest"]
            ),
            "EXACT_CODEQL_SUMMARY_ARTIFACT_ID": str(summary["id"]),
            "EXACT_CODEQL_SUMMARY_ARTIFACT_DIGEST": str(summary["digest"]),
            "EXACT_CODEQL_SUMMARY_ARTIFACT_BYTES": str(summary["bytes"]),
        }
    except (IndexError, KeyError, TypeError) as error:
        raise ExactEvidenceError(f"exact-evidence manifest is missing {error}") from error


def validate_manifest(manifest: Mapping[str, Any]) -> dict[str, Any]:
    values = values_from_manifest(manifest)
    expected = build_manifest(values)
    if manifest != expected:
        raise ExactEvidenceError("exact-evidence manifest is not the canonical complete shape")
    return expected


def write_manifest(path: Path, values: Mapping[str, str]) -> dict[str, Any]:
    manifest = build_manifest(values)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ExactEvidenceError(f"refusing unsafe exact-evidence output: {path}")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(canonical_bytes(manifest))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise
    return manifest


def verify_manifest(path: Path, values: Mapping[str, str]) -> dict[str, Any]:
    actual = load_manifest(path)
    validate_manifest(actual)
    expected = build_manifest(values)
    if actual != expected:
        raise ExactEvidenceError("exact-evidence manifest differs from the verified gate outputs")
    return actual


def parse_gate_output(path: Path) -> dict[str, str]:
    try:
        if path.is_symlink() or not path.is_file():
            raise ExactEvidenceError(f"gate output is not a regular file: {path}")
        payload = path.read_bytes()
    except OSError as error:
        raise ExactEvidenceError(f"cannot read exact-gate output {path}: {error}") from error
    if not payload or len(payload) > MAX_GATE_OUTPUT_BYTES:
        raise ExactEvidenceError("exact-gate output has an invalid byte size")
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ExactEvidenceError("exact-gate output is not UTF-8") from error
    parsed: dict[str, str] = {}
    for raw_line in text.splitlines():
        key, separator, value = raw_line.partition("=")
        if not separator or key not in GATE_OUTPUT_KEYS or key in parsed or not value:
            raise ExactEvidenceError("exact-gate output has an unknown, duplicate, or empty field")
        parsed[key] = value
    if set(parsed) != GATE_OUTPUT_KEYS:
        missing = sorted(GATE_OUTPUT_KEYS - set(parsed))
        raise ExactEvidenceError(f"exact-gate output is incomplete: {missing}")
    return parsed


def values_from_gate_output(
    path: Path, *, repository: str, source_commit: str
) -> dict[str, str]:
    parsed = parse_gate_output(path)
    values = {ENV_FROM_GATE_KEY[key]: value for key, value in parsed.items()}
    values["GITHUB_REPOSITORY"] = repository
    values["EXACT_SOURCE_COMMIT"] = source_commit
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    write_parser = subparsers.add_parser("write", help="write a manifest from EXACT_* environment values")
    write_parser.add_argument("--output", required=True, type=Path)
    verify_parser = subparsers.add_parser("verify", help="verify a manifest against EXACT_* environment values")
    verify_parser.add_argument("--manifest", required=True, type=Path)
    gate_parser = subparsers.add_parser(
        "verify-gate-output", help="verify a manifest against a fresh exact-gate output file"
    )
    gate_parser.add_argument("--manifest", required=True, type=Path)
    gate_parser.add_argument("--gate-output", required=True, type=Path)
    gate_parser.add_argument("--repository", required=True)
    gate_parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()
    try:
        if args.command == "write":
            manifest = write_manifest(args.output, os.environ)
        elif args.command == "verify":
            manifest = verify_manifest(args.manifest, os.environ)
        else:
            values = values_from_gate_output(
                args.gate_output,
                repository=args.repository,
                source_commit=args.source_commit,
            )
            manifest = verify_manifest(args.manifest, values)
    except (ExactEvidenceError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        "Verified exact CI evidence for "
        f"{manifest['sourceCommit']}: Build {manifest['build']['runId']}, "
        f"CI-120 {manifest['ci120']['verifierRunId']}, "
        f"CodeQL {manifest['codeql']['runId']}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
