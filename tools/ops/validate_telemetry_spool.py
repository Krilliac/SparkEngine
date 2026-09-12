#!/usr/bin/env python3
"""Read-only validation of telemetry spool output; makes no consent inference."""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from fs_security import FilesystemPolicyError, SecureRoot
from secret_policy import scan_json_values, scan_payload
from ops_strict_json import StrictJsonError, loads_strict


# Offline-tool policy limits. They are not current Telemetry.h runtime guarantees.
MAX_SPOOL_BYTES = 50 * 1024 * 1024
MAX_BATCH_BYTES = 1024 * 1024
MAX_SPOOL_FILES = 1000
MAX_RETENTION_SECONDS = 7 * 24 * 3600
MAX_EVENTS_PER_BATCH = 500
MAX_EVENT_PROPERTIES = 64
MAX_EVENT_NAME_BYTES = 256
MAX_SESSION_ID_BYTES = 256
MAX_PROPERTY_KEY_BYTES = 256
MAX_PROPERTY_VALUE_BYTES = 4096
MAX_JSON_DEPTH = 4
MAX_CLOCK_SKEW_SECONDS = 5 * 60
VALIDATION_SECONDS = 15.0
UINT64_MAX = 2**64 - 1

TELEMETRY_BATCH_PATTERN = re.compile(r"^telemetry_([0-9]{1,20})(?:_[0-9]{1,20}_[0-9]{1,20})?\.json$")
SESSION_ID_PATTERN = re.compile(r"^session_([0-9]{1,20})$")
EVENT_FIELDS = {"name", "timestamp", "sessionId", "properties", "sequence"}
REQUIRED_EVENT_FIELDS = {"name", "timestamp", "sessionId", "properties"}


@dataclass(frozen=True)
class SpoolError:
    check: str
    message: str
    severity: str = "error"

    def __str__(self) -> str:
        return f"[{self.severity.upper()}] {self.check}: {self.message}"


def _utf8_size(value: str) -> int:
    return len(value.encode("utf-8", errors="strict"))


def _contains_control(value: str) -> bool:
    return any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)


class TelemetrySpoolValidator:
    """Validate all immediate spool entries from one non-reparse root handle."""

    def __init__(self, *, now: float | None = None) -> None:
        self.errors: list[SpoolError] = []
        self.stats: dict[str, Any] = {"files": 0, "batch_files": 0, "total_bytes": 0}
        self._now = time.time() if now is None else now
        self._deadline = time.monotonic() + VALIDATION_SECONDS

    def _error(self, check: str, message: str) -> None:
        self.errors.append(SpoolError(check, message))

    def _validate_age(self, name: str, filename_ms: int, mtime_ns: int) -> None:
        filename_time = filename_ms / 1000.0
        mtime = mtime_ns / 1_000_000_000.0
        for label, observed in (("filename", filename_time), ("mtime", mtime)):
            age = self._now - observed
            if age > MAX_RETENTION_SECONDS:
                self._error("retention", f"{name}: {label} age exceeds {MAX_RETENTION_SECONDS} seconds")
            if age < -MAX_CLOCK_SKEW_SECONDS:
                self._error("future-time", f"{name}: {label} time is too far in the future")

    def validate_spool_directory(self, spool_dir: Path) -> None:
        self._deadline = time.monotonic() + VALIDATION_SECONDS
        total_bytes = 0
        try:
            with SecureRoot(spool_dir) as root:
                names = list(root.iter_names(max_entries=MAX_SPOOL_FILES, deadline=self._deadline))
                self.stats["files"] = len(names)
                for name in sorted(names):
                    match = TELEMETRY_BATCH_PATTERN.fullmatch(name)
                    if match is None:
                        self._error("unexpected-entry", f"{name!r}: non-batch entry is forbidden in the spool")
                    try:
                        data, metadata = root.read_file(name, max_bytes=MAX_BATCH_BYTES, deadline=self._deadline)
                    except FilesystemPolicyError as exc:
                        if exc.size is not None:
                            total_bytes += exc.size
                            if total_bytes > MAX_SPOOL_BYTES:
                                self._error("spool-size", f"spool exceeds offline-tool limit {MAX_SPOOL_BYTES} bytes")
                        self._error("batch-open", f"{name!r}: {exc}")
                        continue
                    total_bytes += metadata.size
                    if total_bytes > MAX_SPOOL_BYTES:
                        self._error("spool-size", f"spool exceeds offline-tool limit {MAX_SPOOL_BYTES} bytes")
                    if match is None:
                        # Unknown content is included in resource accounting but is not parsed as telemetry.
                        continue
                    self.stats["batch_files"] += 1
                    filename_ms = int(match.group(1))
                    if filename_ms > UINT64_MAX:
                        self._error("batch-name", f"{name!r}: timestamp exceeds uint64")
                    else:
                        self._validate_age(name, filename_ms, metadata.mtime_ns)
                    self.validate_batch_bytes(data, source=name)
        except (FilesystemPolicyError, OSError) as exc:
            self._error("spool-root", f"{spool_dir}: {exc}")
        self.stats["total_bytes"] = total_bytes
        self.stats["spool_dir"] = str(spool_dir)

    def validate_batch_bytes(self, data: bytes, *, source: str) -> None:
        if time.monotonic() > self._deadline:
            self._error("time-bound", f"{source}: validation deadline elapsed before JSON inspection")
            return
        try:
            events = loads_strict(
                data,
                source=source,
                max_bytes=MAX_BATCH_BYTES,
                max_depth=MAX_JSON_DEPTH,
                max_collection_entries=MAX_EVENTS_PER_BATCH,
                max_string_bytes=MAX_PROPERTY_VALUE_BYTES,
            )
        except StrictJsonError as exc:
            self._error("batch-json", str(exc))
            return
        if not isinstance(events, list):
            self._error("batch-type", f"{source}: batch root must be a JSON array")
            return
        if len(events) > MAX_EVENTS_PER_BATCH:
            self._error("batch-count", f"{source}: batch exceeds {MAX_EVENTS_PER_BATCH} events")
        session_ids: list[str] = []
        sequences: list[int] = []
        sequenced_events = 0
        for index, event in enumerate(events):
            self.validate_event(event, f"{source}[{index}]")
            if isinstance(event, dict):
                if isinstance(event.get("sessionId"), str):
                    session_ids.append(event["sessionId"])
                if "sequence" in event:
                    sequenced_events += 1
                    if type(event["sequence"]) is int and 1 <= event["sequence"] <= UINT64_MAX:
                        sequences.append(event["sequence"])
        if len(set(session_ids)) > 1:
            self._error("batch-session", f"{source}: a runtime batch must contain exactly one sessionId")
        if sequenced_events not in (0, len(events)):
            self._error("batch-sequence", f"{source}: sequence must be present on every event or no event")
        if sequenced_events == len(events) and len(sequences) == len(events):
            if any(current <= previous for previous, current in zip(sequences, sequences[1:])):
                self._error("batch-sequence", f"{source}: sequence values must be strictly increasing")
        for finding in scan_json_values(events, location=source):
            self._error("secret-in-event", f"{finding.location}: reusable secret matches {finding.rule}")
        raw = scan_payload(data, location=source, suffix=".json")
        for finding in raw.findings:
            self._error("secret-in-event", f"{finding.location}: reusable secret matches {finding.rule}")
        for error in raw.errors:
            self._error("binary-policy", error)
        if time.monotonic() > self._deadline:
            self._error("time-bound", f"{source}: JSON/secret inspection exceeded the validation deadline")

    def validate_event(self, event: Any, source: str) -> None:
        if not isinstance(event, dict):
            self._error("event-type", f"{source}: event must be an object")
            return
        missing = REQUIRED_EVENT_FIELDS - event.keys()
        unknown = event.keys() - EVENT_FIELDS
        if missing:
            self._error("event-fields", f"{source}: missing fields {sorted(missing)}")
        if unknown:
            self._error("event-fields", f"{source}: unknown fields {sorted(unknown)}")

        name = event.get("name")
        if not isinstance(name, str) or not name or _contains_control(name):
            self._error("event-name", f"{source}: name must be a non-empty control-free string")
        elif _utf8_size(name) > MAX_EVENT_NAME_BYTES:
            self._error("event-name", f"{source}: name exceeds {MAX_EVENT_NAME_BYTES} UTF-8 bytes")

        session_id = event.get("sessionId")
        if not isinstance(session_id, str) or not SESSION_ID_PATTERN.fullmatch(session_id):
            self._error("session-id", f"{source}: sessionId must match session_<epoch-ms>")
        elif _utf8_size(session_id) > MAX_SESSION_ID_BYTES:
            self._error("session-id", f"{source}: sessionId exceeds {MAX_SESSION_ID_BYTES} UTF-8 bytes")
        elif int(SESSION_ID_PATTERN.fullmatch(session_id).group(1)) > UINT64_MAX:  # type: ignore[union-attr]
            self._error("session-id", f"{source}: sessionId epoch exceeds uint64")

        timestamp = event.get("timestamp")
        if type(timestamp) is not int or not 0 <= timestamp <= UINT64_MAX:
            self._error("event-timestamp", f"{source}: timestamp must be a uint64 integer")
        else:
            age = self._now - timestamp / 1000.0
            if age > MAX_RETENTION_SECONDS:
                self._error("event-timestamp", f"{source}: timestamp exceeds the offline retention window")
            if age < -MAX_CLOCK_SKEW_SECONDS:
                self._error("event-timestamp", f"{source}: timestamp is too far in the future")
        if "sequence" in event:
            sequence = event["sequence"]
            if type(sequence) is not int or not 1 <= sequence <= UINT64_MAX:
                self._error("event-sequence", f"{source}: sequence must be a positive uint64 integer")

        properties = event.get("properties")
        if not isinstance(properties, dict):
            self._error("properties", f"{source}: properties must be an object")
            return
        if len(properties) > MAX_EVENT_PROPERTIES:
            self._error("properties", f"{source}: properties exceed {MAX_EVENT_PROPERTIES} entries")
        for key, value in properties.items():
            if not key or _contains_control(key) or _utf8_size(key) > MAX_PROPERTY_KEY_BYTES:
                self._error("property-key", f"{source}: property key is empty, unsafe, or oversized")
            if not isinstance(value, str):
                self._error("property-value", f"{source}: property {key!r} must be a string")
            elif _utf8_size(value) > MAX_PROPERTY_VALUE_BYTES:
                self._error("property-value", f"{source}: property {key!r} exceeds {MAX_PROPERTY_VALUE_BYTES} bytes")

    def has_errors(self) -> bool:
        return bool(self.errors)


def _input_is_reparse(path: Path) -> bool:
    try:
        info = os.lstat(path)
    except OSError:
        return False
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate a telemetry spool without inferring consent (OPS-100)")
    parser.add_argument("spool_dir", help="Authoritative telemetry spool root")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable findings")
    args = parser.parse_args(argv)

    validator = TelemetrySpoolValidator()
    path = Path(args.spool_dir)
    try:
        info = os.lstat(path)
    except OSError as exc:
        validator._error("input-open", f"{path}: {exc}")
    else:
        if _input_is_reparse(path):
            validator._error("input-reparse", f"{path}: symlink, junction, or reparse root is forbidden")
        elif not stat.S_ISDIR(info.st_mode):
            validator._error("input-type", f"{path}: input must be a directory")
        else:
            validator.validate_spool_directory(path)

    if args.json:
        json.dump(
            {
                "passed": not validator.has_errors(),
                "errors": [error.__dict__ for error in validator.errors],
                "stats": validator.stats,
                "consent_inferred": False,
                "offline_limits_are_runtime_guarantees": False,
            },
            sys.stdout,
            indent=2,
        )
        print()
    else:
        for error in validator.errors:
            print(error, file=sys.stderr)
        if not validator.has_errors():
            print(
                f"Telemetry spool validation passed: {validator.stats['batch_files']} batches, "
                f"{validator.stats['total_bytes']} bytes (offline tooling policy)."
            )
    return 1 if validator.has_errors() else 0


if __name__ == "__main__":
    raise SystemExit(main())
