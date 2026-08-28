#!/usr/bin/env python3
"""Fail-closed validator for SparkEngine platform certification evidence (PLT-200).

Loads the committed JSON schemas, derives all validation constraints from them,
and enforces strict fail-closed rules.  If it passes, the row is certifiable;
if it fails or errors, the row is not.

Defenses:
  - Strict JSON: duplicate keys and NaN/Infinity rejected at parse time
  - Document size capped at MAX_DOCUMENT_BYTES
  - additionalProperties enforced for every object type
  - Timestamps bounded: no pre-2020, no future beyond 5-minute clock skew
  - Pass probes require positive duration (>= 1 ms)
  - SHA-256: 64 lowercase hex; artifact paths confined (no .., no absolute)
  - Artifact sizeBytes required
  - Complete host matching: OS build, compiler version, GPU vendor/device/driver
  - Canonical profile coverage: no omitted or unexpected certifiable rows
  - Dependency closure required when evidenceRequired includes dependency_closure
  - Resource limits on rows, dependencies, artifacts, and string lengths
"""

from __future__ import annotations

import json
import math
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]
SCHEMA_VERSION = 1

# ── Resource limits ────────────────────────────────────────────────────────
MAX_DOCUMENT_BYTES = 2 * 1024 * 1024
MAX_MATRIX_ROWS = 200
MAX_DEPENDENCIES = 1000
MAX_ARTIFACTS_PER_PROBE = 100
MAX_STRING_LENGTH = 4000
MAX_COLLECTIONS = 500
MIN_TIMESTAMP = datetime(2020, 1, 1, tzinfo=timezone.utc)
CLOCK_SKEW_SECONDS = 300
MIN_PASS_DURATION_MS = 1

# ── Patterns ───────────────────────────────────────────────────────────────
_SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_ROW_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
_PROFILE_RE = re.compile(r"^[a-z][a-z0-9-]*$")

# ── Schema loading (eager, fail-closed) ────────────────────────────────────


def _load_schema_file(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise RuntimeError(f"Required schema file missing: {path}")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _keys_of(schema_obj: dict[str, Any]) -> frozenset[str]:
    return frozenset(schema_obj.get("properties", {}).keys())


_EV_SCHEMA = _load_schema_file(TOOLS_DIR / "evidence_schema.json")
_MX_SCHEMA = _load_schema_file(TOOLS_DIR / "support_matrix_schema.json")

_ev_host = _EV_SCHEMA["properties"]["host"]["properties"]
_ev_defs = _EV_SCHEMA["$defs"]
_mx_defs = _MX_SCHEMA["$defs"]

# Constants derived from schemas
ALL_PROBE_CATEGORIES = frozenset(
    _EV_SCHEMA["properties"]["probes"]["properties"].keys()
)
VALID_OS_FAMILIES = frozenset(
    _ev_host["os"]["properties"]["family"]["enum"]
)
VALID_ARCHES = frozenset(_ev_host["arch"]["enum"])
VALID_GPU_APIS = frozenset(
    _ev_host["gpu"]["properties"]["api"]["enum"]
)
VALID_GPU_VENDORS = frozenset(
    _ev_host["gpu"]["properties"]["vendor"]["enum"]
)
VALID_AUDIO_APIS = frozenset(
    _ev_host["audio"]["properties"]["api"]["enum"]
)
VALID_COMPILER_IDS = frozenset(
    _ev_host["compiler"]["properties"]["id"]["enum"]
)
VALID_COLLECTOR_TYPES = frozenset(
    _EV_SCHEMA["properties"]["collector"]["properties"]["type"]["enum"]
)
VALID_PROBE_STATUSES = frozenset(
    _ev_defs["Probe"]["properties"]["status"]["enum"]
)
VALID_DEP_SOURCES = frozenset(
    _ev_defs["RuntimeDependency"]["properties"]["source"]["enum"]
)
VALID_TIERS = frozenset(
    _mx_defs["SupportRow"]["properties"]["tier"]["enum"]
)
CERTIFIABLE_TIERS = frozenset(["primary", "supported"])

# Allowed key sets (additionalProperties enforcement)
_MX_KEYS = _keys_of(_MX_SCHEMA)
_MX_ROW_KEYS = _keys_of(_mx_defs["SupportRow"])
_MX_OS_KEYS = _keys_of(_mx_defs["OsSpec"])
_MX_COMPILER_KEYS = _keys_of(_mx_defs["CompilerSpec"])
_MX_GPU_KEYS = _keys_of(_mx_defs["GpuSpec"])
_MX_AUDIO_KEYS = _keys_of(_mx_defs["AudioSpec"])

_EV_KEYS = _keys_of(_EV_SCHEMA)
_EV_COLLECTOR_KEYS = _keys_of(_EV_SCHEMA["properties"]["collector"])
_EV_HOST_KEYS = _keys_of(_EV_SCHEMA["properties"]["host"])
_EV_HOST_OS_KEYS = _keys_of(_ev_host["os"])
_EV_HOST_CPU_KEYS = _keys_of(_ev_host["cpu"])
_EV_HOST_RAM_KEYS = _keys_of(_ev_host["ram"])
_EV_HOST_GPU_KEYS = _keys_of(_ev_host["gpu"])
_EV_HOST_AUDIO_KEYS = _keys_of(_ev_host["audio"])
_EV_HOST_COMPILER_KEYS = _keys_of(_ev_host["compiler"])
_EV_PROBE_KEYS = _keys_of(_ev_defs["Probe"])
_EV_ARTIFACT_KEYS = frozenset(
    _ev_defs["Probe"]["properties"]["artifacts"]["items"]["properties"].keys()
)
_EV_DEP_KEYS = _keys_of(_ev_defs["RuntimeDependency"])

# Cross-check: schemas must agree on shared enums
for _cname, _ev_val, _mx_val in [
    (
        "os_families",
        VALID_OS_FAMILIES,
        frozenset(_mx_defs["OsSpec"]["properties"]["family"]["enum"]),
    ),
    (
        "arches",
        VALID_ARCHES,
        frozenset(_mx_defs["SupportRow"]["properties"]["arch"]["enum"]),
    ),
    (
        "gpu_apis",
        VALID_GPU_APIS,
        frozenset(_mx_defs["GpuSpec"]["properties"]["api"]["enum"]),
    ),
    (
        "gpu_vendors",
        VALID_GPU_VENDORS,
        frozenset(_mx_defs["GpuSpec"]["properties"]["vendor"]["enum"]),
    ),
    (
        "audio_apis",
        VALID_AUDIO_APIS,
        frozenset(_mx_defs["AudioSpec"]["properties"]["api"]["enum"]),
    ),
    (
        "compiler_ids",
        VALID_COMPILER_IDS,
        frozenset(_mx_defs["CompilerSpec"]["properties"]["id"]["enum"]),
    ),
    (
        "probe_categories",
        ALL_PROBE_CATEGORIES,
        frozenset(
            _mx_defs["SupportRow"]["properties"]["evidenceRequired"]["items"]["enum"]
        ),
    ),
]:
    if _ev_val != _mx_val:
        raise RuntimeError(f"Schema enum mismatch: {_cname}")

# ── Canonical profiles ─────────────────────────────────────────────────────
CANONICAL_PROFILES: dict[str, dict[str, Any]] = {
    "stable-v1": {
        "required_rows": {
            "win11-x64-msvc143-d3d11": {
                "tier": "primary",
                "evidence_required": ALL_PROBE_CATEGORIES,
            },
            "win11-x64-msvc143-nullrhi": {
                "tier": "supported",
                "evidence_required": ALL_PROBE_CATEGORIES
                - {"renderer", "content", "input", "audio"},
            },
        },
    },
}


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
        lines: list[str] = []
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


# ── Strict JSON parsing ───────────────────────────────────────────────────


def parse_strict_json(text: str, source: str = "<input>") -> Any:
    """Parse JSON text rejecting duplicate keys and non-finite constants."""

    def _reject_dups(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        obj: dict[str, Any] = {}
        for key, value in pairs:
            if key in obj:
                raise ValueError(f"Duplicate JSON key: {key!r}")
            obj[key] = value
        return obj

    def _reject_const(c: str) -> None:
        raise ValueError(f"Non-finite JSON constant: {c!r}")

    try:
        return json.loads(
            text, object_pairs_hook=_reject_dups, parse_constant=_reject_const
        )
    except json.JSONDecodeError as e:
        raise CertificationError(
            f"Invalid JSON in {source}: line {e.lineno} col {e.colno}: {e.msg}"
        ) from e
    except ValueError as e:
        raise CertificationError(f"Invalid JSON in {source}: {e}") from e


def _load_strict_json(path: Path) -> Any:
    """Load JSON file with strict parsing and size limit."""
    if not path.exists():
        raise CertificationError(f"Required file does not exist: {path}")
    raw = path.read_bytes()
    if len(raw) > MAX_DOCUMENT_BYTES:
        raise CertificationError(
            f"Document too large: {len(raw)} bytes (max {MAX_DOCUMENT_BYTES})"
        )
    return parse_strict_json(raw.decode("utf-8"), path.name)


# ── Helpers ────────────────────────────────────────────────────────────────


def _check_no_nonfinite(obj: Any, path: str, errors: list[str]) -> None:
    if isinstance(obj, float) and not math.isfinite(obj):
        errors.append(f"{path}: non-finite numeric value")
    elif isinstance(obj, dict):
        for k, v in obj.items():
            _check_no_nonfinite(v, f"{path}.{k}", errors)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _check_no_nonfinite(v, f"{path}[{i}]", errors)


def _check_string_lengths(obj: Any, path: str, errors: list[str]) -> None:
    if isinstance(obj, str) and len(obj) > MAX_STRING_LENGTH:
        errors.append(
            f"{path}: string too long ({len(obj)} > {MAX_STRING_LENGTH})"
        )
    elif isinstance(obj, dict):
        for k, v in obj.items():
            _check_string_lengths(v, f"{path}.{k}", errors)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _check_string_lengths(v, f"{path}[{i}]", errors)


def _check_extra_keys(
    obj: dict[str, Any],
    allowed: frozenset[str],
    context: str,
    errors: list[str],
) -> None:
    extra = set(obj.keys()) - allowed
    if extra:
        errors.append(f"{context}: unknown fields {sorted(extra)}")


def _parse_bounded_ts(
    value: Any,
    *,
    now: datetime,
    context: str,
    errors: list[str],
) -> datetime | None:
    if not isinstance(value, str) or not value:
        errors.append(f"{context}: missing or invalid timestamp")
        return None
    try:
        dt = datetime.fromisoformat(value)
    except (ValueError, TypeError):
        errors.append(f"{context}: invalid ISO 8601 timestamp: {value!r}")
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    if dt < MIN_TIMESTAMP:
        errors.append(f"{context}: timestamp too old: {value!r}")
        return None
    max_future = now + timedelta(seconds=CLOCK_SKEW_SECONDS)
    if dt > max_future:
        errors.append(f"{context}: timestamp in the future: {value!r}")
        return None
    return dt


def _check_sha1(value: Any, field: str, errors: list[str]) -> None:
    if not isinstance(value, str) or not _SHA1_RE.match(value):
        errors.append(f"{field}: must be 40 lowercase hex chars, got {value!r}")


def _check_artifact(art: Any, context: str, errors: list[str]) -> None:
    if not isinstance(art, dict):
        errors.append(f"{context}: must be an object")
        return
    _check_extra_keys(art, _EV_ARTIFACT_KEYS, context, errors)
    path_val = art.get("path", "")
    if not isinstance(path_val, str) or not path_val:
        errors.append(f"{context}.path: required non-empty string")
    else:
        if path_val.startswith("/") or (
            len(path_val) >= 2 and path_val[1] == ":"
        ):
            errors.append(
                f"{context}.path: must be relative, got {path_val!r}"
            )
        elif ".." in path_val.replace("\\", "/").split("/"):
            errors.append(
                f"{context}.path: directory traversal not allowed"
            )
        elif "\\" in path_val:
            errors.append(f"{context}.path: must use forward slashes")
    sha = art.get("sha256", "")
    if not isinstance(sha, str) or not _SHA256_RE.match(sha):
        errors.append(f"{context}.sha256: must be 64 lowercase hex chars")
    size = art.get("sizeBytes")
    if not isinstance(size, int) or size < 0:
        errors.append(f"{context}.sizeBytes: required non-negative integer")


# ── Matrix validation ─────────────────────────────────────────────────────


def validate_matrix(
    matrix: dict[str, Any], *, now: datetime | None = None
) -> list[str]:
    """Validate support matrix structure. Returns list of errors."""
    errors: list[str] = []
    if not isinstance(matrix, dict):
        return ["Matrix must be a JSON object"]
    if now is None:
        now = datetime.now(timezone.utc)

    _check_no_nonfinite(matrix, "$", errors)
    _check_string_lengths(matrix, "$", errors)
    if errors:
        return errors

    _check_extra_keys(matrix, _MX_KEYS, "matrix", errors)

    if matrix.get("schemaVersion") != SCHEMA_VERSION:
        errors.append(f"Matrix schemaVersion must be {SCHEMA_VERSION}")

    profile = matrix.get("profile", "")
    if not isinstance(profile, str) or not _PROFILE_RE.match(profile):
        errors.append("Matrix profile must match ^[a-z][a-z0-9-]*$")

    _check_sha1(matrix.get("commitSha"), "matrix.commitSha", errors)
    _parse_bounded_ts(
        matrix.get("generatedAt"), now=now, context="matrix.generatedAt", errors=errors
    )

    rows = matrix.get("rows")
    if not isinstance(rows, list) or len(rows) == 0:
        errors.append("Matrix must have at least one row")
        return errors
    if len(rows) > MAX_MATRIX_ROWS:
        errors.append(f"Matrix has {len(rows)} rows (max {MAX_MATRIX_ROWS})")

    seen_ids: set[str] = set()
    for i, row in enumerate(rows):
        ctx = f"rows[{i}]"
        if not isinstance(row, dict):
            errors.append(f"{ctx}: must be an object")
            continue

        _check_extra_keys(row, _MX_ROW_KEYS, ctx, errors)

        row_id = row.get("id", "")
        if not isinstance(row_id, str) or not _ROW_ID_RE.match(row_id):
            errors.append(f"{ctx}: id must match {_ROW_ID_RE.pattern}")
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
            errors.append(f"{ctx}: os must be an object")
        else:
            _check_extra_keys(os_spec, _MX_OS_KEYS, f"{ctx}.os", errors)
            for f in ("family", "version", "build"):
                if not isinstance(os_spec.get(f), str) or not os_spec[f]:
                    errors.append(f"{ctx}: os.{f} is required")
            if os_spec.get("family") and os_spec["family"] not in VALID_OS_FAMILIES:
                errors.append(f"{ctx}: os.family={os_spec['family']!r} invalid")

        if row.get("arch") not in VALID_ARCHES:
            errors.append(f"{ctx}: arch={row.get('arch')!r} invalid")

        compiler = row.get("compiler")
        if not isinstance(compiler, dict):
            errors.append(f"{ctx}: compiler must be an object")
        else:
            _check_extra_keys(compiler, _MX_COMPILER_KEYS, f"{ctx}.compiler", errors)
            for f in ("id", "version", "toolset"):
                if not isinstance(compiler.get(f), str) or not compiler[f]:
                    errors.append(f"{ctx}: compiler.{f} is required")
            if compiler.get("id") and compiler["id"] not in VALID_COMPILER_IDS:
                errors.append(f"{ctx}: compiler.id={compiler['id']!r} invalid")

        gpu = row.get("gpu")
        if not isinstance(gpu, dict):
            errors.append(f"{ctx}: gpu must be an object")
        else:
            _check_extra_keys(gpu, _MX_GPU_KEYS, f"{ctx}.gpu", errors)
            for f in ("api", "device", "vendor", "driverVersion", "featureLevel"):
                if not isinstance(gpu.get(f), str) or not gpu[f]:
                    errors.append(f"{ctx}: gpu.{f} is required")
            if gpu.get("api") and gpu["api"] not in VALID_GPU_APIS:
                errors.append(f"{ctx}: gpu.api={gpu['api']!r} invalid")
            if gpu.get("vendor") and gpu["vendor"] not in VALID_GPU_VENDORS:
                errors.append(f"{ctx}: gpu.vendor={gpu['vendor']!r} invalid")

        audio = row.get("audio")
        if not isinstance(audio, dict):
            errors.append(f"{ctx}: audio must be an object")
        else:
            _check_extra_keys(audio, _MX_AUDIO_KEYS, f"{ctx}.audio", errors)
            if (
                not isinstance(audio.get("api"), str)
                or audio["api"] not in VALID_AUDIO_APIS
            ):
                errors.append(f"{ctx}: audio.api invalid")

        ev_req = row.get("evidenceRequired")
        if not isinstance(ev_req, list) or len(ev_req) == 0:
            errors.append(f"{ctx}: evidenceRequired must be a non-empty list")
        else:
            bad = [e for e in ev_req if e not in ALL_PROBE_CATEGORIES]
            if bad:
                errors.append(f"{ctx}: unknown evidence categories: {bad}")
            if len(set(ev_req)) != len(ev_req):
                errors.append(f"{ctx}: evidenceRequired has duplicates")

    # Canonical profile coverage
    if isinstance(profile, str) and profile in CANONICAL_PROFILES:
        canonical = CANONICAL_PROFILES[profile]
        req_rows = canonical["required_rows"]
        for req_id, req_spec in req_rows.items():
            if req_id not in seen_ids:
                errors.append(
                    f"Profile {profile!r}: missing required row {req_id!r}"
                )
            else:
                actual = next(r for r in rows if r.get("id") == req_id)
                if actual.get("tier") != req_spec["tier"]:
                    errors.append(
                        f"Profile {profile!r}: row {req_id!r} tier must be "
                        f"{req_spec['tier']!r}, got {actual.get('tier')!r}"
                    )
                actual_ev = set(actual.get("evidenceRequired", []))
                if actual_ev != req_spec["evidence_required"]:
                    errors.append(
                        f"Profile {profile!r}: row {req_id!r} evidenceRequired "
                        f"does not match canonical profile"
                    )
        for row in rows:
            rid = row.get("id", "")
            if row.get("tier") in CERTIFIABLE_TIERS and rid not in req_rows:
                errors.append(
                    f"Profile {profile!r}: unexpected certifiable row {rid!r}"
                )

    return errors


# ── Evidence validation ───────────────────────────────────────────────────


def validate_evidence(
    evidence: dict[str, Any], *, now: datetime | None = None
) -> list[str]:
    """Validate a single evidence record. Returns list of errors."""
    errors: list[str] = []
    if not isinstance(evidence, dict):
        return ["Evidence must be a JSON object"]
    if now is None:
        now = datetime.now(timezone.utc)

    _check_no_nonfinite(evidence, "$", errors)
    _check_string_lengths(evidence, "$", errors)
    if errors:
        return errors

    _check_extra_keys(evidence, _EV_KEYS, "evidence", errors)

    if evidence.get("schemaVersion") != SCHEMA_VERSION:
        errors.append(f"Evidence schemaVersion must be {SCHEMA_VERSION}")

    row_id = evidence.get("rowId", "")
    if not isinstance(row_id, str) or not _ROW_ID_RE.match(row_id):
        errors.append("Evidence rowId must match ^[a-z0-9][a-z0-9._-]*$")

    _check_sha1(evidence.get("commitSha"), "evidence.commitSha", errors)
    _parse_bounded_ts(
        evidence.get("collectedAt"),
        now=now,
        context="evidence.collectedAt",
        errors=errors,
    )

    stale = evidence.get("staleBefore")
    if stale is not None:
        _parse_bounded_ts(
            stale, now=now, context="evidence.staleBefore", errors=errors
        )

    collector = evidence.get("collector")
    if not isinstance(collector, dict):
        errors.append("Evidence must have a 'collector' object")
    else:
        _check_extra_keys(collector, _EV_COLLECTOR_KEYS, "collector", errors)
        if collector.get("type") not in VALID_COLLECTOR_TYPES:
            errors.append(f"collector.type={collector.get('type')!r} invalid")
        identity = collector.get("identity", "")
        if not isinstance(identity, str) or not identity.strip():
            errors.append("collector.identity is required (non-blank)")

    host = evidence.get("host")
    if not isinstance(host, dict):
        errors.append("Evidence must have a 'host' object")
    else:
        _validate_host(host, errors)

    probes = evidence.get("probes")
    if not isinstance(probes, dict):
        errors.append("Evidence must have a 'probes' object")
    else:
        _check_extra_keys(probes, ALL_PROBE_CATEGORIES, "probes", errors)
        for name, probe in probes.items():
            if name not in ALL_PROBE_CATEGORIES:
                continue
            _validate_probe(name, probe, errors)

    deps = evidence.get("dependencyClosure")
    if deps is not None:
        if not isinstance(deps, list):
            errors.append("dependencyClosure must be a list")
        else:
            if len(deps) > MAX_DEPENDENCIES:
                errors.append(
                    f"dependencyClosure has {len(deps)} entries "
                    f"(max {MAX_DEPENDENCIES})"
                )
            for i, dep in enumerate(deps):
                if not isinstance(dep, dict):
                    errors.append(f"dependencyClosure[{i}] must be an object")
                    continue
                _check_extra_keys(
                    dep, _EV_DEP_KEYS, f"dependencyClosure[{i}]", errors
                )
                for f in ("name", "version", "source"):
                    if not isinstance(dep.get(f), str) or not dep[f]:
                        errors.append(
                            f"dependencyClosure[{i}].{f} is required"
                        )
                if dep.get("source") and dep["source"] not in VALID_DEP_SOURCES:
                    errors.append(
                        f"dependencyClosure[{i}].source={dep['source']!r} invalid"
                    )

    return errors


def _validate_host(host: dict[str, Any], errors: list[str]) -> None:
    _check_extra_keys(host, _EV_HOST_KEYS, "host", errors)

    os_info = host.get("os")
    if isinstance(os_info, dict):
        _check_extra_keys(os_info, _EV_HOST_OS_KEYS, "host.os", errors)
        for f in ("family", "version", "build", "locale"):
            if not isinstance(os_info.get(f), str) or not os_info[f]:
                errors.append(f"host.os.{f} is required")
        if os_info.get("family") and os_info["family"] not in VALID_OS_FAMILIES:
            errors.append(f"host.os.family={os_info['family']!r} invalid")
    else:
        errors.append("host.os must be an object")

    if host.get("arch") not in VALID_ARCHES:
        errors.append(f"host.arch={host.get('arch')!r} invalid")

    cpu = host.get("cpu")
    if isinstance(cpu, dict):
        _check_extra_keys(cpu, _EV_HOST_CPU_KEYS, "host.cpu", errors)
        if not isinstance(cpu.get("model"), str) or not cpu["model"]:
            errors.append("host.cpu.model is required")
        if not isinstance(cpu.get("features"), list):
            errors.append("host.cpu.features must be a list")
    else:
        errors.append("host.cpu must be an object")

    ram = host.get("ram")
    if isinstance(ram, dict):
        _check_extra_keys(ram, _EV_HOST_RAM_KEYS, "host.ram", errors)
        if not isinstance(ram.get("totalMb"), int) or ram["totalMb"] < 1:
            errors.append("host.ram.totalMb must be a positive integer")
    else:
        errors.append("host.ram must be an object")

    gpu = host.get("gpu")
    if isinstance(gpu, dict):
        _check_extra_keys(gpu, _EV_HOST_GPU_KEYS, "host.gpu", errors)
        for f in ("api", "device", "vendor", "driverVersion", "featureLevel"):
            if not isinstance(gpu.get(f), str) or not gpu[f]:
                errors.append(f"host.gpu.{f} is required")
        if gpu.get("api") and gpu["api"] not in VALID_GPU_APIS:
            errors.append(f"host.gpu.api={gpu['api']!r} invalid")
        if gpu.get("vendor") and gpu["vendor"] not in VALID_GPU_VENDORS:
            errors.append(f"host.gpu.vendor={gpu['vendor']!r} invalid")
    else:
        errors.append("host.gpu must be an object")

    audio = host.get("audio")
    if isinstance(audio, dict):
        _check_extra_keys(audio, _EV_HOST_AUDIO_KEYS, "host.audio", errors)
        if (
            not isinstance(audio.get("api"), str)
            or audio["api"] not in VALID_AUDIO_APIS
        ):
            errors.append("host.audio.api invalid")
    else:
        errors.append("host.audio must be an object")

    compiler = host.get("compiler")
    if isinstance(compiler, dict):
        _check_extra_keys(
            compiler, _EV_HOST_COMPILER_KEYS, "host.compiler", errors
        )
        for f in ("id", "version", "toolset"):
            if not isinstance(compiler.get(f), str) or not compiler[f]:
                errors.append(f"host.compiler.{f} is required")
        if compiler.get("id") and compiler["id"] not in VALID_COMPILER_IDS:
            errors.append(f"host.compiler.id={compiler['id']!r} invalid")
    else:
        errors.append("host.compiler is required")


def _validate_probe(name: str, probe: Any, errors: list[str]) -> None:
    if not isinstance(probe, dict):
        errors.append(f"probes.{name} must be an object")
        return
    _check_extra_keys(probe, _EV_PROBE_KEYS, f"probes.{name}", errors)
    status = probe.get("status")
    if status not in VALID_PROBE_STATUSES:
        errors.append(f"probes.{name}.status={status!r} invalid")
    dur = probe.get("durationMs")
    if not isinstance(dur, int) or dur < 0:
        errors.append(
            f"probes.{name}.durationMs must be a non-negative integer"
        )
    elif status == "pass" and dur < MIN_PASS_DURATION_MS:
        errors.append(
            f"probes.{name}.durationMs={dur} too low for pass "
            f"(minimum {MIN_PASS_DURATION_MS}ms)"
        )
    if status == "skip" and not probe.get("skipReason"):
        errors.append(f"probes.{name}: status=skip requires skipReason")
    artifacts = probe.get("artifacts")
    if artifacts is not None:
        if not isinstance(artifacts, list):
            errors.append(f"probes.{name}.artifacts must be a list")
        else:
            if len(artifacts) > MAX_ARTIFACTS_PER_PROBE:
                errors.append(
                    f"probes.{name}.artifacts has {len(artifacts)} "
                    f"(max {MAX_ARTIFACTS_PER_PROBE})"
                )
            for j, art in enumerate(artifacts):
                _check_artifact(
                    art, f"probes.{name}.artifacts[{j}]", errors
                )


# ── Cross-validation ──────────────────────────────────────────────────────


def cross_validate(
    matrix: dict[str, Any],
    evidence_records: dict[str, dict[str, Any]],
    *,
    max_age_hours: float = 168,
    now: datetime | None = None,
) -> ValidationResult:
    """Cross-validate matrix rows against evidence records."""
    result = ValidationResult()
    if now is None:
        now = datetime.now(timezone.utc)

    matrix_commit = matrix.get("commitSha", "")

    for row in matrix.get("rows", []):
        row_id = row.get("id", "<unknown>")
        tier = row.get("tier", "")
        result.rows_checked += 1

        if tier not in CERTIFIABLE_TIERS:
            result.warn(
                f"[{row_id}] tier={tier!r} is not certifiable, skipping"
            )
            continue

        evidence = evidence_records.get(row_id)
        if evidence is None:
            result.error(
                f"[{row_id}] No evidence record found -- row cannot certify"
            )
            result.rows_failed += 1
            continue

        row_errors: list[str] = []

        ev_commit = evidence.get("commitSha", "")
        if ev_commit != matrix_commit:
            row_errors.append(
                f"Commit mismatch: matrix={matrix_commit[:8]} "
                f"evidence={ev_commit[:8]}"
            )

        collected_at_str = evidence.get("collectedAt", "")
        try:
            collected_at = datetime.fromisoformat(collected_at_str)
            if collected_at.tzinfo is None:
                collected_at = collected_at.replace(tzinfo=timezone.utc)
        except (ValueError, TypeError):
            row_errors.append(f"Invalid collectedAt: {collected_at_str!r}")
            collected_at = None

        if collected_at is not None:
            if collected_at > now + timedelta(seconds=CLOCK_SKEW_SECONDS):
                row_errors.append("Evidence timestamp is in the future")
            age_hours = (now - collected_at).total_seconds() / 3600
            if age_hours > max_age_hours:
                row_errors.append(
                    f"Evidence is {age_hours:.0f}h old "
                    f"(max {max_age_hours:.0f}h)"
                )
            stale_before = evidence.get("staleBefore")
            if stale_before:
                try:
                    stale_dt = datetime.fromisoformat(stale_before)
                    if stale_dt.tzinfo is None:
                        stale_dt = stale_dt.replace(tzinfo=timezone.utc)
                    if collected_at < stale_dt:
                        row_errors.append(
                            "Evidence collected before staleBefore threshold"
                        )
                except (ValueError, TypeError):
                    row_errors.append(
                        f"Invalid staleBefore: {stale_before!r}"
                    )

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

        if "dependency_closure" in required:
            deps = evidence.get("dependencyClosure")
            if not isinstance(deps, list) or len(deps) == 0:
                row_errors.append(
                    "dependency_closure is in evidenceRequired but "
                    "dependencyClosure is missing or empty"
                )

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
    for field in ("family", "version", "build"):
        rv = row_os.get(field)
        hv = host_os.get(field)
        if rv and hv != rv:
            errors.append(f"OS {field} mismatch: row={rv!r} host={hv!r}")

    if row.get("arch") and host.get("arch") != row["arch"]:
        errors.append(
            f"Arch mismatch: row={row['arch']!r} host={host.get('arch')!r}"
        )

    row_compiler = row.get("compiler", {})
    host_compiler = host.get("compiler")
    if not isinstance(host_compiler, dict):
        errors.append("Evidence host missing required compiler section")
    else:
        for field in ("id", "version", "toolset"):
            rv = row_compiler.get(field)
            hv = host_compiler.get(field)
            if rv and hv != rv:
                errors.append(
                    f"Compiler {field} mismatch: row={rv!r} host={hv!r}"
                )

    row_gpu = row.get("gpu", {})
    host_gpu = host.get("gpu", {})
    for field in ("api", "device", "vendor", "driverVersion", "featureLevel"):
        rv = row_gpu.get(field)
        hv = host_gpu.get(field)
        if rv and hv != rv:
            errors.append(f"GPU {field} mismatch: row={rv!r} host={hv!r}")

    row_audio = row.get("audio", {})
    host_audio = host.get("audio", {})
    if row_audio.get("api") and host_audio.get("api") != row_audio["api"]:
        errors.append(
            f"Audio API mismatch: row={row_audio['api']!r} "
            f"host={host_audio.get('api')!r}"
        )

    row_features = set(row.get("cpuFeatures", []))
    host_features = set(host.get("cpu", {}).get("features", []))
    missing = row_features - host_features
    if missing:
        errors.append(f"Host missing required CPU features: {sorted(missing)}")


# ── File-based pipeline ───────────────────────────────────────────────────


def load_and_validate(
    matrix_path: Path,
    evidence_dir: Path,
    *,
    max_age_hours: float = 168,
) -> ValidationResult:
    """Load matrix + evidence files and run full validation."""
    result = ValidationResult()

    matrix = _load_strict_json(matrix_path)
    matrix_errors = validate_matrix(matrix)
    if matrix_errors:
        for e in matrix_errors:
            result.error(f"[matrix] {e}")
        return result

    evidence_records: dict[str, dict[str, Any]] = {}
    if evidence_dir.is_dir():
        ev_files = sorted(evidence_dir.glob("*.json"))
        if len(ev_files) > MAX_COLLECTIONS:
            result.error(
                f"Too many evidence files: {len(ev_files)} "
                f"(max {MAX_COLLECTIONS})"
            )
            return result
        for path in ev_files:
            ev = _load_strict_json(path)
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


# ── CLI ───────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(
        description="Validate platform certification evidence (PLT-200)"
    )
    parser.add_argument(
        "--matrix",
        type=Path,
        default=REPO_ROOT / "docs" / "certification" / "support-matrix.json",
    )
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        default=REPO_ROOT / "docs" / "certification" / "evidence",
    )
    parser.add_argument("--max-age-hours", type=float, default=168)
    parser.add_argument("--matrix-only", action="store_true")

    args = parser.parse_args(argv)

    try:
        if args.matrix_only:
            matrix = _load_strict_json(args.matrix)
            errors = validate_matrix(matrix)
            if errors:
                for e in errors:
                    print(f"  X {e}", file=sys.stderr)
                print(
                    f"FAIL: {len(errors)} matrix error(s)", file=sys.stderr
                )
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
