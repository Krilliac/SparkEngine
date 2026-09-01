#!/usr/bin/env python3
"""Validate the exact CI-120 producer state awaiting external attestation.

This validator deliberately cannot promote producer evidence to ``verified``.
It accepts only the reviewed three-error state emitted after all three
configured profiles have been built and structurally validated.  A separate
protected ``workflow_run`` verifier owns the authority transition.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from pathlib import Path
from typing import Any

import inventory


EXPECTED_PROFILES = (
    "installed-sdk-consumer",
    "windows-shipping",
    "windows-validation",
)
EXPECTED_ERROR_CATEGORY = "codemodel-producer-authority-unavailable"
EXPECTED_WARNING_CATEGORY = "target-name-unresolved"
MAX_INVENTORY_BYTES = 128 * 1024 * 1024
MAX_REPORT_BYTES = 64 * 1024 * 1024


class PendingAuthorityError(ValueError):
    """The producer output is not the one exact externally pending state."""


def _require_mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PendingAuthorityError(f"{label} must be an object")
    return value


def _require_list(value: Any, label: str, maximum: int) -> list[Any]:
    if not isinstance(value, list) or len(value) > maximum:
        raise PendingAuthorityError(f"{label} must be an array of at most {maximum} entries")
    return value


def _full_sha(value: Any, label: str) -> str:
    text = str(value or "").lower()
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", text):
        raise PendingAuthorityError(f"{label} must be a full hexadecimal commit id")
    return text


def _sha256(value: Any, label: str) -> str:
    text = str(value or "")
    if not re.fullmatch(r"[0-9a-f]{64}", text):
        raise PendingAuthorityError(f"{label} must be a lowercase SHA-256 digest")
    return text


def _read_regular_json(path: Path, maximum: int, label: str) -> tuple[dict[str, Any], str]:
    metadata = os.lstat(path)
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise PendingAuthorityError(f"{label} must be a regular file, not a link")
    if metadata.st_size < 2 or metadata.st_size > maximum:
        raise PendingAuthorityError(f"{label} size is outside the accepted bound")
    payload = path.read_bytes()
    if len(payload) != metadata.st_size:
        raise PendingAuthorityError(f"{label} changed while it was read")
    try:
        value = inventory._decode_bounded_json(payload, label)
    except inventory.ReplyValidationError as error:
        raise PendingAuthorityError(f"{label} is not strict bounded JSON: {error}") from error
    return _require_mapping(value, label), hashlib.sha256(payload).hexdigest()


def _validate_report(report: dict[str, Any]) -> dict[str, Any]:
    required = {"schemaVersion", "profile", "state", "errorCount", "warningCount", "findings"}
    if set(report) != required:
        raise PendingAuthorityError("parity report fields do not match schema version 3")
    if (
        report.get("schemaVersion") != 3
        or report.get("profile") != "stable-v1"
        or report.get("state") != "blocked"
        or report.get("errorCount") != len(EXPECTED_PROFILES)
        or report.get("warningCount") != 2
    ):
        raise PendingAuthorityError("parity report is not the exact reviewed externally pending state")

    findings = _require_list(report.get("findings"), "parity findings", 64)
    errors: list[dict[str, Any]] = []
    warnings: list[dict[str, Any]] = []
    for offset, raw in enumerate(findings):
        finding = _require_mapping(raw, f"parity finding {offset}")
        if not {"category", "severity", "message"}.issubset(finding) or not set(finding).issubset(
            {"category", "severity", "message", "detail"}
        ):
            raise PendingAuthorityError(f"parity finding {offset} has unexpected fields")
        if not isinstance(finding.get("message"), str) or not finding["message"]:
            raise PendingAuthorityError(f"parity finding {offset} has no message")
        if finding.get("severity") == "error":
            errors.append(finding)
        elif finding.get("severity") == "warning":
            warnings.append(finding)
        else:
            raise PendingAuthorityError(f"parity finding {offset} has an invalid severity")

    if len(errors) != len(EXPECTED_PROFILES) or len(warnings) != 2:
        raise PendingAuthorityError("parity finding cardinality differs from its counters")
    if {item.get("category") for item in errors} != {EXPECTED_ERROR_CATEGORY}:
        raise PendingAuthorityError("parity has a blocking error other than missing external authority")
    if {item.get("category") for item in warnings} != {EXPECTED_WARNING_CATEGORY}:
        raise PendingAuthorityError("parity warnings differ from the reviewed static target-name warnings")

    error_profiles: set[str] = set()
    for finding in errors:
        match = re.fullmatch(
            r"Profile '([a-z0-9][a-z0-9-]{0,63})' has structurally validated but untrusted CI-120 evidence",
            finding["message"],
        )
        if match is None:
            raise PendingAuthorityError("authority finding does not identify one exact profile")
        error_profiles.add(match.group(1))
    if error_profiles != set(EXPECTED_PROFILES):
        raise PendingAuthorityError("authority findings do not cover the exact stable-v1 profiles")
    return {
        "state": report["state"],
        "errorCount": report["errorCount"],
        "warningCount": report["warningCount"],
        "errorCategory": EXPECTED_ERROR_CATEGORY,
        "warningCategory": EXPECTED_WARNING_CATEGORY,
    }


def _validate_inventory(inventory_document: dict[str, Any]) -> tuple[str, list[dict[str, Any]], dict[str, str]]:
    if inventory_document.get("schemaVersion") != 3:
        raise PendingAuthorityError("inventory schemaVersion must be 3")
    profile = _require_mapping(inventory_document.get("profile"), "inventory profile")
    if profile.get("id") != "stable-v1":
        raise PendingAuthorityError("inventory does not describe stable-v1")
    configurations = _require_list(profile.get("buildConfigurations"), "build configurations", 16)
    configuration_ids = [entry.get("id") for entry in configurations if isinstance(entry, dict)]
    if sorted(configuration_ids) != list(EXPECTED_PROFILES) or len(configuration_ids) != len(EXPECTED_PROFILES):
        raise PendingAuthorityError("inventory build configurations differ from the exact stable-v1 set")

    repository = _require_mapping(inventory_document.get("repository"), "inventory repository")
    if set(repository) != {"root", "commit", "clean", "untrackedPolicy", "statusSha256"}:
        raise PendingAuthorityError("inventory repository identity fields are incomplete")
    commit = _full_sha(repository.get("commit"), "inventory repository commit")
    if (
        not isinstance(repository.get("root"), str)
        or not repository["root"]
        or repository.get("clean") is not True
        or repository.get("untrackedPolicy") != "all-nonignored"
        or repository.get("statusSha256") != hashlib.sha256(b"").hexdigest()
    ):
        raise PendingAuthorityError("inventory repository identity is not an exact clean checkout")

    evidence_list = _require_list(
        inventory_document.get("configuredTargetEvidence"), "configured target evidence", 16
    )
    evidence_by_profile: dict[str, dict[str, Any]] = {}
    for offset, raw in enumerate(evidence_list):
        evidence = _require_mapping(raw, f"configured evidence {offset}")
        identifier = str(evidence.get("profile", ""))
        if identifier in evidence_by_profile:
            raise PendingAuthorityError(f"configured evidence repeats profile {identifier!r}")
        evidence_by_profile[identifier] = evidence
    if set(evidence_by_profile) != set(EXPECTED_PROFILES):
        raise PendingAuthorityError("configured evidence does not cover the exact stable-v1 profiles")

    summaries: list[dict[str, Any]] = []
    shared_ci: dict[str, str] | None = None
    provenance_fields = {
        "state", "authority", "authorityReason", "structuralState", "recordFile",
        "recordSha256", "producer", "profile", "repositoryRoot", "sourceCommit",
        "sourceClean", "untrackedPolicy", "replyDigest", "queryClient", "configureArgv",
        "cmakeExecutable", "cmakeVersion", "ciProvider", "ciRepository", "ciRunId",
        "ciRunAttempt", "ciWorkflowRef", "ciJob", "ciRunnerOs", "artifactState",
    }
    for identifier in EXPECTED_PROFILES:
        evidence = evidence_by_profile[identifier]
        if evidence.get("status") != "available":
            raise PendingAuthorityError(f"{identifier}: configured evidence is not available")
        provenance = _require_mapping(evidence.get("producerProvenance"), f"{identifier} provenance")
        if set(provenance) != provenance_fields:
            raise PendingAuthorityError(f"{identifier}: producer provenance fields are incomplete")
        if (
            provenance.get("state") != "unavailable"
            or provenance.get("authority") != inventory._CI120_EXTERNAL_AUTHORITY
            or provenance.get("structuralState") != "validated"
            or provenance.get("artifactState") != "locally-observed-post-build"
            or provenance.get("producer") != inventory._PROVENANCE_PRODUCER
            or provenance.get("profile") != identifier
            or provenance.get("sourceClean") is not True
            or provenance.get("untrackedPolicy") != "all-nonignored"
            or not isinstance(provenance.get("authorityReason"), str)
            or not provenance["authorityReason"]
        ):
            raise PendingAuthorityError(f"{identifier}: producer provenance is not structurally pending")
        source_commit = _full_sha(provenance.get("sourceCommit"), f"{identifier} source commit")
        if source_commit != commit:
            raise PendingAuthorityError(f"{identifier}: producer source commit differs from inventory")
        record_digest = _sha256(provenance.get("recordSha256"), f"{identifier} record digest")
        reply_digest = _sha256(provenance.get("replyDigest"), f"{identifier} reply digest")
        if provenance.get("recordFile") != f"{identifier}-{inventory._PROVENANCE_FILE}":
            raise PendingAuthorityError(f"{identifier}: provenance record filename is not canonical")

        targets = _require_list(evidence.get("targets"), f"{identifier} configured targets", 4096)
        if not targets:
            raise PendingAuthorityError(f"{identifier}: configured evidence has no targets")
        for offset, raw_target in enumerate(targets):
            target = _require_mapping(raw_target, f"{identifier} target {offset}")
            if target.get("artifactState") != "locally-observed-post-build":
                raise PendingAuthorityError(f"{identifier}: target artifact was not observed post-build")
            identities = _require_list(
                target.get("artifactIdentities"), f"{identifier} target artifact identities", 32
            )
            if not identities:
                raise PendingAuthorityError(f"{identifier}: target has no artifact identities")

        ci = {
            "provider": str(provenance.get("ciProvider", "")),
            "repository": str(provenance.get("ciRepository", "")),
            "runId": str(provenance.get("ciRunId", "")),
            "runAttempt": str(provenance.get("ciRunAttempt", "")),
            "workflowRef": str(provenance.get("ciWorkflowRef", "")),
            "job": str(provenance.get("ciJob", "")),
            "runnerOs": str(provenance.get("ciRunnerOs", "")),
        }
        if (
            ci["provider"] != "github-actions"
            or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", ci["repository"])
            or not ci["runId"].isdigit()
            or not ci["runAttempt"].isdigit()
            or ci["job"] != inventory._CI120_PRODUCER_JOB
            or ci["runnerOs"] != "Windows"
            or ci["workflowRef"]
            != f"{ci['repository']}/{inventory._CI120_WORKFLOW_PATH}@refs/heads/Working"
        ):
            raise PendingAuthorityError(f"{identifier}: producer CI identity is not canonical")
        if shared_ci is None:
            shared_ci = ci
        elif shared_ci != ci:
            raise PendingAuthorityError("configured profiles came from different CI producer runs")

        summaries.append(
            {
                "id": identifier,
                "structuralState": "validated",
                "artifactState": "locally-observed-post-build",
                "recordSha256": record_digest,
                "replyDigest": reply_digest,
                "targetCount": len(targets),
            }
        )
    assert shared_ci is not None
    return commit, summaries, shared_ci


def build_pending_receipt(
    inventory_document: dict[str, Any],
    report: dict[str, Any],
    *,
    inventory_sha256: str,
    report_sha256: str,
) -> dict[str, Any]:
    """Return a deterministic receipt only for the exact pending state."""
    commit, profiles, ci = _validate_inventory(inventory_document)
    parity = _validate_report(report)
    return {
        "schemaVersion": 1,
        "state": "pending-external-attestation",
        "authority": inventory._CI120_EXTERNAL_AUTHORITY,
        "profile": "stable-v1",
        "sourceCommit": commit,
        "producer": ci,
        "inputs": {
            "inventorySha256": _sha256(inventory_sha256, "inventory digest"),
            "parityReportSha256": _sha256(report_sha256, "parity report digest"),
        },
        "profiles": profiles,
        "parity": parity,
    }


def _json_bytes(value: dict[str, Any]) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=False) + "\n").encode("utf-8")


def _write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = _json_bytes(value)
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


def _write_json_stdout(value: dict[str, Any]) -> None:
    sys.stdout.buffer.write(_json_bytes(value))
    sys.stdout.buffer.flush()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        inventory_document, inventory_digest = _read_regular_json(
            args.inventory, MAX_INVENTORY_BYTES, "CI-120 inventory"
        )
        report, report_digest = _read_regular_json(args.report, MAX_REPORT_BYTES, "CI-120 parity report")
        receipt = build_pending_receipt(
            inventory_document,
            report,
            inventory_sha256=inventory_digest,
            report_sha256=report_digest,
        )
        _write_json_atomic(args.output, receipt)
        _write_json_stdout(receipt)
        return 0
    except (OSError, PendingAuthorityError, ValueError, TypeError) as error:
        rejection = {"schemaVersion": 1, "state": "rejected", "reason": str(error)}
        try:
            _write_json_atomic(args.output, rejection)
        except OSError:
            pass
        _write_json_stdout(rejection)
        print(f"CI-120 PENDING STATE REJECTED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
