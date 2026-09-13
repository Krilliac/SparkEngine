#!/usr/bin/env python3
"""Fail-closed validator for SparkEngine platform certification evidence (PLT-200).

An evidence record is hostile input.  It arrives as JSON, it asserts what
hardware ran, what commit was tested, who collected it, which files it
produced and what those files contain -- and every one of those assertions is
worthless until something outside the record confirms it.  This validator is
that something.

What is *measured* rather than believed:

  - the exact revision under test is supplied on the command line (or from the
    CI environment), never read out of the record and never inferred
  - the collector's identity, repository, workflow, run id, run attempt and
    job come from the same trusted channel; a record cannot self-attest
  - every declared artifact and dependency file is opened beneath a confined
    per-row root and hashed; declared size and digest must match the measured
    ones, and no undeclared file may sit in the bundle (see bundle_verify)
  - the dependency closure is matched against the committed dependency
    authority derived from ThirdParty/dependencies.lock, so a fabricated but
    well-formed list fails (see dependency_authority)
  - document structure comes from the committed JSON Schemas, enforced by a
    validator that refuses any keyword it does not implement, so a schema
    rule can never be silently skipped (see schema_validator)

What is *bounded*: max age, document size, nesting, string lengths, row
counts, artifact counts, bundle bytes, evidence-file counts.  A check with no
bound is a check that stops checking under load.

If this program prints PASS the row is certifiable at the supplied commit.
Anything else -- failure, error, or an unexpected exception -- is not.
"""

from __future__ import annotations

import json
import math
import os
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

import bundle_verify
import dependency_authority as da
import safe_fs
from schema_validator import CompiledSchema, SchemaError, cross_check_with_jsonschema

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]
SCHEMA_VERSION = 1

# ── Resource limits ────────────────────────────────────────────────────────
MAX_DOCUMENT_BYTES = 2 * 1024 * 1024
MAX_JSON_DEPTH = 24
MAX_MATRIX_ROWS = 200
MAX_ARTIFACTS_PER_PROBE = 100
MAX_STRING_LENGTH = 4000
MAX_EVIDENCE_FILES = 200
MIN_TIMESTAMP = datetime(2020, 1, 1, tzinfo=timezone.utc)
CLOCK_SKEW_SECONDS = 300
MIN_PASS_DURATION_MS = 1

# An age bound outside this range is a configuration error, not a policy.
MIN_MAX_AGE_HOURS = 1.0 / 3600.0  # one second
MAX_MAX_AGE_HOURS = 24.0 * 366.0  # one year
DEFAULT_MAX_AGE_HOURS = 168.0

# Wall-clock and reported duration must agree to within the larger of one
# second and 5%, which absorbs timer granularity without absorbing a lie.
DURATION_TOLERANCE_MS = 1000
DURATION_TOLERANCE_FRACTION = 0.05

# ── Patterns ───────────────────────────────────────────────────────────────
_SHA1_RE = re.compile(r"\A[0-9a-f]{40}\Z")
_SHA256_RE = re.compile(r"\A[0-9a-f]{64}\Z")
_ROW_ID_RE = re.compile(r"\A[a-z0-9][a-z0-9._-]{0,63}\Z")
_PROFILE_RE = re.compile(r"\A[a-z][a-z0-9-]{0,63}\Z")
_REPOSITORY_RE = re.compile(r"\A[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\Z")
_RUN_ID_RE = re.compile(r"\A[0-9]{1,20}\Z")

VALID_COLLECTOR_TYPES = ("ci", "manual", "automated")

# The only attestation shape this validator knows how to check.  An unknown
# predicate would be an attestation whose meaning we are guessing at.
ATTESTATION_PREDICATE = "https://sparkengine.dev/attestation/certification/v1"


class CertificationError(Exception):
    """A validation failure that makes certification impossible."""


# ── Schema loading (eager, fail-closed) ────────────────────────────────────


def _load_schema_file(path: Path) -> dict[str, Any]:
    try:
        raw = safe_fs.read_bounded(path, max_bytes=MAX_DOCUMENT_BYTES)
    except safe_fs.FileSecurityError as exc:
        raise RuntimeError(f"Required schema file unusable: {path}: {exc}") from exc
    return json.loads(raw.decode("utf-8"))


_EV_SCHEMA = _load_schema_file(TOOLS_DIR / "evidence_schema.json")
_MX_SCHEMA = _load_schema_file(TOOLS_DIR / "support_matrix_schema.json")

try:
    EVIDENCE_SCHEMA = CompiledSchema(_EV_SCHEMA)
    MATRIX_SCHEMA = CompiledSchema(_MX_SCHEMA)
except SchemaError as exc:  # pragma: no cover - a broken schema must not load
    raise RuntimeError(f"Certification schema is not enforceable: {exc}") from exc

_ev_host = _EV_SCHEMA["properties"]["host"]["properties"]
_ev_defs = _EV_SCHEMA["$defs"]
_mx_defs = _MX_SCHEMA["$defs"]

ALL_PROBE_CATEGORIES = frozenset(_EV_SCHEMA["properties"]["probes"]["properties"])
VALID_OS_FAMILIES = frozenset(_ev_host["os"]["properties"]["family"]["enum"])
VALID_ARCHES = frozenset(_ev_host["arch"]["enum"])
VALID_GPU_APIS = frozenset(_ev_host["gpu"]["properties"]["api"]["enum"])
VALID_GPU_VENDORS = frozenset(_ev_host["gpu"]["properties"]["vendor"]["enum"])
VALID_AUDIO_APIS = frozenset(_ev_host["audio"]["properties"]["api"]["enum"])
VALID_COMPILER_IDS = frozenset(_ev_host["compiler"]["properties"]["id"]["enum"])
VALID_PROBE_STATUSES = frozenset(_ev_defs["Probe"]["properties"]["status"]["enum"])
VALID_DEP_SOURCES = frozenset(_ev_defs["RuntimeDependency"]["properties"]["source"]["enum"])
VALID_TIERS = frozenset(_mx_defs["SupportRow"]["properties"]["tier"]["enum"])
CERTIFIABLE_TIERS = frozenset(["primary", "supported"])

# The two schemas describe the same world; a disagreement between them is a
# hole big enough to drive a row through.
for _name, _left, _right in [
    ("os_families", VALID_OS_FAMILIES, frozenset(_mx_defs["OsSpec"]["properties"]["family"]["enum"])),
    ("arches", VALID_ARCHES, frozenset(_mx_defs["SupportRow"]["properties"]["arch"]["enum"])),
    ("gpu_apis", VALID_GPU_APIS, frozenset(_mx_defs["GpuSpec"]["properties"]["api"]["enum"])),
    ("gpu_vendors", VALID_GPU_VENDORS, frozenset(_mx_defs["GpuSpec"]["properties"]["vendor"]["enum"])),
    ("audio_apis", VALID_AUDIO_APIS, frozenset(_mx_defs["AudioSpec"]["properties"]["api"]["enum"])),
    ("compiler_ids", VALID_COMPILER_IDS, frozenset(_mx_defs["CompilerSpec"]["properties"]["id"]["enum"])),
    (
        "probe_categories",
        ALL_PROBE_CATEGORIES,
        frozenset(_mx_defs["SupportRow"]["properties"]["evidenceRequired"]["items"]["enum"]),
    ),
    (
        "dependency_sources",
        VALID_DEP_SOURCES,
        frozenset(da.VALID_SOURCES),
    ),
]:
    if _left != _right:
        raise RuntimeError(f"Schema enum mismatch: {_name}")

# ── Canonical profiles ─────────────────────────────────────────────────────
# What stable-v1 *means*, independent of any matrix file.  Hardware-specific
# strings stay editable; the invariants that decide how much evidence a row
# owes do not.
CANONICAL_PROFILES: dict[str, dict[str, dict[str, Any]]] = {
    "stable-v1": {
        "win11-x64-msvc143-d3d11": {
            "tier": "primary",
            "evidence_required": ALL_PROBE_CATEGORIES,
            "os_family": "windows",
            "arch": "x86_64",
            "compiler_id": "msvc",
            "compiler_toolset": "v143",
            "require_windows_sdk": True,
            "gpu_api": "d3d11",
            "gpu_vendor_not": {"none"},
            "audio_api": "xaudio2",
            "require_audio_version": True,
            "min_cpu_features": frozenset({"SSE4.2", "AVX2"}),
            "patterns": {
                "os.version": r"\A11\Z",
                "os.build": r"\A10\.0\.[0-9]{5,6}\Z",
                "compiler.version": r"\A19\.[0-9]{2}\.[0-9]{4,6}\Z",
                "compiler.windowsSdkVersion": r"\A10\.0\.[0-9]{5}\.[0-9]\Z",
                "gpu.driverVersion": r"\A[0-9]+(\.[0-9]+){1,3}\Z",
                "gpu.featureLevel": r"\A(11\.[01]|12\.[012])\Z",
                "audio.version": r"\A2\.[0-9]\Z",
            },
        },
        "win11-x64-msvc143-nullrhi": {
            "tier": "supported",
            "evidence_required": ALL_PROBE_CATEGORIES
            - {"renderer", "content", "input", "audio"},
            "os_family": "windows",
            "arch": "x86_64",
            "compiler_id": "msvc",
            "compiler_toolset": "v143",
            "require_windows_sdk": True,
            "gpu_api": "nullrhi",
            "gpu_vendor_is": "none",
            "audio_api": "none",
            "require_audio_version": False,
            "min_cpu_features": frozenset({"SSE4.2", "AVX2"}),
            "patterns": {
                "os.version": r"\A11\Z",
                "os.build": r"\A10\.0\.[0-9]{5,6}\Z",
                "compiler.version": r"\A19\.[0-9]{2}\.[0-9]{4,6}\Z",
                "compiler.windowsSdkVersion": r"\A10\.0\.[0-9]{5}\.[0-9]\Z",
                "gpu.driverVersion": r"\Anone\Z",
                "gpu.featureLevel": r"\Anull\Z",
            },
        },
    },
}


# ── Bounded numeric policy inputs ──────────────────────────────────────────


def validate_max_age_hours(value: Any, *, source: str = "max_age_hours") -> float:
    """Return a usable age bound or raise.

    NaN is the reason this function exists.  `age > float('nan')` is False, so
    a NaN bound silently certifies evidence of *any* age -- a check that has
    stopped checking while reporting the reassuring answer.  Booleans are
    rejected because `True` is an `int` in Python and would quietly mean one
    hour.
    """
    if isinstance(value, bool):
        raise CertificationError(f"{source}: must be a number, got a boolean")
    if isinstance(value, str):
        raise CertificationError(f"{source}: must be a number, got a string")
    if not isinstance(value, (int, float)):
        raise CertificationError(
            f"{source}: must be a number, got {type(value).__name__}"
        )
    number = float(value)
    if math.isnan(number):
        raise CertificationError(f"{source}: must be a finite number, got NaN")
    if math.isinf(number):
        raise CertificationError(f"{source}: must be a finite number, got {number}")
    if number <= 0:
        raise CertificationError(f"{source}: must be greater than zero, got {number}")
    if number < MIN_MAX_AGE_HOURS:
        raise CertificationError(
            f"{source}: {number} is below the minimum {MIN_MAX_AGE_HOURS}"
        )
    if number > MAX_MAX_AGE_HOURS:
        raise CertificationError(
            f"{source}: {number} exceeds the maximum {MAX_MAX_AGE_HOURS}"
        )
    return number


def cli_max_age_hours(text: str) -> float:
    """argparse type for --max-age-hours; rejects nan/inf/negative textually.

    Raises ArgumentTypeError so argparse turns the refusal into a usage error
    and a non-zero exit rather than an unhandled traceback.  `float("nan")`
    parses happily, so the textual gate alone is not enough -- the value goes
    through the same bound check the API uses.
    """
    import argparse as _argparse

    try:
        number = float(text)
    except (TypeError, ValueError):
        raise _argparse.ArgumentTypeError(
            f"--max-age-hours: {text!r} is not a number"
        ) from None
    try:
        return validate_max_age_hours(number, source="--max-age-hours")
    except CertificationError as exc:
        raise _argparse.ArgumentTypeError(str(exc)) from exc


NULL_COMMIT_SHA = "0" * 40


def validate_commit_sha(value: Any, *, source: str) -> str:
    """A trusted revision must be a real one.

    The support matrix ships an all-zero SHA as its unstamped placeholder, so
    that value must never be accepted as the commit under validation -- doing
    so would let the placeholder match itself and certify nothing into
    something.
    """
    if not isinstance(value, str) or _SHA1_RE.match(value) is None:
        raise CertificationError(
            f"{source}: must be exactly 40 lowercase hex characters, got {value!r}"
        )
    if value == NULL_COMMIT_SHA:
        raise CertificationError(
            f"{source}: the all-zero SHA is the unstamped placeholder, not a revision"
        )
    return value


# ── Trusted, externally supplied identity ──────────────────────────────────


@dataclass(frozen=True)
class TrustedContext:
    """Everything a record is forbidden to assert about itself."""

    commit_sha: str
    collector_type: str
    identity: str
    repository: str | None = None
    workflow: str | None = None
    run_id: str | None = None
    run_attempt: int | None = None
    job_id: str | None = None

    def __post_init__(self) -> None:
        validate_commit_sha(self.commit_sha, source="--commit")
        if self.collector_type not in VALID_COLLECTOR_TYPES:
            raise CertificationError(
                f"--expect-collector-type: must be one of {list(VALID_COLLECTOR_TYPES)}"
            )
        if not isinstance(self.identity, str) or not (8 <= len(self.identity) <= 512):
            raise CertificationError(
                "--expect-identity: must be 8 to 512 characters of trusted text"
            )
        ci_fields = {
            "--expect-repository": self.repository,
            "--expect-workflow": self.workflow,
            "--expect-run-id": self.run_id,
            "--expect-run-attempt": self.run_attempt,
            "--expect-job": self.job_id,
        }
        if self.collector_type == "ci":
            missing = sorted(k for k, v in ci_fields.items() if v in (None, ""))
            if missing:
                raise CertificationError(
                    f"collector type 'ci' requires {missing}"
                )
            if _REPOSITORY_RE.match(str(self.repository)) is None:
                raise CertificationError("--expect-repository: must be owner/name")
            if _RUN_ID_RE.match(str(self.run_id)) is None:
                raise CertificationError("--expect-run-id: must be 1-20 digits")
            if (
                isinstance(self.run_attempt, bool)
                or not isinstance(self.run_attempt, int)
                or not 1 <= self.run_attempt <= 1000
            ):
                raise CertificationError("--expect-run-attempt: must be 1..1000")
            for label, value in (("--expect-workflow", self.workflow), ("--expect-job", self.job_id)):
                if not isinstance(value, str) or not 1 <= len(value) <= 200:
                    raise CertificationError(f"{label}: must be 1 to 200 characters")
        else:
            supplied = sorted(k for k, v in ci_fields.items() if v not in (None, ""))
            if supplied:
                raise CertificationError(
                    f"collector type {self.collector_type!r} must not supply {supplied}"
                )

    @classmethod
    def from_github_env(
        cls, env: dict[str, str], *, identity: str | None = None
    ) -> TrustedContext:
        """Build a CI context from the runner's own environment variables."""
        missing = [
            name
            for name in (
                "GITHUB_SHA",
                "GITHUB_REPOSITORY",
                "GITHUB_WORKFLOW",
                "GITHUB_RUN_ID",
                "GITHUB_RUN_ATTEMPT",
                "GITHUB_JOB",
            )
            if not env.get(name)
        ]
        if missing:
            raise CertificationError(
                f"--from-github-env: environment is missing {missing}"
            )
        try:
            attempt = int(env["GITHUB_RUN_ATTEMPT"])
        except ValueError:
            raise CertificationError(
                "--from-github-env: GITHUB_RUN_ATTEMPT is not an integer"
            ) from None
        server = env.get("GITHUB_SERVER_URL", "https://github.com").rstrip("/")
        resolved = identity or (
            f"{server}/{env['GITHUB_REPOSITORY']}/actions/runs/"
            f"{env['GITHUB_RUN_ID']}/attempts/{attempt}"
        )
        return cls(
            commit_sha=env["GITHUB_SHA"],
            collector_type="ci",
            identity=resolved,
            repository=env["GITHUB_REPOSITORY"],
            workflow=env["GITHUB_WORKFLOW"],
            run_id=env["GITHUB_RUN_ID"],
            run_attempt=attempt,
            job_id=env["GITHUB_JOB"],
        )


# ── Result accumulation ────────────────────────────────────────────────────


class ValidationResult:
    """Accumulates pass/fail verdicts per row.

    A result without trusted context is useful for inspection only and cannot
    claim certification.
    """

    def __init__(self, *, trusted_context: TrustedContext | None = None) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.structural_errors: list[str] = []
        self.trusted_context = trusted_context
        self.rows_checked = 0
        self.rows_certified = 0
        self.rows_failed = 0

    def error(self, message: str) -> None:
        self.errors.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    @property
    def ok(self) -> bool:
        return not self.errors and self.rows_checked > 0

    @property
    def fully_certified(self) -> bool:
        """True only when every certifiable row passed with trusted context."""
        return (
            self.trusted_context is not None
            and self.ok
            and self.rows_certified > 0
            and self.rows_failed == 0
        )

    def summary(self) -> str:
        lines: list[str] = []
        if self.errors:
            lines.append(f"FAIL: {len(self.errors)} error(s)")
            lines.extend(f"  X {message}" for message in self.errors)
        if self.warnings:
            lines.append(f"WARN: {len(self.warnings)} warning(s)")
            lines.extend(f"  ! {message}" for message in self.warnings)
        lines.append(
            f"Rows: {self.rows_checked} checked, "
            f"{self.rows_certified} certified, {self.rows_failed} failed"
        )
        if self.ok and self.rows_checked > 0:
            lines.insert(0, "PASS: All rows validated")
        return "\n".join(lines)


# ── Strict JSON parsing ────────────────────────────────────────────────────


def parse_strict_json(text: str, source: str = "<input>") -> Any:
    """Parse JSON rejecting duplicate keys, NaN/Infinity, and deep nesting."""

    def _reject_dups(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        obj: dict[str, Any] = {}
        for key, value in pairs:
            if key in obj:
                raise ValueError(f"Duplicate JSON key: {key!r}")
            obj[key] = value
        return obj

    def _reject_const(constant: str) -> None:
        raise ValueError(f"Non-finite JSON constant: {constant!r}")

    try:
        document = json.loads(
            text, object_pairs_hook=_reject_dups, parse_constant=_reject_const
        )
    except json.JSONDecodeError as exc:
        raise CertificationError(
            f"Invalid JSON in {source}: line {exc.lineno} col {exc.colno}: {exc.msg}"
        ) from exc
    except RecursionError as exc:
        raise CertificationError(f"Invalid JSON in {source}: nesting too deep") from exc
    except ValueError as exc:
        raise CertificationError(f"Invalid JSON in {source}: {exc}") from exc

    depth = _measure_depth(document, 0)
    if depth > MAX_JSON_DEPTH:
        raise CertificationError(
            f"Invalid JSON in {source}: nested {depth} deep (max {MAX_JSON_DEPTH})"
        )
    return document


def _measure_depth(value: Any, depth: int) -> int:
    if depth > MAX_JSON_DEPTH:
        return depth
    if isinstance(value, dict):
        return max((_measure_depth(v, depth + 1) for v in value.values()), default=depth)
    if isinstance(value, list):
        return max((_measure_depth(v, depth + 1) for v in value), default=depth)
    return depth


def load_strict_json(path: Path) -> Any:
    """Load a JSON file through the hardened filesystem layer."""
    try:
        raw = safe_fs.read_bounded(path, max_bytes=MAX_DOCUMENT_BYTES)
    except safe_fs.FileSecurityError as exc:
        raise CertificationError(f"cannot read {path}: {exc}") from exc
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise CertificationError(f"{path.name}: not valid UTF-8: {exc}") from exc
    if text.startswith("﻿"):
        raise CertificationError(f"{path.name}: byte-order mark is not allowed")
    return parse_strict_json(text, path.name)


# ── Shared helpers ─────────────────────────────────────────────────────────


def _check_no_nonfinite(obj: Any, path: str, errors: list[str]) -> None:
    if isinstance(obj, float) and not math.isfinite(obj):
        errors.append(f"{path}: non-finite numeric value")
    elif isinstance(obj, dict):
        for key, value in obj.items():
            _check_no_nonfinite(value, f"{path}.{key}", errors)
    elif isinstance(obj, list):
        for index, value in enumerate(obj):
            _check_no_nonfinite(value, f"{path}[{index}]", errors)


def _check_string_lengths(obj: Any, path: str, errors: list[str]) -> None:
    if isinstance(obj, str) and len(obj) > MAX_STRING_LENGTH:
        errors.append(f"{path}: string too long ({len(obj)} > {MAX_STRING_LENGTH})")
    elif isinstance(obj, dict):
        for key, value in obj.items():
            _check_string_lengths(value, f"{path}.{key}", errors)
    elif isinstance(obj, list):
        for index, value in enumerate(obj):
            _check_string_lengths(value, f"{path}[{index}]", errors)


def parse_bounded_ts(
    value: Any, *, now: datetime, context: str, errors: list[str]
) -> datetime | None:
    """Parse an RFC 3339 timestamp, rejecting anything absurd or unbounded."""
    if not isinstance(value, str) or not value:
        errors.append(f"{context}: missing or invalid timestamp")
        return None
    text = value[:-1] + "+00:00" if value.endswith(("Z", "z")) else value
    try:
        parsed = datetime.fromisoformat(text)
    except (ValueError, TypeError):
        errors.append(f"{context}: invalid ISO 8601 timestamp: {value!r}")
        return None
    if parsed.tzinfo is None:
        errors.append(f"{context}: timestamp needs an explicit UTC offset: {value!r}")
        return None
    if parsed < MIN_TIMESTAMP:
        errors.append(f"{context}: timestamp too old: {value!r}")
        return None
    if parsed > now + timedelta(seconds=CLOCK_SKEW_SECONDS):
        errors.append(f"{context}: timestamp in the future: {value!r}")
        return None
    return parsed


def _schema_errors(schema: CompiledSchema, raw: dict[str, Any], document: Any) -> list[str]:
    errors = schema.validate(document)
    errors.extend(cross_check_with_jsonschema(raw, document, errors))
    return errors


# ── Matrix validation ──────────────────────────────────────────────────────


def validate_matrix(
    matrix: Any,
    *,
    now: datetime | None = None,
    allow_unknown_profile: bool = False,
) -> list[str]:
    """Validate support-matrix structure and canonical-profile content."""
    if not isinstance(matrix, dict):
        return ["Matrix must be a JSON object"]
    now = now or datetime.now(timezone.utc)

    errors: list[str] = []
    _check_no_nonfinite(matrix, "$", errors)
    _check_string_lengths(matrix, "$", errors)
    if errors:
        return errors

    errors = _schema_errors(MATRIX_SCHEMA, _MX_SCHEMA, matrix)
    if errors:
        return [f"schema: {message}" for message in errors]

    if matrix["schemaVersion"] != SCHEMA_VERSION:
        errors.append(f"Matrix schemaVersion must be {SCHEMA_VERSION}")
    if _PROFILE_RE.match(matrix["profile"]) is None:
        errors.append("Matrix profile must match ^[a-z][a-z0-9-]{0,63}$")
    parse_bounded_ts(
        matrix["generatedAt"], now=now, context="matrix.generatedAt", errors=errors
    )

    rows = matrix["rows"]
    if len(rows) > MAX_MATRIX_ROWS:
        errors.append(f"Matrix has {len(rows)} rows (max {MAX_MATRIX_ROWS})")
        return errors

    by_id: dict[str, dict[str, Any]] = {}
    for index, row in enumerate(rows):
        row_id = row["id"]
        if _ROW_ID_RE.match(row_id) is None:
            errors.append(f"rows[{index}]: id must match {_ROW_ID_RE.pattern}")
            continue
        if row_id in by_id:
            errors.append(f"rows[{index}]: duplicate row id {row_id!r}")
            continue
        by_id[row_id] = row
        for field, spec in (("os", ("family", "version", "build")),
                            ("compiler", ("id", "version", "toolset")),
                            ("gpu", ("api", "device", "vendor", "driverVersion", "featureLevel"))):
            for key in spec:
                if not str(row[field].get(key, "")).strip():
                    errors.append(f"row[{row_id}]: {field}.{key} must be non-empty")

    errors.extend(
        _check_canonical_profile(
            matrix["profile"], rows, by_id, allow_unknown_profile=allow_unknown_profile
        )
    )
    return errors


def _check_canonical_profile(
    profile: str,
    rows: list[dict[str, Any]],
    by_id: dict[str, dict[str, Any]],
    *,
    allow_unknown_profile: bool,
) -> list[str]:
    canonical = CANONICAL_PROFILES.get(profile)
    if canonical is None:
        # Renaming the profile must not be a way to shed its content rules.
        if allow_unknown_profile:
            return []
        return [
            f"Profile {profile!r} has no canonical definition; known profiles are "
            f"{sorted(CANONICAL_PROFILES)}"
        ]

    errors: list[str] = []
    for row_id, spec in canonical.items():
        row = by_id.get(row_id)
        if row is None:
            errors.append(f"Profile {profile!r}: missing required row {row_id!r}")
            continue
        prefix = f"Profile {profile!r} row {row_id!r}"
        if row["tier"] != spec["tier"]:
            errors.append(f"{prefix}: tier must be {spec['tier']!r}, got {row['tier']!r}")
        if set(row["evidenceRequired"]) != set(spec["evidence_required"]):
            missing = sorted(set(spec["evidence_required"]) - set(row["evidenceRequired"]))
            extra = sorted(set(row["evidenceRequired"]) - set(spec["evidence_required"]))
            errors.append(
                f"{prefix}: evidenceRequired diverges from the canonical profile "
                f"(missing {missing}, unexpected {extra})"
            )
        if row["os"]["family"] != spec["os_family"]:
            errors.append(f"{prefix}: os.family must be {spec['os_family']!r}")
        if row["arch"] != spec["arch"]:
            errors.append(f"{prefix}: arch must be {spec['arch']!r}")
        if row["compiler"]["id"] != spec["compiler_id"]:
            errors.append(f"{prefix}: compiler.id must be {spec['compiler_id']!r}")
        if row["compiler"]["toolset"] != spec["compiler_toolset"]:
            errors.append(f"{prefix}: compiler.toolset must be {spec['compiler_toolset']!r}")
        if spec["require_windows_sdk"] and not str(
            row["compiler"].get("windowsSdkVersion", "")
        ).strip():
            errors.append(f"{prefix}: compiler.windowsSdkVersion is required")
        if row["gpu"]["api"] != spec["gpu_api"]:
            errors.append(f"{prefix}: gpu.api must be {spec['gpu_api']!r}")
        if "gpu_vendor_is" in spec and row["gpu"]["vendor"] != spec["gpu_vendor_is"]:
            errors.append(f"{prefix}: gpu.vendor must be {spec['gpu_vendor_is']!r}")
        if "gpu_vendor_not" in spec and row["gpu"]["vendor"] in spec["gpu_vendor_not"]:
            errors.append(
                f"{prefix}: gpu.vendor must not be {row['gpu']['vendor']!r}"
            )
        if row["audio"]["api"] != spec["audio_api"]:
            errors.append(f"{prefix}: audio.api must be {spec['audio_api']!r}")
        if spec["require_audio_version"] and not str(
            row["audio"].get("version", "")
        ).strip():
            errors.append(f"{prefix}: audio.version is required")
        missing_features = spec["min_cpu_features"] - set(row.get("cpuFeatures", []))
        if missing_features:
            errors.append(
                f"{prefix}: cpuFeatures must include {sorted(missing_features)}"
            )
        for dotted, pattern in spec.get("patterns", {}).items():
            section, _, key = dotted.partition(".")
            value = row.get(section, {}).get(key)
            if not isinstance(value, str) or re.compile(pattern).match(value) is None:
                errors.append(
                    f"{prefix}: {dotted}={value!r} does not match the canonical "
                    f"shape {pattern}"
                )

    for row in rows:
        if row["tier"] in CERTIFIABLE_TIERS and row["id"] not in canonical:
            errors.append(
                f"Profile {profile!r}: unexpected certifiable row {row['id']!r}"
            )
    return errors


# ── Evidence validation ────────────────────────────────────────────────────


def validate_evidence(
    evidence: Any,
    *,
    now: datetime | None = None,
    trusted: TrustedContext | None = None,
) -> list[str]:
    """Validate one evidence record: schema, cross-field rules, and identity."""
    if not isinstance(evidence, dict):
        return ["Evidence must be a JSON object"]
    now = now or datetime.now(timezone.utc)

    errors: list[str] = []
    _check_no_nonfinite(evidence, "$", errors)
    _check_string_lengths(evidence, "$", errors)
    if errors:
        return errors

    errors = _schema_errors(EVIDENCE_SCHEMA, _EV_SCHEMA, evidence)
    if errors:
        return [f"schema: {message}" for message in errors]

    if evidence["schemaVersion"] != SCHEMA_VERSION:
        errors.append(f"Evidence schemaVersion must be {SCHEMA_VERSION}")
    if _ROW_ID_RE.match(evidence["rowId"]) is None:
        errors.append(f"Evidence rowId must match {_ROW_ID_RE.pattern}")

    collected_at = parse_bounded_ts(
        evidence["collectedAt"], now=now, context="evidence.collectedAt", errors=errors
    )
    stale_before = evidence.get("staleBefore")
    if stale_before is not None:
        parse_bounded_ts(
            stale_before, now=now, context="evidence.staleBefore", errors=errors
        )

    errors.extend(_validate_collector(evidence["collector"], collected_at, now))
    errors.extend(_validate_host_semantics(evidence["host"]))

    checkout_at = parse_bounded_ts(
        evidence["collector"]["provenance"]["checkoutAt"],
        now=now,
        context="collector.provenance.checkoutAt",
        errors=[],
    )
    for name in sorted(evidence["probes"]):
        errors.extend(
            _validate_probe(
                name, evidence["probes"][name], collected_at, now, checkout_at
            )
        )

    if trusted is not None:
        errors.extend(_bind_to_trusted(evidence, trusted))
    return errors


def _validate_collector(
    collector: dict[str, Any], collected_at: datetime | None, now: datetime
) -> list[str]:
    errors: list[str] = []
    provenance = collector["provenance"]
    attestation = collector["attestation"]

    checkout_at = parse_bounded_ts(
        provenance["checkoutAt"],
        now=now,
        context="collector.provenance.checkoutAt",
        errors=errors,
    )
    signed_at = parse_bounded_ts(
        attestation["signedAt"],
        now=now,
        context="collector.attestation.signedAt",
        errors=errors,
    )

    if attestation["predicateType"] != ATTESTATION_PREDICATE:
        errors.append(
            f"collector.attestation.predicateType {attestation['predicateType']!r} "
            f"is not the recognised predicate {ATTESTATION_PREDICATE!r}"
        )

    if collected_at is not None and checkout_at is not None and collected_at <= checkout_at:
        errors.append(
            "collector.provenance.checkoutAt: evidence must be collected strictly "
            "after the source was checked out"
        )
    if collected_at is not None and signed_at is not None and signed_at < collected_at:
        errors.append(
            "collector.attestation.signedAt: cannot predate evidence collection"
        )

    run_id = collector.get("runId")
    if run_id is not None and run_id != provenance["runId"]:
        errors.append(
            f"collector.runId {run_id!r} disagrees with "
            f"collector.provenance.runId {provenance['runId']!r}"
        )
    if collector["type"] == "ci" and provenance["repository"].count("/") != 1:
        errors.append("collector.provenance.repository must be exactly owner/name")
    return errors


def _validate_host_semantics(host: dict[str, Any]) -> list[str]:
    """Rules the schema cannot express: emptiness and internal agreement."""
    errors: list[str] = []
    if not host["cpu"]["features"]:
        errors.append("host.cpu.features must list at least one feature")
    if len(set(host["cpu"]["features"])) != len(host["cpu"]["features"]):
        errors.append("host.cpu.features contains duplicates")
    gpu = host["gpu"]
    if gpu["api"] == "nullrhi" and gpu["vendor"] != "none":
        errors.append("host.gpu: the nullrhi backend cannot report a hardware vendor")
    if gpu["api"] != "nullrhi" and gpu["vendor"] == "none":
        errors.append(f"host.gpu: api {gpu['api']!r} cannot report vendor 'none'")
    if host["os"]["family"] == "windows" and not str(
        host["compiler"].get("windowsSdkVersion", "")
    ).strip():
        errors.append("host.compiler.windowsSdkVersion is required on Windows hosts")
    return errors


def _validate_probe(
    name: str,
    probe: dict[str, Any],
    collected_at: datetime | None,
    now: datetime,
    checkout_at: datetime | None = None,
) -> list[str]:
    """Cross-field probe rules; Draft 2020-12 cannot express any of these."""
    errors: list[str] = []
    context = f"probes.{name}"
    status = probe["status"]
    duration = probe["durationMs"]
    artifacts = probe.get("artifacts", [])

    if len(artifacts) > MAX_ARTIFACTS_PER_PROBE:
        errors.append(f"{context}.artifacts: {len(artifacts)} exceeds {MAX_ARTIFACTS_PER_PROBE}")

    if status == "skip":
        if not probe.get("skipReason"):
            errors.append(f"{context}: status 'skip' requires skipReason")
        return errors
    if probe.get("skipReason"):
        errors.append(f"{context}: skipReason is only allowed when status is 'skip'")

    if status == "fail" and probe.get("exitCode") == 0:
        errors.append(f"{context}: status 'fail' cannot report exitCode 0")

    if status != "pass":
        return errors

    if duration < MIN_PASS_DURATION_MS:
        errors.append(
            f"{context}.durationMs={duration} is too low for a pass "
            f"(minimum {MIN_PASS_DURATION_MS}ms)"
        )
    if probe.get("exitCode") != 0:
        errors.append(
            f"{context}: a pass requires exitCode 0, got {probe.get('exitCode')!r}"
        )
    if not artifacts:
        errors.append(f"{context}: a pass requires at least one artifact")

    started = parse_bounded_ts(
        probe.get("startedAt"), now=now, context=f"{context}.startedAt", errors=errors
    )
    completed = parse_bounded_ts(
        probe.get("completedAt"), now=now, context=f"{context}.completedAt", errors=errors
    )
    if started is None or completed is None:
        return errors
    if completed <= started:
        errors.append(f"{context}: completedAt must be strictly after startedAt")
        return errors
    if collected_at is not None and completed > collected_at:
        errors.append(
            f"{context}: completedAt is after the record was collected"
        )
    if checkout_at is not None and started < checkout_at:
        errors.append(
            f"{context}: startedAt is before the source was checked out"
        )

    elapsed_ms = (completed - started).total_seconds() * 1000.0
    tolerance = max(DURATION_TOLERANCE_MS, elapsed_ms * DURATION_TOLERANCE_FRACTION)
    if abs(elapsed_ms - duration) > tolerance:
        errors.append(
            f"{context}: durationMs={duration} disagrees with the "
            f"{elapsed_ms:.0f}ms between startedAt and completedAt"
        )
    return errors


def _bind_to_trusted(evidence: dict[str, Any], trusted: TrustedContext) -> list[str]:
    """Refuse a record whose identity is not the one we were told to expect."""
    errors: list[str] = []
    collector = evidence["collector"]
    provenance = collector["provenance"]

    if evidence["commitSha"] != trusted.commit_sha:
        errors.append(
            f"commitSha {evidence['commitSha'][:12]} is not the commit under "
            f"validation ({trusted.commit_sha[:12]})"
        )
    if provenance["commitSha"] != trusted.commit_sha:
        errors.append(
            f"collector.provenance.commitSha {provenance['commitSha'][:12]} is not "
            f"the commit under validation ({trusted.commit_sha[:12]})"
        )
    if collector["type"] != trusted.collector_type:
        errors.append(
            f"collector.type {collector['type']!r} is not the expected "
            f"{trusted.collector_type!r}"
        )
    if collector["identity"] != trusted.identity:
        errors.append("collector.identity does not match the trusted identity")

    if trusted.collector_type != "ci":
        return errors
    for field, expected, label in (
        ("repository", trusted.repository, "repository"),
        ("workflow", trusted.workflow, "workflow"),
        ("runId", trusted.run_id, "runId"),
        ("jobId", trusted.job_id, "jobId"),
    ):
        if provenance[field] != expected:
            errors.append(
                f"collector.provenance.{label} {provenance[field]!r} is not the "
                f"expected {expected!r}"
            )
    if provenance["runAttempt"] != trusted.run_attempt:
        errors.append(
            f"collector.provenance.runAttempt {provenance['runAttempt']!r} is not "
            f"the expected {trusted.run_attempt!r}"
        )
    return errors


# ── Cross-validation ───────────────────────────────────────────────────────


def cross_validate(
    matrix: dict[str, Any],
    evidence_records: dict[str, dict[str, Any]],
    *,
    max_age_hours: Any = DEFAULT_MAX_AGE_HOURS,
    now: datetime | None = None,
    artifact_root: Path | None = None,
    authority: da.Authority | None = None,
    trusted: TrustedContext | None = None,
) -> ValidationResult:
    """Cross-validate rows; without trust, the result is inspection-only."""
    bound_age = validate_max_age_hours(max_age_hours)
    result = ValidationResult(trusted_context=trusted)
    now = now or datetime.now(timezone.utc)

    if trusted is not None and matrix.get("commitSha") != trusted.commit_sha:
        result.error(
            f"[matrix] commitSha {str(matrix.get('commitSha'))[:12]} is not the "
            f"commit under validation ({trusted.commit_sha[:12]})"
        )

    for row in matrix.get("rows", []):
        row_id = row.get("id", "<unknown>")
        result.rows_checked += 1

        if row.get("tier") not in CERTIFIABLE_TIERS:
            result.warn(f"[{row_id}] tier={row.get('tier')!r} is not certifiable, skipping")
            continue

        evidence = evidence_records.get(row_id)
        if evidence is None:
            result.error(f"[{row_id}] No evidence record found -- row cannot certify")
            result.rows_failed += 1
            continue

        row_errors = _cross_validate_row(
            row,
            evidence,
            matrix_commit=matrix.get("commitSha"),
            now=now,
            max_age_hours=bound_age,
            artifact_root=artifact_root,
            authority=authority,
        )
        if row_errors:
            for message in row_errors:
                result.error(f"[{row_id}] {message}")
            result.rows_failed += 1
        else:
            result.rows_certified += 1

    return result


def _cross_validate_row(
    row: dict[str, Any],
    evidence: dict[str, Any],
    *,
    matrix_commit: Any,
    now: datetime,
    max_age_hours: float,
    artifact_root: Path | None,
    authority: da.Authority | None,
) -> list[str]:
    errors: list[str] = []
    row_id = row["id"]

    if evidence.get("commitSha") != matrix_commit:
        errors.append(
            f"Commit mismatch: matrix={str(matrix_commit)[:12]} "
            f"evidence={str(evidence.get('commitSha'))[:12]}"
        )

    collected_at = parse_bounded_ts(
        evidence.get("collectedAt"), now=now, context="collectedAt", errors=errors
    )
    if collected_at is not None:
        age_hours = (now - collected_at).total_seconds() / 3600.0
        if age_hours > max_age_hours:
            errors.append(
                f"Evidence is {age_hours:.1f}h old (max {max_age_hours:g}h)"
            )
        stale_before = evidence.get("staleBefore")
        if stale_before:
            stale_dt = parse_bounded_ts(
                stale_before, now=now, context="staleBefore", errors=errors
            )
            if stale_dt is not None and collected_at < stale_dt:
                errors.append("Evidence was collected before its staleBefore threshold")

    errors.extend(_cross_validate_host(row, evidence))

    required = set(row.get("evidenceRequired", []))
    probes = evidence.get("probes", {})
    for category in sorted(required):
        probe = probes.get(category)
        if probe is None:
            errors.append(f"Missing required probe: {category}")
        elif probe.get("status") != "pass":
            detail = str(probe.get("detail", ""))[:100]
            suffix = f": {detail}" if detail else ""
            errors.append(f"Probe {category} status={probe.get('status')!r}{suffix}")

    # Artifacts are measured before any probe verdict is trusted, because a
    # pass whose artifacts do not exist is not a pass.
    if artifact_root is None:
        errors.append(
            "No artifact root supplied -- declared artifacts cannot be measured"
        )
        measured_digest = None
    else:
        bundle_errors, measured_digest = bundle_verify.verify_bundle(
            evidence, artifact_root=artifact_root, row_id=row_id
        )
        errors.extend(bundle_errors)

    collector = evidence.get("collector")
    attestation = collector.get("attestation") if isinstance(collector, dict) else None
    attested = attestation.get("bundleSha256") if isinstance(attestation, dict) else None
    if not isinstance(attested, str) or _SHA256_RE.match(attested) is None:
        errors.append("collector.attestation.bundleSha256 is missing or malformed")
    elif measured_digest is None:
        errors.append(
            "Bundle digest could not be measured, so the attestation stands unverified"
        )
    elif measured_digest != attested:
        errors.append(
            f"collector.attestation.bundleSha256 {attested[:16]}... does not match "
            f"the measured bundle digest {measured_digest[:16]}..."
        )

    if "dependency_closure" in required:
        if authority is None:
            errors.append(
                "dependency_closure is required but no dependency authority was loaded"
            )
        else:
            errors.extend(
                bundle_verify.check_dependency_closure(
                    row_id, evidence.get("dependencyClosure"), authority
                )
            )
    return errors


def _cross_validate_host(row: dict[str, Any], evidence: dict[str, Any]) -> list[str]:
    """Every declared row attribute must be the one the host actually had."""
    errors: list[str] = []
    host = evidence.get("host", {})

    for field in ("family", "version", "build"):
        expected = row.get("os", {}).get(field)
        actual = host.get("os", {}).get(field)
        if expected and actual != expected:
            errors.append(f"OS {field} mismatch: row={expected!r} host={actual!r}")

    if row.get("arch") and host.get("arch") != row["arch"]:
        errors.append(f"Arch mismatch: row={row['arch']!r} host={host.get('arch')!r}")

    host_compiler = host.get("compiler")
    if not isinstance(host_compiler, dict):
        errors.append("Evidence host is missing the compiler section")
    else:
        for field in ("id", "version", "toolset", "windowsSdkVersion"):
            expected = row.get("compiler", {}).get(field)
            actual = host_compiler.get(field)
            if expected and actual != expected:
                errors.append(f"Compiler {field} mismatch: row={expected!r} host={actual!r}")

    for field in ("api", "device", "vendor", "driverVersion", "featureLevel"):
        expected = row.get("gpu", {}).get(field)
        actual = host.get("gpu", {}).get(field)
        if expected and actual != expected:
            errors.append(f"GPU {field} mismatch: row={expected!r} host={actual!r}")

    for field in ("api", "version"):
        expected = row.get("audio", {}).get(field)
        actual = host.get("audio", {}).get(field)
        if expected and actual != expected:
            errors.append(f"Audio {field} mismatch: row={expected!r} host={actual!r}")

    missing = set(row.get("cpuFeatures", [])) - set(host.get("cpu", {}).get("features", []))
    if missing:
        errors.append(f"Host is missing required CPU features: {sorted(missing)}")
    return errors


# ── Evidence discovery ─────────────────────────────────────────────────────

ALLOWED_NON_RECORD_NAMES = frozenset({".gitkeep"})


def discover_evidence_files(evidence_dir: Path) -> list[Path]:
    """Enumerate evidence records, refusing anything that could hide one."""
    try:
        safe_fs.lstat_checked(evidence_dir, expect="dir")
    except safe_fs.FileSecurityError as exc:
        raise CertificationError(f"evidence directory unusable: {exc}") from exc

    try:
        entries = sorted(os.scandir(evidence_dir), key=lambda entry: entry.name)
    except OSError as exc:
        raise CertificationError(f"cannot list {evidence_dir}: {exc}") from exc

    records: list[Path] = []
    seen: dict[str, str] = {}
    for entry in entries:
        child = Path(entry.path)
        try:
            safe_fs.lstat_checked(child, expect="file")
        except safe_fs.FileSecurityError as exc:
            raise CertificationError(
                f"evidence directory holds an entry that is not a plain file: {exc}"
            ) from exc

        name = entry.name
        folded = safe_fs.case_collision_key(name)
        if folded in seen:
            raise CertificationError(
                f"evidence directory holds {name!r} and {seen[folded]!r}, which "
                f"collide on a case-insensitive filesystem"
            )
        seen[folded] = name

        if name in ALLOWED_NON_RECORD_NAMES:
            continue
        if not name.endswith(".json") or name == ".json":
            raise CertificationError(
                f"unexpected entry in the evidence directory: {name!r} -- only "
                f"*.json records and {sorted(ALLOWED_NON_RECORD_NAMES)} are allowed"
            )
        records.append(child)

    if len(records) > MAX_EVIDENCE_FILES:
        raise CertificationError(
            f"evidence directory holds {len(records)} records (max {MAX_EVIDENCE_FILES})"
        )
    return records


def load_and_validate(
    matrix_path: Path,
    evidence_dir: Path,
    *,
    max_age_hours: Any = DEFAULT_MAX_AGE_HOURS,
    artifact_root: Path | None = None,
    authority: da.Authority | None = None,
    trusted: TrustedContext | None = None,
    now: datetime | None = None,
    allow_unknown_profile: bool = False,
) -> ValidationResult:
    """Load matrix plus evidence records and run trusted full validation."""
    bound_age = validate_max_age_hours(max_age_hours)
    now = now or datetime.now(timezone.utc)
    result = ValidationResult(trusted_context=trusted)

    if trusted is None:
        message = (
            "trusted context is required for full validation: supply the exact "
            "commit SHA and collector provenance from a trusted channel"
        )
        result.error(message)
        result.structural_errors.append(message)
        return result

    matrix = load_strict_json(matrix_path)
    # Unknown profiles may be inspected through the matrix-only diagnostic
    # mode, but they must never reach evidence cross-validation.  Otherwise a
    # caller could opt out of the canonical row contract and still obtain a
    # fully certified result from a complete-looking bundle.
    matrix_errors = validate_matrix(matrix, now=now, allow_unknown_profile=False)
    if matrix_errors:
        for message in matrix_errors:
            result.error(f"[matrix] {message}")
            result.structural_errors.append(f"[matrix] {message}")
        return result

    records: dict[str, dict[str, Any]] = {}
    for path in discover_evidence_files(evidence_dir):
        evidence = load_strict_json(path)
        errors = validate_evidence(evidence, now=now, trusted=trusted)
        if errors:
            for message in errors:
                result.error(f"[{path.name}] {message}")
                result.structural_errors.append(f"[{path.name}] {message}")
            continue
        row_id = evidence["rowId"]
        if path.stem != row_id:
            message = (
                f"[{path.name}] filename does not name its row {row_id!r}; a "
                f"record must live at <rowId>.json"
            )
            result.error(message)
            result.structural_errors.append(message)
            continue
        if row_id in records:
            message = f"Duplicate evidence for row {row_id!r}"
            result.error(message)
            result.structural_errors.append(message)
            continue
        records[row_id] = evidence

    known_rows = {row["id"] for row in matrix["rows"]}
    for row_id in sorted(set(records) - known_rows):
        message = (
            f"[{row_id}.json] names row {row_id!r}, which the support matrix "
            f"does not declare"
        )
        result.error(message)
        result.structural_errors.append(message)

    if result.errors:
        return result

    cross = cross_validate(
        matrix,
        records,
        max_age_hours=bound_age,
        now=now,
        artifact_root=artifact_root,
        authority=authority,
        trusted=trusted,
    )
    result.errors.extend(cross.errors)
    result.warnings.extend(cross.warnings)
    result.rows_checked = cross.rows_checked
    result.rows_certified = cross.rows_certified
    result.rows_failed = cross.rows_failed
    return result


# ── Dependency authority loading ───────────────────────────────────────────


def load_authority(repo_root: Path) -> da.Authority:
    """Load the committed authority and prove it still matches the manifest."""
    path = repo_root / da.AUTHORITY_RELPATH
    try:
        raw = safe_fs.read_bounded(path, max_bytes=da.MAX_AUTHORITY_BYTES)
    except safe_fs.FileSecurityError as exc:
        raise CertificationError(f"dependency authority unusable: {exc}") from exc
    document = parse_strict_json(raw.decode("utf-8"), da.AUTHORITY_RELPATH)
    try:
        da.validate_authority_document(document)
        authority = da.Authority(document)
    except da.AuthorityError as exc:
        raise CertificationError(f"dependency authority is invalid: {exc}") from exc

    lock_path = repo_root / da.LOCK_RELPATH
    try:
        lock_bytes = safe_fs.read_bounded(lock_path, max_bytes=da.MAX_LOCK_BYTES)
        measured = da.manifest_digest(lock_bytes)
    except safe_fs.FileSecurityError as exc:
        raise CertificationError(f"dependency manifest unusable: {exc}") from exc
    if measured != document["sourceSha256"]:
        raise CertificationError(
            f"dependency authority records {da.LOCK_RELPATH} as "
            f"{document['sourceSha256'][:12]} but it measures {measured[:12]}; "
            f"regenerate with Tools/platform-cert/dependency_authority.py --generate"
        )
    return authority


# ── Ledger truthfulness ────────────────────────────────────────────────────

OPEN_STATUSES = frozenset({"open", "in-progress", "blocked"})


def check_ledger_claim(ledger_path: Path, result: ValidationResult) -> list[str]:
    """A ledger may only claim PLT-200 done when every row actually certified."""
    document = load_strict_json(ledger_path)
    if not isinstance(document, dict) or not isinstance(document.get("workItems"), list):
        return [f"{ledger_path.name}: not a work-item ledger"]
    for item in document["workItems"]:
        if not isinstance(item, dict) or item.get("id") != "PLT-200":
            continue
        status = item.get("status")
        if status in OPEN_STATUSES:
            return []
        if not result.fully_certified:
            return [
                f"PLT-200 is marked {status!r} but certification did not pass "
                f"({result.rows_certified} certified, {result.rows_failed} failed, "
                f"{len(result.errors)} error(s))"
            ]
        return []
    return [f"{ledger_path.name}: no PLT-200 work item found"]


# ── Convenience exports (used by tests and collector) ─────────────────────

load_strict_json_file = load_strict_json


def compute_bundle_digest(evidence: dict[str, Any]) -> str:
    """Compute the attestation bundle digest from an evidence record."""
    items = bundle_verify._declared_items(evidence)
    pairs = [(p, s) for _, p, _, s in items
             if isinstance(p, str) and isinstance(s, str)]
    return bundle_verify.bundle_digest(pairs)


def parse_rfc3339(
    value: str, context: str, errors: list[str], *, now: datetime | None = None
) -> datetime | None:
    """Parse an RFC 3339 timestamp with optional bounds checking."""
    return parse_bounded_ts(
        value, now=now or datetime.now(timezone.utc), context=context, errors=errors
    )


# ── CLI ────────────────────────────────────────────────────────────────────


def build_parser() -> Any:
    import argparse

    parser = argparse.ArgumentParser(
        description="Validate platform certification evidence (PLT-200)"
    )
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
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
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=None,
        help="confined root holding <rowId>/<artifact path> for every record",
    )
    parser.add_argument("--max-age-hours", type=cli_max_age_hours, default=DEFAULT_MAX_AGE_HOURS)
    parser.add_argument("--matrix-only", action="store_true")
    parser.add_argument(
        "--allow-unknown-profile",
        action="store_true",
        help="accept a profile with no canonical definition (never in CI)",
    )
    parser.add_argument(
        "--allow-self-hosted",
        action="store_true",
        help=(
            "acknowledge that a manual or automated collector is weaker evidence "
            "than a CI run before accepting one"
        ),
    )
    parser.add_argument(
        "--commit", default=None, help="exact 40-hex commit under validation"
    )
    parser.add_argument(
        "--expect-collector-type", choices=list(VALID_COLLECTOR_TYPES), default=None
    )
    parser.add_argument("--expect-identity", default=None)
    parser.add_argument("--expect-repository", default=None)
    parser.add_argument("--expect-workflow", default=None)
    parser.add_argument("--expect-run-id", default=None)
    parser.add_argument("--expect-run-attempt", type=int, default=None)
    parser.add_argument("--expect-job", default=None)
    parser.add_argument(
        "--from-github-env",
        action="store_true",
        help="take the trusted identity from GITHUB_* runner variables",
    )
    parser.add_argument(
        "--mode",
        choices=["report", "require-certified", "ledger-consistency"],
        default="report",
        help=(
            "report: fail on any error. require-certified: fail unless every "
            "certifiable row certified. ledger-consistency: fail on any "
            "structural error, or whenever the work-item ledger's PLT-200 "
            "claim disagrees with the measured certification state."
        ),
    )
    parser.add_argument(
        "--ledger",
        type=Path,
        default=None,
        help="work-item ledger whose PLT-200 status must match reality",
    )
    return parser


def _trusted_from_args(args: Any) -> TrustedContext:
    if args.from_github_env:
        return TrustedContext.from_github_env(dict(os.environ), identity=args.expect_identity)
    if args.commit is None:
        raise CertificationError(
            "--commit is required for evidence validation (or use --from-github-env)"
        )
    if args.expect_collector_type is None:
        raise CertificationError("--expect-collector-type is required")
    if args.expect_identity is None:
        raise CertificationError("--expect-identity is required")
    return TrustedContext(
        commit_sha=args.commit,
        collector_type=args.expect_collector_type,
        identity=args.expect_identity,
        repository=args.expect_repository,
        workflow=args.expect_workflow,
        run_id=args.expect_run_id,
        run_attempt=args.expect_run_attempt,
        job_id=args.expect_job,
    )


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    try:
        if args.matrix_only:
            errors = validate_matrix(
                load_strict_json(args.matrix),
                allow_unknown_profile=args.allow_unknown_profile,
            )
            if errors:
                for message in errors:
                    print(f"  X {message}", file=sys.stderr)
                print(f"FAIL: {len(errors)} matrix error(s)", file=sys.stderr)
                return 1
            print("PASS: Matrix schema and canonical profile are valid")
            return 0

        trusted = _trusted_from_args(args)
        if trusted.collector_type != "ci" and not args.allow_self_hosted:
            raise CertificationError(
                f"collector type {trusted.collector_type!r} is self-hosted evidence; "
                f"pass --allow-self-hosted to accept it deliberately"
            )
        if args.artifact_root is None:
            raise CertificationError(
                "--artifact-root is required: declared artifacts must be measured "
                "beneath an explicit confined root"
            )
        authority = load_authority(args.repo_root)

        result = load_and_validate(
            args.matrix,
            args.evidence_dir,
            max_age_hours=args.max_age_hours,
            artifact_root=args.artifact_root,
            authority=authority,
            trusted=trusted,
            allow_unknown_profile=args.allow_unknown_profile,
        )
        print(result.summary())

        ledger_errors: list[str] = []
        if args.ledger is not None:
            ledger_errors = check_ledger_claim(args.ledger, result)
            for message in ledger_errors:
                print(f"  X ledger: {message}", file=sys.stderr)

        if args.mode == "require-certified":
            if not result.fully_certified or ledger_errors:
                print(
                    "FAIL: --mode require-certified, but not every certifiable "
                    "row is certified",
                    file=sys.stderr,
                )
                return 1
            return 0

        if args.mode == "ledger-consistency":
            if result.structural_errors:
                print(
                    f"FAIL: {len(result.structural_errors)} structural error(s) in "
                    f"the matrix or in a present evidence record",
                    file=sys.stderr,
                )
                return 1
            if ledger_errors:
                return 1
            if result.fully_certified:
                print("Every certifiable row is certified at this commit.")
            else:
                print(
                    "Not certified at this commit, and the ledger says so: "
                    "PLT-200 remains open."
                )
            return 0

        return 0 if (result.ok and not ledger_errors) else 1

    except CertificationError as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:  # noqa: BLE001 - an unexpected error is not a pass
        print(f"FATAL: Unexpected error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
