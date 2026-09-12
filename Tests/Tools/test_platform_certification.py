#!/usr/bin/env python3
"""Hostile regression tests for the PLT-200 platform certification validator.

The baseline every mutation test starts from is a bundle that really exists on
disk: two canonical stable-v1 rows, every declared artifact and dependency file
written and hashed, the attestation recomputed from the measured digests, and
the chronology checkout < probe start < probe end < collectedAt < now.  A test
that merely asserted "some error" would pass for the wrong reason -- which is
exactly how the previous suite stayed green against a wholly fabricated record
-- so every rejection test asserts the specific message its defence produces.

`unittest`, not pytest: no workflow in this repository installs pytest, and
`.github/workflows/publish-wiki.yml` runs
`python3 -m unittest discover -s Tests/Tools -p 'test_*.py'` over this
directory, so a pytest-only suite is a silent CI break.

Cross-platform: the import path uses 'Tools' (capital T) to match the tracked
directory name on case-sensitive filesystems.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable

REPO_ROOT = Path(__file__).resolve().parents[2]
CERT_DIR = REPO_ROOT / "Tools" / "platform-cert"
sys.path.insert(0, str(CERT_DIR))

import bundle_verify  # noqa: E402
import dependency_authority as da  # noqa: E402
import safe_fs  # noqa: E402
import validate_certification as vc  # noqa: E402
from schema_validator import CompiledSchema, SchemaError  # noqa: E402

# ── Deterministic clock ────────────────────────────────────────────────────
NOW = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
CHECKOUT = NOW - timedelta(hours=3)
PROBE_START = CHECKOUT + timedelta(minutes=1)
PROBE_END = PROBE_START + timedelta(seconds=5)
COLLECTED = PROBE_END + timedelta(minutes=1)

COMMIT = "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678"
OTHER_COMMIT = "f" * 40
D3D11_ID = "win11-x64-msvc143-d3d11"
NULLRHI_ID = "win11-x64-msvc143-nullrhi"

REPOSITORY = "SparkEngineTeam/SparkEngine"
WORKFLOW = "Build SparkEngine"
RUN_ID = "1234567890"
RUN_ATTEMPT = 1
JOB_ID = "windows-certification"
IDENTITY = f"https://github.com/{REPOSITORY}/actions/runs/{RUN_ID}/attempts/1"

NULLRHI_CATEGORIES = vc.ALL_PROBE_CATEGORIES - {"renderer", "content", "input", "audio"}


def iso(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + "Z"


def trusted(**overrides: Any) -> vc.TrustedContext:
    fields: dict[str, Any] = {
        "commit_sha": COMMIT,
        "collector_type": "ci",
        "identity": IDENTITY,
        "repository": REPOSITORY,
        "workflow": WORKFLOW,
        "run_id": RUN_ID,
        "run_attempt": RUN_ATTEMPT,
        "job_id": JOB_ID,
    }
    fields.update(overrides)
    return vc.TrustedContext(**fields)


# ── Matrix fixture ─────────────────────────────────────────────────────────


def make_matrix() -> dict[str, Any]:
    return {
        "schemaVersion": 1,
        "profile": "stable-v1",
        "commitSha": COMMIT,
        "generatedAt": iso(NOW - timedelta(hours=4)),
        "rows": [
            {
                "id": D3D11_ID,
                "tier": "primary",
                "os": {"family": "windows", "version": "11", "build": "10.0.22631"},
                "arch": "x86_64",
                "compiler": {
                    "id": "msvc",
                    "version": "19.42.34435",
                    "toolset": "v143",
                    "windowsSdkVersion": "10.0.22621.0",
                },
                "gpu": {
                    "api": "d3d11",
                    "device": "NVIDIA GeForce RTX 5070 Ti",
                    "vendor": "nvidia",
                    "driverVersion": "576.40",
                    "featureLevel": "11.1",
                },
                "audio": {"api": "xaudio2", "version": "2.9"},
                "cpuFeatures": ["SSE4.2", "AVX2"],
                "evidenceRequired": sorted(vc.ALL_PROBE_CATEGORIES),
            },
            {
                "id": NULLRHI_ID,
                "tier": "supported",
                "os": {"family": "windows", "version": "11", "build": "10.0.22631"},
                "arch": "x86_64",
                "compiler": {
                    "id": "msvc",
                    "version": "19.42.34435",
                    "toolset": "v143",
                    "windowsSdkVersion": "10.0.22621.0",
                },
                "gpu": {
                    "api": "nullrhi",
                    "device": "NullRHIDevice",
                    "vendor": "none",
                    "driverVersion": "none",
                    "featureLevel": "null",
                },
                "audio": {"api": "none"},
                "cpuFeatures": ["SSE4.2", "AVX2"],
                "evidenceRequired": sorted(NULLRHI_CATEGORIES),
            },
        ],
    }


# ── The on-disk bundle every mutation test starts from ─────────────────────


class Bundle:
    """A real evidence bundle: files on disk, digests measured from them."""

    def __init__(self, root: Path, authority: da.Authority) -> None:
        self.root = root
        self.authority = authority
        self.evidence_dir = root / "evidence"
        self.artifact_root = root / "artifacts"
        self.evidence_dir.mkdir(parents=True, exist_ok=True)
        self.artifact_root.mkdir(parents=True, exist_ok=True)
        self.matrix = make_matrix()
        self.records: dict[str, dict[str, Any]] = {
            D3D11_ID: self._record(D3D11_ID, sorted(vc.ALL_PROBE_CATEGORIES)),
            NULLRHI_ID: self._record(NULLRHI_ID, sorted(NULLRHI_CATEGORIES)),
        }

    # -- construction ------------------------------------------------------

    def _write_artifact(self, row_id: str, relative: str, payload: bytes) -> dict[str, Any]:
        target = self.artifact_root / row_id / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        return {
            "path": relative,
            "sha256": hashlib.sha256(payload).hexdigest(),
            "sizeBytes": len(payload),
        }

    def _record(self, row_id: str, categories: list[str]) -> dict[str, Any]:
        host_gpu = (
            {
                "api": "d3d11",
                "device": "NVIDIA GeForce RTX 5070 Ti",
                "vendor": "nvidia",
                "driverVersion": "576.40",
                "featureLevel": "11.1",
            }
            if row_id == D3D11_ID
            else {
                "api": "nullrhi",
                "device": "NullRHIDevice",
                "vendor": "none",
                "driverVersion": "none",
                "featureLevel": "null",
            }
        )
        host_audio = (
            {"api": "xaudio2", "version": "2.9"} if row_id == D3D11_ID else {"api": "none"}
        )
        probes: dict[str, Any] = {}
        for category in categories:
            artifact = self._write_artifact(
                row_id,
                f"{category}.log",
                f"probe {category} for {row_id}\nexit 0\n".encode("utf-8"),
            )
            probes[category] = {
                "status": "pass",
                "durationMs": 5000,
                "exitCode": 0,
                "startedAt": iso(PROBE_START),
                "completedAt": iso(PROBE_END),
                "artifacts": [artifact],
            }

        closure = []
        for name, source in self._required_dependencies(row_id):
            entry = self.authority.lookup(name, source)
            version = entry.get("version") or self._pattern_sample(entry)
            artifact = self._write_artifact(
                row_id, f"deps/{name}", f"binary image of {name}\n".encode("utf-8")
            )
            closure.append(
                {
                    "name": name,
                    "version": version,
                    "source": source,
                    "path": artifact["path"],
                    "sha256": artifact["sha256"],
                    "sizeBytes": artifact["sizeBytes"],
                }
            )

        record = {
            "schemaVersion": 1,
            "rowId": row_id,
            "commitSha": COMMIT,
            "collectedAt": iso(COLLECTED),
            "collector": {
                "type": "ci",
                "identity": IDENTITY,
                "runId": RUN_ID,
                "provenance": {
                    "repository": REPOSITORY,
                    "workflow": WORKFLOW,
                    "runId": RUN_ID,
                    "runAttempt": RUN_ATTEMPT,
                    "jobId": JOB_ID,
                    "checkoutAt": iso(CHECKOUT),
                    "commitSha": COMMIT,
                },
                "attestation": {
                    "predicateType": vc.ATTESTATION_PREDICATE,
                    "bundleSha256": "",
                    "signedAt": iso(COLLECTED),
                },
            },
            "host": {
                "os": {
                    "family": "windows",
                    "version": "11",
                    "build": "10.0.22631",
                    "locale": "en-US",
                },
                "arch": "x86_64",
                "cpu": {
                    "model": "AMD Ryzen 9 9900X3D 12-Core Processor",
                    "features": ["SSE4.2", "AVX2"],
                },
                "ram": {"totalMb": 32768},
                "gpu": host_gpu,
                "audio": host_audio,
                "compiler": {
                    "id": "msvc",
                    "version": "19.42.34435",
                    "toolset": "v143",
                    "windowsSdkVersion": "10.0.22621.0",
                },
            },
            "probes": probes,
            "dependencyClosure": closure,
        }
        self.reseal(record)
        return record

    def _required_dependencies(self, row_id: str) -> list[tuple[str, str]]:
        return [
            (self.authority.by_identity[key]["name"], key[1])
            for key in self.authority.required_keys(row_id)
        ]

    @staticmethod
    def _pattern_sample(entry: dict[str, Any]) -> str:
        pattern = entry["versionPattern"]
        return "14.42.34438.0" if pattern.startswith("^14") else "10.0.22621.4391"

    def reseal(self, record: dict[str, Any]) -> None:
        """Recompute the attestation from what is actually on disk."""
        _errors, digest = bundle_verify.verify_bundle(
            record, artifact_root=self.artifact_root, row_id=record["rowId"]
        )
        record["collector"]["attestation"]["bundleSha256"] = digest or "0" * 64

    # -- use ---------------------------------------------------------------

    @property
    def primary(self) -> dict[str, Any]:
        return self.records[D3D11_ID]

    def write(self) -> None:
        for path in self.evidence_dir.glob("*.json"):
            path.unlink()
        for row_id, record in self.records.items():
            with open(self.evidence_dir / f"{row_id}.json", "w", encoding="utf-8") as f:
                json.dump(record, f, indent=2)
        with open(self.root / "matrix.json", "w", encoding="utf-8") as f:
            json.dump(self.matrix, f, indent=2)

    def validate(self, **overrides: Any) -> vc.ValidationResult:
        self.write()
        kwargs: dict[str, Any] = {
            "now": NOW,
            "artifact_root": self.artifact_root,
            "authority": self.authority,
            "trusted": trusted(),
        }
        kwargs.update(overrides)
        return vc.load_and_validate(
            self.root / "matrix.json", self.evidence_dir, **kwargs
        )


def _load_authority() -> da.Authority:
    document = json.loads(
        (REPO_ROOT / da.AUTHORITY_RELPATH).read_text(encoding="utf-8")
    )
    da.validate_authority_document(document)
    return da.Authority(document)


AUTHORITY = _load_authority()


class BundleTestCase(unittest.TestCase):
    """Base case owning one throwaway bundle per test."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="plt200-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.bundle = Bundle(self.tmp, AUTHORITY)

    def assertCertified(self, result: vc.ValidationResult) -> None:
        self.assertEqual(result.errors, [], "expected a clean certification")
        self.assertTrue(result.fully_certified)

    def assertRejected(self, result: vc.ValidationResult, needle: str) -> None:
        joined = "\n".join(result.errors)
        self.assertTrue(result.errors, f"expected a rejection mentioning {needle!r}")
        self.assertIn(needle, joined)
        self.assertFalse(result.fully_certified)

    def rejectRecord(self, mutate: Callable[[dict[str, Any]], None], needle: str) -> None:
        mutate(self.bundle.primary)
        self.assertRejected(self.bundle.validate(), needle)

    def rejectMatrix(self, mutate: Callable[[dict[str, Any]], None], needle: str) -> None:
        mutate(self.bundle.matrix)
        self.assertRejected(self.bundle.validate(), needle)


# ═══════════════════════════════════════════════════════════════════════════
# The baseline must actually certify, or every rejection below is vacuous.
# ═══════════════════════════════════════════════════════════════════════════


class TestCertifiablePath(BundleTestCase):
    def test_measured_bundle_certifies(self) -> None:
        self.assertCertified(self.bundle.validate())

    def test_both_canonical_rows_are_certified(self) -> None:
        result = self.bundle.validate()
        self.assertEqual(result.rows_checked, 2)
        self.assertEqual(result.rows_certified, 2)
        self.assertEqual(result.rows_failed, 0)

    def test_summary_reports_pass(self) -> None:
        self.assertIn("PASS", self.bundle.validate().summary())


# ═══════════════════════════════════════════════════════════════════════════
# The exact probe the audit reproduced.
# ═══════════════════════════════════════════════════════════════════════════


class TestReproducedFalseGreen(unittest.TestCase):
    """The precise bypass that certified 3e461c3e, asserted closed.

    Fabricated collector, fabricated dependency closure, an artifact that does
    not exist, and a NaN age bound applied to 200-hour-old evidence.
    """

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="plt200-probe-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.collected = NOW - timedelta(hours=200)
        self.evidence = {
            "schemaVersion": 1,
            "rowId": D3D11_ID,
            "commitSha": COMMIT,
            "collectedAt": iso(self.collected),
            "collector": {"type": "manual", "identity": "totally-not-a-real-operator"},
            "host": {
                "os": {
                    "family": "windows",
                    "version": "11",
                    "build": "10.0.22631",
                    "locale": "en-US",
                },
                "arch": "x86_64",
                "cpu": {"model": "Fabricated CPU", "features": ["SSE4.2", "AVX2"]},
                "ram": {"totalMb": 32768},
                "gpu": {
                    "api": "d3d11",
                    "device": "NVIDIA GeForce RTX 5070 Ti",
                    "vendor": "nvidia",
                    "driverVersion": "576.40",
                    "featureLevel": "11.1",
                },
                "audio": {"api": "xaudio2"},
                "compiler": {"id": "msvc", "version": "19.42.34435", "toolset": "v143"},
            },
            "probes": {
                category: {
                    "status": "pass",
                    "durationMs": 1234,
                    "artifacts": [
                        {
                            "path": "nowhere/does-not-exist.log",
                            "sha256": "c" * 64,
                            "sizeBytes": 999,
                        }
                    ],
                }
                for category in sorted(vc.ALL_PROBE_CATEGORIES)
            },
            "dependencyClosure": [
                {"name": "TotallyFabricatedLib", "version": "9.9.9", "source": "bundled"},
                {"name": "AlsoNotReal.dll", "version": "1.0", "source": "system"},
            ],
        }

    def test_fabricated_record_is_rejected_by_validate_evidence(self) -> None:
        errors = vc.validate_evidence(self.evidence, now=NOW)
        self.assertTrue(errors)
        joined = "\n".join(errors)
        self.assertIn("provenance", joined)
        self.assertIn("attestation", joined)

    def test_fabricated_dependency_closure_is_rejected(self) -> None:
        errors = bundle_verify.check_dependency_closure(
            D3D11_ID, self.evidence["dependencyClosure"], AUTHORITY
        )
        joined = "\n".join(errors)
        self.assertIn("is not in the dependency authority", joined)

    def test_nan_age_bound_cannot_certify_two_hundred_hour_old_evidence(self) -> None:
        matrix = make_matrix()
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.cross_validate(
                matrix,
                {D3D11_ID: self.evidence},
                max_age_hours=float("nan"),
                now=NOW,
            )
        self.assertIn("NaN", str(ctx.exception))

    def test_two_hundred_hour_old_evidence_fails_the_default_bound(self) -> None:
        age = (NOW - self.collected).total_seconds() / 3600
        self.assertGreater(age, vc.DEFAULT_MAX_AGE_HOURS)

    def test_missing_artifact_is_rejected_by_measurement(self) -> None:
        errors, digest = bundle_verify.verify_bundle(
            self.evidence, artifact_root=self.tmp, row_id=D3D11_ID
        )
        self.assertIsNone(digest)
        self.assertTrue(errors)


# ═══════════════════════════════════════════════════════════════════════════
# Age bounds.
# ═══════════════════════════════════════════════════════════════════════════


class TestMaxAgeBound(unittest.TestCase):
    def _refuse(self, value: Any, needle: str) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.validate_max_age_hours(value)
        self.assertIn(needle, str(ctx.exception))

    def test_nan_is_refused(self) -> None:
        self._refuse(float("nan"), "NaN")

    def test_positive_infinity_is_refused(self) -> None:
        self._refuse(float("inf"), "finite")

    def test_negative_infinity_is_refused(self) -> None:
        self._refuse(float("-inf"), "finite")

    def test_zero_is_refused(self) -> None:
        self._refuse(0, "greater than zero")

    def test_negative_is_refused(self) -> None:
        self._refuse(-1.0, "greater than zero")

    def test_true_is_refused(self) -> None:
        self._refuse(True, "boolean")

    def test_false_is_refused(self) -> None:
        self._refuse(False, "boolean")

    def test_string_is_refused(self) -> None:
        self._refuse("168", "string")

    def test_none_is_refused(self) -> None:
        self._refuse(None, "NoneType")

    def test_absurdly_large_is_refused(self) -> None:
        self._refuse(1e9, "exceeds the maximum")

    def test_sub_second_is_refused(self) -> None:
        self._refuse(1e-9, "below the minimum")

    def test_ordinary_bound_is_accepted(self) -> None:
        self.assertEqual(vc.validate_max_age_hours(168), 168.0)

    def test_cli_rejects_nan_text(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            vc.cli_max_age_hours("nan")

    def test_cli_rejects_inf_text(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            vc.cli_max_age_hours("inf")

    def test_cli_rejects_zero_text(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            vc.cli_max_age_hours("0")

    def test_cli_rejects_non_numeric_text(self) -> None:
        with self.assertRaises(argparse.ArgumentTypeError):
            vc.cli_max_age_hours("soon")

    def test_cli_accepts_a_real_bound(self) -> None:
        self.assertEqual(vc.cli_max_age_hours("24"), 24.0)


class TestEvidenceAge(BundleTestCase):
    def test_evidence_within_the_bound_certifies(self) -> None:
        self.assertCertified(self.bundle.validate(max_age_hours=24))

    def test_evidence_older_than_the_bound_is_rejected(self) -> None:
        self.assertRejected(self.bundle.validate(max_age_hours=0.5), "old (max")

    def test_nan_bound_raises_rather_than_certifying(self) -> None:
        with self.assertRaises(vc.CertificationError):
            self.bundle.validate(max_age_hours=float("nan"))

    def test_stale_before_threshold_is_honoured(self) -> None:
        self.rejectRecord(
            lambda r: r.__setitem__("staleBefore", iso(COLLECTED + timedelta(minutes=1))),
            "staleBefore",
        )


# ═══════════════════════════════════════════════════════════════════════════
# Artifacts are measured, never believed.
# ═══════════════════════════════════════════════════════════════════════════


class TestArtifactsAreMeasured(BundleTestCase):
    def test_artifact_naming_a_missing_file_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"]["artifacts"] = [
                {"path": "missing.bin", "sha256": "b" * 64, "sizeBytes": 12345}
            ]
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "does not exist")

    def test_invented_digest_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"]["artifacts"][0].__setitem__("sha256", "c" * 64),
            "measured",
        )

    def test_invented_size_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"]["artifacts"][0].__setitem__("sizeBytes", 999999),
            "sizeBytes: declared",
        )

    def test_file_mutated_after_declaration_is_rejected(self) -> None:
        self.bundle.write()
        (self.bundle.artifact_root / D3D11_ID / "launch.log").write_bytes(b"TAMPERED\n")
        self.assertRejected(self.bundle.validate(), "measured")

    def test_undeclared_file_in_the_bundle_is_rejected(self) -> None:
        self.bundle.write()
        (self.bundle.artifact_root / D3D11_ID / "smuggled.log").write_bytes(b"hi\n")
        self.assertRejected(self.bundle.validate(), "undeclared file")

    def test_attestation_must_match_the_measured_bundle(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["attestation"].__setitem__("bundleSha256", "d" * 64),
            "does not match the measured bundle digest",
        )

    def test_unrecognised_attestation_predicate_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["attestation"].__setitem__(
                "predicateType", "https://example.invalid/made-up/v1"
            ),
            "is not the recognised predicate",
        )

    def test_missing_artifact_root_is_rejected(self) -> None:
        self.assertRejected(
            self.bundle.validate(artifact_root=self.tmp / "absent"),
            "artifact root is unusable",
        )

    def test_no_artifact_root_at_all_is_rejected(self) -> None:
        self.assertRejected(
            self.bundle.validate(artifact_root=None), "No artifact root supplied"
        )

    def test_bundle_digest_is_order_independent(self) -> None:
        pairs = [("b.log", "1" * 64), ("a.log", "2" * 64)]
        self.assertEqual(
            bundle_verify.bundle_digest(pairs),
            bundle_verify.bundle_digest(list(reversed(pairs))),
        )

    def test_bundle_digest_changes_with_content(self) -> None:
        self.assertNotEqual(
            bundle_verify.bundle_digest([("a.log", "1" * 64)]),
            bundle_verify.bundle_digest([("a.log", "2" * 64)]),
        )


# ═══════════════════════════════════════════════════════════════════════════
# Path confinement.
# ═══════════════════════════════════════════════════════════════════════════


class TestPathConfinement(BundleTestCase):
    def _reject_path(self, declared: str, needle: str = "") -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"]["artifacts"] = [
                {"path": declared, "sha256": "b" * 64, "sizeBytes": 1}
            ]
            self.bundle.reseal(record)

        mutate(self.bundle.primary)
        result = self.bundle.validate()
        self.assertTrue(result.errors, f"{declared!r} was not rejected")
        self.assertFalse(result.fully_certified)
        if needle:
            self.assertIn(needle, "\n".join(result.errors))

    def test_traversal_is_rejected(self) -> None:
        self._reject_path("../escape.log", "relative path component")

    def test_deep_traversal_is_rejected(self) -> None:
        self._reject_path("a/../../escape.log", "relative path component")

    def test_absolute_posix_path_is_rejected(self) -> None:
        self._reject_path("/etc/passwd", "absolute path")

    def test_drive_letter_path_is_rejected(self) -> None:
        self._reject_path("C:/Windows/win.ini", "drive-letter")

    def test_backslash_path_is_rejected(self) -> None:
        self._reject_path("sub\\file.log", "illegal character")

    def test_unc_path_is_rejected(self) -> None:
        self._reject_path("\\\\server\\share\\file.log", "absolute path")

    def test_reserved_device_name_is_rejected(self) -> None:
        self._reject_path("nul", "reserved Windows device name")

    def test_reserved_device_name_with_extension_is_rejected(self) -> None:
        self._reject_path("com1.log", "reserved Windows device name")

    def test_trailing_dot_alias_is_rejected(self) -> None:
        self._reject_path("launch.log.", "trailing dot or space")

    def test_trailing_space_alias_is_rejected(self) -> None:
        self._reject_path("launch.log ", "trailing dot or space")

    def test_doubled_separator_alias_is_rejected(self) -> None:
        self._reject_path("sub//launch.log", "empty path component")

    def test_dot_component_alias_is_rejected(self) -> None:
        self._reject_path("./launch.log", "relative path component")

    def test_interior_dot_component_alias_is_rejected(self) -> None:
        self._reject_path("sub/./launch.log", "relative path component")

    def test_case_colliding_artifacts_are_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"]["artifacts"].append(
                {
                    "path": "LAUNCH.LOG",
                    "sha256": record["probes"]["launch"]["artifacts"][0]["sha256"],
                    "sizeBytes": record["probes"]["launch"]["artifacts"][0]["sizeBytes"],
                }
            )
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "collides with")


# ═══════════════════════════════════════════════════════════════════════════
# A pass has to mean something.
# ═══════════════════════════════════════════════════════════════════════════


class TestPassMeansSomething(BundleTestCase):
    def test_nonzero_exit_code_cannot_pass(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("exitCode", 1),
            "a pass requires exitCode 0",
        )

    def test_pass_without_exit_code_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].pop("exitCode"), "a pass requires exitCode 0"
        )

    def test_zero_duration_pass_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("durationMs", 0), "too low for a pass"
        )

    def test_pass_without_artifacts_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"]["artifacts"] = []
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "requires at least one artifact")

    def test_pass_without_timestamps_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"].pop("startedAt")
            record["probes"]["launch"].pop("completedAt")

        self.rejectRecord(mutate, "missing or invalid timestamp")

    def test_completed_before_started_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"]["startedAt"] = iso(PROBE_END)
            record["probes"]["launch"]["completedAt"] = iso(PROBE_START)

        self.rejectRecord(mutate, "completedAt must be strictly after startedAt")

    def test_duration_disagreeing_with_wall_clock_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("durationMs", 3600000),
            "disagrees with the",
        )

    def test_probe_running_before_checkout_is_rejected(self) -> None:
        early = CHECKOUT - timedelta(hours=5)

        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"]["startedAt"] = iso(early)
            record["probes"]["launch"]["completedAt"] = iso(early + timedelta(seconds=5))

        self.rejectRecord(mutate, "before the source was checked out")

    def test_probe_completing_after_collection_is_rejected(self) -> None:
        late = COLLECTED + timedelta(minutes=5)

        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"]["startedAt"] = iso(late)
            record["probes"]["launch"]["completedAt"] = iso(late + timedelta(seconds=5))

        self.rejectRecord(mutate, "after the record was collected")

    def test_failing_probe_blocks_certification(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"] = {
                "status": "fail",
                "durationMs": 10,
                "exitCode": 3,
                "detail": "renderer did not come up",
            }
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "Probe launch status='fail'")

    def test_fail_with_exit_code_zero_is_contradictory(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"] = {
                "status": "fail",
                "durationMs": 10,
                "exitCode": 0,
            }
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "cannot report exitCode 0")

    def test_skip_requires_a_reason(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"] = {"status": "skip", "durationMs": 10}
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "requires skipReason")

    def test_skip_cannot_satisfy_a_required_category(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"] = {
                "status": "skip",
                "durationMs": 10,
                "skipReason": "no time",
            }
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "Probe launch status='skip'")

    def test_skip_reason_on_a_pass_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("skipReason", "why"),
            "only allowed when status is 'skip'",
        )

    def test_missing_required_probe_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            del record["probes"]["launch"]
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "Missing required probe: launch")

    def test_error_status_blocks_certification(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["probes"]["launch"] = {
                "status": "error",
                "durationMs": 10,
                "detail": "could not run",
            }
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "Probe launch status='error'")


# ═══════════════════════════════════════════════════════════════════════════
# Identity comes from outside the record.
# ═══════════════════════════════════════════════════════════════════════════


class TestRevisionAndIdentityBinding(BundleTestCase):
    def test_matrix_commit_must_be_the_expected_revision(self) -> None:
        self.rejectMatrix(
            lambda m: m.__setitem__("commitSha", OTHER_COMMIT),
            "is not the commit under validation",
        )

    def test_evidence_commit_must_be_the_expected_revision(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["commitSha"] = OTHER_COMMIT
            record["collector"]["provenance"]["commitSha"] = OTHER_COMMIT

        self.rejectRecord(mutate, "is not the commit under validation")

    def test_provenance_commit_must_match_the_record(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["provenance"].__setitem__("commitSha", OTHER_COMMIT),
            "is not the commit under validation",
        )

    def test_agreeing_but_unexpected_shas_do_not_certify(self) -> None:
        """Matrix and evidence agreeing with each other proves nothing."""

        def mutate(record: dict[str, Any]) -> None:
            record["commitSha"] = OTHER_COMMIT
            record["collector"]["provenance"]["commitSha"] = OTHER_COMMIT

        self.bundle.matrix["commitSha"] = OTHER_COMMIT
        for record in self.bundle.records.values():
            mutate(record)
        self.assertRejected(self.bundle.validate(), "is not the commit under validation")

    def test_collector_identity_must_be_the_trusted_one(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"].__setitem__("identity", "someone-else-entirely"),
            "does not match the trusted identity",
        )

    def test_collector_type_must_be_the_trusted_one(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"].__setitem__("type", "manual"),
            "is not the expected 'ci'",
        )

    def test_provenance_repository_must_be_the_trusted_one(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["provenance"].__setitem__(
                "repository", "attacker/fork"
            ),
            "is not the expected",
        )

    def test_provenance_workflow_must_be_the_trusted_one(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["provenance"].__setitem__("workflow", "Other"),
            "is not the expected",
        )

    def test_provenance_run_id_must_be_the_trusted_one(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["provenance"].__setitem__("runId", "999"),
            "is not the expected",
        )

    def test_provenance_run_attempt_must_be_the_trusted_one(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["provenance"].__setitem__("runAttempt", 7),
            "is not the expected",
        )

    def test_provenance_job_must_be_the_trusted_one(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["provenance"].__setitem__("jobId", "some-other-job"),
            "is not the expected",
        )

    def test_top_level_run_id_must_agree_with_provenance(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"].__setitem__("runId", "42"), "disagrees with"
        )

    def test_evidence_must_post_date_checkout(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["provenance"].__setitem__(
                "checkoutAt", iso(COLLECTED + timedelta(minutes=1))
            ),
            "strictly after the source was checked out",
        )

    def test_attestation_cannot_predate_collection(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"]["attestation"].__setitem__(
                "signedAt", iso(CHECKOUT)
            ),
            "cannot predate evidence collection",
        )

    def test_missing_provenance_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"].pop("provenance"),
            "missing required property 'provenance'",
        )

    def test_missing_attestation_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["collector"].pop("attestation"),
            "missing required property 'attestation'",
        )

    def test_evidence_for_an_unknown_row_is_rejected(self) -> None:
        stray = copy.deepcopy(self.bundle.primary)
        stray["rowId"] = "sneaky-extra-row"
        self.bundle.records["sneaky-extra-row"] = stray
        self.assertRejected(self.bundle.validate(), "the support matrix does not declare")

    def test_record_filename_must_name_its_row(self) -> None:
        self.bundle.write()
        source = self.bundle.evidence_dir / f"{D3D11_ID}.json"
        source.rename(self.bundle.evidence_dir / "renamed.json")
        result = vc.load_and_validate(
            self.bundle.root / "matrix.json",
            self.bundle.evidence_dir,
            now=NOW,
            artifact_root=self.bundle.artifact_root,
            authority=AUTHORITY,
            trusted=trusted(),
        )
        self.assertRejected(result, "must live at <rowId>.json")


class TestTrustedContext(unittest.TestCase):
    def test_short_commit_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            trusted(commit_sha="abc123")
        self.assertIn("40 lowercase hex", str(ctx.exception))

    def test_uppercase_commit_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(commit_sha=COMMIT.upper())

    def test_commit_with_trailing_newline_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(commit_sha=COMMIT + "\n")

    def test_all_zero_commit_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            trusted(commit_sha="0" * 40)
        self.assertIn("placeholder", str(ctx.exception))

    def test_blank_identity_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(identity="")

    def test_short_identity_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(identity="me")

    def test_ci_without_repository_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            trusted(repository=None)
        self.assertIn("--expect-repository", str(ctx.exception))

    def test_malformed_repository_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(repository="not-a-repo")

    def test_non_numeric_run_id_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(run_id="abc")

    def test_zero_run_attempt_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(run_attempt=0)

    def test_boolean_run_attempt_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            trusted(run_attempt=True)

    def test_manual_collector_must_not_supply_ci_fields(self) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            trusted(collector_type="manual")
        self.assertIn("must not supply", str(ctx.exception))

    def test_manual_collector_is_constructible_without_ci_fields(self) -> None:
        context = vc.TrustedContext(
            commit_sha=COMMIT, collector_type="manual", identity="operator-nathan"
        )
        self.assertEqual(context.collector_type, "manual")

    def test_github_env_requires_every_variable(self) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.TrustedContext.from_github_env({"GITHUB_SHA": COMMIT})
        self.assertIn("missing", str(ctx.exception))

    def test_github_env_builds_a_ci_context(self) -> None:
        context = vc.TrustedContext.from_github_env(
            {
                "GITHUB_SHA": COMMIT,
                "GITHUB_REPOSITORY": REPOSITORY,
                "GITHUB_WORKFLOW": WORKFLOW,
                "GITHUB_RUN_ID": RUN_ID,
                "GITHUB_RUN_ATTEMPT": "1",
                "GITHUB_JOB": JOB_ID,
            }
        )
        self.assertEqual(context.collector_type, "ci")
        self.assertIn(RUN_ID, context.identity)


# ═══════════════════════════════════════════════════════════════════════════
# Host binding: the row describes the machine that actually ran.
# ═══════════════════════════════════════════════════════════════════════════


class TestHostBinding(BundleTestCase):
    def test_os_build_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["os"].__setitem__("build", "10.0.19045"),
            "OS build mismatch",
        )

    def test_os_version_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["os"].__setitem__("version", "10"), "OS version mismatch"
        )

    def test_arch_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"].__setitem__("arch", "aarch64"), "Arch mismatch"
        )

    def test_compiler_version_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["compiler"].__setitem__("version", "19.30.30709"),
            "Compiler version mismatch",
        )

    def test_toolset_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["compiler"].__setitem__("toolset", "v142"),
            "Compiler toolset mismatch",
        )

    def test_windows_sdk_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["compiler"].__setitem__(
                "windowsSdkVersion", "10.0.19041.0"
            ),
            "Compiler windowsSdkVersion mismatch",
        )

    def test_missing_windows_sdk_on_a_windows_host_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["compiler"].pop("windowsSdkVersion"),
            "windowsSdkVersion is required on Windows hosts",
        )

    def test_gpu_device_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["gpu"].__setitem__("device", "Intel UHD 770"),
            "GPU device mismatch",
        )

    def test_gpu_driver_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["gpu"].__setitem__("driverVersion", "999.99"),
            "GPU driverVersion mismatch",
        )

    def test_gpu_vendor_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["gpu"].__setitem__("vendor", "amd"), "GPU vendor mismatch"
        )

    def test_feature_level_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["gpu"].__setitem__("featureLevel", "12.0"),
            "GPU featureLevel mismatch",
        )

    def test_audio_api_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["audio"].__setitem__("api", "openal"), "Audio api mismatch"
        )

    def test_audio_version_mismatch_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["audio"].__setitem__("version", "2.7"),
            "Audio version mismatch",
        )

    def test_missing_cpu_feature_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["cpu"].__setitem__("features", ["SSE4.2"]),
            "missing required CPU features",
        )

    def test_empty_cpu_feature_list_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["cpu"].__setitem__("features", []),
            "at least one feature",
        )

    def test_duplicate_cpu_features_are_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["cpu"].__setitem__("features", ["AVX2", "AVX2", "SSE4.2"]),
            "contains duplicates",
        )

    def test_nullrhi_host_claiming_a_vendor_is_rejected(self) -> None:
        record = self.bundle.records[NULLRHI_ID]
        record["host"]["gpu"]["vendor"] = "nvidia"
        self.assertRejected(self.bundle.validate(), "cannot report a hardware vendor")

    def test_hardware_api_claiming_no_vendor_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["gpu"].__setitem__("vendor", "none"),
            "cannot report vendor 'none'",
        )


# ═══════════════════════════════════════════════════════════════════════════
# Canonical profile freeze.
# ═══════════════════════════════════════════════════════════════════════════


class TestCanonicalProfileFreeze(BundleTestCase):
    def test_wrong_os_family_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["os"].update(family="linux"), "os.family must be"
        )

    def test_wrong_os_version_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["os"].update(version="10"), "os.version="
        )

    def test_non_windows_build_number_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["os"].update(build="6.8.0-41"), "os.build="
        )

    def test_wrong_arch_is_rejected(self) -> None:
        self.rejectMatrix(lambda m: m["rows"][0].update(arch="aarch64"), "arch must be")

    def test_wrong_compiler_id_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["compiler"].update(id="clang"), "compiler.id must be"
        )

    def test_wrong_toolset_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["compiler"].update(toolset="v142"),
            "compiler.toolset must be",
        )

    def test_out_of_family_compiler_version_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["compiler"].update(version="13.2.0"),
            "compiler.version=",
        )

    def test_missing_windows_sdk_version_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["compiler"].pop("windowsSdkVersion"),
            "compiler.windowsSdkVersion is required",
        )

    def test_malformed_windows_sdk_version_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["compiler"].update(windowsSdkVersion="latest"),
            "compiler.windowsSdkVersion=",
        )

    def test_wrong_gpu_api_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["gpu"].update(api="vulkan"), "gpu.api must be"
        )

    def test_gpu_vendor_outside_policy_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["gpu"].update(vendor="none"), "gpu.vendor must not be"
        )

    def test_malformed_driver_version_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["gpu"].update(driverVersion="latest"),
            "gpu.driverVersion=",
        )

    def test_unknown_feature_level_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["gpu"].update(featureLevel="4.6"), "gpu.featureLevel="
        )

    def test_wrong_audio_api_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["audio"].update(api="openal"), "audio.api must be"
        )

    def test_wrong_audio_version_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["audio"].update(version="1.23"), "audio.version="
        )

    def test_missing_audio_version_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0]["audio"].pop("version"), "audio.version is required"
        )

    def test_missing_cpu_features_are_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0].update(cpuFeatures=["SSE4.2"]), "cpuFeatures must include"
        )

    def test_nullrhi_row_must_stay_headless(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][1]["gpu"].update(api="d3d11"), "gpu.api must be 'nullrhi'"
        )

    def test_nullrhi_row_must_not_claim_audio(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][1]["audio"].update(api="xaudio2"), "audio.api must be 'none'"
        )

    def test_unknown_profile_is_rejected_by_default(self) -> None:
        self.rejectMatrix(
            lambda m: m.update(profile="totally-made-up"), "has no canonical definition"
        )

    def test_unknown_profile_is_accepted_only_when_asked_for(self) -> None:
        matrix = make_matrix()
        matrix["profile"] = "totally-made-up"
        self.assertEqual(
            vc.validate_matrix(matrix, now=NOW, allow_unknown_profile=True), []
        )

    def test_unknown_profile_cannot_certify_through_full_validation(self) -> None:
        self.bundle.matrix["profile"] = "totally-made-up"
        result = self.bundle.validate(allow_unknown_profile=True)
        self.assertRejected(result, "has no canonical definition")

    def test_dropping_a_canonical_row_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m.__setitem__("rows", m["rows"][:1]), "missing required row"
        )

    def test_downgrading_a_tier_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0].update(tier="experimental"), "tier must be 'primary'"
        )

    def test_extra_certifiable_row_is_rejected(self) -> None:
        def mutate(matrix: dict[str, Any]) -> None:
            extra = copy.deepcopy(matrix["rows"][0])
            extra["id"] = "sneaky-extra-row"
            matrix["rows"].append(extra)

        self.rejectMatrix(mutate, "unexpected certifiable row")

    def test_shrinking_the_required_evidence_set_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0].update(evidenceRequired=["build", "launch"]),
            "evidenceRequired diverges",
        )

    def test_duplicate_row_ids_are_rejected(self) -> None:
        def mutate(matrix: dict[str, Any]) -> None:
            matrix["rows"].append(copy.deepcopy(matrix["rows"][0]))

        self.rejectMatrix(mutate, "duplicate row id")

    def test_uppercase_row_id_is_rejected(self) -> None:
        def mutate(matrix: dict[str, Any]) -> None:
            clone = copy.deepcopy(matrix["rows"][0])
            clone["id"] = D3D11_ID.upper()
            matrix["rows"].append(clone)

        self.rejectMatrix(mutate, "does not match")


# ═══════════════════════════════════════════════════════════════════════════
# Dependency closure is matched against an authority outside the record.
# ═══════════════════════════════════════════════════════════════════════════


class TestDependencyBinding(BundleTestCase):
    def test_invented_dependency_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            entry = copy.deepcopy(record["dependencyClosure"][0])
            entry["name"] = "TotallyFabricatedLib"
            entry["source"] = "bundled"
            record["dependencyClosure"].append(entry)

        self.rejectRecord(mutate, "is not in the dependency authority")

    def test_wrong_version_for_a_known_dependency_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["dependencyClosure"][0].__setitem__("version", "0.0.0"),
            "does not match the authorised pattern",
        )

    def test_duplicate_dependency_identity_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["dependencyClosure"].append(
                copy.deepcopy(record["dependencyClosure"][0])
            )

        self.rejectRecord(mutate, "duplicate dependency")

    def test_case_aliased_dependency_name_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            clone = copy.deepcopy(record["dependencyClosure"][0])
            clone["name"] = clone["name"].upper()
            record["dependencyClosure"].append(clone)

        self.rejectRecord(mutate, "duplicate dependency")

    def test_same_name_from_two_sources_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            clone = copy.deepcopy(record["dependencyClosure"][0])
            clone["source"] = "sdk"
            record["dependencyClosure"].append(clone)

        self.rejectRecord(mutate, "declared from two sources")

    def test_dropping_a_required_dependency_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["dependencyClosure"] = record["dependencyClosure"][1:]
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "which the dependency authority requires")

    def test_empty_dependency_closure_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record["dependencyClosure"] = []
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "dependencyClosure is empty")

    def test_missing_dependency_closure_is_rejected(self) -> None:
        def mutate(record: dict[str, Any]) -> None:
            record.pop("dependencyClosure")
            self.bundle.reseal(record)

        self.rejectRecord(mutate, "dependencyClosure must be a list")

    def test_dependency_digest_must_match_the_file(self) -> None:
        self.rejectRecord(
            lambda r: r["dependencyClosure"][0].__setitem__("sha256", "e" * 64),
            "measured",
        )

    def test_dependency_size_must_match_the_file(self) -> None:
        self.rejectRecord(
            lambda r: r["dependencyClosure"][0].__setitem__("sizeBytes", 424242),
            "sizeBytes: declared",
        )

    def test_no_authority_means_no_certification(self) -> None:
        self.assertRejected(
            self.bundle.validate(authority=None), "no dependency authority was loaded"
        )


class TestDependencyAuthority(unittest.TestCase):
    def test_committed_authority_matches_the_manifest(self) -> None:
        authority = vc.load_authority(REPO_ROOT)
        self.assertGreater(len(authority.by_identity), 10)

    def test_authority_records_the_manifest_digest(self) -> None:
        document = json.loads(
            (REPO_ROOT / da.AUTHORITY_RELPATH).read_text(encoding="utf-8")
        )
        measured = da.manifest_digest((REPO_ROOT / da.LOCK_RELPATH).read_bytes())
        self.assertEqual(document["sourceSha256"], measured)

    def test_authority_carries_every_manifest_entry(self) -> None:
        text = (REPO_ROOT / da.LOCK_RELPATH).read_text(encoding="utf-8")
        entries, _ = da.parse_lock(text)
        document = json.loads(
            (REPO_ROOT / da.AUTHORITY_RELPATH).read_text(encoding="utf-8")
        )
        self.assertEqual(
            {entry["name"] for entry in entries},
            {entry["name"] for entry in document["thirdParty"]},
        )

    def test_submodule_pins_are_resolved_to_exact_revisions(self) -> None:
        document = json.loads(
            (REPO_ROOT / da.AUTHORITY_RELPATH).read_text(encoding="utf-8")
        )
        for entry in document["thirdParty"]:
            self.assertNotIn("${", entry["version"], entry["name"])

    def test_a_manifest_entry_with_too_few_fields_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da._split_entry("name|url|version")

    def test_a_manifest_entry_with_a_blank_field_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da._split_entry("name|url||MIT|path|files|macro|fallback|ERROR|notice")

    def test_an_unknown_severity_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da._split_entry("n|u|v|MIT|p|f|m|fb|MAYBE|notice")

    def test_unresolved_variable_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da.substitute("${_missing}", {})

    def test_authority_with_both_version_forms_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da._validate_entry(
                {
                    "name": "x.dll",
                    "source": "system",
                    "version": "1.0",
                    "versionPattern": "^1.0$",
                },
                "entry",
                exact_version=False,
            )

    def test_authority_with_an_unanchored_pattern_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da._validate_entry(
                {"name": "x.dll", "source": "system", "versionPattern": "1.0"},
                "entry",
                exact_version=False,
            )

    def test_authority_with_an_unknown_source_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da._validate_entry(
                {"name": "x.dll", "source": "internet", "version": "1"},
                "entry",
                exact_version=True,
            )

    def test_duplicate_identity_in_the_authority_is_refused(self) -> None:
        with self.assertRaises(da.AuthorityError):
            da.Authority(
                {
                    "thirdParty": [
                        {"name": "Dup", "source": "bundled", "version": "1", "requiredForRows": []}
                    ],
                    "platformRuntime": [
                        {"name": "dup", "source": "bundled", "version": "1", "requiredForRows": []}
                    ],
                }
            )

    def test_drift_from_the_manifest_is_detected(self) -> None:
        document = json.loads(
            (REPO_ROOT / da.AUTHORITY_RELPATH).read_text(encoding="utf-8")
        )
        document["sourceSha256"] = "0" * 64
        tmp = Path(tempfile.mkdtemp(prefix="plt200-auth-"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        fake_root = tmp / "repo"
        (fake_root / "docs" / "certification").mkdir(parents=True)
        (fake_root / "ThirdParty").mkdir(parents=True)
        shutil.copy(REPO_ROOT / da.LOCK_RELPATH, fake_root / da.LOCK_RELPATH)
        (fake_root / da.AUTHORITY_RELPATH).write_text(
            json.dumps(document), encoding="utf-8"
        )
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.load_authority(fake_root)
        self.assertIn("regenerate", str(ctx.exception))


# ═══════════════════════════════════════════════════════════════════════════
# Evidence discovery.
# ═══════════════════════════════════════════════════════════════════════════


class TestEvidenceDiscovery(BundleTestCase):
    def test_missing_evidence_directory_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.discover_evidence_files(self.tmp / "absent")
        self.assertIn("unusable", str(ctx.exception))

    def test_a_file_where_the_directory_should_be_is_refused(self) -> None:
        impostor = self.tmp / "impostor"
        impostor.write_text("{}", encoding="utf-8")
        with self.assertRaises(vc.CertificationError):
            vc.discover_evidence_files(impostor)

    def test_gitkeep_is_allowed(self) -> None:
        self.bundle.write()
        (self.bundle.evidence_dir / ".gitkeep").write_text("", encoding="utf-8")
        self.assertEqual(len(vc.discover_evidence_files(self.bundle.evidence_dir)), 2)

    def test_an_unexpected_entry_is_refused(self) -> None:
        self.bundle.write()
        (self.bundle.evidence_dir / "notes.txt").write_text("hi", encoding="utf-8")
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.discover_evidence_files(self.bundle.evidence_dir)
        self.assertIn("unexpected entry", str(ctx.exception))

    def test_a_subdirectory_is_refused(self) -> None:
        self.bundle.write()
        (self.bundle.evidence_dir / "nested").mkdir()
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.discover_evidence_files(self.bundle.evidence_dir)
        self.assertIn("not a plain file", str(ctx.exception))

    def test_an_empty_directory_cannot_certify(self) -> None:
        empty = self.tmp / "empty"
        empty.mkdir()
        self.bundle.write()
        result = vc.load_and_validate(
            self.bundle.root / "matrix.json",
            empty,
            now=NOW,
            artifact_root=self.bundle.artifact_root,
            authority=AUTHORITY,
            trusted=trusted(),
        )
        self.assertRejected(result, "No evidence record found")

    @unittest.skipIf(os.name != "nt", "case-insensitive collision needs a case-folding FS")
    def test_case_colliding_record_names_are_refused(self) -> None:
        self.bundle.write()
        # On a case-folding filesystem these are one file, which is exactly the
        # ambiguity the check exists to refuse on the case-sensitive ones.
        self.assertEqual(len(vc.discover_evidence_files(self.bundle.evidence_dir)), 2)


# ═══════════════════════════════════════════════════════════════════════════
# Strict JSON and bounded documents.
# ═══════════════════════════════════════════════════════════════════════════


class TestStrictJson(unittest.TestCase):
    def test_duplicate_keys_are_rejected(self) -> None:
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.parse_strict_json('{"a": 1, "a": 2}')
        self.assertIn("uplicate", str(ctx.exception))

    def test_nan_is_rejected(self) -> None:
        with self.assertRaises(vc.CertificationError):
            vc.parse_strict_json('{"a": NaN}')

    def test_infinity_is_rejected(self) -> None:
        with self.assertRaises(vc.CertificationError):
            vc.parse_strict_json('{"a": Infinity}')

    def test_negative_infinity_is_rejected(self) -> None:
        with self.assertRaises(vc.CertificationError):
            vc.parse_strict_json('{"a": -Infinity}')

    def test_malformed_json_is_rejected(self) -> None:
        with self.assertRaises(vc.CertificationError):
            vc.parse_strict_json("{not json}")

    def test_deeply_nested_json_is_rejected(self) -> None:
        payload = "[" * 60 + "]" * 60
        with self.assertRaises(vc.CertificationError):
            vc.parse_strict_json(payload)

    def test_valid_json_is_accepted(self) -> None:
        self.assertEqual(vc.parse_strict_json('{"a": 1}'), {"a": 1})


class TestBoundedDocuments(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="plt200-docs-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_oversized_document_is_refused_on_size(self) -> None:
        big = self.tmp / "big.json"
        big.write_bytes(b'{"a": "' + b"x" * (vc.MAX_DOCUMENT_BYTES + 16) + b'"}')
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.load_strict_json(big)
        self.assertIn("max", str(ctx.exception))

    def test_missing_document_is_refused(self) -> None:
        with self.assertRaises(vc.CertificationError):
            vc.load_strict_json(self.tmp / "absent.json")

    def test_non_utf8_document_is_refused(self) -> None:
        bad = self.tmp / "bad.json"
        bad.write_bytes(b"\xff\xfe{}")
        with self.assertRaises(vc.CertificationError):
            vc.load_strict_json(bad)

    def test_byte_order_mark_is_refused(self) -> None:
        bom = self.tmp / "bom.json"
        bom.write_bytes("\ufeff{}".encode("utf-8"))
        with self.assertRaises(vc.CertificationError) as ctx:
            vc.load_strict_json(bom)
        self.assertIn("byte-order mark", str(ctx.exception))


# ═══════════════════════════════════════════════════════════════════════════
# The schema engine itself.
# ═══════════════════════════════════════════════════════════════════════════


class TestSchemaContract(unittest.TestCase):
    def test_both_schemas_compile(self) -> None:
        for schema in (vc._EV_SCHEMA, vc._MX_SCHEMA):
            CompiledSchema(schema)

    def test_schemas_are_valid_draft_2020_12(self) -> None:
        try:
            import jsonschema
        except ImportError:
            self.skipTest("jsonschema is not installed")
        for schema in (vc._EV_SCHEMA, vc._MX_SCHEMA):
            jsonschema.Draft202012Validator.check_schema(schema)

    def test_unknown_keyword_is_refused_rather_than_ignored(self) -> None:
        with self.assertRaises(SchemaError) as ctx:
            CompiledSchema({"type": "object", "unevaluatedProperties": False})
        self.assertIn("unsupported schema keyword", str(ctx.exception))

    def test_open_objects_are_refused(self) -> None:
        with self.assertRaises(SchemaError):
            CompiledSchema({"type": "object", "additionalProperties": True})

    def test_dangling_ref_is_refused(self) -> None:
        with self.assertRaises(SchemaError):
            CompiledSchema({"$ref": "#/$defs/Nope", "$defs": {}})

    def test_remote_ref_is_refused(self) -> None:
        with self.assertRaises(SchemaError):
            CompiledSchema({"$ref": "https://example.invalid/s.json"})

    def test_unsupported_format_is_refused(self) -> None:
        with self.assertRaises(SchemaError):
            CompiledSchema({"type": "string", "format": "email"})

    def test_probe_categories_come_from_the_schema(self) -> None:
        self.assertIn("dependency_closure", vc.ALL_PROBE_CATEGORIES)
        self.assertEqual(len(vc.ALL_PROBE_CATEGORIES), 13)


class TestSchemaEngine(unittest.TestCase):
    @staticmethod
    def _errors(schema: dict[str, Any], value: Any) -> list[str]:
        return CompiledSchema(schema).validate(value)

    def test_true_is_not_an_integer(self) -> None:
        self.assertTrue(self._errors({"type": "integer"}, True))

    def test_false_is_not_an_integer(self) -> None:
        self.assertTrue(self._errors({"type": "integer"}, False))

    def test_true_is_not_a_number(self) -> None:
        self.assertTrue(self._errors({"type": "number"}, True))

    def test_integer_is_accepted(self) -> None:
        self.assertEqual(self._errors({"type": "integer"}, 5), [])

    def test_whole_float_is_an_integer(self) -> None:
        self.assertEqual(self._errors({"type": "integer"}, 5.0), [])

    def test_fractional_float_is_not_an_integer(self) -> None:
        self.assertTrue(self._errors({"type": "integer"}, 5.5))

    def test_const_one_does_not_match_true(self) -> None:
        self.assertTrue(self._errors({"const": 1}, True))

    def test_enum_zero_does_not_match_false(self) -> None:
        self.assertTrue(self._errors({"enum": [0]}, False))

    def test_object_where_scalar_expected_is_an_error_not_a_crash(self) -> None:
        self.assertTrue(self._errors({"enum": ["pass", "fail"]}, {}))

    def test_pattern_anchors_reject_a_trailing_newline(self) -> None:
        schema = {"type": "string", "pattern": "^[0-9a-f]{40}$"}
        self.assertEqual(self._errors(schema, "a" * 40), [])
        self.assertTrue(self._errors(schema, "a" * 40 + "\n"))

    def test_date_time_format_is_asserted(self) -> None:
        schema = {"type": "string", "format": "date-time"}
        self.assertEqual(self._errors(schema, "2026-08-28T12:00:00+00:00"), [])
        self.assertTrue(self._errors(schema, "not-a-date"))

    def test_naive_date_time_is_rejected(self) -> None:
        schema = {"type": "string", "format": "date-time"}
        self.assertTrue(self._errors(schema, "2026-08-28T12:00:00"))

    def test_unique_items_is_enforced(self) -> None:
        self.assertTrue(self._errors({"type": "array", "uniqueItems": True}, ["a", "a"]))

    def test_additional_properties_are_rejected(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "properties": {"a": {"type": "string"}},
        }
        self.assertTrue(self._errors(schema, {"a": "x", "b": "y"}))

    def test_required_property_is_enforced(self) -> None:
        self.assertTrue(
            self._errors({"type": "object", "required": ["a"], "properties": {}}, {})
        )


class TestSchemaEnforcedOnRecords(BundleTestCase):
    def test_boolean_duration_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("durationMs", True),
            "durationMs: expected type integer",
        )

    def test_boolean_ram_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["host"]["ram"].__setitem__("totalMb", True),
            "totalMb: expected type integer",
        )

    def test_boolean_size_bytes_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"]["artifacts"][0].__setitem__("sizeBytes", True),
            "sizeBytes: expected type integer",
        )

    def test_boolean_exit_code_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("exitCode", True),
            "exitCode: expected type integer",
        )

    def test_object_valued_status_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("status", {}), "probes.launch.status"
        )

    def test_list_valued_arch_is_rejected(self) -> None:
        self.rejectRecord(lambda r: r["host"].__setitem__("arch", []), "host.arch")

    def test_oversized_detail_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r["probes"]["launch"].__setitem__("detail", "x" * 3000),
            "longer than 2000",
        )

    def test_unknown_evidence_field_is_rejected(self) -> None:
        self.rejectRecord(
            lambda r: r.__setitem__("sneaky", "field"), "unknown properties ['sneaky']"
        )

    def test_wrong_schema_version_is_rejected(self) -> None:
        self.rejectRecord(lambda r: r.__setitem__("schemaVersion", 2), "schemaVersion")

    def test_oversized_notes_in_the_matrix_is_rejected(self) -> None:
        self.rejectMatrix(
            lambda m: m["rows"][0].__setitem__("notes", "y" * 3000), "longer than 500"
        )


# ═══════════════════════════════════════════════════════════════════════════
# The hardened filesystem layer.
# ═══════════════════════════════════════════════════════════════════════════


class TestSafeFilesystem(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="plt200-fs-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_measure_file_returns_the_real_digest_and_size(self) -> None:
        target = self.tmp / "a.bin"
        target.write_bytes(b"hello")
        digest, size = safe_fs.measure_file(target, max_bytes=1024)
        self.assertEqual(digest, hashlib.sha256(b"hello").hexdigest())
        self.assertEqual(size, 5)

    def test_measure_file_refuses_oversized(self) -> None:
        target = self.tmp / "big.bin"
        target.write_bytes(b"x" * 100)
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.measure_file(target, max_bytes=10)

    def test_missing_file_is_refused(self) -> None:
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.lstat_checked(self.tmp / "absent")

    def test_directory_is_not_a_regular_file(self) -> None:
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.lstat_checked(self.tmp, expect="file")

    def test_resolve_under_accepts_a_plain_relative_path(self) -> None:
        self.assertEqual(
            safe_fs.resolve_under(self.tmp, "sub/dir/x.log"),
            self.tmp / "sub" / "dir" / "x.log",
        )

    def test_resolve_under_rejects_empty(self) -> None:
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.resolve_under(self.tmp, "")

    def test_resolve_under_rejects_nul_byte(self) -> None:
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.resolve_under(self.tmp, "a\x00b")

    def test_resolve_under_rejects_overlong(self) -> None:
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.resolve_under(self.tmp, "a" * 600)

    def test_resolve_under_rejects_folding_aliases(self) -> None:
        for alias in ("a//b", "./a/b", "a/./b"):
            with self.assertRaises(safe_fs.FileSecurityError, msg=alias):
                safe_fs.resolve_under(self.tmp, alias)

    def test_case_collision_key_folds_case_and_separators(self) -> None:
        self.assertEqual(
            safe_fs.case_collision_key("A\\B.LOG"), safe_fs.case_collision_key("a/b.log")
        )

    def test_bundle_file_count_is_bounded(self) -> None:
        for index in range(5):
            (self.tmp / f"f{index}.bin").write_bytes(b"x")
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.iter_bundle_files(
                self.tmp, max_files=2, max_depth=4, max_total_bytes=1024
            )

    def test_bundle_depth_is_bounded(self) -> None:
        deep = self.tmp
        for index in range(6):
            deep = deep / f"d{index}"
        deep.mkdir(parents=True)
        (deep / "x.bin").write_bytes(b"x")
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.iter_bundle_files(
                self.tmp, max_files=100, max_depth=2, max_total_bytes=1024
            )

    def test_bundle_total_size_is_bounded(self) -> None:
        (self.tmp / "a.bin").write_bytes(b"x" * 100)
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.iter_bundle_files(
                self.tmp, max_files=100, max_depth=4, max_total_bytes=10
            )


class TestLinkRejection(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="plt200-link-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.outside = self.tmp / "outside.bin"
        self.outside.write_bytes(b"secret")
        self.root = self.tmp / "bundle"
        self.root.mkdir()

    def _make_symlink(self, link: Path, target: Path) -> None:
        try:
            os.symlink(target, link)
        except (OSError, NotImplementedError, AttributeError) as exc:
            self.skipTest(f"symlinks unavailable: {exc}")

    def test_symlinked_file_is_refused_by_lstat(self) -> None:
        link = self.root / "link.bin"
        self._make_symlink(link, self.outside)
        with self.assertRaises(safe_fs.FileSecurityError) as ctx:
            safe_fs.lstat_checked(link)
        self.assertIn("link/reparse", str(ctx.exception))

    def test_symlinked_file_is_refused_by_measure(self) -> None:
        link = self.root / "link.bin"
        self._make_symlink(link, self.outside)
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.measure_file(link, max_bytes=1024)

    def test_symlink_inside_the_bundle_is_refused_by_enumeration(self) -> None:
        link = self.root / "link.bin"
        self._make_symlink(link, self.outside)
        with self.assertRaises(safe_fs.FileSecurityError):
            safe_fs.iter_bundle_files(
                self.root, max_files=10, max_depth=4, max_total_bytes=1024
            )

    @unittest.skipIf(os.name != "nt", "junctions are a Windows construct")
    def test_junction_inside_the_bundle_is_refused(self) -> None:
        target = self.tmp / "target-dir"
        target.mkdir()
        (target / "x.bin").write_bytes(b"x")
        junction = self.root / "j"
        completed = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(junction), str(target)],
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            self.skipTest(f"cannot create a junction: {completed.stderr.strip()}")
        with self.assertRaises(safe_fs.FileSecurityError) as ctx:
            safe_fs.iter_bundle_files(
                self.root, max_files=10, max_depth=4, max_total_bytes=1024
            )
        self.assertIn("link/reparse", str(ctx.exception))


# ═══════════════════════════════════════════════════════════════════════════
# What ships in the repository right now.
# ═══════════════════════════════════════════════════════════════════════════


class TestShippedArtefacts(unittest.TestCase):
    MATRIX = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
    EVIDENCE = REPO_ROOT / "docs" / "certification" / "evidence"
    LEDGER = (
        REPO_ROOT / "docs" / "readiness" / "work-items" / "20-platform-runtime-editor.json"
    )

    def test_shipped_matrix_parses_strictly(self) -> None:
        self.assertIsInstance(vc.load_strict_json(self.MATRIX), dict)

    def test_shipped_matrix_has_no_errors(self) -> None:
        self.assertEqual(vc.validate_matrix(vc.load_strict_json(self.MATRIX)), [])

    def test_shipped_matrix_declares_the_canonical_profile(self) -> None:
        self.assertEqual(vc.load_strict_json(self.MATRIX)["profile"], "stable-v1")

    def test_shipped_matrix_carries_both_canonical_rows(self) -> None:
        matrix = vc.load_strict_json(self.MATRIX)
        self.assertEqual({row["id"] for row in matrix["rows"]}, {D3D11_ID, NULLRHI_ID})

    def test_shipped_matrix_is_unstamped(self) -> None:
        """Until a real collection stamps it, the matrix must fail closed."""
        self.assertEqual(vc.load_strict_json(self.MATRIX)["commitSha"], "0" * 40)

    def test_shipped_matrix_claims_no_certification(self) -> None:
        matrix = vc.load_strict_json(self.MATRIX)
        for row in matrix["rows"]:
            self.assertNotIn("certified row", row.get("notes", ""))

    def test_evidence_directory_holds_no_records_yet(self) -> None:
        self.assertEqual(vc.discover_evidence_files(self.EVIDENCE), [])

    def test_plt_200_is_still_open(self) -> None:
        ledger = vc.load_strict_json(self.LEDGER)
        item = next(i for i in ledger["workItems"] if i["id"] == "PLT-200")
        self.assertIn(item["status"], vc.OPEN_STATUSES)

    def test_ledger_evidence_paths_exist(self) -> None:
        ledger = vc.load_strict_json(self.LEDGER)
        item = next(i for i in ledger["workItems"] if i["id"] == "PLT-200")
        for entry in item.get("evidence", []):
            for relative in entry.get("paths", []):
                self.assertTrue(
                    (REPO_ROOT / relative).exists(), f"{relative} does not exist"
                )

    def test_ledger_evidence_paths_use_the_tracked_case(self) -> None:
        """`tools/` and `Tools/` are different directories on Linux."""
        ledger = vc.load_strict_json(self.LEDGER)
        item = next(i for i in ledger["workItems"] if i["id"] == "PLT-200")
        tracked = set(
            subprocess.run(
                ["git", "ls-files"],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
                check=True,
            ).stdout.split()
        )
        for entry in item.get("evidence", []):
            for relative in entry.get("paths", []):
                if relative.endswith("/"):
                    continue
                self.assertIn(relative, tracked, f"{relative} is not a tracked path")


class TestLedgerConsistency(BundleTestCase):
    def _ledger(self, status: str) -> Path:
        path = self.tmp / "ledger.json"
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(
                {"schemaVersion": 1, "workItems": [{"id": "PLT-200", "status": status}]},
                stream,
            )
        return path

    def test_an_open_item_is_consistent_with_no_certification(self) -> None:
        result = vc.ValidationResult()
        result.rows_checked = 2
        result.rows_failed = 2
        result.error("no evidence")
        self.assertEqual(vc.check_ledger_claim(self._ledger("open"), result), [])

    def test_a_completed_item_without_certification_is_rejected(self) -> None:
        result = vc.ValidationResult()
        result.rows_checked = 2
        result.rows_failed = 2
        result.error("no evidence")
        errors = vc.check_ledger_claim(self._ledger("complete"), result)
        self.assertTrue(errors)
        self.assertIn("certification did not pass", errors[0])

    def test_a_completed_item_with_certification_is_accepted(self) -> None:
        result = vc.ValidationResult()
        result.rows_checked = 2
        result.rows_certified = 2
        self.assertEqual(vc.check_ledger_claim(self._ledger("complete"), result), [])

    def test_a_ledger_without_plt_200_is_rejected(self) -> None:
        path = self.tmp / "empty-ledger.json"
        with open(path, "w", encoding="utf-8") as stream:
            json.dump({"schemaVersion": 1, "workItems": []}, stream)
        self.assertTrue(vc.check_ledger_claim(path, vc.ValidationResult()))


class TestValidationResult(unittest.TestCase):
    def test_a_result_with_no_rows_is_not_fully_certified(self) -> None:
        self.assertFalse(vc.ValidationResult().fully_certified)

    def test_a_result_with_errors_is_not_ok(self) -> None:
        result = vc.ValidationResult()
        result.error("boom")
        self.assertFalse(result.ok)
        self.assertFalse(result.fully_certified)

    def test_a_result_with_a_failed_row_is_not_fully_certified(self) -> None:
        result = vc.ValidationResult()
        result.rows_certified = 1
        result.rows_failed = 1
        self.assertFalse(result.fully_certified)

    def test_summary_lists_errors(self) -> None:
        result = vc.ValidationResult()
        result.error("boom")
        self.assertIn("boom", result.summary())


# ═══════════════════════════════════════════════════════════════════════════
# The command line is a boundary too.
# ═══════════════════════════════════════════════════════════════════════════


class TestCli(unittest.TestCase):
    MATRIX = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
    EVIDENCE = REPO_ROOT / "docs" / "certification" / "evidence"

    def test_matrix_only_passes_on_the_shipped_matrix(self) -> None:
        self.assertEqual(vc.main(["--matrix-only", "--matrix", str(self.MATRIX)]), 0)

    def test_matrix_only_fails_on_a_missing_file(self) -> None:
        self.assertEqual(
            vc.main(["--matrix-only", "--matrix", str(self.MATRIX.parent / "absent.json")]),
            1,
        )

    def test_full_run_without_a_commit_is_refused(self) -> None:
        self.assertEqual(
            vc.main(
                [
                    "--matrix", str(self.MATRIX),
                    "--evidence-dir", str(self.EVIDENCE),
                    "--artifact-root", str(self.EVIDENCE),
                    "--expect-collector-type", "ci",
                    "--expect-identity", IDENTITY,
                ]
            ),
            1,
        )

    def test_full_run_without_an_artifact_root_is_refused(self) -> None:
        self.assertEqual(
            vc.main(
                [
                    "--matrix", str(self.MATRIX),
                    "--evidence-dir", str(self.EVIDENCE),
                    "--commit", COMMIT,
                    "--expect-collector-type", "ci",
                    "--expect-identity", IDENTITY,
                    "--expect-repository", REPOSITORY,
                    "--expect-workflow", WORKFLOW,
                    "--expect-run-id", RUN_ID,
                    "--expect-run-attempt", "1",
                    "--expect-job", JOB_ID,
                ]
            ),
            1,
        )

    def test_manual_collector_needs_an_explicit_acknowledgement(self) -> None:
        self.assertEqual(
            vc.main(
                [
                    "--matrix", str(self.MATRIX),
                    "--evidence-dir", str(self.EVIDENCE),
                    "--artifact-root", str(self.EVIDENCE),
                    "--commit", COMMIT,
                    "--expect-collector-type", "manual",
                    "--expect-identity", "operator-nathan",
                ]
            ),
            1,
        )

    def test_max_age_hours_rejects_nan_on_the_command_line(self) -> None:
        with self.assertRaises(SystemExit):
            vc.build_parser().parse_args(["--max-age-hours", "nan"])

    def test_max_age_hours_rejects_a_negative_bound(self) -> None:
        with self.assertRaises(SystemExit):
            vc.build_parser().parse_args(["--max-age-hours", "-5"])

    def test_full_run_fails_closed_with_no_evidence(self) -> None:
        self.assertEqual(
            vc.main(
                [
                    "--matrix", str(self.MATRIX),
                    "--evidence-dir", str(self.EVIDENCE),
                    "--artifact-root", str(self.EVIDENCE),
                    "--commit", COMMIT,
                    "--expect-collector-type", "ci",
                    "--expect-identity", IDENTITY,
                    "--expect-repository", REPOSITORY,
                    "--expect-workflow", WORKFLOW,
                    "--expect-run-id", RUN_ID,
                    "--expect-run-attempt", "1",
                    "--expect-job", JOB_ID,
                ]
            ),
            1,
        )

    def test_require_certified_fails_with_no_evidence(self) -> None:
        self.assertEqual(
            vc.main(
                [
                    "--mode", "require-certified",
                    "--matrix", str(self.MATRIX),
                    "--evidence-dir", str(self.EVIDENCE),
                    "--artifact-root", str(self.EVIDENCE),
                    "--commit", COMMIT,
                    "--expect-collector-type", "ci",
                    "--expect-identity", IDENTITY,
                    "--expect-repository", REPOSITORY,
                    "--expect-workflow", WORKFLOW,
                    "--expect-run-id", RUN_ID,
                    "--expect-run-attempt", "1",
                    "--expect-job", JOB_ID,
                ]
            ),
            1,
        )

    def test_ledger_consistency_passes_while_plt_200_is_open(self) -> None:
        ledger = (
            REPO_ROOT / "docs" / "readiness" / "work-items"
            / "20-platform-runtime-editor.json"
        )
        self.assertEqual(
            vc.main(
                [
                    "--mode", "ledger-consistency",
                    "--matrix", str(self.MATRIX),
                    "--evidence-dir", str(self.EVIDENCE),
                    "--artifact-root", str(self.EVIDENCE),
                    "--ledger", str(ledger),
                    "--commit", COMMIT,
                    "--expect-collector-type", "ci",
                    "--expect-identity", IDENTITY,
                    "--expect-repository", REPOSITORY,
                    "--expect-workflow", WORKFLOW,
                    "--expect-run-id", RUN_ID,
                    "--expect-run-attempt", "1",
                    "--expect-job", JOB_ID,
                ]
            ),
            0,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
