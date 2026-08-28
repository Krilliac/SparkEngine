#!/usr/bin/env python3
"""Fail-closed validation for PERF-100 performance-budget evidence.

The module accepts untrusted JSON documents. File ingestion is bounded,
duplicate object keys are rejected, and the public validators return stable
error strings instead of raising for malformed Python values.
"""

from __future__ import annotations

import hashlib
import heapq
import json
import math
import os
import re
import stat as stat_module
import sys
from collections.abc import Mapping
from datetime import datetime
from pathlib import Path
from typing import Any


class BudgetValidationError(Exception):
    """A structural or governance violation in budget data."""


class DuplicateJSONKeyError(BudgetValidationError):
    """Raised internally when a JSON object repeats a key."""


SUPPORTED_SCHEMA_VERSION = "1.0.0"
SUPPORTED_BUDGET_VERSION = "v1"
SUPPORTED_BASELINE_VERSION = "v1"

MAX_JSON_BYTES = 2 * 1024 * 1024
MAX_HARDWARE_ROWS = 128
MAX_METRICS = 2048
MAX_BASELINES = 2048
MAX_MEASUREMENTS = 2048
MAX_SAMPLE_COUNT = 100_000_000
MAX_SHORT_STRING = 256
MAX_DESCRIPTION = 4096
MAX_NUMERIC_VALUE = 1.0e18
MAX_DIRECTORY_ENTRIES = 4096
MAX_DIAGNOSTIC_KEYS = 16
MAX_PATH_COMPONENTS = 128

VALID_UNITS = frozenset({
    "ms", "us", "ns", "s",
    "bytes", "kilobytes", "megabytes", "gigabytes",
    "bytes_per_hour",
    "count", "percent", "fps",
})
VALID_DIRECTIONS = frozenset({"lower_is_better", "higher_is_better"})
VALID_CATEGORIES = frozenset({
    "frame_time", "tick_time", "startup_time",
    "memory", "package_size", "soak", "visual", "throughput",
})
VALID_METRIC_STATUSES = frozenset({
    "pending_measurement", "active", "suspended", "retired",
})
VALID_BACKENDS = frozenset({
    "d3d11", "d3d12", "vulkan", "metal", "opengl", "nullrhi",
})
VALID_PERCENTILES = frozenset({"p50", "p90", "p95", "p99", "p999"})

HARDWARE_TOP_LEVEL_REQUIRED = frozenset({"schemaVersion", "rows"})
HARDWARE_ROW_REQUIRED = frozenset({
    "id", "certified", "os", "cpu", "cpuCores",
    "gpu", "gpuVramGb", "gpuDriverMinVersion", "ramGb",
    "certifiedAt", "certifiedBy", "certifiedCommit", "notes",
})
BUDGET_TOP_LEVEL_REQUIRED = frozenset({
    "schemaVersion", "budgetVersion", "metadata", "metrics",
})
BUDGET_METADATA_REQUIRED = frozenset({
    "createdAt", "createdBy", "commitSha", "description",
})
METRIC_REQUIRED = frozenset({
    "id", "category", "scene", "backend", "product",
    "unit", "direction", "percentile",
    "hardwareRowId", "budget", "status",
})
BASELINES_TOP_LEVEL_REQUIRED = frozenset({
    "schemaVersion", "baselineVersion", "approvalPolicy", "baselines",
})
APPROVAL_POLICY_REQUIRED = frozenset({
    "description", "requiredFields", "selfApprovalAllowed",
})
REQUIRED_APPROVAL_FIELDS = frozenset({
    "approvedBy", "approvedAt", "approvalCommit", "budgetDefinitionDigest",
})
BASELINE_ENTRY_REQUIRED = frozenset({
    "metricId", "value", "unit", "commitSha", "hardwareRowId",
    "measuredAt", "sampleCount",
    "approvedBy", "approvedAt", "approvalCommit", "budgetDefinitionDigest",
})
RESULT_TOP_LEVEL_REQUIRED = frozenset({
    "commitSha", "timestamp", "hardwareRowId", "measurements",
})
RESULT_MEASUREMENT_REQUIRED = frozenset({
    "metricId", "value", "unit", "sampleCount",
})

_RFC3339_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}"
    r"(?:\.\d{1,9})?(?:Z|[+-]\d{2}:\d{2})$"
)
_IDENTIFIER_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
_DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
_METRIC_DIGEST_DOMAIN = "sparkengine.perf-budget.metric/v1"


def _display(value: Any, limit: int = 96) -> str:
    """Bound an untrusted value before including it in diagnostics."""
    try:
        rendered = repr(value)
    except Exception:
        rendered = f"<{type(value).__name__}>"
    if len(rendered) > limit:
        return rendered[:limit - 3] + "..."
    return rendered


def _sorted_key_displays(values: set[Any]) -> str:
    shown = heapq.nsmallest(
        MAX_DIAGNOSTIC_KEYS,
        (_display(value) for value in values),
        key=str,
    )
    suffix = (
        f" (showing {len(shown)} of {len(values)})"
        if len(values) > len(shown) else ""
    )
    return f"{shown}{suffix}"


def _is_strict_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_finite_number(value: Any) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, int):
        # Do not pass arbitrary-precision integers through math.isfinite():
        # conversion to a C double raises OverflowError for otherwise valid JSON.
        return True
    return isinstance(value, float) and math.isfinite(value)


def _check_keys(obj: Any, required: frozenset[str], context: str) -> list[str]:
    if not isinstance(obj, dict):
        return [f"{context}: must be an object"]
    present = set(obj.keys())
    errors: list[str] = []
    missing = required - present
    extra = present - required
    if missing:
        errors.append(f"{context}: missing required keys: {sorted(missing)}")
    if extra:
        errors.append(
            f"{context}: unknown keys (fail-closed): {_sorted_key_displays(extra)}"
        )
    return errors


def _check_version(value: Any, expected: str, field: str,
                   context: str) -> list[str]:
    if not isinstance(value, str):
        return [f"{context}: {field} must be the string {expected!r}"]
    if value != expected:
        return [f"{context}: unsupported {field}={value!r}; expected {expected!r}"]
    return []


def _check_string(value: Any, field: str, context: str, *,
                  allow_null: bool = False, allow_empty: bool = False,
                  max_length: int = MAX_SHORT_STRING,
                  identifier: bool = False) -> list[str]:
    if value is None and allow_null:
        return []
    if not isinstance(value, str):
        return [f"{context}: {field} must be a string"]
    if not allow_empty and not value.strip():
        return [f"{context}: {field} must be a non-empty string"]
    if len(value) > max_length:
        return [
            f"{context}: {field} exceeds maximum length {max_length} "
            f"(got {len(value)})"
        ]
    if any(ord(char) < 32 and char not in "\t\r\n" for char in value):
        return [f"{context}: {field} contains a disallowed control character"]
    if identifier and value and not _IDENTIFIER_RE.fullmatch(value):
        return [
            f"{context}: {field} must be a lowercase identifier using only "
            "letters, digits, '.', '_', or '-'"
        ]
    return []


def _check_enum(value: Any, valid: frozenset[str], field: str,
                context: str, *, allow_null: bool = False) -> list[str]:
    if value is None and allow_null:
        return []
    if not isinstance(value, str):
        return [f"{context}: {field} must be a string from {sorted(valid)}"]
    if value not in valid:
        return [f"{context}: {field}={value!r} not in {sorted(valid)}"]
    return []


def _check_sha(value: Any, field: str, context: str, *,
               allow_null: bool = False) -> list[str]:
    if value is None and allow_null:
        return []
    if not isinstance(value, str):
        return [f"{context}: {field} must be a hexadecimal commit SHA"]
    # All provenance is exact. Abbreviated SHAs can be prefix aliases for the
    # same object and therefore cannot prove independent review.
    allowed_lengths = {40, 64}
    if len(value) not in allowed_lengths:
        return [
            f"{context}: {field} must be a full 40- or 64-character hexadecimal commit SHA "
            f"(got length {len(value)})"
        ]
    if not all(char in "0123456789abcdefABCDEF" for char in value):
        return [f"{context}: {field} contains non-hexadecimal characters"]
    return []


def _parse_rfc3339(value: Any, field: str,
                   context: str) -> tuple[datetime | None, list[str]]:
    errors = _check_string(value, field, context, max_length=64)
    if errors:
        return None, [
            f"{context}: {field} must be a timezone-aware RFC3339/ISO-8601 string"
        ]
    assert isinstance(value, str)
    if not _RFC3339_RE.fullmatch(value):
        return None, [
            f"{context}: {field} must be a timezone-aware RFC3339 timestamp"
        ]
    if value.endswith("-00:00"):
        return None, [
            f"{context}: {field} must use a known RFC3339 timezone offset; "
            "-00:00 denotes an unknown local offset"
        ]
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    try:
        parsed = datetime.fromisoformat(normalized)
    except ValueError:
        return None, [f"{context}: {field} is not a real RFC3339 timestamp"]
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None, [
            f"{context}: {field} must include a timezone offset or 'Z'"
        ]
    return parsed, []


def _check_rfc3339(value: Any, field: str, context: str, *,
                   allow_null: bool = False) -> list[str]:
    if value is None and allow_null:
        return []
    return _parse_rfc3339(value, field, context)[1]


def _check_list(value: Any, field: str, context: str,
                maximum: int) -> list[str]:
    if not isinstance(value, list):
        return [f"{context}: {field} must be a list"]
    if len(value) > maximum:
        return [
            f"{context}: {field} exceeds maximum count {maximum} "
            f"(got {len(value)})"
        ]
    return []


def _normalise_id_set(values: Any) -> frozenset[str]:
    if isinstance(values, (set, frozenset, list, tuple)):
        return frozenset(value for value in values if isinstance(value, str))
    return frozenset()


def _duplicate_aware_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJSONKeyError(f"duplicate JSON object key {key!r}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> Any:
    raise BudgetValidationError(f"non-finite JSON number {value!r} is not allowed")


def budget_definition_digest(metric: Any) -> str | None:
    """Return the canonical approval digest for one exact metric definition."""
    if not isinstance(metric, dict) or set(metric) != set(METRIC_REQUIRED):
        return None
    projection = {key: metric[key] for key in sorted(METRIC_REQUIRED)}
    try:
        encoded = json.dumps(
            {"domain": _METRIC_DIGEST_DOMAIN, "metric": projection},
            ensure_ascii=True,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError, OverflowError):
        return None
    return hashlib.sha256(encoded).hexdigest()


def _is_reparse_stat(file_stat: os.stat_result) -> bool:
    attributes = getattr(file_stat, "st_file_attributes", 0)
    reparse_flag = getattr(stat_module, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return stat_module.S_ISLNK(file_stat.st_mode) or bool(attributes & reparse_flag)


def _identity(file_stat: os.stat_result) -> tuple[int, int]:
    return file_stat.st_dev, file_stat.st_ino


def validate_trusted_directory(path: Path, label: str) -> list[str]:
    """Reject aliases or reparse points anywhere in a governance-root path."""
    absolute = Path(os.path.abspath(path))
    components = list(reversed(absolute.parents)) + [absolute]
    if len(components) > MAX_PATH_COMPONENTS:
        return [
            f"{label}: path exceeds maximum component count {MAX_PATH_COMPONENTS}"
        ]
    for component in components:
        try:
            component_stat = component.lstat()
        except FileNotFoundError:
            return [f"{label}: missing required directory"]
        except OSError as exc:
            return [
                f"{label}: cannot inspect directory path: "
                f"{exc.strerror or type(exc).__name__}"
            ]
        if _is_reparse_stat(component_stat):
            return [
                f"{label}: directory path must not contain a symlink, junction, "
                "or reparse point"
            ]
        if component == absolute and not stat_module.S_ISDIR(component_stat.st_mode):
            return [f"{label}: required path is not a directory"]
    try:
        canonical = absolute.resolve(strict=True)
    except OSError as exc:
        return [
            f"{label}: cannot resolve directory path: "
            f"{exc.strerror or type(exc).__name__}"
        ]
    # Ignore drive/UNC anchor spelling, which is not a directory entry. On
    # Windows resolve() returns the stored long-name case for every component.
    if absolute.parts[1:] != canonical.parts[1:]:
        return [f"{label}: directory path contains a wrong-case or short-name alias"]
    return []


def _check_exact_entry(path: Path, label: str) -> list[str]:
    """Require the exact directory-entry spelling and cap enumeration work."""
    exact = False
    aliases: list[str] = []
    try:
        with os.scandir(path.parent) as entries:
            for count, entry in enumerate(entries, start=1):
                if count > MAX_DIRECTORY_ENTRIES:
                    return [
                        f"{label}: parent directory exceeds maximum entry count "
                        f"{MAX_DIRECTORY_ENTRIES}"
                    ]
                if entry.name == path.name:
                    exact = True
                elif entry.name.casefold() == path.name.casefold():
                    aliases.append(entry.name)
    except FileNotFoundError:
        return [f"{label}: parent directory is missing"]
    except OSError as exc:
        return [f"{label}: cannot enumerate parent directory: {exc.strerror or type(exc).__name__}"]
    if aliases:
        qualifier = "ambiguous case alias" if exact else "case alias"
        return [
            f"{label}: required filename case is {path.name!r}; found {qualifier} "
            f"{aliases[0]!r}"
        ]
    if exact:
        return []
    return [f"{label}: missing required file"]


def load_bounded_json(path: Path, label: str, *,
                      max_bytes: int = MAX_JSON_BYTES,
                      trusted_root: Path | None = None) -> tuple[Any | None, list[str]]:
    """Load one snapshot-stable, bounded UTF-8 JSON file without aliases.

    The loader rejects case aliases, symlinks/junctions/reparse points, hard
    links, containment escapes, and identity/content changes across the read.
    """
    root = trusted_root if trusted_root is not None else path.parent
    root_errors = validate_trusted_directory(root, f"{label} root")
    if root_errors:
        return None, root_errors
    try:
        root_before = root.lstat()
    except OSError as exc:
        return None, [
            f"{label}: cannot bind trusted root identity: "
            f"{exc.strerror or type(exc).__name__}"
        ]
    exact_errors = _check_exact_entry(path, label)
    if exact_errors:
        return None, exact_errors
    try:
        root_resolved = root.resolve(strict=True)
        resolved = path.resolve(strict=True)
        expected = root_resolved / path.name
        if resolved != expected:
            return None, [f"{label}: resolved path escapes its trusted root"]
        before = path.lstat()
    except FileNotFoundError:
        return None, [f"{label}: missing required file"]
    except OSError as exc:
        return None, [f"{label}: cannot inspect file: {exc.strerror or type(exc).__name__}"]
    if _is_reparse_stat(before):
        return None, [f"{label}: file must not be a symlink, junction, or reparse point"]
    if not stat_module.S_ISREG(before.st_mode):
        return None, [f"{label}: required path is not a regular file"]
    if before.st_nlink != 1:
        return None, [f"{label}: hard-linked files are not accepted"]
    if before.st_size > max_bytes:
        return None, [
            f"{label}: file exceeds maximum size {max_bytes} bytes "
            f"(got {before.st_size})"
        ]
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor: int | None = None
    try:
        descriptor = os.open(path, flags)
        opened_before = os.fstat(descriptor)
        if _identity(opened_before) != _identity(before):
            return None, [f"{label}: file identity changed before open"]
        if (_is_reparse_stat(opened_before)
                or not stat_module.S_ISREG(opened_before.st_mode)):
            return None, [f"{label}: opened path is not a regular non-reparse file"]
        if opened_before.st_nlink != 1:
            return None, [f"{label}: hard-linked files are not accepted"]
        chunks: list[bytes] = []
        remaining = max_bytes + 1
        while remaining:
            chunk = os.read(descriptor, min(64 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        payload = b"".join(chunks)
        opened_after = os.fstat(descriptor)
    except OSError as exc:
        return None, [f"{label}: cannot read file: {exc.strerror or type(exc).__name__}"]
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if len(payload) > max_bytes:
        return None, [f"{label}: file grew beyond maximum size {max_bytes} bytes"]
    if (_identity(opened_before) != _identity(opened_after)
            or opened_before.st_size != opened_after.st_size
            or opened_before.st_mtime_ns != opened_after.st_mtime_ns
            or opened_before.st_ctime_ns != opened_after.st_ctime_ns):
        return None, [f"{label}: file changed while it was being read"]
    try:
        after = path.lstat()
        root_after = root.lstat()
    except OSError as exc:
        return None, [f"{label}: cannot re-inspect file: {exc.strerror or type(exc).__name__}"]
    final_root_errors = validate_trusted_directory(root, f"{label} root")
    if final_root_errors:
        return None, final_root_errors
    final_exact_errors = _check_exact_entry(path, label)
    if final_exact_errors:
        return None, final_exact_errors
    # Windows reports a different creation/change-time view for path and open
    # handle stats. Compare each view across time, while binding both by inode.
    if (_is_reparse_stat(after) or _identity(after) != _identity(opened_after)
            or after.st_size != before.st_size
            or after.st_mtime_ns != before.st_mtime_ns
            or after.st_ctime_ns != before.st_ctime_ns):
        return None, [f"{label}: path identity changed while it was being read"]
    if (_is_reparse_stat(root_after)
            or _identity(root_after) != _identity(root_before)):
        return None, [f"{label}: trusted root identity changed while it was being read"]
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        return None, [f"{label}: invalid UTF-8 at byte {exc.start}"]
    try:
        data = json.loads(
            text,
            object_pairs_hook=_duplicate_aware_object,
            parse_constant=_reject_json_constant,
        )
    except DuplicateJSONKeyError as exc:
        return None, [f"{label}: {exc}"]
    except BudgetValidationError as exc:
        return None, [f"{label}: {exc}"]
    except json.JSONDecodeError as exc:
        return None, [
            f"{label}: malformed JSON at line {exc.lineno}, column {exc.colno}"
        ]
    except (RecursionError, ValueError) as exc:
        return None, [f"{label}: JSON nesting/value is invalid: {type(exc).__name__}"]
    return data, []


def validate_hardware(data: Any) -> list[str]:
    errors = _check_keys(data, HARDWARE_TOP_LEVEL_REQUIRED, "hardware")
    if not isinstance(data, dict):
        return errors
    errors.extend(_check_version(
        data.get("schemaVersion"), SUPPORTED_SCHEMA_VERSION,
        "schemaVersion", "hardware",
    ))
    rows = data.get("rows")
    errors.extend(_check_list(rows, "rows", "hardware", MAX_HARDWARE_ROWS))
    if not isinstance(rows, list):
        return errors
    if not rows:
        errors.append("hardware: rows must contain at least one hardware row")

    seen_ids: set[str] = set()
    for index, row in enumerate(rows[:MAX_HARDWARE_ROWS]):
        context = f"hardware.rows[{index}]"
        errors.extend(_check_keys(row, HARDWARE_ROW_REQUIRED, context))
        if not isinstance(row, dict):
            continue

        row_id = row.get("id")
        errors.extend(_check_string(
            row_id, "id", context, max_length=128, identifier=True,
        ))
        if isinstance(row_id, str) and row_id:
            identity = row_id.casefold()
            if identity in seen_ids:
                errors.append(f"{context}: duplicate hardware row id {row_id!r}")
            else:
                seen_ids.add(identity)

        for field in ("os", "cpu", "gpu"):
            errors.extend(_check_string(row.get(field), field, context))
        errors.extend(_check_string(
            row.get("gpuDriverMinVersion"), "gpuDriverMinVersion", context,
            allow_null=True,
        ))
        errors.extend(_check_string(
            row.get("notes"), "notes", context, allow_empty=True,
            max_length=MAX_DESCRIPTION,
        ))

        certified = row.get("certified")
        if type(certified) is not bool:
            errors.append(f"{context}: certified must be a boolean")
        elif certified:
            errors.extend(_check_rfc3339(
                row.get("certifiedAt"), "certifiedAt", context,
            ))
            errors.extend(_check_string(
                row.get("certifiedBy"), "certifiedBy", context,
            ))
            errors.extend(_check_sha(
                row.get("certifiedCommit"), "certifiedCommit", context,
            ))
        else:
            for field in ("certifiedAt", "certifiedBy", "certifiedCommit"):
                if row.get(field) is not None:
                    errors.append(
                        f"{context}: certified=false requires {field}=null"
                    )

        cores = row.get("cpuCores")
        if not _is_strict_int(cores) or not 1 <= cores <= 4096:
            errors.append(f"{context}: cpuCores must be an integer in [1, 4096]")
        ram = row.get("ramGb")
        if (not _is_finite_number(ram) or not 0 < ram <= 1_000_000):
            errors.append(
                f"{context}: ramGb must be a finite number in (0, 1000000]"
            )
        vram = row.get("gpuVramGb")
        if (not _is_finite_number(vram) or not 0 <= vram <= 1_000_000):
            errors.append(
                f"{context}: gpuVramGb must be a finite number in [0, 1000000]"
            )
    return errors


def validate_budget(data: Any, hardware_ids: Any) -> list[str]:
    errors = _check_keys(data, BUDGET_TOP_LEVEL_REQUIRED, "budget")
    if not isinstance(data, dict):
        return errors
    errors.extend(_check_version(
        data.get("schemaVersion"), SUPPORTED_SCHEMA_VERSION,
        "schemaVersion", "budget",
    ))
    errors.extend(_check_version(
        data.get("budgetVersion"), SUPPORTED_BUDGET_VERSION,
        "budgetVersion", "budget",
    ))

    metadata = data.get("metadata")
    errors.extend(_check_keys(metadata, BUDGET_METADATA_REQUIRED, "budget.metadata"))
    if isinstance(metadata, dict):
        errors.extend(_check_rfc3339(
            metadata.get("createdAt"), "createdAt", "budget.metadata",
        ))
        errors.extend(_check_string(
            metadata.get("createdBy"), "createdBy", "budget.metadata",
        ))
        errors.extend(_check_sha(
            metadata.get("commitSha"), "commitSha", "budget.metadata",
            allow_null=True,
        ))
        errors.extend(_check_string(
            metadata.get("description"), "description", "budget.metadata",
            max_length=MAX_DESCRIPTION,
        ))

    metrics = data.get("metrics")
    errors.extend(_check_list(metrics, "metrics", "budget", MAX_METRICS))
    if not isinstance(metrics, list):
        return errors
    if not metrics:
        errors.append("budget: metrics must contain at least one definition")

    known_hardware = _normalise_id_set(hardware_ids)
    seen_ids: set[str] = set()
    for index, metric in enumerate(metrics[:MAX_METRICS]):
        context = f"budget.metrics[{index}]"
        errors.extend(_check_keys(metric, METRIC_REQUIRED, context))
        if not isinstance(metric, dict):
            continue

        metric_id = metric.get("id")
        errors.extend(_check_string(
            metric_id, "id", context, max_length=128, identifier=True,
        ))
        if isinstance(metric_id, str) and metric_id:
            identity = metric_id.casefold()
            if identity in seen_ids:
                errors.append(f"{context}: duplicate metric id {metric_id!r}")
            else:
                seen_ids.add(identity)

        errors.extend(_check_enum(
            metric.get("category"), VALID_CATEGORIES, "category", context,
        ))
        errors.extend(_check_string(
            metric.get("scene"), "scene", context, allow_null=True,
        ))
        errors.extend(_check_enum(
            metric.get("backend"), VALID_BACKENDS, "backend", context,
            allow_null=True,
        ))
        errors.extend(_check_string(metric.get("product"), "product", context))
        errors.extend(_check_enum(
            metric.get("unit"), VALID_UNITS, "unit", context,
        ))
        errors.extend(_check_enum(
            metric.get("direction"), VALID_DIRECTIONS, "direction", context,
        ))
        errors.extend(_check_enum(
            metric.get("percentile"), VALID_PERCENTILES, "percentile", context,
            allow_null=True,
        ))
        errors.extend(_check_enum(
            metric.get("status"), VALID_METRIC_STATUSES, "status", context,
        ))

        hardware_id = metric.get("hardwareRowId")
        errors.extend(_check_string(
            hardware_id, "hardwareRowId", context, allow_null=True,
            max_length=128, identifier=True,
        ))
        if isinstance(hardware_id, str) and hardware_id not in known_hardware:
            errors.append(
                f"{context}: hardwareRowId={hardware_id!r} not found in hardware rows"
            )

        budget_value = metric.get("budget")
        if budget_value is not None:
            if (not _is_finite_number(budget_value)
                    or not 0 <= budget_value <= MAX_NUMERIC_VALUE):
                errors.append(
                    f"{context}: budget must be null or a finite non-negative "
                    f"number no greater than {MAX_NUMERIC_VALUE:g}"
                )
        status = metric.get("status")
        if status == "active" and budget_value is None:
            errors.append(
                f"{context}: status='active' cannot use budget=null; "
                "a numeric budget is required"
            )
        elif status == "pending_measurement" and budget_value is not None:
            errors.append(
                f"{context}: status='pending_measurement' requires budget=null"
            )
        elif status == "retired" and budget_value is not None:
            errors.append(f"{context}: status='retired' requires budget=null")
        # suspended intentionally permits a retained or null budget; the
        # comparator always excludes it and records the skip explicitly.
    return errors


def _metric_lookup(metric_definitions: Any) -> tuple[frozenset[str], dict[str, dict]]:
    if isinstance(metric_definitions, Mapping):
        lookup = {
            key: value for key, value in metric_definitions.items()
            if isinstance(key, str) and isinstance(value, dict)
        }
        return frozenset(lookup), lookup
    return _normalise_id_set(metric_definitions), {}


def validate_baselines(data: Any, metric_definitions: Any,
                       hardware_ids: Any, *,
                       allow_bootstrap_self_approval: bool = False) -> list[str]:
    errors = _check_keys(data, BASELINES_TOP_LEVEL_REQUIRED, "baselines")
    if not isinstance(data, dict):
        return errors
    errors.extend(_check_version(
        data.get("schemaVersion"), SUPPORTED_SCHEMA_VERSION,
        "schemaVersion", "baselines",
    ))
    errors.extend(_check_version(
        data.get("baselineVersion"), SUPPORTED_BASELINE_VERSION,
        "baselineVersion", "baselines",
    ))

    policy = data.get("approvalPolicy")
    errors.extend(_check_keys(
        policy, APPROVAL_POLICY_REQUIRED, "baselines.approvalPolicy",
    ))
    self_approval_allowed = False
    if isinstance(policy, dict):
        errors.extend(_check_string(
            policy.get("description"), "description", "baselines.approvalPolicy",
            max_length=MAX_DESCRIPTION,
        ))
        required_fields = policy.get("requiredFields")
        errors.extend(_check_list(
            required_fields, "requiredFields", "baselines.approvalPolicy", 16,
        ))
        if isinstance(required_fields, list):
            valid_fields = [field for field in required_fields if isinstance(field, str)]
            if len(valid_fields) != len(required_fields):
                errors.append(
                    "baselines.approvalPolicy: requiredFields entries must be strings"
                )
            if len(set(valid_fields)) != len(valid_fields):
                errors.append(
                    "baselines.approvalPolicy: requiredFields contains duplicates"
                )
            if frozenset(valid_fields) != REQUIRED_APPROVAL_FIELDS:
                errors.append(
                    "baselines.approvalPolicy: requiredFields must be exactly "
                    f"{sorted(REQUIRED_APPROVAL_FIELDS)}"
                )
        self_value = policy.get("selfApprovalAllowed")
        if type(self_value) is not bool:
            errors.append(
                "baselines.approvalPolicy: selfApprovalAllowed must be a boolean"
            )
        else:
            if self_value and not allow_bootstrap_self_approval:
                errors.append(
                    "baselines.approvalPolicy: selfApprovalAllowed=true is forbidden "
                    "by blocking/release validation"
                )
            self_approval_allowed = self_value and allow_bootstrap_self_approval

    baselines = data.get("baselines")
    errors.extend(_check_list(
        baselines, "baselines", "baselines", MAX_BASELINES,
    ))
    if not isinstance(baselines, list):
        return errors

    metric_ids, metric_lookup = _metric_lookup(metric_definitions)
    known_hardware = _normalise_id_set(hardware_ids)
    seen_keys: set[tuple[str, str]] = set()
    for index, baseline in enumerate(baselines[:MAX_BASELINES]):
        context = f"baselines.baselines[{index}]"
        errors.extend(_check_keys(baseline, BASELINE_ENTRY_REQUIRED, context))
        if not isinstance(baseline, dict):
            continue

        metric_id = baseline.get("metricId")
        errors.extend(_check_string(
            metric_id, "metricId", context, max_length=128, identifier=True,
        ))
        if isinstance(metric_id, str) and metric_id not in metric_ids:
            errors.append(
                f"{context}: metricId={metric_id!r} not found in budget metrics"
            )

        hardware_id = baseline.get("hardwareRowId")
        errors.extend(_check_string(
            hardware_id, "hardwareRowId", context,
            max_length=128, identifier=True,
        ))
        if isinstance(hardware_id, str) and hardware_id not in known_hardware:
            errors.append(
                f"{context}: hardwareRowId={hardware_id!r} not in hardware rows"
            )

        if isinstance(metric_id, str) and isinstance(hardware_id, str):
            duplicate_key = (metric_id.casefold(), hardware_id.casefold())
            if duplicate_key in seen_keys:
                errors.append(
                    f"{context}: duplicate baseline for metric {metric_id!r} "
                    f"and hardware {hardware_id!r}"
                )
            else:
                seen_keys.add(duplicate_key)

        unit = baseline.get("unit")
        errors.extend(_check_enum(unit, VALID_UNITS, "unit", context))
        metric = metric_lookup.get(metric_id) if isinstance(metric_id, str) else None
        if metric is not None:
            metric_unit = metric.get("unit")
            if isinstance(unit, str) and unit != metric_unit:
                errors.append(
                    f"{context}: unit={unit!r} does not match budget unit={metric_unit!r}"
                )

        definition_digest = baseline.get("budgetDefinitionDigest")
        if not isinstance(definition_digest, str) or not _DIGEST_RE.fullmatch(
                definition_digest):
            errors.append(
                f"{context}: budgetDefinitionDigest must be exactly 64 lowercase "
                "hexadecimal characters"
            )
        elif metric is not None:
            expected_digest = budget_definition_digest(metric)
            if expected_digest is None or definition_digest != expected_digest:
                errors.append(
                    f"{context}: budgetDefinitionDigest does not match the exact "
                    "approved budget definition"
                )

        if metric is not None:
            metric_hardware = metric.get("hardwareRowId")
            if metric_hardware is not None and hardware_id != metric_hardware:
                errors.append(
                    f"{context}: hardwareRowId={_display(hardware_id)} does not "
                    f"match budget hardwareRowId={metric_hardware!r}"
                )

        value = baseline.get("value")
        if (not _is_finite_number(value)
                or not 0 <= value <= MAX_NUMERIC_VALUE):
            errors.append(
                f"{context}: value must be a finite number in "
                f"[0, {MAX_NUMERIC_VALUE:g}]"
            )
        errors.extend(_check_sha(baseline.get("commitSha"), "commitSha", context))
        measured_at, measured_errors = _parse_rfc3339(
            baseline.get("measuredAt"), "measuredAt", context,
        )
        errors.extend(measured_errors)
        sample_count = baseline.get("sampleCount")
        if (not _is_strict_int(sample_count)
                or not 1 <= sample_count <= MAX_SAMPLE_COUNT):
            errors.append(
                f"{context}: sampleCount must be an integer in "
                f"[1, {MAX_SAMPLE_COUNT}]"
            )
        errors.extend(_check_string(
            baseline.get("approvedBy"), "approvedBy", context,
        ))
        approved_at, approved_errors = _parse_rfc3339(
            baseline.get("approvedAt"), "approvedAt", context,
        )
        errors.extend(approved_errors)
        errors.extend(_check_sha(
            baseline.get("approvalCommit"), "approvalCommit", context,
        ))
        if (measured_at is not None and approved_at is not None
                and approved_at < measured_at):
            errors.append(
                f"{context}: approvedAt must not precede measuredAt"
            )

        commit_sha = baseline.get("commitSha")
        approval_commit = baseline.get("approvalCommit")
        if (isinstance(commit_sha, str) and isinstance(approval_commit, str)
                and commit_sha.lower() == approval_commit.lower()
                and not self_approval_allowed):
            errors.append(
                f"{context}: approvalCommit equals commitSha; self-approval is not allowed"
            )
    if metric_lookup:
        for metric_id, metric in metric_lookup.items():
            if metric.get("status") != "active":
                continue
            metric_hardware = metric.get("hardwareRowId")
            applicable_hardware = (
                known_hardware if metric_hardware is None
                else frozenset({metric_hardware})
            )
            for hardware_id in sorted(
                    value for value in applicable_hardware if isinstance(value, str)):
                key = (metric_id.casefold(), hardware_id.casefold())
                if key not in seen_keys:
                    errors.append(
                        f"baselines: active metric {metric_id!r} requires a reviewed "
                        f"baseline for hardware {hardware_id!r}"
                    )
    return errors


def validate_result(data: Any, hardware_ids: Any, *,
                    expected_sha: str | None = None) -> list[str]:
    errors = _check_keys(data, RESULT_TOP_LEVEL_REQUIRED, "result")
    if not isinstance(data, dict):
        return errors
    result_sha = data.get("commitSha")
    errors.extend(_check_sha(result_sha, "commitSha", "result"))
    if expected_sha is not None:
        expected_errors = _check_sha(expected_sha, "expectedSha", "result")
        errors.extend(expected_errors)
        if (not expected_errors and isinstance(result_sha, str)
                and result_sha.lower() != expected_sha.lower()):
            errors.append(
                "result: commitSha does not exactly match externally supplied expectedSha"
            )
    errors.extend(_check_rfc3339(data.get("timestamp"), "timestamp", "result"))

    known_hardware = _normalise_id_set(hardware_ids)
    hardware_id = data.get("hardwareRowId")
    errors.extend(_check_string(
        hardware_id, "hardwareRowId", "result",
        max_length=128, identifier=True,
    ))
    if isinstance(hardware_id, str) and hardware_id not in known_hardware:
        errors.append(
            f"result: hardwareRowId={hardware_id!r} not in hardware rows"
        )

    measurements = data.get("measurements")
    errors.extend(_check_list(
        measurements, "measurements", "result", MAX_MEASUREMENTS,
    ))
    if not isinstance(measurements, list):
        return errors

    seen_ids: set[str] = set()
    for index, measurement in enumerate(measurements[:MAX_MEASUREMENTS]):
        context = f"result.measurements[{index}]"
        errors.extend(_check_keys(
            measurement, RESULT_MEASUREMENT_REQUIRED, context,
        ))
        if not isinstance(measurement, dict):
            continue
        metric_id = measurement.get("metricId")
        errors.extend(_check_string(
            metric_id, "metricId", context, max_length=128, identifier=True,
        ))
        if isinstance(metric_id, str) and metric_id:
            identity = metric_id.casefold()
            if identity in seen_ids:
                errors.append(f"{context}: duplicate measurement for {metric_id!r}")
            else:
                seen_ids.add(identity)
        value = measurement.get("value")
        if (not _is_finite_number(value)
                or not 0 <= value <= MAX_NUMERIC_VALUE):
            errors.append(
                f"{context}: value must be a finite number in "
                f"[0, {MAX_NUMERIC_VALUE:g}]"
            )
        errors.extend(_check_enum(
            measurement.get("unit"), VALID_UNITS, "unit", context,
        ))
        sample_count = measurement.get("sampleCount")
        if (not _is_strict_int(sample_count)
                or not 1 <= sample_count <= MAX_SAMPLE_COUNT):
            errors.append(
                f"{context}: sampleCount must be an integer in "
                f"[1, {MAX_SAMPLE_COUNT}]"
            )
    return errors


def validate_suite(budget_dir: Path) -> list[str]:
    """Validate all three files in a versioned budget directory."""
    directory_errors = validate_trusted_directory(budget_dir, "budget directory")
    if directory_errors:
        return directory_errors
    hardware_data, hardware_load_errors = load_bounded_json(
        budget_dir / "hardware.json", "hardware.json", trusted_root=budget_dir,
    )
    budget_data, budget_load_errors = load_bounded_json(
        budget_dir / "budget.json", "budget.json", trusted_root=budget_dir,
    )
    baselines_data, baseline_load_errors = load_bounded_json(
        budget_dir / "baselines.json", "baselines.json", trusted_root=budget_dir,
    )
    errors = hardware_load_errors + budget_load_errors + baseline_load_errors
    if errors:
        return errors

    errors.extend(validate_hardware(hardware_data))
    hardware_ids = frozenset(
        row.get("id") for row in (
            hardware_data.get("rows", [])[:MAX_HARDWARE_ROWS]
            if isinstance(hardware_data, dict)
            and isinstance(hardware_data.get("rows"), list)
            else []
        )
        if isinstance(row, dict) and isinstance(row.get("id"), str)
    )
    errors.extend(validate_budget(budget_data, hardware_ids))
    metric_lookup = {
        metric.get("id"): metric for metric in (
            budget_data.get("metrics", [])[:MAX_METRICS]
            if isinstance(budget_data, dict)
            and isinstance(budget_data.get("metrics"), list)
            else []
        )
        if isinstance(metric, dict) and isinstance(metric.get("id"), str)
    }
    errors.extend(validate_baselines(baselines_data, metric_lookup, hardware_ids))
    return errors


def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    if len(args) != 1:
        print("Usage: validate_budget.py <budget-dir>", file=sys.stderr)
        print("  e.g. validate_budget.py perf-budgets/v1", file=sys.stderr)
        return 2
    budget_dir = Path(args[0])
    errors = validate_suite(budget_dir)
    if errors:
        print(f"FAIL: {len(errors)} validation error(s):", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"OK: {budget_dir} passes all validations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
