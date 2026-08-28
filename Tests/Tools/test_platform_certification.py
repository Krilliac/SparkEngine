"""Hostile regression tests for PLT-200 platform certification validator.

Every test targets a specific audit-proven defect. Fixtures default to
profile='test-v1' so canonical-profile checks only fire in dedicated tests.

Cross-platform: import path uses 'Tools' (capital T) to match the actual
directory name on case-sensitive filesystems.
"""

from __future__ import annotations

import copy
import json
import sys
import textwrap
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Tools" / "platform-cert"))

import validate_certification as vc  # noqa: E402

# ── Deterministic time ────────────────────────────────────────────────────
NOW = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
GOOD_TS = "2026-08-28T11:00:00Z"
GOOD_COMMIT = "a" * 40
GOOD_SHA256 = "b" * 64

# ── Evidence categories ──────────────────────────────────────────────────
ALL_CATEGORIES = sorted(vc.ALL_PROBE_CATEGORIES)
NULLRHI_CATEGORIES = sorted(
    vc.ALL_PROBE_CATEGORIES - {"renderer", "content", "input", "audio"}
)


# ── Test fixtures ─────────────────────────────────────────────────────────


def _make_host(**overrides: Any) -> dict[str, Any]:
    host: dict[str, Any] = {
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
            "device": "NVIDIA GeForce RTX 5070 Ti",
            "vendor": "nvidia",
            "driverVersion": "576.40",
            "featureLevel": "11.1",
        },
        "audio": {"api": "xaudio2"},
        "compiler": {
            "id": "msvc",
            "version": "19.42.34435",
            "toolset": "v143",
        },
    }
    host.update(overrides)
    return host


def _make_probe(
    status: str = "pass", duration: int = 100, **extra: Any
) -> dict[str, Any]:
    p: dict[str, Any] = {"status": status, "durationMs": duration}
    p.update(extra)
    return p


def _make_probes(
    categories: list[str] | None = None, **overrides: Any
) -> dict[str, Any]:
    cats = categories or ALL_CATEGORIES
    probes: dict[str, Any] = {}
    for c in cats:
        if c in overrides:
            probes[c] = overrides[c]
        else:
            probes[c] = _make_probe()
    return probes


def _make_evidence(
    row_id: str = "test-row",
    commit: str = GOOD_COMMIT,
    collected_at: str = GOOD_TS,
    categories: list[str] | None = None,
    **overrides: Any,
) -> dict[str, Any]:
    ev: dict[str, Any] = {
        "schemaVersion": 1,
        "rowId": row_id,
        "commitSha": commit,
        "collectedAt": collected_at,
        "collector": {"type": "ci", "identity": "github-actions/12345"},
        "host": _make_host(),
        "probes": _make_probes(categories),
    }
    ev.update(overrides)
    return ev


def _make_row(
    row_id: str = "test-row",
    tier: str = "primary",
    categories: list[str] | None = None,
    **overrides: Any,
) -> dict[str, Any]:
    cats = list(categories) if categories is not None else list(ALL_CATEGORIES)
    row: dict[str, Any] = {
        "id": row_id,
        "tier": tier,
        "os": {"family": "windows", "version": "11", "build": "10.0.22631"},
        "arch": "x86_64",
        "compiler": {
            "id": "msvc",
            "version": "19.42.34435",
            "toolset": "v143",
        },
        "gpu": {
            "api": "d3d11",
            "device": "NVIDIA GeForce RTX 5070 Ti",
            "vendor": "nvidia",
            "driverVersion": "576.40",
            "featureLevel": "11.1",
        },
        "audio": {"api": "xaudio2"},
        "evidenceRequired": cats,
    }
    row.update(overrides)
    return row


def _make_matrix(
    profile: str = "test-v1",
    rows: list[dict[str, Any]] | None = None,
    commit: str = GOOD_COMMIT,
    generated_at: str = GOOD_TS,
    **overrides: Any,
) -> dict[str, Any]:
    m: dict[str, Any] = {
        "schemaVersion": 1,
        "profile": profile,
        "commitSha": commit,
        "generatedAt": generated_at,
        "rows": rows if rows is not None else [_make_row()],
    }
    m.update(overrides)
    return m


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 1: Schema loading and import-time validation
# ═══════════════════════════════════════════════════════════════════════════


class TestSchemaLoading:
    """Defect #1: Committed JSON schemas were never loaded."""

    def test_evidence_schema_loaded(self) -> None:
        assert vc._EV_SCHEMA is not None
        assert vc._EV_SCHEMA["type"] == "object"

    def test_matrix_schema_loaded(self) -> None:
        assert vc._MX_SCHEMA is not None
        assert vc._MX_SCHEMA["type"] == "object"

    def test_probe_categories_derived_from_schema(self) -> None:
        assert "build" in vc.ALL_PROBE_CATEGORIES
        assert "dependency_closure" in vc.ALL_PROBE_CATEGORIES
        assert len(vc.ALL_PROBE_CATEGORIES) == 13

    def test_valid_tiers_derived_from_schema(self) -> None:
        assert vc.VALID_TIERS == {"primary", "supported", "experimental", "unsupported"}

    def test_schema_enum_cross_check(self) -> None:
        ev_os = frozenset(
            vc._EV_SCHEMA["properties"]["host"]["properties"]["os"]["properties"][
                "family"
            ]["enum"]
        )
        mx_os = frozenset(
            vc._MX_SCHEMA["$defs"]["OsSpec"]["properties"]["family"]["enum"]
        )
        assert ev_os == mx_os

    def test_allowed_keys_extracted(self) -> None:
        assert "schemaVersion" in vc._MX_KEYS
        assert "rows" in vc._MX_KEYS
        assert "id" in vc._MX_ROW_KEYS
        assert "collector" in vc._EV_KEYS


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 2: Strict JSON parsing
# ═══════════════════════════════════════════════════════════════════════════


class TestStrictJsonParsing:
    """Defects #3 (duplicate keys) and #4 (NaN/Infinity)."""

    def test_duplicate_key_rejected(self) -> None:
        with pytest.raises(vc.CertificationError, match="Duplicate JSON key"):
            vc.parse_strict_json('{"a": 1, "a": 2}')

    def test_nested_duplicate_key_rejected(self) -> None:
        with pytest.raises(vc.CertificationError, match="Duplicate JSON key"):
            vc.parse_strict_json('{"x": {"b": 1, "b": 2}}')

    def test_nan_rejected(self) -> None:
        with pytest.raises(vc.CertificationError, match="Non-finite"):
            vc.parse_strict_json('{"x": NaN}')

    def test_infinity_rejected(self) -> None:
        with pytest.raises(vc.CertificationError, match="Non-finite"):
            vc.parse_strict_json('{"x": Infinity}')

    def test_negative_infinity_rejected(self) -> None:
        with pytest.raises(vc.CertificationError, match="Non-finite"):
            vc.parse_strict_json('{"x": -Infinity}')

    def test_valid_json_accepted(self) -> None:
        result = vc.parse_strict_json('{"a": 1, "b": [2, 3]}')
        assert result == {"a": 1, "b": [2, 3]}

    def test_malformed_json_rejected(self) -> None:
        with pytest.raises(vc.CertificationError, match="Invalid JSON"):
            vc.parse_strict_json("{bad json}")


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 3: additionalProperties enforcement
# ═══════════════════════════════════════════════════════════════════════════


class TestAdditionalProperties:
    """Defect #2: Extra top-level matrix fields accepted."""

    def test_extra_matrix_field_rejected(self) -> None:
        m = _make_matrix()
        m["surprise"] = "value"
        errors = vc.validate_matrix(m, now=NOW)
        assert any("unknown fields" in e and "surprise" in e for e in errors)

    def test_extra_matrix_row_field_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["bonus"] = True
        errors = vc.validate_matrix(m, now=NOW)
        assert any("unknown fields" in e and "bonus" in e for e in errors)

    def test_extra_evidence_field_rejected(self) -> None:
        ev = _make_evidence()
        ev["extra"] = "oops"
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("unknown fields" in e and "extra" in e for e in errors)

    def test_extra_host_field_rejected(self) -> None:
        ev = _make_evidence()
        ev["host"]["magic"] = "injection"
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("unknown fields" in e and "magic" in e for e in errors)

    def test_extra_probe_field_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["hackField"] = True
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("unknown fields" in e and "hackField" in e for e in errors)

    def test_extra_collector_field_rejected(self) -> None:
        ev = _make_evidence()
        ev["collector"]["admin"] = True
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("unknown fields" in e and "admin" in e for e in errors)

    def test_extra_gpu_field_in_matrix_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["gpu"]["raytracing"] = "yes"
        errors = vc.validate_matrix(m, now=NOW)
        assert any("unknown fields" in e and "raytracing" in e for e in errors)

    def test_extra_artifact_field_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "log.txt", "sha256": GOOD_SHA256, "sizeBytes": 100, "extra": 1}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("unknown fields" in e and "extra" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 4: Timestamp validation
# ═══════════════════════════════════════════════════════════════════════════


class TestTimestampValidation:
    """Defect #5: Stale or century-future timestamps accepted."""

    def test_future_timestamp_rejected(self) -> None:
        future = (NOW + timedelta(hours=1)).isoformat()
        m = _make_matrix(generated_at=future)
        errors = vc.validate_matrix(m, now=NOW)
        assert any("future" in e for e in errors)

    def test_near_future_within_skew_accepted(self) -> None:
        near = (NOW + timedelta(seconds=200)).isoformat()
        m = _make_matrix(generated_at=near)
        errors = vc.validate_matrix(m, now=NOW)
        assert not any("future" in e for e in errors)

    def test_century_old_timestamp_rejected(self) -> None:
        old = "1920-01-01T00:00:00Z"
        m = _make_matrix(generated_at=old)
        errors = vc.validate_matrix(m, now=NOW)
        assert any("too old" in e for e in errors)

    def test_pre_2020_timestamp_rejected(self) -> None:
        old = "2019-12-31T23:59:59Z"
        m = _make_matrix(generated_at=old)
        errors = vc.validate_matrix(m, now=NOW)
        assert any("too old" in e for e in errors)

    def test_evidence_future_timestamp_rejected(self) -> None:
        future = (NOW + timedelta(hours=1)).isoformat()
        ev = _make_evidence(collected_at=future)
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("future" in e for e in errors)

    def test_evidence_stale_before_violated(self) -> None:
        ev = _make_evidence(
            collected_at="2026-08-27T10:00:00Z",
            staleBefore="2026-08-28T00:00:00Z",
        )
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("staleBefore" in e for e in result.errors)

    def test_valid_timestamp_accepted(self) -> None:
        m = _make_matrix(generated_at=GOOD_TS)
        errors = vc.validate_matrix(m, now=NOW)
        ts_errors = [e for e in errors if "timestamp" in e.lower() or "future" in e]
        assert not ts_errors

    def test_invalid_iso_format_rejected(self) -> None:
        m = _make_matrix(generated_at="not-a-date")
        errors = vc.validate_matrix(m, now=NOW)
        assert any("invalid" in e.lower() and "timestamp" in e.lower() for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 5: Compiler identity required
# ═══════════════════════════════════════════════════════════════════════════


class TestCompilerRequired:
    """Defect #6: Missing compiler identity/version can pass."""

    def test_evidence_without_compiler_fails(self) -> None:
        ev = _make_evidence()
        del ev["host"]["compiler"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("compiler" in e.lower() for e in errors)

    def test_compiler_missing_version_fails(self) -> None:
        ev = _make_evidence()
        del ev["host"]["compiler"]["version"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("compiler.version" in e for e in errors)

    def test_compiler_missing_toolset_fails(self) -> None:
        ev = _make_evidence()
        del ev["host"]["compiler"]["toolset"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("compiler.toolset" in e for e in errors)

    def test_compiler_invalid_id_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["compiler"]["id"] = "turbo-c"
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("compiler.id" in e for e in errors)

    def test_compiler_present_passes(self) -> None:
        ev = _make_evidence()
        errors = vc.validate_evidence(ev, now=NOW)
        compiler_errors = [e for e in errors if "compiler" in e.lower()]
        assert not compiler_errors

    def test_matrix_compiler_missing_fails(self) -> None:
        m = _make_matrix()
        del m["rows"][0]["compiler"]
        errors = vc.validate_matrix(m, now=NOW)
        assert any("compiler" in e.lower() for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 6: Zero-duration pass probes
# ═══════════════════════════════════════════════════════════════════════════


class TestZeroDurationPass:
    """Defect #7: Zero-duration self-asserted pass rows can certify."""

    def test_pass_with_zero_duration_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(status="pass", duration=0)
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("durationMs=0" in e and "too low" in e for e in errors)

    def test_pass_with_negative_duration_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(status="pass", duration=-1)
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("non-negative" in e for e in errors)

    def test_pass_with_1ms_duration_accepted(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(status="pass", duration=1)
        errors = vc.validate_evidence(ev, now=NOW)
        dur_errors = [e for e in errors if "durationMs" in e]
        assert not dur_errors

    def test_fail_with_zero_duration_accepted(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(status="fail", duration=0)
        errors = vc.validate_evidence(ev, now=NOW)
        dur_errors = [e for e in errors if "too low" in e]
        assert not dur_errors

    def test_skip_with_zero_duration_accepted(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(
            status="skip", duration=0, skipReason="not needed"
        )
        errors = vc.validate_evidence(ev, now=NOW)
        dur_errors = [e for e in errors if "too low" in e]
        assert not dur_errors


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 7: SHA-256 and artifact validation
# ═══════════════════════════════════════════════════════════════════════════


class TestArtifactValidation:
    """Defect #8: SHA-256 checked only by length, no path confinement."""

    def test_sha256_uppercase_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "log.txt", "sha256": "A" * 64, "sizeBytes": 100}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("sha256" in e.lower() and "lowercase" in e.lower() for e in errors)

    def test_sha256_wrong_length_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "log.txt", "sha256": "a" * 63, "sizeBytes": 100}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("sha256" in e.lower() for e in errors)

    def test_absolute_path_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "/etc/passwd", "sha256": GOOD_SHA256, "sizeBytes": 100}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("relative" in e for e in errors)

    def test_windows_absolute_path_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "C:\\Windows\\System32\\cmd.exe", "sha256": GOOD_SHA256, "sizeBytes": 100}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("relative" in e or "forward slashes" in e for e in errors)

    def test_directory_traversal_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "logs/../../../etc/passwd", "sha256": GOOD_SHA256, "sizeBytes": 100}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("traversal" in e for e in errors)

    def test_backslash_path_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "logs\\build.log", "sha256": GOOD_SHA256, "sizeBytes": 100}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("forward slashes" in e for e in errors)

    def test_missing_size_bytes_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "log.txt", "sha256": GOOD_SHA256}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("sizeBytes" in e for e in errors)

    def test_valid_artifact_accepted(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": "logs/build.log", "sha256": GOOD_SHA256, "sizeBytes": 1024}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        art_errors = [e for e in errors if "artifact" in e.lower() or "sha256" in e.lower() or "path" in e.lower() or "sizeBytes" in e]
        assert not art_errors

    def test_too_many_artifacts_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"]["artifacts"] = [
            {"path": f"log{i}.txt", "sha256": GOOD_SHA256, "sizeBytes": 10}
            for i in range(vc.MAX_ARTIFACTS_PER_PROBE + 1)
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("max" in e and "artifacts" in e.lower() for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 8: Canonical profile coverage
# ═══════════════════════════════════════════════════════════════════════════


class TestCanonicalProfile:
    """Defect #9: Matrix rows can be omitted because matrix is not cross-checked."""

    def test_stable_v1_missing_d3d11_row_rejected(self) -> None:
        m = _make_matrix(
            profile="stable-v1",
            rows=[
                _make_row(
                    row_id="win11-x64-msvc143-nullrhi",
                    tier="supported",
                    categories=NULLRHI_CATEGORIES,
                ),
            ],
        )
        errors = vc.validate_matrix(m, now=NOW)
        assert any("win11-x64-msvc143-d3d11" in e and "missing" in e for e in errors)

    def test_stable_v1_missing_nullrhi_row_rejected(self) -> None:
        m = _make_matrix(
            profile="stable-v1",
            rows=[
                _make_row(row_id="win11-x64-msvc143-d3d11", tier="primary"),
            ],
        )
        errors = vc.validate_matrix(m, now=NOW)
        assert any("win11-x64-msvc143-nullrhi" in e and "missing" in e for e in errors)

    def test_stable_v1_unexpected_certifiable_row_rejected(self) -> None:
        m = _make_matrix(
            profile="stable-v1",
            rows=[
                _make_row(row_id="win11-x64-msvc143-d3d11", tier="primary"),
                _make_row(
                    row_id="win11-x64-msvc143-nullrhi",
                    tier="supported",
                    categories=NULLRHI_CATEGORIES,
                ),
                _make_row(row_id="linux-x64-gcc13-vulkan", tier="primary"),
            ],
        )
        errors = vc.validate_matrix(m, now=NOW)
        assert any(
            "unexpected" in e and "linux-x64-gcc13-vulkan" in e for e in errors
        )

    def test_stable_v1_wrong_tier_rejected(self) -> None:
        m = _make_matrix(
            profile="stable-v1",
            rows=[
                _make_row(
                    row_id="win11-x64-msvc143-d3d11", tier="supported"
                ),
                _make_row(
                    row_id="win11-x64-msvc143-nullrhi",
                    tier="supported",
                    categories=NULLRHI_CATEGORIES,
                ),
            ],
        )
        errors = vc.validate_matrix(m, now=NOW)
        assert any(
            "tier" in e and "primary" in e and "win11-x64-msvc143-d3d11" in e
            for e in errors
        )

    def test_stable_v1_wrong_evidence_required(self) -> None:
        m = _make_matrix(
            profile="stable-v1",
            rows=[
                _make_row(
                    row_id="win11-x64-msvc143-d3d11",
                    tier="primary",
                    categories=NULLRHI_CATEGORIES,
                ),
                _make_row(
                    row_id="win11-x64-msvc143-nullrhi",
                    tier="supported",
                    categories=NULLRHI_CATEGORIES,
                ),
            ],
        )
        errors = vc.validate_matrix(m, now=NOW)
        assert any("evidenceRequired" in e for e in errors)

    def test_stable_v1_complete_passes(self) -> None:
        m = _make_matrix(
            profile="stable-v1",
            rows=[
                _make_row(row_id="win11-x64-msvc143-d3d11", tier="primary"),
                _make_row(
                    row_id="win11-x64-msvc143-nullrhi",
                    tier="supported",
                    categories=NULLRHI_CATEGORIES,
                ),
            ],
        )
        errors = vc.validate_matrix(m, now=NOW)
        profile_errors = [e for e in errors if "Profile" in e or "stable-v1" in e]
        assert not profile_errors

    def test_experimental_row_allowed_in_stable_v1(self) -> None:
        m = _make_matrix(
            profile="stable-v1",
            rows=[
                _make_row(row_id="win11-x64-msvc143-d3d11", tier="primary"),
                _make_row(
                    row_id="win11-x64-msvc143-nullrhi",
                    tier="supported",
                    categories=NULLRHI_CATEGORIES,
                ),
                _make_row(
                    row_id="linux-x64-gcc13-vulkan", tier="experimental"
                ),
            ],
        )
        errors = vc.validate_matrix(m, now=NOW)
        profile_errors = [e for e in errors if "unexpected" in e]
        assert not profile_errors

    def test_custom_profile_no_canonical_check(self) -> None:
        m = _make_matrix(profile="test-v1", rows=[_make_row()])
        errors = vc.validate_matrix(m, now=NOW)
        profile_errors = [e for e in errors if "Profile" in e]
        assert not profile_errors

    def test_duplicate_row_ids_rejected(self) -> None:
        m = _make_matrix(
            rows=[_make_row(row_id="dupe"), _make_row(row_id="dupe")]
        )
        errors = vc.validate_matrix(m, now=NOW)
        assert any("duplicate" in e.lower() for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 9: Complete host matching
# ═══════════════════════════════════════════════════════════════════════════


class TestHostMatching:
    """Defect #10: Host matching omits OS, compiler, GPU, driver identity."""

    def test_os_family_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["os"]["family"] = "linux"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("OS family mismatch" in e for e in result.errors)

    def test_os_version_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["os"]["version"] = "10"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("OS version mismatch" in e for e in result.errors)

    def test_os_build_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["os"]["build"] = "10.0.19041"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("OS build mismatch" in e for e in result.errors)

    def test_arch_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["arch"] = "aarch64"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("Arch mismatch" in e for e in result.errors)

    def test_compiler_id_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["compiler"]["id"] = "gcc"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("Compiler id mismatch" in e for e in result.errors)

    def test_compiler_version_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["compiler"]["version"] = "19.41.00000"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("Compiler version mismatch" in e for e in result.errors)

    def test_compiler_toolset_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["compiler"]["toolset"] = "v144"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("Compiler toolset mismatch" in e for e in result.errors)

    def test_gpu_api_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["gpu"]["api"] = "vulkan"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("GPU api mismatch" in e for e in result.errors)

    def test_gpu_vendor_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["gpu"]["vendor"] = "amd"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("GPU vendor mismatch" in e for e in result.errors)

    def test_gpu_device_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["gpu"]["device"] = "AMD Radeon RX 7900"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("GPU device mismatch" in e for e in result.errors)

    def test_gpu_driver_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["gpu"]["driverVersion"] = "560.00"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("GPU driverVersion mismatch" in e for e in result.errors)

    def test_gpu_feature_level_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["gpu"]["featureLevel"] = "12.0"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("GPU featureLevel mismatch" in e for e in result.errors)

    def test_audio_api_mismatch_fails(self) -> None:
        ev = _make_evidence()
        ev["host"]["audio"]["api"] = "openal"
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("Audio API mismatch" in e for e in result.errors)

    def test_cpu_features_missing_fails(self) -> None:
        m = _make_matrix(commit=GOOD_COMMIT)
        m["rows"][0]["cpuFeatures"] = ["SSE4.2", "AVX2", "AVX-512"]
        ev = _make_evidence()
        result = vc.cross_validate(m, {"test-row": ev}, now=NOW)
        assert any("CPU features" in e and "AVX-512" in e for e in result.errors)

    def test_complete_host_match_passes(self) -> None:
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"name": "vcruntime140", "version": "14.42", "source": "vcredist"}
        ]
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert result.ok
        assert result.rows_certified == 1


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 10: Dependencies
# ═══════════════════════════════════════════════════════════════════════════


class TestDependencyValidation:
    """Defect #11: Dependencies are optional."""

    def test_dependency_closure_required_when_in_evidence_required(self) -> None:
        m = _make_matrix(commit=GOOD_COMMIT)
        assert "dependency_closure" in m["rows"][0]["evidenceRequired"]
        ev = _make_evidence()
        result = vc.cross_validate(m, {"test-row": ev}, now=NOW)
        assert any(
            "dependency_closure" in e and "dependencyClosure" in e
            for e in result.errors
        )

    def test_dependency_closure_with_deps_passes(self) -> None:
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"name": "vcruntime140", "version": "14.42", "source": "vcredist"}
        ]
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        dep_errors = [e for e in result.errors if "dependencyClosure" in e]
        assert not dep_errors

    def test_dependency_missing_name_rejected(self) -> None:
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"version": "14.42", "source": "vcredist"}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("name" in e and "required" in e for e in errors)

    def test_dependency_invalid_source_rejected(self) -> None:
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"name": "lib", "version": "1.0", "source": "cargo"}
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("source" in e and "invalid" in e for e in errors)

    def test_too_many_dependencies_rejected(self) -> None:
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"name": f"lib{i}", "version": "1.0", "source": "system"}
            for i in range(vc.MAX_DEPENDENCIES + 1)
        ]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("dependencyClosure" in e and "max" in e.lower() for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 11: Collector identity
# ═══════════════════════════════════════════════════════════════════════════


class TestCollectorIdentity:
    """Defect #12: Collector identity is unattested."""

    def test_missing_collector_rejected(self) -> None:
        ev = _make_evidence()
        del ev["collector"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("collector" in e.lower() for e in errors)

    def test_collector_missing_type_rejected(self) -> None:
        ev = _make_evidence()
        ev["collector"] = {"identity": "test"}
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("collector.type" in e for e in errors)

    def test_collector_invalid_type_rejected(self) -> None:
        ev = _make_evidence()
        ev["collector"]["type"] = "magic"
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("collector.type" in e for e in errors)

    def test_collector_blank_identity_rejected(self) -> None:
        ev = _make_evidence()
        ev["collector"]["identity"] = "   "
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("identity" in e and "required" in e for e in errors)

    def test_collector_missing_identity_rejected(self) -> None:
        ev = _make_evidence()
        del ev["collector"]["identity"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("identity" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 12: Extra evidence probes
# ═══════════════════════════════════════════════════════════════════════════


class TestExtraEvidence:
    """Defect #13: Extra evidence categories silently ignored."""

    def test_unknown_probe_category_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["performance"] = _make_probe()
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("unknown" in e.lower() and "performance" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 13: Resource limits
# ═══════════════════════════════════════════════════════════════════════════


class TestResourceLimits:
    """Defect #15: Resource limits absent."""

    def test_too_many_matrix_rows_rejected(self) -> None:
        rows = [_make_row(row_id=f"row-{i}") for i in range(vc.MAX_MATRIX_ROWS + 1)]
        m = _make_matrix(rows=rows)
        errors = vc.validate_matrix(m, now=NOW)
        assert any("max" in e.lower() and str(vc.MAX_MATRIX_ROWS) in e for e in errors)

    def test_string_too_long_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["gpu"]["device"] = "X" * (vc.MAX_STRING_LENGTH + 1)
        errors = vc.validate_matrix(m, now=NOW)
        assert any("string too long" in e for e in errors)

    def test_evidence_string_too_long_rejected(self) -> None:
        ev = _make_evidence()
        ev["host"]["gpu"]["device"] = "Y" * (vc.MAX_STRING_LENGTH + 1)
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("string too long" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 14: Non-finite numerics in deep structures
# ═══════════════════════════════════════════════════════════════════════════


class TestNonFiniteNumeric:
    """Defect #4 extension: NaN/Infinity in nested structures (post-parse)."""

    def test_nonfinite_float_in_evidence_rejected(self) -> None:
        ev = _make_evidence()
        ev["host"]["ram"]["totalMb"] = float("inf")
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("non-finite" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 15: Cross-validation
# ═══════════════════════════════════════════════════════════════════════════


class TestCrossValidation:
    """Cross-validation between matrix and evidence."""

    def test_commit_mismatch_fails(self) -> None:
        m = _make_matrix(commit=GOOD_COMMIT)
        ev = _make_evidence(commit="c" * 40)
        result = vc.cross_validate(m, {"test-row": ev}, now=NOW)
        assert any("Commit mismatch" in e for e in result.errors)

    def test_missing_evidence_fails(self) -> None:
        m = _make_matrix(commit=GOOD_COMMIT)
        result = vc.cross_validate(m, {}, now=NOW)
        assert any("No evidence" in e for e in result.errors)
        assert result.rows_failed == 1

    def test_missing_required_probe_fails(self) -> None:
        ev = _make_evidence()
        del ev["probes"]["build"]
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("Missing required probe: build" in e for e in result.errors)

    def test_failing_probe_fails_certification(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(status="fail")
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("status='fail'" in e for e in result.errors)

    def test_stale_evidence_rejected(self) -> None:
        old_ts = (NOW - timedelta(hours=200)).isoformat()
        ev = _make_evidence(collected_at=old_ts)
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            max_age_hours=168,
            now=NOW,
        )
        assert any("old" in e for e in result.errors)

    def test_experimental_row_skipped(self) -> None:
        m = _make_matrix(
            rows=[_make_row(row_id="exp-row", tier="experimental")]
        )
        result = vc.cross_validate(m, {}, now=NOW)
        assert result.ok
        assert result.rows_failed == 0
        assert len(result.warnings) == 1

    def test_complete_certification_passes(self) -> None:
        ev = _make_evidence()
        ev["dependencyClosure"] = [
            {"name": "vcruntime140", "version": "14.42", "source": "vcredist"}
        ]
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert result.ok
        assert result.rows_certified == 1
        assert result.rows_failed == 0

    def test_evidence_future_timestamp_cross_rejected(self) -> None:
        future = (NOW + timedelta(hours=2)).isoformat()
        ev = _make_evidence(collected_at=future)
        result = vc.cross_validate(
            _make_matrix(commit=GOOD_COMMIT),
            {"test-row": ev},
            now=NOW,
        )
        assert any("future" in e for e in result.errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 16: Matrix validation basics
# ═══════════════════════════════════════════════════════════════════════════


class TestMatrixValidation:
    """Basic matrix structural validation."""

    def test_valid_matrix_passes(self) -> None:
        m = _make_matrix()
        errors = vc.validate_matrix(m, now=NOW)
        assert not errors

    def test_missing_schema_version_rejected(self) -> None:
        m = _make_matrix()
        del m["schemaVersion"]
        errors = vc.validate_matrix(m, now=NOW)
        assert any("schemaVersion" in e for e in errors)

    def test_wrong_schema_version_rejected(self) -> None:
        m = _make_matrix()
        m["schemaVersion"] = 99
        errors = vc.validate_matrix(m, now=NOW)
        assert any("schemaVersion" in e for e in errors)

    def test_invalid_profile_pattern_rejected(self) -> None:
        m = _make_matrix(profile="Invalid Profile!")
        errors = vc.validate_matrix(m, now=NOW)
        assert any("profile" in e.lower() for e in errors)

    def test_bad_commit_sha_rejected(self) -> None:
        m = _make_matrix(commit="not-a-sha")
        errors = vc.validate_matrix(m, now=NOW)
        assert any("commitSha" in e for e in errors)

    def test_empty_rows_rejected(self) -> None:
        m = _make_matrix(rows=[])
        errors = vc.validate_matrix(m, now=NOW)
        assert any("at least one row" in e for e in errors)

    def test_invalid_tier_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["tier"] = "mega"
        errors = vc.validate_matrix(m, now=NOW)
        assert any("tier" in e for e in errors)

    def test_invalid_arch_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["arch"] = "mips"
        errors = vc.validate_matrix(m, now=NOW)
        assert any("arch" in e for e in errors)

    def test_invalid_os_family_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["os"]["family"] = "android"
        errors = vc.validate_matrix(m, now=NOW)
        assert any("os.family" in e for e in errors)

    def test_unknown_evidence_category_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["evidenceRequired"].append("performance")
        errors = vc.validate_matrix(m, now=NOW)
        assert any("unknown evidence" in e.lower() for e in errors)

    def test_duplicate_evidence_required_rejected(self) -> None:
        m = _make_matrix()
        m["rows"][0]["evidenceRequired"] = ["build", "build"]
        errors = vc.validate_matrix(m, now=NOW)
        assert any("duplicate" in e.lower() for e in errors)

    def test_non_object_matrix_rejected(self) -> None:
        errors = vc.validate_matrix("not a dict", now=NOW)  # type: ignore[arg-type]
        assert any("JSON object" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 17: Evidence validation basics
# ═══════════════════════════════════════════════════════════════════════════


class TestEvidenceValidation:
    """Basic evidence structural validation."""

    def test_valid_evidence_passes(self) -> None:
        ev = _make_evidence()
        errors = vc.validate_evidence(ev, now=NOW)
        assert not errors

    def test_missing_row_id_rejected(self) -> None:
        ev = _make_evidence()
        del ev["rowId"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("rowId" in e for e in errors)

    def test_invalid_row_id_rejected(self) -> None:
        ev = _make_evidence(row_id="INVALID ROW!")
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("rowId" in e for e in errors)

    def test_bad_commit_in_evidence_rejected(self) -> None:
        ev = _make_evidence(commit="xyz")
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("commitSha" in e for e in errors)

    def test_missing_host_rejected(self) -> None:
        ev = _make_evidence()
        del ev["host"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("host" in e.lower() for e in errors)

    def test_missing_probes_rejected(self) -> None:
        ev = _make_evidence()
        del ev["probes"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("probes" in e.lower() for e in errors)

    def test_non_object_evidence_rejected(self) -> None:
        errors = vc.validate_evidence([], now=NOW)  # type: ignore[arg-type]
        assert any("JSON object" in e for e in errors)

    def test_skip_without_reason_rejected(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(status="skip", duration=0)
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("skipReason" in e for e in errors)

    def test_skip_with_reason_accepted(self) -> None:
        ev = _make_evidence()
        ev["probes"]["build"] = _make_probe(
            status="skip", duration=0, skipReason="Not applicable"
        )
        errors = vc.validate_evidence(ev, now=NOW)
        skip_errors = [e for e in errors if "skipReason" in e]
        assert not skip_errors

    def test_host_missing_os_locale_rejected(self) -> None:
        ev = _make_evidence()
        del ev["host"]["os"]["locale"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("os.locale" in e for e in errors)

    def test_host_missing_ram_rejected(self) -> None:
        ev = _make_evidence()
        del ev["host"]["ram"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("ram" in e.lower() for e in errors)

    def test_host_zero_ram_rejected(self) -> None:
        ev = _make_evidence()
        ev["host"]["ram"]["totalMb"] = 0
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("ram" in e.lower() and "positive" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 18: ValidationResult
# ═══════════════════════════════════════════════════════════════════════════


class TestValidationResult:
    def test_empty_result_is_ok(self) -> None:
        r = vc.ValidationResult()
        assert r.ok

    def test_result_with_error_is_not_ok(self) -> None:
        r = vc.ValidationResult()
        r.error("test error")
        assert not r.ok

    def test_summary_includes_pass(self) -> None:
        r = vc.ValidationResult()
        r.rows_checked = 1
        r.rows_certified = 1
        assert "PASS" in r.summary()

    def test_summary_includes_fail(self) -> None:
        r = vc.ValidationResult()
        r.error("bad")
        assert "FAIL" in r.summary()


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 19: CLI entry point (matrix-only)
# ═══════════════════════════════════════════════════════════════════════════


class TestCLI:
    def test_matrix_only_on_valid_matrix(self) -> None:
        ret = vc.main(["--matrix-only"])
        assert ret == 0

    def test_matrix_only_on_missing_file(self) -> None:
        ret = vc.main(["--matrix", "nonexistent.json", "--matrix-only"])
        assert ret == 1

    def test_full_validation_no_evidence(self) -> None:
        ret = vc.main([])
        assert ret == 1


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 20: Seed matrix validation
# ═══════════════════════════════════════════════════════════════════════════


class TestSeedMatrix:
    """Validates the actual shipped support-matrix.json file."""

    def test_seed_matrix_valid(self) -> None:
        path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        errors = vc.validate_matrix(data, now=NOW)
        profile_errors = [e for e in errors if "Profile" in e or "stable-v1" in e]
        ts_errors = [e for e in errors if "timestamp" in e.lower() or "future" in e or "too old" in e]
        other_errors = [e for e in errors if e not in profile_errors and e not in ts_errors]
        assert not other_errors, f"Structural errors: {other_errors}"

    def test_seed_matrix_has_two_rows(self) -> None:
        path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        assert len(data["rows"]) == 2

    def test_seed_matrix_strict_json(self) -> None:
        path = REPO_ROOT / "docs" / "certification" / "support-matrix.json"
        raw = path.read_text(encoding="utf-8")
        data = vc.parse_strict_json(raw, "support-matrix.json")
        assert data["profile"] == "stable-v1"


# ═══════════════════════════════════════════════════════════════════════════
# SECTION 21: Host sub-object validation (evidence)
# ═══════════════════════════════════════════════════════════════════════════


class TestHostSubObjects:
    def test_missing_cpu_model_rejected(self) -> None:
        ev = _make_evidence()
        del ev["host"]["cpu"]["model"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("cpu.model" in e for e in errors)

    def test_missing_cpu_features_rejected(self) -> None:
        ev = _make_evidence()
        del ev["host"]["cpu"]["features"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("cpu.features" in e for e in errors)

    def test_gpu_missing_device_rejected(self) -> None:
        ev = _make_evidence()
        del ev["host"]["gpu"]["device"]
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("gpu.device" in e for e in errors)

    def test_gpu_invalid_vendor_rejected(self) -> None:
        ev = _make_evidence()
        ev["host"]["gpu"]["vendor"] = "samsung"
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("gpu.vendor" in e for e in errors)

    def test_audio_invalid_api_rejected(self) -> None:
        ev = _make_evidence()
        ev["host"]["audio"]["api"] = "fmod"
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("audio" in e.lower() for e in errors)

    def test_invalid_arch_rejected(self) -> None:
        ev = _make_evidence()
        ev["host"]["arch"] = "arm32"
        errors = vc.validate_evidence(ev, now=NOW)
        assert any("arch" in e for e in errors)
