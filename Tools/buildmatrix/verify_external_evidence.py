#!/usr/bin/env python3
"""Verify build-matrix artifacts in a protected downstream workflow.

Downloaded source-run files are untrusted data.  This module never imports or
executes anything from the artifact.  It reparses bounded CMake File API JSON
with the trusted default-branch implementation, rehashes every declared product,
rebuilds the static inventory and parity report, and emits the only build-matrix
receipt allowed to use an externally verified authority state.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any

import check_parity
import inventory
import validate_pending_authority as pending


AUTHORITY = "github-actions-protected-workflow-run-v1"
SOURCE_WORKFLOW_NAME = "Build SparkEngine"
SOURCE_WORKFLOW_PATH = ".github/workflows/build.yml"
SOURCE_JOB_NAME = "Windows Shipping build matrix"
SOURCE_FINAL_STEP = "Record build-matrix evidence"
VERIFIER_WORKFLOW_PATH = ".github/workflows/build-matrix-verifier.yml"
MAX_METADATA_BYTES = 1024 * 1024
MAX_PENDING_RECEIPT_BYTES = 1024 * 1024
MAX_FILE_COUNT = 100_000
MAX_TOTAL_BYTES = 8 * 1024 * 1024 * 1024
MAX_SINGLE_FILE_BYTES = 2 * 1024 * 1024 * 1024
MAX_COMPRESSED_ARTIFACT_BYTES = 4 * 1024 * 1024 * 1024

PROFILE_BUILD_DIRS = {
    "installed-sdk-consumer": "build/installed-sdk-consumer",
    "windows-shipping": "build/windows-shipping",
    "windows-validation": "build/windows-release",
}

_AUTHORITY_REASON = (
    "A same-job GitHub Actions token, environment, checkout, provenance record, "
    "artifact path, and hash are producer-controlled inputs. A protected external "
    "attestation verifier must independently validate the captured artifact before "
    "the build matrix can report producer-verified evidence."
)


class ExternalEvidenceError(ValueError):
    """The downloaded artifact or its trusted source binding is invalid."""


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ExternalEvidenceError(f"{label} must be an object")
    return value


def _list(value: Any, label: str, maximum: int) -> list[Any]:
    if not isinstance(value, list) or len(value) > maximum:
        raise ExternalEvidenceError(f"{label} must be an array of at most {maximum} entries")
    return value


def _exact_fields(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise ExternalEvidenceError(f"{label} fields must be exactly {sorted(expected)}")


def _full_sha(value: Any, label: str) -> str:
    text = str(value or "").lower()
    if not re.fullmatch(r"[0-9a-f]{40}", text):
        raise ExternalEvidenceError(f"{label} must be a full 40-character Git commit id")
    return text


def _hex_digest(value: Any, label: str, *, prefix: bool = False) -> str:
    text = str(value or "").lower()
    pattern = r"sha256:[0-9a-f]{64}" if prefix else r"[0-9a-f]{64}"
    if not re.fullmatch(pattern, text):
        raise ExternalEvidenceError(f"{label} must be a SHA-256 digest")
    return text


def _positive_integer(value: Any, label: str) -> int:
    if type(value) is not int or value < 1:
        raise ExternalEvidenceError(f"{label} must be a positive integer")
    return value


def _read_json_payload(path: Path, maximum: int, label: str) -> tuple[dict[str, Any], bytes]:
    try:
        before = os.lstat(path)
    except OSError as error:
        raise ExternalEvidenceError(f"cannot inspect {label}: {error}") from error
    if not stat.S_ISREG(before.st_mode) or stat.S_ISLNK(before.st_mode):
        raise ExternalEvidenceError(f"{label} must be a regular file, not a link")
    if before.st_size < 2 or before.st_size > maximum:
        raise ExternalEvidenceError(f"{label} size is outside the accepted bound")
    payload = path.read_bytes()
    after = os.lstat(path)
    if len(payload) != before.st_size or (
        before.st_dev,
        before.st_ino,
        before.st_size,
        before.st_mtime_ns,
    ) != (
        after.st_dev,
        after.st_ino,
        after.st_size,
        after.st_mtime_ns,
    ):
        raise ExternalEvidenceError(f"{label} changed while it was read")
    try:
        parsed = inventory._decode_bounded_json(payload, label)
    except inventory.ReplyValidationError as error:
        raise ExternalEvidenceError(f"{label} is not strict bounded JSON: {error}") from error
    return _mapping(parsed, label), payload


def _recorded_path(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 4096:
        raise ExternalEvidenceError(f"{label} is not a bounded path")
    text = value.replace("\\", "/").rstrip("/")
    if not re.fullmatch(r"[A-Za-z]:/[^\x00-\x1f]*", text):
        raise ExternalEvidenceError(f"{label} is not an absolute Windows producer path")
    tail = text[3:]
    parts = PurePosixPath(tail).parts
    if any(part in {"", ".", ".."} for part in parts):
        raise ExternalEvidenceError(f"{label} is not canonical")
    return text


def _safe_relative(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 2048 or "\\" in value:
        raise ExternalEvidenceError(f"{label} is not a safe relative path")
    candidate = PurePosixPath(value)
    if candidate.is_absolute() or any(part in {"", ".", ".."} for part in candidate.parts):
        raise ExternalEvidenceError(f"{label} is not a contained relative path")
    if any(":" in part or any(ord(character) < 32 for character in part) for part in candidate.parts):
        raise ExternalEvidenceError(f"{label} contains an unsafe component")
    return candidate.as_posix()


def _portable_join(root: str, relative: str) -> str:
    root_value = _recorded_path(root, "recorded repository root")
    relative_value = _safe_relative(relative, "recorded relative path")
    return f"{root_value}/{relative_value}"


def _same_recorded_path(left: Any, right: Any) -> bool:
    try:
        return _recorded_path(left, "path").casefold() == _recorded_path(right, "path").casefold()
    except ExternalEvidenceError:
        return False


def _rebase_extracted_path(path_value: Any, extracted_root: Path, recorded_root: str, label: str) -> str:
    if not isinstance(path_value, str) or not path_value:
        raise ExternalEvidenceError(f"{label} has no path")
    root = Path(os.path.abspath(extracted_root))
    candidate = Path(os.path.abspath(path_value))
    try:
        relative = candidate.relative_to(root).as_posix()
    except ValueError as error:
        raise ExternalEvidenceError(f"{label} escapes the extracted build directory") from error
    return _portable_join(recorded_root, _safe_relative(relative, label))


def validate_source_metadata(document: dict[str, Any]) -> dict[str, Any]:
    """Validate metadata written only by the trusted preflight step."""
    _exact_fields(document, {"schemaVersion", "repository", "source", "artifact", "verifier"}, "metadata")
    if document.get("schemaVersion") != 1:
        raise ExternalEvidenceError("source metadata schemaVersion must be 1")

    repository = _mapping(document.get("repository"), "metadata repository")
    _exact_fields(repository, {"id", "fullName", "defaultBranch"}, "metadata repository")
    _positive_integer(repository.get("id"), "repository id")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", str(repository.get("fullName", ""))):
        raise ExternalEvidenceError("metadata repository full name is invalid")
    if repository.get("defaultBranch") != "Working":
        raise ExternalEvidenceError("build-matrix trusted verification is restricted to the Working default branch")

    source = _mapping(document.get("source"), "metadata source")
    _exact_fields(
        source,
        {
            "workflowId", "workflowName", "workflowPath", "runId", "runNumber", "runAttempt",
            "event", "conclusion", "headBranch", "headSha", "jobId", "jobName",
            "jobConclusion", "finalStepName", "finalStepConclusion",
        },
        "metadata source",
    )
    for field in ("workflowId", "runId", "runNumber", "runAttempt", "jobId"):
        _positive_integer(source.get(field), f"source {field}")
    source_sha = _full_sha(source.get("headSha"), "source head SHA")
    if (
        source.get("workflowName") != SOURCE_WORKFLOW_NAME
        or source.get("workflowPath") != SOURCE_WORKFLOW_PATH
        or source.get("event") not in {"push", "workflow_dispatch"}
        or source.get("conclusion") != "success"
        or source.get("headBranch") != "Working"
        or source.get("jobName") != SOURCE_JOB_NAME
        or source.get("jobConclusion") != "success"
        or source.get("finalStepName") != SOURCE_FINAL_STEP
        or source.get("finalStepConclusion") != "success"
    ):
        raise ExternalEvidenceError("source run is not the exact successful build-matrix producer execution")

    artifact = _mapping(document.get("artifact"), "metadata artifact")
    _exact_fields(artifact, {"id", "name", "bytes", "digest"}, "metadata artifact")
    _positive_integer(artifact.get("id"), "artifact id")
    size = _positive_integer(artifact.get("bytes"), "artifact size")
    if size > MAX_COMPRESSED_ARTIFACT_BYTES:
        raise ExternalEvidenceError("source artifact exceeds the compressed-size bound")
    expected_name = f"build-matrix-stable-v1-{source_sha}-{source['runAttempt']}"
    if artifact.get("name") != expected_name:
        raise ExternalEvidenceError("source artifact name does not bind the exact SHA and run attempt")
    _hex_digest(artifact.get("digest"), "source artifact API digest", prefix=True)

    verifier = _mapping(document.get("verifier"), "metadata verifier")
    _exact_fields(
        verifier,
        {
            "repository", "checkoutSha", "workflowSha", "workflowRef",
            "sourceWorkflowBlobSha", "trustedWorkflowBlobSha",
        },
        "metadata verifier",
    )
    checkout_sha = _full_sha(verifier.get("checkoutSha"), "trusted checkout SHA")
    workflow_sha = _full_sha(verifier.get("workflowSha"), "trusted workflow SHA")
    source_workflow_blob_sha = _full_sha(
        verifier.get("sourceWorkflowBlobSha"), "source workflow blob SHA"
    )
    trusted_workflow_blob_sha = _full_sha(
        verifier.get("trustedWorkflowBlobSha"), "trusted workflow blob SHA"
    )
    expected_ref = f"{repository['fullName']}/{VERIFIER_WORKFLOW_PATH}@refs/heads/Working"
    if (
        verifier.get("repository") != repository["fullName"]
        or checkout_sha != workflow_sha
        or source_workflow_blob_sha != trusted_workflow_blob_sha
        or verifier.get("workflowRef") != expected_ref
    ):
        raise ExternalEvidenceError(
            "trusted verifier identity or byte-identical source workflow attestation is not exact"
        )
    return document


def _allowed_artifact_path(relative: str) -> bool:
    if relative in {
        "build-matrix-inventory.json",
        "build-matrix-parity-findings.json",
        "build-matrix-parity-stdout.json",
        "build-matrix-pending-receipt.json",
        "build-matrix-pending-receipt-stdout.json",
    }:
        return True
    if re.fullmatch(r"build-matrix-[A-Za-z0-9_.-]+\.log", relative):
        return True
    prefixes = (
        "build/windows-shipping/.cmake/api/v1/reply/",
        "build/windows-shipping/.cmake/api/v1/provenance/",
        "build/windows-shipping/bin/MinSizeRel/",
        "build/windows-shipping/lib/MinSizeRel/",
        "build/windows-release/.cmake/api/v1/reply/",
        "build/windows-release/.cmake/api/v1/provenance/",
        "build/windows-release/bin/Release/",
        "build/windows-release/lib/Release/",
        "build/installed-sdk-consumer/.cmake/api/v1/reply/",
        "build/installed-sdk-consumer/.cmake/api/v1/provenance/",
        "build/installed-sdk-consumer/Release/",
    )
    return any(relative.startswith(prefix) for prefix in prefixes)


def _scan_artifact_tree(root: Path) -> dict[str, int]:
    root = Path(os.path.abspath(root))
    metadata = os.lstat(root)
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise ExternalEvidenceError("artifact root must be a real directory")
    file_count = 0
    total_bytes = 0
    for current, directories, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        for name in directories:
            directory = current_path / name
            entry = os.lstat(directory)
            if not stat.S_ISDIR(entry.st_mode) or stat.S_ISLNK(entry.st_mode):
                raise ExternalEvidenceError(f"artifact contains a linked or non-directory entry: {directory}")
        for name in files:
            path = current_path / name
            relative = path.relative_to(root).as_posix()
            entry = os.lstat(path)
            if not stat.S_ISREG(entry.st_mode) or stat.S_ISLNK(entry.st_mode):
                raise ExternalEvidenceError(f"artifact contains a linked or non-regular file: {relative}")
            if not _allowed_artifact_path(relative):
                raise ExternalEvidenceError(f"artifact contains an unexpected file: {relative}")
            if entry.st_size > MAX_SINGLE_FILE_BYTES:
                raise ExternalEvidenceError(f"artifact file exceeds the per-file bound: {relative}")
            file_count += 1
            total_bytes += entry.st_size
            if file_count > MAX_FILE_COUNT or total_bytes > MAX_TOTAL_BYTES:
                raise ExternalEvidenceError("artifact exceeds the file-count or total-size bound")
    if file_count == 0:
        raise ExternalEvidenceError("artifact is empty")
    return {"fileCount": file_count, "bytes": total_bytes}


def _profile_contract(profile: str, repository_root: str) -> dict[str, str]:
    profile_data = inventory.load_stable_profile()
    config = next(
        (entry for entry in profile_data.get("buildConfigurations", []) if entry.get("id") == profile),
        None,
    )
    if not isinstance(config, dict):
        raise ExternalEvidenceError(f"unknown stable-v1 profile {profile!r}")
    expected_build_relative = PROFILE_BUILD_DIRS[profile]
    source_relative = str(config.get("sourceDirectory", ""))
    source = repository_root if not source_relative else _portable_join(repository_root, source_relative)
    result = {
        "preset": str(config.get("preset", "")),
        "configuration": str(config.get("configuration", "")),
        "source": source,
        "build": _portable_join(repository_root, expected_build_relative),
        "generator": str(config.get("generator", "")),
        "architecture": str(config.get("architecture", "")),
        "toolset": str(config.get("toolset", "")),
        "package": "",
        "expectedVersion": str(config.get("expectedEngineVersion", "")),
    }
    if result["preset"]:
        resolved = inventory.resolve_configure_preset(inventory.extract_cmake_presets(), result["preset"])
        binary = str(resolved.get("resolvedBinaryDir", "")).replace("\\", "/")
        expected_binary = f"${{sourceDir}}/{expected_build_relative}"
        if binary != expected_binary:
            raise ExternalEvidenceError(f"{profile}: trusted preset binaryDir is not canonical")
        result["generator"] = str(resolved.get("generator", ""))
        result["architecture"] = str(resolved.get("architecture", ""))
        result["toolset"] = str(resolved.get("toolset", ""))
    else:
        result["package"] = _portable_join(repository_root, str(config.get("packageDirectory", "")))
    return result


def _validate_configure_argv(
    argv: Any,
    executable: str,
    contract: dict[str, str],
    profile: str,
) -> list[str]:
    values = _list(argv, f"{profile} configure argv", 32)
    if any(not isinstance(value, str) or not value for value in values):
        raise ExternalEvidenceError(f"{profile}: configure argv contains an invalid value")
    if not _same_recorded_path(values[0], executable):
        raise ExternalEvidenceError(f"{profile}: configure argv does not bind its CMake executable")
    if contract["preset"]:
        if values[1:] != ["--preset", contract["preset"]]:
            raise ExternalEvidenceError(f"{profile}: configure argv is not the canonical preset invocation")
        return values
    if len(values) != 13:
        raise ExternalEvidenceError(f"{profile}: installed consumer configure argv has wrong cardinality")
    if values[1] != "-S" or not _same_recorded_path(values[2], contract["source"]):
        raise ExternalEvidenceError(f"{profile}: configure source directory differs")
    if values[3] != "-B" or not _same_recorded_path(values[4], contract["build"]):
        raise ExternalEvidenceError(f"{profile}: configure build directory differs")
    expected_tail = [
        "-G", contract["generator"], "-A", contract["architecture"], "-T", contract["toolset"]
    ]
    if values[5:11] != expected_tail:
        raise ExternalEvidenceError(f"{profile}: configure toolchain arguments differ")
    if not values[11].startswith("-DSparkEngine_DIR=") or not _same_recorded_path(
        values[11].split("=", 1)[1], contract["package"]
    ):
        raise ExternalEvidenceError(f"{profile}: installed package directory differs")
    if values[12] != f"-DSPARK_EXPECTED_ENGINE_VERSION={contract['expectedVersion']}":
        raise ExternalEvidenceError(f"{profile}: installed package version expectation differs")
    return values


def _portable_manifest(
    manifest: list[dict[str, Any]], extracted_build: Path, recorded_build: str, profile: str
) -> list[dict[str, Any]]:
    result = copy.deepcopy(manifest)
    for target_offset, target in enumerate(result):
        identities = _list(
            target.get("artifactIdentities"),
            f"{profile} actual artifact identities {target_offset}",
            32,
        )
        for identity_offset, identity_value in enumerate(identities):
            identity = _mapping(identity_value, f"{profile} actual artifact identity {identity_offset}")
            identity["path"] = _rebase_extracted_path(
                identity.get("path"),
                extracted_build,
                recorded_build,
                f"{profile} artifact identity {identity_offset}",
            )
    return result


def _portable_evidence_paths(evidence: dict[str, Any], extracted_build: Path, recorded_build: str) -> None:
    evidence["evidenceDirectory"] = recorded_build
    for target_offset, target_value in enumerate(evidence.get("targets", [])):
        target = _mapping(target_value, f"portable target {target_offset}")
        target["artifacts"] = [
            _rebase_extracted_path(path, extracted_build, recorded_build, "configured target artifact")
            for path in _list(target.get("artifacts"), "configured target artifacts", 32)
        ]


def _verify_profile(
    artifact_root: Path,
    profile: str,
    producer_evidence: dict[str, Any],
    source_metadata: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    build_relative = PROFILE_BUILD_DIRS[profile]
    extracted_build = artifact_root / Path(build_relative)
    record_path = inventory._provenance_path(extracted_build, profile)
    record, record_payload = _read_json_payload(
        record_path, inventory._MAX_PROVENANCE_BYTES, f"{profile} provenance record"
    )
    canonical_payload = (json.dumps(record, indent=2, sort_keys=False) + "\n").encode("utf-8")
    if record_payload != canonical_payload:
        raise ExternalEvidenceError(f"{profile}: provenance record is not canonical JSON")
    record_digest = hashlib.sha256(record_payload).hexdigest()

    required_record = {
        "schemaVersion", "producer", "profile", "evidenceDirectory", "ci",
        "transaction", "observed", "artifacts", "reply",
    }
    _exact_fields(record, required_record, f"{profile} provenance record")
    if (
        record.get("schemaVersion") != inventory._PROVENANCE_SCHEMA
        or record.get("producer") != inventory._PROVENANCE_PRODUCER
        or record.get("profile") != profile
    ):
        raise ExternalEvidenceError(f"{profile}: provenance producer/schema identity differs")

    try:
        selected_index, client_name, query = inventory._provenance_selection(record, profile)
        core = inventory._extract_reply_core(
            extracted_build,
            profile,
            selected_index=selected_index,
            client_name=client_name,
            query=query,
        )
    except (inventory.InventoryError, inventory.ReplyValidationError, OSError, ValueError) as error:
        raise ExternalEvidenceError(f"{profile}: File API evidence is invalid: {error}") from error
    if core is None:
        raise ExternalEvidenceError(f"{profile}: File API evidence is absent")
    evidence, snapshot, records = core
    try:
        snapshot.assert_stable()
    finally:
        snapshot.close()

    transaction = _mapping(record.get("transaction"), f"{profile} transaction")
    _exact_fields(
        transaction,
        {
            "runId", "queryClient", "querySha256", "query", "profile", "preset",
            "configuration", "sourceDirectory", "buildDirectory", "configure",
            "repositoryBefore", "repositoryAfter",
        },
        f"{profile} transaction",
    )
    if transaction.get("profile") != profile or transaction.get("queryClient") != client_name:
        raise ExternalEvidenceError(f"{profile}: transaction profile/client differs")
    query_payload = (json.dumps(query, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
    if transaction.get("querySha256") != hashlib.sha256(query_payload).hexdigest():
        raise ExternalEvidenceError(f"{profile}: query digest differs")

    before = _mapping(transaction.get("repositoryBefore"), f"{profile} repository before")
    after = _mapping(transaction.get("repositoryAfter"), f"{profile} repository after")
    repository_fields = {"root", "commit", "clean", "untrackedPolicy", "statusSha256"}
    _exact_fields(before, repository_fields, f"{profile} repository before")
    _exact_fields(after, repository_fields, f"{profile} repository after")
    if before != after:
        raise ExternalEvidenceError(f"{profile}: repository identity changed across configure/build")
    repository_root = _recorded_path(before.get("root"), f"{profile} repository root")
    source_sha = source_metadata["source"]["headSha"]
    if (
        _full_sha(before.get("commit"), f"{profile} repository commit") != source_sha
        or before.get("clean") is not True
        or before.get("untrackedPolicy") != "all-nonignored"
        or before.get("statusSha256") != hashlib.sha256(b"").hexdigest()
    ):
        raise ExternalEvidenceError(f"{profile}: repository identity is not the exact clean source commit")
    contract = _profile_contract(profile, repository_root)
    recorded_build = _recorded_path(record.get("evidenceDirectory"), f"{profile} evidence directory")
    if not _same_recorded_path(recorded_build, contract["build"]):
        raise ExternalEvidenceError(f"{profile}: evidence directory is not the canonical profile build")

    ci = _mapping(record.get("ci"), f"{profile} CI context")
    _exact_fields(
        ci,
        {"provider", "repository", "sourceCommit", "runId", "runAttempt", "workflowRef", "job", "runnerOs"},
        f"{profile} CI context",
    )
    expected_ci = {
        "provider": "github-actions",
        "repository": source_metadata["repository"]["fullName"],
        "sourceCommit": source_sha,
        "runId": str(source_metadata["source"]["runId"]),
        "runAttempt": str(source_metadata["source"]["runAttempt"]),
        "workflowRef": (
            f"{source_metadata['repository']['fullName']}/{SOURCE_WORKFLOW_PATH}@refs/heads/Working"
        ),
        "job": inventory._BUILD_MATRIX_PRODUCER_JOB,
        "runnerOs": "Windows",
    }
    if ci != expected_ci:
        raise ExternalEvidenceError(f"{profile}: producer CI record differs from the authorized source run")

    configure = _mapping(transaction.get("configure"), f"{profile} configure")
    _exact_fields(
        configure,
        {"executable", "executableIdentity", "version", "argv", "cwd", "exitCode"},
        f"{profile} configure",
    )
    executable = _recorded_path(configure.get("executable"), f"{profile} CMake executable")
    if configure.get("exitCode") != 0 or not _same_recorded_path(configure.get("cwd"), contract["source"]):
        raise ExternalEvidenceError(f"{profile}: configure did not succeed from the canonical source directory")
    configure_argv = _validate_configure_argv(configure.get("argv"), executable, contract, profile)
    executable_identity = _mapping(configure.get("executableIdentity"), f"{profile} executable identity")
    _exact_fields(executable_identity, {"bytes", "sha256"}, f"{profile} executable identity")
    if (
        type(executable_identity.get("bytes")) is not int
        or not 0 < executable_identity["bytes"] <= 256 * 1024 * 1024
    ):
        raise ExternalEvidenceError(f"{profile}: CMake executable size is invalid")
    _hex_digest(executable_identity.get("sha256"), f"{profile} executable digest")
    if not isinstance(configure.get("version"), str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", configure["version"]):
        raise ExternalEvidenceError(f"{profile}: CMake version is invalid")

    portable_evidence = copy.deepcopy(evidence)
    _portable_evidence_paths(portable_evidence, extracted_build, recorded_build)
    if not _same_recorded_path(portable_evidence.get("sourceDirectory"), contract["source"]) or not _same_recorded_path(
        portable_evidence.get("buildDirectory"), contract["build"]
    ):
        raise ExternalEvidenceError(f"{profile}: selected codemodel paths differ from the canonical profile")
    if (
        transaction.get("preset") != contract["preset"]
        or transaction.get("configuration") != contract["configuration"]
        or not _same_recorded_path(transaction.get("sourceDirectory"), contract["source"])
        or not _same_recorded_path(transaction.get("buildDirectory"), contract["build"])
    ):
        raise ExternalEvidenceError(f"{profile}: transaction contract differs from stable-v1")

    observed = _mapping(record.get("observed"), f"{profile} observed state")
    _exact_fields(
        observed,
        {
            "sourceDirectory", "buildDirectory", "preset", "configuration", "generator",
            "architecture", "toolset", "cacheVariables", "cmakeProducer",
        },
        f"{profile} observed state",
    )
    for field in ("generator", "architecture", "toolset", "cacheVariables", "cmakeProducer"):
        if observed.get(field) != portable_evidence.get(field):
            raise ExternalEvidenceError(f"{profile}: observed {field} differs from the selected reply")
    if (
        observed.get("preset") != contract["preset"]
        or observed.get("configuration") != contract["configuration"]
        or not _same_recorded_path(observed.get("sourceDirectory"), contract["source"])
        or not _same_recorded_path(observed.get("buildDirectory"), contract["build"])
    ):
        raise ExternalEvidenceError(f"{profile}: observed profile identity differs")
    cmake_producer = _mapping(observed.get("cmakeProducer"), f"{profile} CMake producer")
    if (
        not _same_recorded_path(cmake_producer.get("executable"), executable)
        or cmake_producer.get("version") != configure.get("version")
        or cmake_producer.get("generator") != observed.get("generator")
    ):
        raise ExternalEvidenceError(f"{profile}: File API producer differs from the configure transaction")

    reply = _mapping(record.get("reply"), f"{profile} reply record")
    _exact_fields(reply, {"index", "files", "digest"}, f"{profile} reply record")
    claimed_files = _list(reply.get("files"), f"{profile} reply files", inventory._MAX_REPLY_FILES)
    reply_digest = inventory._reply_records_digest(records)
    if claimed_files != records or reply.get("digest") != reply_digest or reply.get("index") != evidence["replyIndex"]:
        raise ExternalEvidenceError(f"{profile}: reply identities differ from the selected File API client")

    artifact_record = _mapping(record.get("artifacts"), f"{profile} artifact record")
    _exact_fields(artifact_record, {"state", "build", "targets"}, f"{profile} artifact record")
    if artifact_record.get("state") != "locally-observed-post-build":
        raise ExternalEvidenceError(f"{profile}: artifacts were not observed after a successful build")
    build = _mapping(artifact_record.get("build"), f"{profile} build transaction")
    _exact_fields(build, {"argv", "exitCode"}, f"{profile} build transaction")
    build_argv = _list(build.get("argv"), f"{profile} build argv", 16)
    if (
        build.get("exitCode") != 0
        or len(build_argv) != 6
        or not _same_recorded_path(build_argv[0], executable)
        or build_argv[1] != "--build"
        or not _same_recorded_path(build_argv[2], contract["build"])
        or build_argv[3:] != ["--config", contract["configuration"], "--parallel"]
    ):
        raise ExternalEvidenceError(f"{profile}: build transaction is not canonical and successful")
    try:
        actual_manifest = inventory._capture_artifact_manifest(evidence, extracted_build)
    except (inventory.InventoryError, OSError, ValueError) as error:
        raise ExternalEvidenceError(f"{profile}: cannot rehash configured products: {error}") from error
    portable_manifest = _portable_manifest(actual_manifest, extracted_build, recorded_build, profile)
    claimed_manifest = _list(
        artifact_record.get("targets"), f"{profile} claimed artifact manifest", inventory._MAX_TARGET_REFERENCES
    )
    if portable_manifest != claimed_manifest:
        raise ExternalEvidenceError(f"{profile}: downloaded product identities differ from producer provenance")
    try:
        inventory._apply_verified_artifact_manifest(portable_evidence, claimed_manifest)
    except inventory.InventoryError as error:
        raise ExternalEvidenceError(
            f"{profile}: configured targets do not match the verified artifact manifest: {error}"
        ) from error

    provenance_summary = {
        "state": "unavailable",
        "authority": inventory._BUILD_MATRIX_EXTERNAL_AUTHORITY,
        "authorityReason": _AUTHORITY_REASON,
        "structuralState": "validated",
        "recordFile": record_path.name,
        "recordSha256": record_digest,
        "producer": inventory._PROVENANCE_PRODUCER,
        "profile": profile,
        "repositoryRoot": repository_root,
        "sourceCommit": source_sha,
        "sourceClean": True,
        "untrackedPolicy": "all-nonignored",
        "replyDigest": reply_digest,
        "queryClient": client_name,
        "configureArgv": configure_argv,
        "cmakeExecutable": executable,
        "cmakeVersion": configure["version"],
        "ciProvider": ci["provider"],
        "ciRepository": ci["repository"],
        "ciRunId": ci["runId"],
        "ciRunAttempt": ci["runAttempt"],
        "ciWorkflowRef": ci["workflowRef"],
        "ciJob": ci["job"],
        "ciRunnerOs": ci["runnerOs"],
        "artifactState": "locally-observed-post-build",
    }
    portable_evidence["producerProvenance"] = provenance_summary
    if portable_evidence != producer_evidence:
        raise ExternalEvidenceError(f"{profile}: producer inventory differs from trusted raw-evidence reconstruction")

    profile_receipt = {
        "id": profile,
        "recordSha256": record_digest,
        "replyDigest": reply_digest,
        "artifactManifestSha256": hashlib.sha256(
            (json.dumps(claimed_manifest, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
        ).hexdigest(),
        "targetCount": len(portable_evidence.get("targets", [])),
        "artifactCount": sum(len(entry.get("artifactIdentities", [])) for entry in claimed_manifest),
    }
    return portable_evidence, profile_receipt


def _verify_pending_receipt_handoff(
    artifact_root: Path,
    reconstructed_receipt: dict[str, Any],
) -> None:
    receipt, receipt_payload = _read_json_payload(
        artifact_root / "build-matrix-pending-receipt.json",
        MAX_PENDING_RECEIPT_BYTES,
        "producer pending receipt",
    )
    stdout_receipt, stdout_payload = _read_json_payload(
        artifact_root / "build-matrix-pending-receipt-stdout.json",
        MAX_PENDING_RECEIPT_BYTES,
        "producer pending receipt stdout",
    )
    if receipt_payload != stdout_payload or receipt != stdout_receipt:
        raise ExternalEvidenceError("producer pending receipt file and stdout payload differ")
    if receipt != reconstructed_receipt:
        raise ExternalEvidenceError("producer pending receipt differs from trusted reconstruction")
    if receipt_payload != pending._json_bytes(reconstructed_receipt):
        raise ExternalEvidenceError("producer pending receipt is not canonical JSON")


def verify_external_evidence(
    artifact_root: Path,
    source_metadata: dict[str, Any],
) -> dict[str, Any]:
    metadata = validate_source_metadata(source_metadata)
    tree = _scan_artifact_tree(artifact_root)
    inventory_path = artifact_root / "build-matrix-inventory.json"
    report_path = artifact_root / "build-matrix-parity-findings.json"
    stdout_path = artifact_root / "build-matrix-parity-stdout.json"
    producer_inventory, inventory_payload = _read_json_payload(
        inventory_path, pending.MAX_INVENTORY_BYTES, "producer inventory"
    )
    producer_report, report_payload = _read_json_payload(
        report_path, pending.MAX_REPORT_BYTES, "producer parity report"
    )
    stdout_report, stdout_payload = _read_json_payload(
        stdout_path, pending.MAX_REPORT_BYTES, "producer parity stdout"
    )
    if report_payload != stdout_payload or producer_report != stdout_report:
        raise ExternalEvidenceError("producer parity file and stdout payload differ")

    producer_entries = _list(
        producer_inventory.get("configuredTargetEvidence"), "producer configured evidence", 16
    )
    by_profile: dict[str, dict[str, Any]] = {}
    for value in producer_entries:
        entry = _mapping(value, "producer configured evidence entry")
        identifier = str(entry.get("profile", ""))
        if identifier in by_profile:
            raise ExternalEvidenceError(f"producer inventory repeats profile {identifier!r}")
        by_profile[identifier] = entry
    if set(by_profile) != set(pending.EXPECTED_PROFILES):
        raise ExternalEvidenceError("producer inventory does not contain the exact stable-v1 profiles")

    reconstructed: list[dict[str, Any]] = []
    profile_receipts: list[dict[str, Any]] = []
    for profile in pending.EXPECTED_PROFILES:
        evidence, profile_receipt = _verify_profile(
            artifact_root, profile, by_profile[profile], metadata
        )
        reconstructed.append(evidence)
        profile_receipts.append(profile_receipt)

    reconstructed_inventory = inventory.build_inventory()
    reconstructed_inventory["repository"] = copy.deepcopy(producer_inventory.get("repository"))
    reconstructed_inventory["configuredTargetEvidence"] = reconstructed
    if reconstructed_inventory != producer_inventory:
        raise ExternalEvidenceError("producer inventory differs from the trusted exact-commit reconstruction")
    reconstructed_report = check_parity.build_report(reconstructed_inventory)
    if reconstructed_report != producer_report:
        raise ExternalEvidenceError("producer parity report differs from the trusted checker result")

    inventory_digest = hashlib.sha256(inventory_payload).hexdigest()
    report_digest = hashlib.sha256(report_payload).hexdigest()
    pending_receipt = pending.build_pending_receipt(
        reconstructed_inventory,
        reconstructed_report,
        inventory_sha256=inventory_digest,
        report_sha256=report_digest,
    )
    _verify_pending_receipt_handoff(artifact_root, pending_receipt)
    source = metadata["source"]
    artifact = metadata["artifact"]
    verifier = metadata["verifier"]
    return {
        "schemaVersion": 1,
        "kind": "spark-build-matrix-trusted-workflow-run",
        "state": "verified",
        "authority": AUTHORITY,
        "profile": "stable-v1",
        "source": {
            "repository": metadata["repository"]["fullName"],
            "workflowId": source["workflowId"],
            "workflowName": source["workflowName"],
            "workflowPath": source["workflowPath"],
            "runId": source["runId"],
            "runNumber": source["runNumber"],
            "runAttempt": source["runAttempt"],
            "event": source["event"],
            "conclusion": source["conclusion"],
            "headBranch": source["headBranch"],
            "headSha": source["headSha"],
            "jobId": source["jobId"],
            "jobName": source["jobName"],
            "jobConclusion": source["jobConclusion"],
            "expectedFinalStep": source["finalStepName"],
        },
        "verifier": {
            "repository": verifier["repository"],
            "workflowRef": verifier["workflowRef"],
            "workflowSha": verifier["workflowSha"],
            "checkoutSha": verifier["checkoutSha"],
            "sourceWorkflowBlobSha": verifier["sourceWorkflowBlobSha"],
            "trustedWorkflowBlobSha": verifier["trustedWorkflowBlobSha"],
        },
        "inputArtifact": {
            "id": artifact["id"],
            "name": artifact["name"],
            "bytes": artifact["bytes"],
            "digest": artifact["digest"],
            "extractedFileCount": tree["fileCount"],
            "extractedBytes": tree["bytes"],
            "inventorySha256": inventory_digest,
            "parityReportSha256": report_digest,
        },
        "profiles": profile_receipts,
        "pendingState": {
            "state": pending_receipt["state"],
            "authority": pending_receipt["authority"],
            "errorCount": pending_receipt["parity"]["errorCount"],
            "warningCount": pending_receipt["parity"]["warningCount"],
        },
    }


def _write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(value, indent=2, sort_keys=False) + "\n").encode("utf-8")
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
    except Exception:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-root", required=True, type=Path)
    parser.add_argument("--source-metadata", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        metadata, _ = _read_json_payload(args.source_metadata, MAX_METADATA_BYTES, "trusted source metadata")
        receipt = verify_external_evidence(args.artifact_root, metadata)
        _write_json_atomic(args.output, receipt)
        print(json.dumps(receipt, indent=2))
        return 0
    except (
        ExternalEvidenceError,
        pending.PendingAuthorityError,
        inventory.InventoryError,
        inventory.ReplyValidationError,
        OSError,
        ValueError,
        TypeError,
    ) as error:
        rejection = {"schemaVersion": 1, "state": "rejected", "reason": str(error)}
        try:
            _write_json_atomic(args.output, rejection)
        except OSError:
            pass
        print(json.dumps(rejection, indent=2))
        print(f"BUILD-MATRIX EXTERNAL EVIDENCE REJECTED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
