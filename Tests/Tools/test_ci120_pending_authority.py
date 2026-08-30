#!/usr/bin/env python3
"""Acceptance tests for the CI-120 external-authority handoff state."""

from __future__ import annotations

import copy
import hashlib
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Tools" / "buildmatrix"))

import inventory  # noqa: E402
import validate_pending_authority as pending  # noqa: E402


COMMIT = "a" * 40
DIGEST = "b" * 64


def provenance(profile: str) -> dict[str, Any]:
    return {
        "state": "unavailable",
        "authority": inventory._CI120_EXTERNAL_AUTHORITY,
        "authorityReason": "A protected external verifier is required.",
        "structuralState": "validated",
        "recordFile": f"{profile}-{inventory._PROVENANCE_FILE}",
        "recordSha256": DIGEST,
        "producer": inventory._PROVENANCE_PRODUCER,
        "profile": profile,
        "repositoryRoot": "D:/a/SparkEngine/SparkEngine",
        "sourceCommit": COMMIT,
        "sourceClean": True,
        "untrackedPolicy": "all-nonignored",
        "replyDigest": "c" * 64,
        "queryClient": f"client-spark-ci120-{profile}",
        "configureArgv": ["C:/cmake.exe", "--preset", profile],
        "cmakeExecutable": "C:/cmake.exe",
        "cmakeVersion": "4.2.0",
        "ciProvider": "github-actions",
        "ciRepository": "Krilliac/SparkEngine",
        "ciRunId": "120",
        "ciRunAttempt": "1",
        "ciWorkflowRef": "Krilliac/SparkEngine/.github/workflows/build.yml@refs/heads/Working",
        "ciJob": inventory._CI120_PRODUCER_JOB,
        "ciRunnerOs": "Windows",
        "artifactState": "locally-observed-post-build",
    }


def valid_documents() -> tuple[dict[str, Any], dict[str, Any]]:
    evidence = []
    for profile in pending.EXPECTED_PROFILES:
        evidence.append(
            {
                "profile": profile,
                "status": "available",
                "targets": [
                    {
                        "target": f"target-{profile}",
                        "artifactState": "locally-observed-post-build",
                        "artifactIdentities": [{"bytes": 1, "sha256": "d" * 64}],
                    }
                ],
                "producerProvenance": provenance(profile),
            }
        )
    inventory_document = {
        "schemaVersion": 3,
        "profile": {
            "id": "stable-v1",
            "buildConfigurations": [{"id": profile} for profile in pending.EXPECTED_PROFILES],
        },
        "repository": {
            "root": "D:/a/SparkEngine/SparkEngine",
            "commit": COMMIT,
            "clean": True,
            "untrackedPolicy": "all-nonignored",
            "statusSha256": hashlib.sha256(b"").hexdigest(),
        },
        "configuredTargetEvidence": evidence,
    }
    report = {
        "schemaVersion": 3,
        "profile": "stable-v1",
        "state": "blocked",
        "errorCount": 3,
        "warningCount": 2,
        "findings": [
            {
                "category": pending.EXPECTED_WARNING_CATEGORY,
                "severity": "warning",
                "message": "Target name '${TARGET_NAME}' cannot be resolved statically",
            },
            {
                "category": pending.EXPECTED_WARNING_CATEGORY,
                "severity": "warning",
                "message": "Another target name cannot be resolved statically",
            },
            *[
                {
                    "category": pending.EXPECTED_ERROR_CATEGORY,
                    "severity": "error",
                    "message": (
                        f"Profile '{profile}' has structurally validated but untrusted CI-120 evidence"
                    ),
                }
                for profile in pending.EXPECTED_PROFILES
            ],
        ],
    }
    return inventory_document, report


class PendingAuthorityTests(unittest.TestCase):
    def test_duplicate_json_keys_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "duplicate.json"
            path.write_text('{"schemaVersion": 3, "schemaVersion": 3}\n', encoding="utf-8")
            with self.assertRaisesRegex(pending.PendingAuthorityError, "strict bounded JSON"):
                pending._read_regular_json(path, 1024, "duplicate document")

    def receipt(self, inventory_document: dict[str, Any], report: dict[str, Any]) -> dict[str, Any]:
        return pending.build_pending_receipt(
            inventory_document,
            report,
            inventory_sha256="e" * 64,
            report_sha256="f" * 64,
        )

    def test_fresh_exact_pending_state_is_accepted(self) -> None:
        inventory_document, report = valid_documents()
        receipt = self.receipt(inventory_document, report)
        self.assertEqual(receipt["state"], "pending-external-attestation")
        self.assertEqual(receipt["sourceCommit"], COMMIT)
        self.assertEqual([entry["id"] for entry in receipt["profiles"]], list(pending.EXPECTED_PROFILES))

    def test_old_job_local_verified_state_is_rejected(self) -> None:
        inventory_document, report = valid_documents()
        inventory_document["configuredTargetEvidence"][0]["producerProvenance"]["state"] = "verified"
        with self.assertRaisesRegex(pending.PendingAuthorityError, "not structurally pending"):
            self.receipt(inventory_document, report)

    def test_mixed_available_and_absent_profiles_are_rejected(self) -> None:
        inventory_document, report = valid_documents()
        inventory_document["configuredTargetEvidence"][1]["status"] = "absent"
        with self.assertRaisesRegex(pending.PendingAuthorityError, "not available"):
            self.receipt(inventory_document, report)

    def test_unreviewed_extra_parity_error_is_rejected(self) -> None:
        inventory_document, report = valid_documents()
        report["findings"].append(
            {"category": "configured-target-missing", "severity": "error", "message": "missing"}
        )
        report["errorCount"] = 4
        with self.assertRaisesRegex(pending.PendingAuthorityError, "exact reviewed"):
            self.receipt(inventory_document, report)

    def test_replayed_profile_from_another_commit_is_rejected(self) -> None:
        inventory_document, report = valid_documents()
        inventory_document["configuredTargetEvidence"][2]["producerProvenance"]["sourceCommit"] = "9" * 40
        with self.assertRaisesRegex(pending.PendingAuthorityError, "differs from inventory"):
            self.receipt(inventory_document, report)

    def test_malformed_or_extra_provenance_field_is_rejected(self) -> None:
        inventory_document, report = valid_documents()
        forged = copy.deepcopy(inventory_document)
        forged["configuredTargetEvidence"][0]["producerProvenance"]["attested"] = True
        with self.assertRaisesRegex(pending.PendingAuthorityError, "fields are incomplete"):
            self.receipt(forged, report)


if __name__ == "__main__":
    unittest.main()
