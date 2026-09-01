#!/usr/bin/env python3
"""Adversarial acceptance tests for the protected CI-120 verifier."""

from __future__ import annotations

import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Tools" / "buildmatrix"))

import inventory  # noqa: E402
import verify_external_evidence as verifier  # noqa: E402
import workflow  # noqa: E402


COMMIT = "a" * 40
REPOSITORY_ROOT = "D:/a/SparkEngine/SparkEngine"
EXECUTABLE = "C:/Program Files/CMake/bin/cmake.exe"


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(value, indent=2) + "\n").encode("utf-8"))


def source_metadata(commit: str = COMMIT) -> dict[str, Any]:
    return {
        "schemaVersion": 1,
        "repository": {
            "id": 123,
            "fullName": "Krilliac/SparkEngine",
            "defaultBranch": "Working",
        },
        "source": {
            "workflowId": 456,
            "workflowName": verifier.SOURCE_WORKFLOW_NAME,
            "workflowPath": verifier.SOURCE_WORKFLOW_PATH,
            "runId": 789,
            "runNumber": 12,
            "runAttempt": 1,
            "event": "push",
            "conclusion": "failure",
            "headBranch": "Working",
            "headSha": commit,
            "jobId": 987,
            "jobName": verifier.SOURCE_JOB_NAME,
            "jobConclusion": "failure",
            "finalStepName": verifier.SOURCE_FINAL_STEP,
            "finalStepConclusion": "failure",
        },
        "artifact": {
            "id": 654,
            "name": f"ci120-untrusted-stable-v1-{commit}-1",
            "bytes": 1024,
            "digest": f"sha256:{'b' * 64}",
        },
        "verifier": {
            "repository": "Krilliac/SparkEngine",
            "checkoutSha": commit,
            "workflowSha": commit,
            "workflowRef": "Krilliac/SparkEngine/.github/workflows/ci120-report.yml@refs/heads/Working",
            "sourceWorkflowBlobSha": "d" * 40,
            "trustedWorkflowBlobSha": "d" * 40,
        },
    }


def shipping_fixture(artifact_root: Path, *, include_pdb: bool = False) -> tuple[dict[str, Any], Path]:
    """Create one real reply/provenance/product tree with recorded Windows paths."""
    profile = "windows-shipping"
    build = artifact_root / "build" / "windows-shipping"
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    recorded_build = f"{REPOSITORY_ROOT}/build/windows-shipping"
    artifact = build / "bin" / "MinSizeRel" / "SparkEngine.exe"
    artifact.parent.mkdir(parents=True)
    artifact.write_bytes(b"synthetic-spark-engine-product")
    if include_pdb:
        artifact.with_suffix(".pdb").write_bytes(b"synthetic-debug-symbols")

    target_id = "SparkEngine::@synthetic-0"
    configurations: list[dict[str, Any]] = []
    generator_targets = (
        ("ALL_BUILD", "ALL_BUILD::@root"),
        ("ALL_BUILD", "ALL_BUILD::@subdir"),
        ("ZERO_CHECK", "ZERO_CHECK::@root"),
    )
    for configuration in ("Debug", "Release", "MinSizeRel", "RelWithDebInfo"):
        references: list[dict[str, str]] = []
        product_file = f"target-SparkEngine-{configuration}.json"
        write_json(
            reply / product_file,
            {
                "name": "SparkEngine",
                "id": target_id,
                "type": "EXECUTABLE",
                "nameOnDisk": "SparkEngine.exe",
                "artifacts": [
                    {"path": f"bin/{configuration}/SparkEngine.exe"},
                    {"path": f"bin/{configuration}/SparkEngine.pdb"},
                ],
            },
        )
        references.append({"name": "SparkEngine", "id": target_id, "jsonFile": product_file})
        for offset, (name, generator_id) in enumerate(generator_targets):
            target_file = f"target-generator-{offset}-{configuration}.json"
            write_json(
                reply / target_file,
                {
                    "name": name,
                    "id": generator_id,
                    "type": "UTILITY",
                    "isGeneratorProvided": True,
                    "sources": [],
                },
            )
            references.append({"name": name, "id": generator_id, "jsonFile": target_file})
        configurations.append({"name": configuration, "targets": references})
    write_json(
        reply / "codemodel-v2.json",
        {
            "paths": {"source": REPOSITORY_ROOT, "build": recorded_build},
            "configurations": configurations,
        },
    )
    resolved = inventory.resolve_configure_preset(
        inventory.extract_cmake_presets(), "windows-shipping"
    )
    cache = {
        "CMAKE_GENERATOR": "Visual Studio 17 2022",
        "CMAKE_GENERATOR_PLATFORM": "x64",
        "CMAKE_GENERATOR_TOOLSET": "v143",
        "CMAKE_HOME_DIRECTORY": REPOSITORY_ROOT,
        **{name: str(value) for name, value in resolved.get("cacheVariables", {}).items()},
    }
    write_json(
        reply / "cache-v2.json",
        {"entries": [{"name": name, "value": value} for name, value in sorted(cache.items())]},
    )
    run_id = "1" * 32
    client = inventory._CAPTURE_CLIENT_PREFIX + run_id
    query = inventory._capture_query(profile, run_id)
    objects = [
        {"kind": "codemodel", "version": {"major": 2}, "jsonFile": "codemodel-v2.json"},
        {"kind": "cache", "version": {"major": 2}, "jsonFile": "cache-v2.json"},
    ]
    write_json(
        reply / "index-0001.json",
        {
            "cmake": {
                "version": {"string": "9.9.9"},
                "paths": {"cmake": EXECUTABLE},
                "generator": {
                    "name": "Visual Studio 17 2022",
                    "platform": "x64",
                    "multiConfig": True,
                },
            },
            "objects": objects,
            "reply": {
                client: {
                    "query.json": {
                        "client": query["client"],
                        "requests": query["requests"],
                        "responses": objects,
                    }
                }
            },
        },
    )
    core = inventory._extract_reply_core(build, profile, client_name=client, query=query)
    assert core is not None
    evidence, snapshot, records = core
    snapshot.close()
    actual_manifest = inventory._capture_artifact_manifest(evidence, build)
    claimed_manifest = verifier._portable_manifest(actual_manifest, build, recorded_build, profile)
    repository = {
        "root": REPOSITORY_ROOT,
        "commit": COMMIT,
        "clean": True,
        "untrackedPolicy": "all-nonignored",
        "statusSha256": hashlib.sha256(b"").hexdigest(),
    }
    configure_argv = [EXECUTABLE, "--preset", "windows-shipping"]
    ci = {
        "provider": "github-actions",
        "repository": "Krilliac/SparkEngine",
        "sourceCommit": COMMIT,
        "runId": "789",
        "runAttempt": "1",
        "workflowRef": "Krilliac/SparkEngine/.github/workflows/build.yml@refs/heads/Working",
        "job": inventory._CI120_PRODUCER_JOB,
        "runnerOs": "Windows",
    }
    record = {
        "schemaVersion": inventory._PROVENANCE_SCHEMA,
        "producer": inventory._PROVENANCE_PRODUCER,
        "profile": profile,
        "evidenceDirectory": recorded_build,
        "ci": ci,
        "transaction": {
            "runId": run_id,
            "queryClient": client,
            "querySha256": hashlib.sha256(
                (json.dumps(query, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
            ).hexdigest(),
            "query": query,
            "profile": profile,
            "preset": "windows-shipping",
            "configuration": "MinSizeRel",
            "sourceDirectory": REPOSITORY_ROOT,
            "buildDirectory": recorded_build,
            "configure": {
                "executable": EXECUTABLE,
                "executableIdentity": {"bytes": 1024, "sha256": "c" * 64},
                "version": "9.9.9",
                "argv": configure_argv,
                "cwd": REPOSITORY_ROOT,
                "exitCode": 0,
            },
            "repositoryBefore": repository,
            "repositoryAfter": copy.deepcopy(repository),
        },
        "observed": {
            "sourceDirectory": evidence["sourceDirectory"],
            "buildDirectory": evidence["buildDirectory"],
            "preset": "windows-shipping",
            "configuration": "MinSizeRel",
            "generator": evidence["generator"],
            "architecture": evidence["architecture"],
            "toolset": evidence["toolset"],
            "cacheVariables": evidence["cacheVariables"],
            "cmakeProducer": evidence["cmakeProducer"],
        },
        "artifacts": {
            "state": "locally-observed-post-build",
            "build": {
                "argv": [EXECUTABLE, "--build", recorded_build, "--config", "MinSizeRel", "--parallel"],
                "exitCode": 0,
            },
            "targets": claimed_manifest,
        },
        "reply": {
            "index": evidence["replyIndex"],
            "files": records,
            "digest": inventory._reply_records_digest(records),
        },
    }
    record_path = inventory._provenance_path(build, profile)
    write_json(record_path, record)
    record_payload = record_path.read_bytes()
    portable = copy.deepcopy(evidence)
    verifier._portable_evidence_paths(portable, build, recorded_build)
    inventory._apply_verified_artifact_manifest(portable, claimed_manifest)
    portable["producerProvenance"] = {
        "state": "unavailable",
        "authority": inventory._CI120_EXTERNAL_AUTHORITY,
        "authorityReason": verifier._AUTHORITY_REASON,
        "structuralState": "validated",
        "recordFile": record_path.name,
        "recordSha256": hashlib.sha256(record_payload).hexdigest(),
        "producer": inventory._PROVENANCE_PRODUCER,
        "profile": profile,
        "repositoryRoot": REPOSITORY_ROOT,
        "sourceCommit": COMMIT,
        "sourceClean": True,
        "untrackedPolicy": "all-nonignored",
        "replyDigest": inventory._reply_records_digest(records),
        "queryClient": client,
        "configureArgv": configure_argv,
        "cmakeExecutable": EXECUTABLE,
        "cmakeVersion": "9.9.9",
        "ciProvider": ci["provider"],
        "ciRepository": ci["repository"],
        "ciRunId": ci["runId"],
        "ciRunAttempt": ci["runAttempt"],
        "ciWorkflowRef": ci["workflowRef"],
        "ciJob": ci["job"],
        "ciRunnerOs": ci["runnerOs"],
        "artifactState": "locally-observed-post-build",
    }
    return portable, artifact


class SourceMetadataTests(unittest.TestCase):
    def test_nonfinite_json_is_rejected_before_metadata_validation(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "nonfinite.json"
            path.write_text('{"value": NaN}\n', encoding="utf-8")
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "strict bounded JSON"):
                verifier._read_json_payload(path, 1024, "untrusted document")

    def test_trusted_workflow_is_staged_fail_closed_and_attests_only_verified_output(self) -> None:
        workflow_path = REPO_ROOT / ".github" / "workflows" / "ci120-report.yml"
        text = workflow_path.read_text(encoding="utf-8")
        producer_text = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        document = workflow.parse_workflow_yaml(text)
        self.assertEqual(document["name"], "CI-120 Trusted Verifier")
        self.assertIn("workflow_run", document["on"])
        self.assertNotIn("pull_request_target", document["on"])
        self.assertIn("run.conclusion !== 'failure'", text)
        self.assertIn("digest-mismatch: error", text)
        self.assertIn("verify_external_evidence.py", text)
        self.assertIn("actions/attest@1e69f48acb82d1966a394da916b4c1698aa569d6", text)
        self.assertLess(text.index("verify_external_evidence.py"), text.index("actions/attest@"))
        upload_start = producer_text.index("- name: Upload untrusted CI-120 structural evidence")
        upload_end = producer_text.index("- name: Enforce reviewed CI-120 findings", upload_start)
        upload_step = producer_text[upload_start:upload_end]
        self.assertIn("build/windows-shipping/.cmake/api/v1/reply", upload_step)
        self.assertIn("build-matrix-pending-receipt.json", upload_step)
        self.assertIn("build-matrix-pending-receipt-stdout.json", upload_step)
        self.assertIn("include-hidden-files: true", upload_step)

    def test_pending_receipt_artifact_names_are_exactly_bounded(self) -> None:
        self.assertTrue(verifier._allowed_artifact_path("build-matrix-pending-receipt.json"))
        self.assertTrue(verifier._allowed_artifact_path("build-matrix-pending-receipt-stdout.json"))
        for relative in (
            "build-matrix-pending-receipt.json.bak",
            "Build-Matrix-Pending-Receipt.json",
            "nested/build-matrix-pending-receipt.json",
        ):
            self.assertFalse(verifier._allowed_artifact_path(relative))

    def test_exact_staged_source_metadata_is_accepted(self) -> None:
        self.assertEqual(verifier.validate_source_metadata(source_metadata())["source"]["headSha"], COMMIT)

    def test_historical_source_is_accepted_when_trusted_workflow_blob_matches(self) -> None:
        metadata = source_metadata()
        metadata["verifier"]["checkoutSha"] = "8" * 40
        metadata["verifier"]["workflowSha"] = "8" * 40
        self.assertEqual(verifier.validate_source_metadata(metadata)["source"]["headSha"], COMMIT)

    def test_changed_source_workflow_blob_is_rejected(self) -> None:
        metadata = source_metadata()
        metadata["verifier"]["sourceWorkflowBlobSha"] = "7" * 40
        with self.assertRaisesRegex(verifier.ExternalEvidenceError, "not exact"):
            verifier.validate_source_metadata(metadata)

    def test_stale_trusted_checkout_is_rejected(self) -> None:
        metadata = source_metadata()
        metadata["verifier"]["checkoutSha"] = "9" * 40
        with self.assertRaisesRegex(verifier.ExternalEvidenceError, "not exact"):
            verifier.validate_source_metadata(metadata)

    def test_old_successful_source_shape_is_not_accepted_during_staged_rollout(self) -> None:
        metadata = source_metadata()
        metadata["source"]["conclusion"] = "success"
        metadata["source"]["jobConclusion"] = "success"
        metadata["source"]["finalStepConclusion"] = "success"
        with self.assertRaisesRegex(verifier.ExternalEvidenceError, "staged fail-closed"):
            verifier.validate_source_metadata(metadata)

    def test_malformed_artifact_digest_is_rejected(self) -> None:
        metadata = source_metadata()
        metadata["artifact"]["digest"] = "sha256:not-a-digest"
        with self.assertRaisesRegex(verifier.ExternalEvidenceError, "SHA-256"):
            verifier.validate_source_metadata(metadata)

    def test_recorded_path_traversal_is_rejected(self) -> None:
        with self.assertRaisesRegex(verifier.ExternalEvidenceError, "contained relative"):
            verifier._portable_join(REPOSITORY_ROOT, "build/../escape")


class RawProfileEvidenceTests(unittest.TestCase):
    def test_absent_optional_pdb_is_accepted_only_with_hashed_primary_product(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            producer_evidence, _ = shipping_fixture(root)
            reconstructed, receipt = verifier._verify_profile(
                root, "windows-shipping", producer_evidence, source_metadata()
            )

        target = reconstructed["targets"][0]
        self.assertEqual(
            [Path(path).name for path in target["artifacts"]],
            ["SparkEngine.exe"],
        )
        self.assertEqual(
            [Path(row["path"]).name for row in target["artifactIdentities"]],
            ["SparkEngine.exe"],
        )
        self.assertEqual(receipt["artifactCount"], 1)

    def test_present_optional_pdb_is_retained_and_hashed(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            producer_evidence, _ = shipping_fixture(root, include_pdb=True)
            reconstructed, receipt = verifier._verify_profile(
                root, "windows-shipping", producer_evidence, source_metadata()
            )

        target = reconstructed["targets"][0]
        self.assertEqual(
            [Path(path).name for path in target["artifacts"]],
            ["SparkEngine.exe", "SparkEngine.pdb"],
        )
        self.assertEqual(
            [Path(row["path"]).name for row in target["artifactIdentities"]],
            ["SparkEngine.exe", "SparkEngine.pdb"],
        )
        self.assertEqual(receipt["artifactCount"], 2)

    def test_absent_primary_product_is_rejected_when_optional_pdb_is_also_absent(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            producer_evidence, artifact = shipping_fixture(root)
            artifact.unlink()
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "cannot inspect"):
                verifier._verify_profile(root, "windows-shipping", producer_evidence, source_metadata())

    def test_fresh_raw_reply_record_and_product_are_reconstructed(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            producer_evidence, _ = shipping_fixture(root)
            reconstructed, receipt = verifier._verify_profile(
                root, "windows-shipping", producer_evidence, source_metadata()
            )
        self.assertEqual(reconstructed, producer_evidence)
        self.assertEqual(receipt["targetCount"], 1)
        self.assertEqual(receipt["artifactCount"], 1)
        self.assertEqual(
            reconstructed["configurations"],
            ["Debug", "MinSizeRel", "RelWithDebInfo", "Release"],
        )
        self.assertEqual(reconstructed["replyFileCount"], 19)

    def test_noncanonical_configuration_document_tampering_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            producer_evidence, _ = shipping_fixture(root)
            target_path = (
                root
                / "build"
                / "windows-shipping"
                / ".cmake"
                / "api"
                / "v1"
                / "reply"
                / "target-SparkEngine-Debug.json"
            )
            target = json.loads(target_path.read_text(encoding="utf-8"))
            target["name"] = "TamperedDebugTarget"
            write_json(target_path, target)
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "disagrees with target document"):
                verifier._verify_profile(root, "windows-shipping", producer_evidence, source_metadata())

    def test_post_upload_product_tampering_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            producer_evidence, artifact = shipping_fixture(root)
            artifact.write_bytes(b"tampered-after-producer")
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "product identities differ"):
                verifier._verify_profile(root, "windows-shipping", producer_evidence, source_metadata())

    def test_replayed_record_from_another_source_sha_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            producer_evidence, _ = shipping_fixture(root)
            replay = source_metadata("9" * 40)
            replay["artifact"]["name"] = f"ci120-untrusted-stable-v1-{'9' * 40}-1"
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "repository identity"):
                verifier._verify_profile(root, "windows-shipping", producer_evidence, replay)


class PendingReceiptHandoffTests(unittest.TestCase):
    def write_receipts(self, root: Path, file_payload: bytes, stdout_payload: bytes) -> None:
        (root / "build-matrix-pending-receipt.json").write_bytes(file_payload)
        (root / "build-matrix-pending-receipt-stdout.json").write_bytes(stdout_payload)

    def test_exact_canonical_receipt_and_stdout_are_accepted(self) -> None:
        receipt = {"kind": "spark-ci120-pending-authority", "state": "pending-external-attestation"}
        payload = verifier.pending._json_bytes(receipt)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self.write_receipts(root, payload, payload)
            verifier._verify_pending_receipt_handoff(root, receipt)

    def test_file_and_stdout_byte_mismatch_is_rejected(self) -> None:
        receipt = {"kind": "spark-ci120-pending-authority", "state": "pending-external-attestation"}
        payload = verifier.pending._json_bytes(receipt)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self.write_receipts(root, payload, payload + b" ")
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "file and stdout"):
                verifier._verify_pending_receipt_handoff(root, receipt)

    def test_semantically_equal_noncanonical_receipt_is_rejected(self) -> None:
        receipt = {"kind": "spark-ci120-pending-authority", "state": "pending-external-attestation"}
        noncanonical = json.dumps(receipt, separators=(",", ":")).encode("utf-8")
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self.write_receipts(root, noncanonical, noncanonical)
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "canonical"):
                verifier._verify_pending_receipt_handoff(root, receipt)

    def test_receipt_semantic_mismatch_is_rejected(self) -> None:
        expected = {"kind": "spark-ci120-pending-authority", "state": "pending-external-attestation"}
        tampered = {**expected, "state": "verified"}
        payload = verifier.pending._json_bytes(tampered)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self.write_receipts(root, payload, payload)
            with self.assertRaisesRegex(verifier.ExternalEvidenceError, "reconstruction"):
                verifier._verify_pending_receipt_handoff(root, expected)


if __name__ == "__main__":
    unittest.main()
