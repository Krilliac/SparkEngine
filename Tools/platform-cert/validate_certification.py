#!/usr/bin/env python3
"""Fail-closed validator for SparkEngine platform certification evidence.

This validator enforces that every support-matrix row has complete, current,
matching evidence before it can be marked certified.  It is designed to be
the sole gate for PLT-200 (G08) — if it passes, the row is certifiable;
if it fails or errors, the row is not.

Fail-closed rules:
  - Missing evidence file → row fails
  - Missing required probe → row fails
  - Probe status != "pass" → row fails
  - Commit SHA mismatch between matrix and evidence → row fails
  - Host fields that don't match the matrix row → row fails
  - Evidence older than staleBefore threshold → row fails
  - Evidence older than maxAgeHours → row fails
  - Schema validation failure → entire run fails
  - Any uncaught exception → exit 1
"""

from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]

ALL_PROBE_CATEGORIES = frozenset([
    "build", "install", "launch", "renderer", "content", "input",
    "audio", "save", "crash", "upgrade", "rollback", "uninstall",
    "dependency_closure",
])

VALID_TIERS = frozenset(["primary", "supported", "experimental", "unsupported"])
CERTIFIABLE_TIERS = frozenset(["primary", "supported"])
VALID_OS_FAMILIES = frozenset(["windows", "linux", "macos"])
VALID_ARCHES = frozenset(["x86_64", "aarch64"])
VALID_COMPILER_IDS = frozenset(["msvc", "gcc", "clang", "apple-clang", "mingw"])
VALID_GPU_APIS = frozenset(["d3d11", "d3d12", "vulkan", "opengl", "metal", "nullrhi"])
VALID_GPU_VENDORS = frozenset(["nvidia", "amd", "intel", "apple", "microsoft", "mesa", "none"])
VALID_AUDIO_APIS = frozenset(["xaudio2", "openal", "none"])
VALID_PROBE_STATUSES = frozenset(["pass", "fail", "skip", "error"])
VALID_COLLECTOR_TYPES = frozenset(["ci", "manual", "automated"])
VALID_DEP_SOURCES = frozenset(["system", "bundled", "vcredist", "directx", "sdk"])
SHA_PATTERN_LEN = 40


class CertificationError(Exception):
    """A validation failure that makes certification impossible."""


class ValidationResult:
    """Accumulates pass/fail verdicts per row."""

    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.rows_checked = 0
        self.rows_certified = 0
        self.rows_failed = 0

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    @property
    def ok(self) -> bool:
        return len(self.errors) == 0

    def summary(self) -> str:
        lines = []
        if self.errors:
            lines.append(f"FAIL: {len(self.errors)} error(s)")
            for e in self.errors:
                lines.append(f"  X {e}")
        if self.warnings:
            lines.append(f"WARN: {len(self.warnings)} warning(s)")
            for w in self.warnings:
                lines.append(f"  ! {w}")
        lines.append(
            f"Rows: {self.rows_checked} checked, "
            f"{self.rows_certified} certified, "
            f"{self.rows_failed} failed"
        )
        if self.ok and self.rows_checked > 0:
            lines.insert(0, "PASS: All rows validated")
        return "\n".join(lines)


def _load_json(path: Path) -> Any:
    if not path.exists():
        raise CertificationError(f"Required file does not exist: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise CertificationError(
            f"Invalid JSON in {path}: line {e.lineno} col {e.colno}: {e.msg}"
        ) from e


def _is_sha(value: str) -> bool:
    return (
        isinstance(value, str)
        and len(value) == SHA_PATTERN_LEN
        and all(c in "0123456789abcdef" for c in value)
    )


def _parse_iso(value: str) -> datetime:
    try:
        dt = datetime.fromisoformat(value)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt
    except (ValueError, TypeError) as e:
        raise CertificationError(f"Invalid ISO 8601 timestamp: {value!r}") from e


def _check_enum(value: Any, allowed: frozenset[str], field: str, context: str) -> None:
    if value not in allowed:
        raise CertificationError(
            f"{context}: {field}={value!r} not in {sorted(allowed)}"
        )


def validate_matrix(matrix: dict[str, Any]) -> list[str]:
    """Validate the support matrix structure. Returns list of errors."""
    errors: list[str] = []

    if matrix.get("schemaVersion") != SCHEMA_VERSION:
        errors.append(f"Matrix schemaVersion must be {SCHEMA_VERSION}")

    if not isinstance(matrix.get("profile"), str) or not matrix["profile"]:
        errors.append("Matrix must have a non-empty 'profile' string")

    commit = matrix.get("commitSha", "")
    if not _is_sha(commit):
        errors.append(f"Matrix commitSha must be a 40-char hex string, got {commit!r}")

    if not isinstance(matrix.get("generatedAt"), str):
        errors.append("Matrix must have a 'generatedAt' ISO 8601 timestamp")
    else:
        try:
            _parse_iso(matrix["generatedAt"])
        except CertificationError as e:
            errors.append(str(e))

    rows = matrix.get("rows")
    if not isinstance(rows, list) or len(rows) == 0:
        errors.append("Matrix must have at least one row")
        return errors

    seen_ids: set[str] = set()
    for i, row in enumerate(rows):
        ctx = f"rows[{i}]"
        if not isinstance(row, dict):
            errors.append(f"{ctx}: must be an object")
            continue

        row_id = row.get("id", "")
        if not isinstance(row_id, str) or not row_id:
            errors.append(f"{ctx}: missing or empty 'id'")
        elif row_id in seen_ids:
            errors.append(f"{ctx}: duplicate row id {row_id!r}")
        else:
            seen_ids.add(row_id)
            ctx = f"row[{row_id}]"

        tier = row.get("tier")
        if tier not in VALID_TIERS:
            errors.append(f"{ctx}: tier={tier!r} not in {sorted(VALID_TIERS)}")

        os_spec = row.get("os")
        if not isinstance(os_spec, dict):
            errors.append(f"{ctx}: 'os' must be an object")
        else:
            for f in ("family", "version", "build"):
                if not isinstance(os_spec.get(f), str) or not os_spec[f]:
                    errors.append(f"{ctx}: os.{f} is required")
            if os_spec.get("family") and os_spec["family"] not in VALID_OS_FAMILIES:
                errors.append(f"{ctx}: os.family={os_spec['family']!r} invalid")

        if row.get("arch") not in VALID_ARCHES:
            errors.append(f"{ctx}: arch={row.get('arch')!r} invalid")

        compiler = row.get("compiler")
        if not isinstance(compiler, dict):
            errors.append(f"{ctx}: 'compiler' must be an object")
        else:
            for f in ("id", "version", "toolset"):
                if not isinstance(compiler.get(f), str) or not compiler[f]:
                    errors.append(f"{ctx}: compiler.{f} is required")
            if compiler.get("id") and compiler["id"] not in VALID_COMPILER_IDS:
                errors.append(f"{ctx}: compiler.id={compiler['id']!r} invalid")

        gpu = row.get("gpu")
        if not isinstance(gpu, dict):
            errors.append(f"{ctx}: 'gpu' must be an object")
        else:
            for f in ("api", "device", "vendor", "driverVersion", "featureLevel"):
                if not isinstance(gpu.get(f), str) or not gpu[f]:
                    errors.append(f"{ctx}: gpu.{f} is required")
            if gpu.get("api") and gpu["api"] not in VALID_GPU_APIS:
                errors.append(f"{ctx}: gpu.api={gpu['api']!r} invalid")
            if gpu.get("vendor") and gpu["vendor"] not in VALID_GPU_VENDORS:
                errors.append(f"{ctx}: gpu.vendor={gpu['vendor']!r} invalid")

        audio = row.get("audio")
        if not isinstance(audio, dict):
            errors.append(f"{ctx}: 'audio' must be an object")
        else:
            if not isinstance(audio.get("api"), str) or audio["api"] not in VALID_AUDIO_APIS:
                errors.append(f"{ctx}: audio.api invalid")

        ev_req = row.get("evidenceRequired")
        if not isinstance(ev_req, list) or len(ev_req) == 0:
            errors.append(f"{ctx}: evidenceRequired must be a non-empty list")
        elif not all(e in ALL_PROBE_CATEGORIES for e in ev_req):
            bad = [e for e in ev_req if e not in ALL_PROBE_CATEGORIES]
            errors.append(f"{ctx}: unknown evidence categories: {bad}")
        elif len(set(ev_req)) != len(ev_req):
            errors.append(f"{ctx}: evidenceRequired has duplicates")

    return errors


def validate_evidence(evidence: dict[str, Any]) -> list[str]:
    """Validate a single evidence record structure. Returns list of errors."""
    errors: list[str] = []

    if evidence.get("schemaVersion") != SCHEMA_VERSION:
        errors.append(f"Evidence schemaVersion must be {SCHEMA_VERSION}")

    row_id = evidence.get("rowId", "")
    if not isinstance(row_id, str) or not row_id:
        errors.append("Evidence must have a non-empty 'rowId'")

    commit = evidence.get("commitSha", "")
    if not _is_sha(commit):
        errors.append(f"Evidence commitSha must be 40-char hex, got {commit!r}")

    if not isinstance(evidence.get("collectedAt"), str):
        errors.append("Evidence must have 'collectedAt' timestamp")
    else:
        try:
            _parse_iso(evidence["collectedAt"])
        except CertificationError as e:
            errors.append(str(e))

    collector = evidence.get("collector")
    if not isinstance(collector, dict):
        errors.append("Evidence must have a 'collector' object")
    else:
        if collector.get("type") not in VALID_COLLECTOR_TYPES:
            errors.append(f"collector.type={collector.get('type')!r} invalid")
        if not isinstance(collector.get("identity"), str) or not collector["identity"]:
            errors.append("collector.identity is required")

    host = evidence.get("host")
    if not isinstance(host, dict):
        errors.append("Evidence must have a 'host' object")
    else:
        _validate_host(host, errors)

    probes = evidence.get("probes")
    if not isinstance(probes, dict):
        errors.append("Evidence must have a 'probes' object")
    else:
        for name, probe in probes.items():
            if name not in ALL_PROBE_CATEGORIES:
                errors.append(f"Unknown probe category: {name!r}")
                continue
            _validate_probe(name, probe, errors)

    deps = evidence.get("dependencyClosure")
    if deps is not None:
        if not isinstance(deps, list):
            errors.append("dependencyClosure must be a list")
        else:
            for i, dep in enumerate(deps):
                if not isinstance(dep, dict):
                    errors.append(f"dependencyClosure[{i}] must be an object")
                    continue
                for f in ("name", "version", "source"):
                    if not isinstance(dep.get(f), str) or not dep[f]:
                        errors.append(f"dependencyClosure[{i}].{f} is required")
                if dep.get("source") and dep["source"] not in VALID_DEP_SOURCES:
                    errors.append(
                        f"dependencyClosure[{i}].source={dep['source']!r} invalid"
                    )

    return errors


def _validate_host(host: dict[str, Any], errors: list[str]) -> None:
    os_info = host.get("os")
    if isinstance(os_info, dict):
        for f in ("family", "version", "build", "locale"):
            if not isinstance(os_info.get(f), str) or not os_info[f]:
                errors.append(f"host.os.{f} is required")
    else:
        errors.append("host.os must be an object")

    if host.get("arch") not in VALID_ARCHES:
        errors.append(f"host.arch={host.get('arch')!r} invalid")

    cpu = host.get("cpu")
    if isinstance(cpu, dict):
        if not isinstance(cpu.get("model"), str) or not cpu["model"]:
            errors.append("host.cpu.model is required")
        if not isinstance(cpu.get("features"), list):
            errors.append("host.cpu.features must be a list")
    else:
        errors.append("host.cpu must be an object")

    ram = host.get("ram")
    if isinstance(ram, dict):
        if not isinstance(ram.get("totalMb"), int) or ram["totalMb"] < 1:
            errors.append("host.ram.totalMb must be a positive integer")
    else:
        errors.append("host.ram must be an object")

    gpu = host.get("gpu")
    if isinstance(gpu, dict):
        for f in ("api", "device", "vendor", "driverVersion", "featureLevel"):
            if not isinstance(gpu.get(f), str) or not gpu[f]:
                errors.append(f"host.gpu.{f} is required")
    else:
        errors.append("host.gpu must be an object")

    audio = host.get("audio")
    if isinstance(audio, dict):
        if not isinstance(audio.get("api"), str) or audio["api"] not in VALID_AUDIO_APIS:
            errors.append("host.audio.api invalid")
    else:
        errors.append("host.audio must be an object")


def _validate_probe(name: str, probe: dict[str, Any], errors: list[str]) -> None:
    if not isinstance(probe, dict):
        errors.append(f"probes.{name} must be an object")
        return
    status = probe.get("status")
    if status not in VALID_PROBE_STATUSES:
        errors.append(f"probes.{name}.status={status!r} invalid")
    if not isinstance(probe.get("durationMs"), int) or probe["durationMs"] < 0:
        errors.append(f"probes.{name}.durationMs must be a non-negative integer")
    if status == "skip" and not probe.get("skipReason"):
        errors.append(f"probes.{name}: status=skip requires skipReason")
    artifacts = probe.get("artifacts")
    if artifacts is not None:
        if not isinstance(artifacts, list):
            errors.append(f"probes.{name}.artifacts must be a list")
        else:
            for j, art in enumerate(artifacts):
                if not isinstance(art, dict):
                    errors.append(f"probes.{name}.artifacts[{j}] must be an object")
                    continue
                if not isinstance(art.get("path"), str) or not art["path"]:
                    errors.append(f"probes.{name}.artifacts[{j}].path is required")
                sha = art.get("sha256", "")
                if not isinstance(sha, str) or len(sha) != 64:
                    errors.append(f"probes.{name}.artifacts[{j}].sha256 invalid")


def cross_validate(
    matrix: dict[str, Any],
    evidence_records: dict[str, dict[str, Any]],
    *,
    max_age_hours: float = 168,
    now: datetime | None = None,
) -> ValidationResult:
    """Cross-validate matrix rows against their evidence records.

    This is the core certification gate. It checks:
    1. Every certifiable row has an evidence record
    2. Commit SHAs match between matrix and evidence
    3. Host environment matches the declared row
    4. All required probes are present and passing
    5. Evidence is not stale
    """
    result = ValidationResult()
    if now is None:
        now = datetime.now(timezone.utc)

    matrix_commit = matrix.get("commitSha", "")

    for row in matrix.get("rows", []):
        row_id = row.get("id", "<unknown>")
        tier = row.get("tier", "")
        result.rows_checked += 1

        if tier not in CERTIFIABLE_TIERS:
            result.warn(f"[{row_id}] tier={tier!r} is not certifiable, skipping")
            continue

        evidence = evidence_records.get(row_id)
        if evidence is None:
            result.error(f"[{row_id}] No evidence record found -- row cannot certify")
            result.rows_failed += 1
            continue

        row_errors: list[str] = []

        ev_commit = evidence.get("commitSha", "")
        if ev_commit != matrix_commit:
            row_errors.append(
                f"Commit mismatch: matrix={matrix_commit[:8]} evidence={ev_commit[:8]}"
            )

        collected_at_str = evidence.get("collectedAt", "")
        try:
            collected_at = _parse_iso(collected_at_str)
        except CertificationError:
            row_errors.append(f"Invalid collectedAt: {collected_at_str!r}")
            collected_at = None

        if collected_at is not None:
            age_hours = (now - collected_at).total_seconds() / 3600
            if age_hours > max_age_hours:
                row_errors.append(
                    f"Evidence is {age_hours:.0f}h old (max {max_age_hours:.0f}h)"
                )
            stale_before = evidence.get("staleBefore")
            if stale_before:
                try:
                    stale_dt = _parse_iso(stale_before)
                    if collected_at < stale_dt:
                        row_errors.append(
                            f"Evidence collected before staleBefore threshold"
                        )
                except CertificationError:
                    row_errors.append(f"Invalid staleBefore: {stale_before!r}")

        _cross_validate_host(row, evidence, row_errors)

        required = set(row.get("evidenceRequired", []))
        probes = evidence.get("probes", {})
        for category in required:
            probe = probes.get(category)
            if probe is None:
                row_errors.append(f"Missing required probe: {category}")
            elif probe.get("status") != "pass":
                status = probe.get("status", "<missing>")
                detail = probe.get("detail", "")
                msg = f"Probe {category} status={status!r}"
                if detail:
                    msg += f": {detail[:100]}"
                row_errors.append(msg)

        if row_errors:
            for e in row_errors:
                result.error(f"[{row_id}] {e}")
            result.rows_failed += 1
        else:
            result.rows_certified += 1

    return result


def _cross_validate_host(
    row: dict[str, Any],
    evidence: dict[str, Any],
    errors: list[str],
) -> None:
    """Verify the evidence host matches the matrix row declaration."""
    host = evidence.get("host", {})

    row_os = row.get("os", {})
    host_os = host.get("os", {})
    if row_os.get("family") and host_os.get("family") != row_os["family"]:
        errors.append(
            f"OS family mismatch: row={row_os['family']} host={host_os.get('family')}"
        )
    if row_os.get("version") and host_os.get("version") != row_os["version"]:
        errors.append(
            f"OS version mismatch: row={row_os['version']} host={host_os.get('version')}"
        )

    if row.get("arch") and host.get("arch") != row["arch"]:
        errors.append(f"Arch mismatch: row={row['arch']} host={host.get('arch')}")

    row_gpu = row.get("gpu", {})
    host_gpu = host.get("gpu", {})
    if row_gpu.get("api") and host_gpu.get("api") != row_gpu["api"]:
        errors.append(
            f"GPU API mismatch: row={row_gpu['api']} host={host_gpu.get('api')}"
        )

    row_compiler = row.get("compiler", {})
    host_compiler = host.get("compiler", {})
    if host_compiler:
        if row_compiler.get("id") and host_compiler.get("id") != row_compiler["id"]:
            errors.append(
                f"Compiler mismatch: row={row_compiler['id']} host={host_compiler.get('id')}"
            )
        if row_compiler.get("toolset") and host_compiler.get("toolset") != row_compiler["toolset"]:
            errors.append(
                f"Toolset mismatch: row={row_compiler['toolset']} host={host_compiler.get('toolset')}"
            )

    row_cpu_features = set(row.get("cpuFeatures", []))
    host_cpu_features = set(host.get("cpu", {}).get("features", []))
    missing = row_cpu_features - host_cpu_features
    if missing:
        errors.append(f"Host missing required CPU features: {sorted(missing)}")

    row_audio = row.get("audio", {})
    host_audio = host.get("audio", {})
    if row_audio.get("api") and host_audio.get("api") != row_audio["api"]:
        errors.append(
            f"Audio API mismatch: row={row_audio['api']} host={host_audio.get('api')}"
        )


def load_and_validate(
    matrix_path: Path,
    evidence_dir: Path,
    *,
    max_age_hours: float = 168,
) -> ValidationResult:
    """Load matrix + evidence files and run full validation."""
    result = ValidationResult()

    matrix = _load_json(matrix_path)
    matrix_errors = validate_matrix(matrix)
    if matrix_errors:
        for e in matrix_errors:
            result.error(f"[matrix] {e}")
        return result

    evidence_records: dict[str, dict[str, Any]] = {}
    if evidence_dir.is_dir():
        for path in sorted(evidence_dir.glob("*.json")):
            ev = _load_json(path)
            ev_errors = validate_evidence(ev)
            if ev_errors:
                for e in ev_errors:
                    result.error(f"[{path.name}] {e}")
                continue
            row_id = ev.get("rowId", "")
            if row_id in evidence_records:
                result.error(f"Duplicate evidence for row {row_id!r}")
            else:
                evidence_records[row_id] = ev

    if result.errors:
        return result

    cross = cross_validate(
        matrix, evidence_records, max_age_hours=max_age_hours
    )
    result.errors.extend(cross.errors)
    result.warnings.extend(cross.warnings)
    result.rows_checked = cross.rows_checked
    result.rows_certified = cross.rows_certified
    result.rows_failed = cross.rows_failed
    return result


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Validate platform certification evidence"
    )
    parser.add_argument(
        "--matrix",
        type=Path,
        default=REPO_ROOT / "docs" / "certification" / "support-matrix.json",
        help="Path to the support matrix JSON",
    )
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        default=REPO_ROOT / "docs" / "certification" / "evidence",
        help="Directory containing evidence JSON files",
    )
    parser.add_argument(
        "--max-age-hours",
        type=float,
        default=168,
        help="Maximum evidence age in hours (default: 168 = 7 days)",
    )
    parser.add_argument(
        "--matrix-only",
        action="store_true",
        help="Only validate the matrix schema, skip evidence",
    )

    args = parser.parse_args(argv)

    try:
        if args.matrix_only:
            matrix = _load_json(args.matrix)
            errors = validate_matrix(matrix)
            if errors:
                for e in errors:
                    print(f"  X {e}", file=sys.stderr)
                print(f"FAIL: {len(errors)} matrix error(s)", file=sys.stderr)
                return 1
            print("PASS: Matrix schema is valid")
            return 0

        result = load_and_validate(
            args.matrix, args.evidence_dir, max_age_hours=args.max_age_hours
        )
        print(result.summary())
        return 0 if result.ok else 1

    except CertificationError as e:
        print(f"FATAL: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"FATAL: Unexpected error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
