#!/usr/bin/env python3
"""Adversarial test suite for platform certification validation (PLT-200).

Tests validate that the support-matrix schema, evidence schema, and
cross-validation logic are fail-closed: every missing, stale, mismatched,
or corrupted input must be rejected.  No false-green is acceptable.

All tests operate on in-memory data — no tracked files are modified.
"""

from __future__ import annotations

import copy
import json
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "platform-cert"))

from validate_certification import (
    ALL_PROBE_CATEGORIES,
    CertificationError,
    ValidationResult,
    cross_validate,
    validate_evidence,
    validate_matrix,
)

COMMIT_A = "a" * 40
COMMIT_B = "b" * 40
NOW = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)


def _make_matrix(
    *,
    commit: str = COMMIT_A,
    rows: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Build a minimal valid support matrix."""
    if rows is None:
        rows = [_make_row()]
    return {
        "schemaVersion": 1,
        "profile": "stable-v1",
        "commitSha": commit,
        "generatedAt": "2026-08-28T12:00:00Z",
        "rows": rows,
    }


def _make_row(
    *,
    row_id: str = "win11-x64-msvc143-d3d11",
    tier: str = "primary",
    evidence_required: list[str] | None = None,
) -> dict[str, Any]:
    """Build a minimal valid support row."""
    return {
        "id": row_id,
        "tier": tier,
        "os": {"family": "windows", "version": "11", "build": "10.0.22631"},
        "arch": "x86_64",
        "compiler": {"id": "msvc", "version": "19.42.34435", "toolset": "v143"},
        "gpu": {
            "api": "d3d11",
            "device": "RTX 5070 Ti",
            "vendor": "nvidia",
            "driverVersion": "576.40",
            "featureLevel": "11.1",
        },
        "audio": {"api": "xaudio2"},
        "cpuFeatures": ["SSE4.2", "AVX2"],
        "evidenceRequired": evidence_required or ["build", "launch"],
    }


def _make_evidence(
    *,
    row_id: str = "win11-x64-msvc143-d3d11",
    commit: str = COMMIT_A,
    collected_at: str = "2026-08-28T11:00:00Z",
    probes: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build a minimal valid evidence record."""
    if probes is None:
        probes = {
            "build": {"status": "pass", "durationMs": 120000},
            "launch": {"status": "pass", "durationMs": 5000},
        }
    return {
        "schemaVersion": 1,
        "rowId": row_id,
        "commitSha": commit,
        "collectedAt": collected_at,
        "collector": {"type": "manual", "identity": "test-operator"},
        "host": {
            "os": {
                "family": "windows",
                "version": "11",
                "build": "10.0.22631",
                "locale": "en-US",
            },
            "arch": "x86_64",
            "cpu": {"model": "AMD Ryzen 9 9900X3D", "features": ["SSE4.2", "AVX2"]},
            "ram": {"totalMb": 32768},
            "gpu": {
                "api": "d3d11",
                "device": "RTX 5070 Ti",
                "vendor": "nvidia",
                "driverVersion": "576.40",
                "featureLevel": "11.1",
            },
            "audio": {"api": "xaudio2"},
        },
        "probes": probes,
    }


# ---------------------------------------------------------------------------
# Matrix schema validation
# ---------------------------------------------------------------------------

class TestMatrixSchemaValidation(unittest.TestCase):
    """Verify the matrix validator catches structural defects."""

    def test_valid_matrix_passes(self):
        errors = validate_matrix(_make_matrix())
        self.assertEqual(errors, [])

    def test_wrong_schema_version_fails(self):
        m = _make_matrix()
        m["schemaVersion"] = 99
        errors = validate_matrix(m)
        self.assertTrue(any("schemaVersion" in e for e in errors))

    def test_missing_commit_fails(self):
        m = _make_matrix()
        m["commitSha"] = ""
        errors = validate_matrix(m)
        self.assertTrue(any("commitSha" in e for e in errors))

    def test_short_commit_fails(self):
        m = _make_matrix()
        m["commitSha"] = "abc123"
        errors = validate_matrix(m)
        self.assertTrue(any("commitSha" in e for e in errors))

    def test_uppercase_commit_fails(self):
        m = _make_matrix()
        m["commitSha"] = "A" * 40
        errors = validate_matrix(m)
        self.assertTrue(any("commitSha" in e for e in errors))

    def test_empty_rows_fails(self):
        m = _make_matrix()
        m["rows"] = []
        errors = validate_matrix(m)
        self.assertTrue(any("at least one row" in e for e in errors))

    def test_duplicate_row_ids_fails(self):
        row = _make_row()
        m = _make_matrix(rows=[row, copy.deepcopy(row)])
        errors = validate_matrix(m)
        self.assertTrue(any("duplicate" in e for e in errors))

    def test_invalid_tier_fails(self):
        row = _make_row()
        row["tier"] = "gold"
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("tier" in e for e in errors))

    def test_missing_os_family_fails(self):
        row = _make_row()
        del row["os"]["family"]
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("os.family" in e for e in errors))

    def test_invalid_arch_fails(self):
        row = _make_row()
        row["arch"] = "arm32"
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("arch" in e for e in errors))

    def test_invalid_compiler_id_fails(self):
        row = _make_row()
        row["compiler"]["id"] = "borland"
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("compiler.id" in e for e in errors))

    def test_invalid_gpu_api_fails(self):
        row = _make_row()
        row["gpu"]["api"] = "directx9"
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("gpu.api" in e for e in errors))

    def test_invalid_gpu_vendor_fails(self):
        row = _make_row()
        row["gpu"]["vendor"] = "3dfx"
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("gpu.vendor" in e for e in errors))

    def test_invalid_audio_api_fails(self):
        row = _make_row()
        row["audio"]["api"] = "fmod"
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("audio.api" in e for e in errors))

    def test_empty_evidence_required_fails(self):
        row = _make_row()
        row["evidenceRequired"] = []
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("evidenceRequired" in e for e in errors))

    def test_unknown_evidence_category_fails(self):
        row = _make_row()
        row["evidenceRequired"] = ["build", "teleportation"]
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("unknown evidence" in e.lower() or "teleportation" in e for e in errors))

    def test_duplicate_evidence_categories_fails(self):
        row = _make_row()
        row["evidenceRequired"] = ["build", "build"]
        errors = validate_matrix(_make_matrix(rows=[row]))
        self.assertTrue(any("duplicate" in e.lower() for e in errors))

    def test_invalid_generated_at_fails(self):
        m = _make_matrix()
        m["generatedAt"] = "not-a-date"
        errors = validate_matrix(m)
        self.assertTrue(any("timestamp" in e.lower() or "Invalid" in e for e in errors))

    def test_missing_profile_fails(self):
        m = _make_matrix()
        m["profile"] = ""
        errors = validate_matrix(m)
        self.assertTrue(any("profile" in e for e in errors))


# ---------------------------------------------------------------------------
# Evidence schema validation
# ---------------------------------------------------------------------------

class TestEvidenceSchemaValidation(unittest.TestCase):
    """Verify evidence records are structurally validated."""

    def test_valid_evidence_passes(self):
        errors = validate_evidence(_make_evidence())
        self.assertEqual(errors, [])

    def test_wrong_schema_version_fails(self):
        ev = _make_evidence()
        ev["schemaVersion"] = 2
        errors = validate_evidence(ev)
        self.assertTrue(any("schemaVersion" in e for e in errors))

    def test_empty_row_id_fails(self):
        ev = _make_evidence()
        ev["rowId"] = ""
        errors = validate_evidence(ev)
        self.assertTrue(any("rowId" in e for e in errors))

    def test_invalid_commit_fails(self):
        ev = _make_evidence()
        ev["commitSha"] = "not-a-sha"
        errors = validate_evidence(ev)
        self.assertTrue(any("commitSha" in e for e in errors))

    def test_invalid_collector_type_fails(self):
        ev = _make_evidence()
        ev["collector"]["type"] = "robot"
        errors = validate_evidence(ev)
        self.assertTrue(any("collector.type" in e for e in errors))

    def test_missing_collector_identity_fails(self):
        ev = _make_evidence()
        ev["collector"]["identity"] = ""
        errors = validate_evidence(ev)
        self.assertTrue(any("collector.identity" in e for e in errors))

    def test_missing_host_os_locale_fails(self):
        ev = _make_evidence()
        del ev["host"]["os"]["locale"]
        errors = validate_evidence(ev)
        self.assertTrue(any("host.os.locale" in e for e in errors))

    def test_invalid_host_arch_fails(self):
        ev = _make_evidence()
        ev["host"]["arch"] = "mips"
        errors = validate_evidence(ev)
        self.assertTrue(any("host.arch" in e for e in errors))

    def test_missing_cpu_model_fails(self):
        ev = _make_evidence()
        ev["host"]["cpu"]["model"] = ""
        errors = validate_evidence(ev)
        self.assertTrue(any("host.cpu.model" in e for e in errors))

    def test_invalid_ram_fails(self):
        ev = _make_evidence()
        ev["host"]["ram"]["totalMb"] = 0
        errors = validate_evidence(ev)
        self.assertTrue(any("host.ram.totalMb" in e for e in errors))

    def test_string_ram_fails(self):
        ev = _make_evidence()
        ev["host"]["ram"]["totalMb"] = "16384"
        errors = validate_evidence(ev)
        self.assertTrue(any("host.ram.totalMb" in e for e in errors))

    def test_unknown_probe_category_fails(self):
        ev = _make_evidence()
        ev["probes"]["teleportation"] = {"status": "pass", "durationMs": 1}
        errors = validate_evidence(ev)
        self.assertTrue(any("teleportation" in e for e in errors))

    def test_invalid_probe_status_fails(self):
        ev = _make_evidence()
        ev["probes"]["build"]["status"] = "maybe"
        errors = validate_evidence(ev)
        self.assertTrue(any("status" in e for e in errors))

    def test_negative_duration_fails(self):
        ev = _make_evidence()
        ev["probes"]["build"]["durationMs"] = -1
        errors = validate_evidence(ev)
        self.assertTrue(any("durationMs" in e for e in errors))

    def test_skip_without_reason_fails(self):
        ev = _make_evidence()
        ev["probes"]["build"]["status"] = "skip"
        errors = validate_evidence(ev)
        self.assertTrue(any("skipReason" in e for e in errors))

    def test_skip_with_reason_passes(self):
        ev = _make_evidence()
        ev["probes"]["build"]["status"] = "skip"
        ev["probes"]["build"]["skipReason"] = "NullRHI does not render"
        errors = validate_evidence(ev)
        self.assertFalse(any("skipReason" in e for e in errors))

    def test_invalid_artifact_sha_fails(self):
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "build.log", "sha256": "tooshort"}
        ]
        errors = validate_evidence(ev)
        self.assertTrue(any("sha256" in e for e in errors))

    def test_valid_artifact_passes(self):
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "build.log", "sha256": "a" * 64, "sizeBytes": 1024}
        ]
        errors = validate_evidence(ev)
        self.assertEqual(errors, [])

    def test_invalid_dep_source_fails(self):
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"name": "libc", "version": "6.0", "source": "magic"}
        ]
        errors = validate_evidence(ev)
        self.assertTrue(any("source" in e for e in errors))

    def test_valid_dep_closure_passes(self):
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"name": "vcredist", "version": "14.40", "source": "vcredist"}
        ]
        errors = validate_evidence(ev)
        self.assertEqual(errors, [])


# ---------------------------------------------------------------------------
# Cross-validation (the core certification gate)
# ---------------------------------------------------------------------------

class TestCrossValidation(unittest.TestCase):
    """Adversarial tests for the cross-validation logic."""

    def test_matching_row_and_evidence_certifies(self):
        matrix = _make_matrix()
        evidence = {"win11-x64-msvc143-d3d11": _make_evidence()}
        result = cross_validate(matrix, evidence, now=NOW)
        self.assertTrue(result.ok, result.summary())
        self.assertEqual(result.rows_certified, 1)

    def test_missing_evidence_fails_closed(self):
        """A certifiable row with no evidence must fail — never pass by default."""
        matrix = _make_matrix()
        result = cross_validate(matrix, {}, now=NOW)
        self.assertFalse(result.ok)
        self.assertEqual(result.rows_failed, 1)
        self.assertTrue(any("No evidence" in e for e in result.errors))

    def test_commit_sha_mismatch_fails(self):
        """Evidence from a different commit is not valid for this matrix."""
        matrix = _make_matrix(commit=COMMIT_A)
        ev = _make_evidence(commit=COMMIT_B)
        result = cross_validate(matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW)
        self.assertFalse(result.ok)
        self.assertTrue(any("Commit mismatch" in e for e in result.errors))

    def test_stale_evidence_fails(self):
        """Evidence older than max_age_hours must be rejected."""
        old_time = (NOW - timedelta(hours=200)).isoformat()
        ev = _make_evidence(collected_at=old_time)
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, max_age_hours=168, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("old" in e.lower() for e in result.errors))

    def test_stale_before_threshold_fails(self):
        """Evidence collected before staleBefore is rejected."""
        ev = _make_evidence(collected_at="2026-08-27T10:00:00Z")
        ev["staleBefore"] = "2026-08-28T00:00:00Z"
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("staleBefore" in e for e in result.errors))

    def test_missing_required_probe_fails(self):
        """A row requiring 'build' and 'launch' must have both probes."""
        ev = _make_evidence(probes={
            "build": {"status": "pass", "durationMs": 100},
        })
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("Missing required probe: launch" in e for e in result.errors))

    def test_failed_probe_fails_row(self):
        """A probe with status='fail' prevents certification."""
        ev = _make_evidence(probes={
            "build": {"status": "fail", "durationMs": 100, "detail": "link error"},
            "launch": {"status": "pass", "durationMs": 5000},
        })
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("build" in e and "fail" in e for e in result.errors))

    def test_error_probe_fails_row(self):
        """A probe with status='error' prevents certification."""
        ev = _make_evidence(probes={
            "build": {"status": "error", "durationMs": 0, "detail": "timeout"},
            "launch": {"status": "pass", "durationMs": 5000},
        })
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("error" in e for e in result.errors))

    def test_skip_probe_fails_required(self):
        """A skipped probe in a required category prevents certification."""
        ev = _make_evidence(probes={
            "build": {"status": "skip", "durationMs": 0, "skipReason": "lazy"},
            "launch": {"status": "pass", "durationMs": 5000},
        })
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)

    def test_os_family_mismatch_fails(self):
        """Evidence from Linux cannot certify a Windows row."""
        ev = _make_evidence()
        ev["host"]["os"]["family"] = "linux"
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("OS family mismatch" in e for e in result.errors))

    def test_os_version_mismatch_fails(self):
        """Evidence from Windows 10 cannot certify a Windows 11 row."""
        ev = _make_evidence()
        ev["host"]["os"]["version"] = "10"
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("OS version mismatch" in e for e in result.errors))

    def test_arch_mismatch_fails(self):
        """Evidence from aarch64 cannot certify an x86_64 row."""
        ev = _make_evidence()
        ev["host"]["arch"] = "aarch64"
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("Arch mismatch" in e for e in result.errors))

    def test_gpu_api_mismatch_fails(self):
        """Evidence with Vulkan cannot certify a D3D11 row."""
        ev = _make_evidence()
        ev["host"]["gpu"]["api"] = "vulkan"
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("GPU API mismatch" in e for e in result.errors))

    def test_compiler_mismatch_fails(self):
        """Evidence from GCC cannot certify an MSVC row."""
        ev = _make_evidence()
        ev["host"]["compiler"] = {
            "id": "gcc",
            "version": "14.0",
            "toolset": "gnu",
        }
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("Compiler mismatch" in e for e in result.errors))

    def test_toolset_mismatch_fails(self):
        """Evidence from v145 toolset cannot certify a v143 row."""
        ev = _make_evidence()
        ev["host"]["compiler"] = {
            "id": "msvc",
            "version": "19.50.00000",
            "toolset": "v145",
        }
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("Toolset mismatch" in e for e in result.errors))

    def test_missing_cpu_features_fails(self):
        """Host lacking required CPU features must fail."""
        ev = _make_evidence()
        ev["host"]["cpu"]["features"] = ["SSE4.2"]
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("CPU features" in e for e in result.errors))

    def test_audio_api_mismatch_fails(self):
        """Evidence with OpenAL cannot certify an XAudio2 row."""
        ev = _make_evidence()
        ev["host"]["audio"]["api"] = "openal"
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("Audio API mismatch" in e for e in result.errors))

    def test_experimental_tier_skipped_with_warning(self):
        """Experimental rows are skipped, not failed."""
        row = _make_row(tier="experimental")
        matrix = _make_matrix(rows=[row])
        result = cross_validate(matrix, {}, now=NOW)
        self.assertTrue(result.ok)
        self.assertEqual(result.rows_failed, 0)
        self.assertTrue(any("not certifiable" in w for w in result.warnings))

    def test_unsupported_tier_skipped(self):
        """Unsupported rows are skipped."""
        row = _make_row(tier="unsupported")
        matrix = _make_matrix(rows=[row])
        result = cross_validate(matrix, {}, now=NOW)
        self.assertTrue(result.ok)

    def test_multiple_rows_independent(self):
        """Each row is validated independently — one failure doesn't taint another."""
        good_row = _make_row(row_id="good", evidence_required=["build"])
        bad_row = _make_row(row_id="bad", evidence_required=["build"])
        matrix = _make_matrix(rows=[good_row, bad_row])
        evidence = {
            "good": _make_evidence(
                row_id="good",
                probes={"build": {"status": "pass", "durationMs": 100}},
            ),
        }
        result = cross_validate(matrix, evidence, now=NOW)
        self.assertFalse(result.ok)
        self.assertEqual(result.rows_certified, 1)
        self.assertEqual(result.rows_failed, 1)

    def test_extra_probes_do_not_harm(self):
        """Probes beyond what's required don't cause failure."""
        ev = _make_evidence(probes={
            "build": {"status": "pass", "durationMs": 100},
            "launch": {"status": "pass", "durationMs": 5000},
            "renderer": {"status": "pass", "durationMs": 3000},
        })
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertTrue(result.ok)

    def test_no_compiler_in_evidence_does_not_crash(self):
        """Evidence without compiler section still validates if row matches."""
        ev = _make_evidence()
        assert "compiler" not in ev["host"]
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertTrue(result.ok)


# ---------------------------------------------------------------------------
# Tampered / corrupted evidence
# ---------------------------------------------------------------------------

class TestTamperedEvidence(unittest.TestCase):
    """Adversarial tests for tampered or fabricated evidence."""

    def test_all_zeros_commit_still_validated(self):
        """A plausible-looking commit of all zeros is accepted structurally
        but will mismatch any real matrix commit."""
        matrix = _make_matrix(commit=COMMIT_A)
        ev = _make_evidence(commit="0" * 40)
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("Commit mismatch" in e for e in result.errors))

    def test_future_timestamp_not_special(self):
        """Evidence with a future timestamp is not inherently rejected
        (clock skew is possible), but age calculation still works."""
        future = (NOW + timedelta(hours=1)).isoformat()
        ev = _make_evidence(collected_at=future)
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertTrue(result.ok)

    def test_empty_probes_object_fails_required(self):
        """An evidence record with an empty probes dict fails all required categories."""
        ev = _make_evidence(probes={})
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("Missing required probe" in e for e in result.errors))

    def test_pass_with_zero_duration_passes(self):
        """A probe that passed in 0ms is structurally valid (instantaneous check)."""
        ev = _make_evidence(probes={
            "build": {"status": "pass", "durationMs": 0},
            "launch": {"status": "pass", "durationMs": 0},
        })
        matrix = _make_matrix()
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertTrue(result.ok)


# ---------------------------------------------------------------------------
# Full evidence pipeline (all 13 categories)
# ---------------------------------------------------------------------------

class TestFullCertificationPipeline(unittest.TestCase):
    """Test that all 13 evidence categories can be validated."""

    def test_all_13_categories_pass(self):
        """Every known probe category passes."""
        probes = {}
        for cat in ALL_PROBE_CATEGORIES:
            probes[cat] = {"status": "pass", "durationMs": 1000}
        row = _make_row(evidence_required=sorted(ALL_PROBE_CATEGORIES))
        matrix = _make_matrix(rows=[row])
        ev = _make_evidence(probes=probes)
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertTrue(result.ok, result.summary())
        self.assertEqual(result.rows_certified, 1)

    def test_one_of_13_fails_blocks_certification(self):
        """If even one required category fails, the row cannot certify."""
        probes = {}
        for cat in ALL_PROBE_CATEGORIES:
            probes[cat] = {"status": "pass", "durationMs": 1000}
        probes["rollback"] = {"status": "fail", "durationMs": 500, "detail": "state mismatch"}
        row = _make_row(evidence_required=sorted(ALL_PROBE_CATEGORIES))
        matrix = _make_matrix(rows=[row])
        ev = _make_evidence(probes=probes)
        result = cross_validate(
            matrix, {"win11-x64-msvc143-d3d11": ev}, now=NOW
        )
        self.assertFalse(result.ok)
        self.assertTrue(any("rollback" in e for e in result.errors))

    def test_result_summary_format(self):
        """Summary includes pass/fail counts and is human-readable."""
        matrix = _make_matrix()
        evidence = {"win11-x64-msvc143-d3d11": _make_evidence()}
        result = cross_validate(matrix, evidence, now=NOW)
        summary = result.summary()
        self.assertIn("PASS", summary)
        self.assertIn("1 checked", summary)
        self.assertIn("1 certified", summary)


# ---------------------------------------------------------------------------
# Seed matrix file validation
# ---------------------------------------------------------------------------

class TestSeedMatrixFile(unittest.TestCase):
    """Validate the checked-in seed support-matrix.json."""

    def test_seed_matrix_validates(self):
        matrix_path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        if not matrix_path.exists():
            self.skipTest("Seed matrix not yet created")
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        errors = validate_matrix(matrix)
        self.assertEqual(errors, [], f"Seed matrix errors: {errors}")

    def test_seed_matrix_has_two_rows(self):
        matrix_path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        if not matrix_path.exists():
            self.skipTest("Seed matrix not yet created")
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        self.assertEqual(len(matrix["rows"]), 2)
        ids = {r["id"] for r in matrix["rows"]}
        self.assertIn("win11-x64-msvc143-d3d11", ids)
        self.assertIn("win11-x64-msvc143-nullrhi", ids)

    def test_seed_matrix_profile_is_stable_v1(self):
        matrix_path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        if not matrix_path.exists():
            self.skipTest("Seed matrix not yet created")
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        self.assertEqual(matrix["profile"], "stable-v1")

    def test_primary_row_requires_all_13_categories(self):
        matrix_path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        if not matrix_path.exists():
            self.skipTest("Seed matrix not yet created")
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        primary = [r for r in matrix["rows"] if r["tier"] == "primary"][0]
        self.assertEqual(
            set(primary["evidenceRequired"]),
            ALL_PROBE_CATEGORIES,
        )

    def test_nullrhi_row_excludes_renderer_probes(self):
        matrix_path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        if not matrix_path.exists():
            self.skipTest("Seed matrix not yet created")
        matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
        nullrhi = [r for r in matrix["rows"] if "nullrhi" in r["id"]][0]
        required = set(nullrhi["evidenceRequired"])
        self.assertNotIn("renderer", required)
        self.assertNotIn("content", required)
        self.assertNotIn("input", required)
        self.assertNotIn("audio", required)


# ---------------------------------------------------------------------------
# ValidationResult edge cases
# ---------------------------------------------------------------------------

class TestValidationResult(unittest.TestCase):
    """Unit tests for the result accumulator."""

    def test_empty_result_is_ok(self):
        r = ValidationResult()
        self.assertTrue(r.ok)

    def test_error_makes_not_ok(self):
        r = ValidationResult()
        r.error("something broke")
        self.assertFalse(r.ok)

    def test_warning_keeps_ok(self):
        r = ValidationResult()
        r.warn("minor issue")
        self.assertTrue(r.ok)

    def test_summary_contains_counts(self):
        r = ValidationResult()
        r.rows_checked = 3
        r.rows_certified = 2
        r.rows_failed = 1
        r.error("test error")
        s = r.summary()
        self.assertIn("3 checked", s)
        self.assertIn("2 certified", s)
        self.assertIn("1 failed", s)
        self.assertIn("FAIL", s)


if __name__ == "__main__":
    unittest.main()
