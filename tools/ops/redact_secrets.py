#!/usr/bin/env python3
"""Bounded secret scanner and stdout-only redactor for operational artifacts."""

from __future__ import annotations

import argparse
import json
import os
import stat
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from fs_security import FileMetadata, FilesystemPolicyError, SecureRoot, validate_portable_filename
from secret_policy import SecretFinding, redact_text, scan_json_values, scan_payload
from strict_json import StrictJsonError, loads_strict


MAX_SCAN_FILE_BYTES = 32 * 1024 * 1024
MAX_SCAN_TOTAL_BYTES = 96 * 1024 * 1024
MAX_SCAN_FILES = 256
MAX_JSON_DEPTH = 16
MAX_JSON_ENTRIES = 4096
MAX_JSON_STRING_BYTES = 256 * 1024
SCAN_SECONDS = 15.0


@dataclass(frozen=True)
class ScanIssue:
    kind: str
    source: str
    detail: str


class ArtifactScanner:
    def __init__(self) -> None:
        self.findings: list[SecretFinding] = []
        self.errors: list[ScanIssue] = []
        self.total_bytes = 0
        self.file_count = 0
        self._deadline = time.monotonic() + SCAN_SECONDS

    def _error(self, source: str, detail: str) -> None:
        self.errors.append(ScanIssue("error", source, detail))

    def _consume(self, data: bytes, *, source: str, suffix: str) -> None:
        if time.monotonic() > self._deadline:
            self._error(source, "scan deadline elapsed before content inspection")
            return
        self.file_count += 1
        self.total_bytes += len(data)
        if self.file_count > MAX_SCAN_FILES:
            self._error(source, f"file count exceeds {MAX_SCAN_FILES}")
            return
        if self.total_bytes > MAX_SCAN_TOTAL_BYTES:
            self._error(source, f"aggregate content exceeds {MAX_SCAN_TOTAL_BYTES} bytes")
            return
        result = scan_payload(data, location=source, suffix=suffix)
        self.findings.extend(result.findings)
        for detail in result.errors:
            self._error(source, detail)

        if suffix == ".json":
            try:
                value = loads_strict(
                    data,
                    source=source,
                    max_bytes=MAX_SCAN_FILE_BYTES,
                    max_depth=MAX_JSON_DEPTH,
                    max_collection_entries=MAX_JSON_ENTRIES,
                    max_string_bytes=MAX_JSON_STRING_BYTES,
                )
            except StrictJsonError as exc:
                self._error(source, f"JSON cannot receive structured secret inspection: {exc}")
            else:
                self.findings.extend(scan_json_values(value, location=source))
        if time.monotonic() > self._deadline:
            self._error(source, "content inspection exceeded the scan deadline")

    def _scan_named_file(self, root: SecureRoot, name: str, display: str) -> tuple[bytes | None, FileMetadata | None]:
        try:
            data, metadata = root.read_file(name, max_bytes=MAX_SCAN_FILE_BYTES, deadline=self._deadline)
        except FilesystemPolicyError as exc:
            self.file_count += 1
            if exc.size is not None:
                self.total_bytes += exc.size
                if self.total_bytes > MAX_SCAN_TOTAL_BYTES:
                    self._error(display, f"aggregate content exceeds {MAX_SCAN_TOTAL_BYTES} bytes")
            self._error(display, str(exc))
            return None, None
        self._consume(data, source=display, suffix=Path(name).suffix.lower())
        return data, metadata

    def scan_file(self, path: Path) -> bytes | None:
        self._deadline = time.monotonic() + SCAN_SECONDS
        try:
            name = validate_portable_filename(path.name)
            with SecureRoot(path.parent or Path(".")) as root:
                data, _ = self._scan_named_file(root, name, str(path))
                return data
        except (FilesystemPolicyError, OSError) as exc:
            self._error(str(path), str(exc))
            return None

    def scan_directory(self, path: Path) -> None:
        self._deadline = time.monotonic() + SCAN_SECONDS
        try:
            with SecureRoot(path) as root:
                names_before = sorted(root.iter_names(max_entries=MAX_SCAN_FILES, deadline=self._deadline))
                file_identities: dict[str, FileMetadata] = {}
                for name in names_before:
                    _, metadata = self._scan_named_file(root, name, f"{path}{os.sep}{name}")
                    if metadata is not None:
                        file_identities[name] = metadata

                if time.monotonic() > self._deadline:
                    self._error(str(path), "scan deadline elapsed before re-enumeration")
                    return

                try:
                    names_after = sorted(root.iter_names(max_entries=MAX_SCAN_FILES, deadline=self._deadline))
                except FilesystemPolicyError as exc:
                    self._error(str(path), f"post-scan re-enumeration failed: {exc}")
                    return

                added = set(names_after) - set(names_before)
                removed = set(names_before) - set(names_after)
                if added:
                    self._error(str(path), f"concurrent addition detected: {sorted(added)[:5]}")
                if removed:
                    self._error(str(path), f"concurrent removal detected: {sorted(removed)[:5]}")

                for name in names_after:
                    if name in file_identities:
                        try:
                            _, current_meta = root.read_file(
                                name, max_bytes=MAX_SCAN_FILE_BYTES, deadline=self._deadline
                            )
                        except FilesystemPolicyError:
                            self._error(
                                f"{path}{os.sep}{name}",
                                "file became unreadable after scan (possible replacement)",
                            )
                            continue
                        before = file_identities[name]
                        if (
                            current_meta.device != before.device
                            or current_meta.file_id != before.file_id
                            or current_meta.mtime_ns != before.mtime_ns
                            or current_meta.size != before.size
                        ):
                            self._error(
                                f"{path}{os.sep}{name}",
                                "file identity/metadata changed after scan (concurrent replacement)",
                            )
        except (FilesystemPolicyError, OSError) as exc:
            self._error(str(path), str(exc))


def _lstat_kind(path: Path) -> tuple[str, os.stat_result | None]:
    try:
        info = os.lstat(path)
    except OSError:
        return "missing", None
    attributes = getattr(info, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    if stat.S_ISLNK(info.st_mode) or attributes & reparse:
        return "reparse", info
    if stat.S_ISREG(info.st_mode):
        return "file", info
    if stat.S_ISDIR(info.st_mode):
        return "directory", info
    return "other", info


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Scan operational artifacts for reusable secrets (OPS-100)")
    parser.add_argument("paths", nargs="+", help="Regular files or authoritative artifact roots")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable results without secret previews")
    parser.add_argument("--redact", action="store_true", help="Print a UTF-8 text file redacted; never modify it")
    args = parser.parse_args(argv)

    if args.redact and (len(args.paths) != 1 or args.json):
        parser.error("--redact requires exactly one file and cannot be combined with --json")

    scanner = ArtifactScanner()
    redact_data: bytes | None = None
    redact_path: Path | None = None
    for text_path in args.paths:
        path = Path(text_path)
        kind, _ = _lstat_kind(path)
        if kind == "file":
            data = scanner.scan_file(path)
            if args.redact:
                redact_data, redact_path = data, path
        elif kind == "directory":
            if args.redact:
                scanner._error(str(path), "--redact accepts a regular text file, not a directory")
            else:
                scanner.scan_directory(path)
        elif kind == "reparse":
            scanner._error(str(path), "symlink, junction, or reparse input is forbidden")
        elif kind == "missing":
            scanner._error(str(path), "input does not exist or cannot be inspected")
        else:
            scanner._error(str(path), "input is not a regular file or directory")

    if args.redact:
        if scanner.errors:
            pass
        elif redact_data is not None and redact_path is not None:
            if redact_path.suffix.lower() in {".zip", ".7z", ".rar", ".tar", ".gz", ".bz2", ".xz"}:
                scanner._error(str(redact_path), "archives cannot be emitted through text redaction")
            else:
                try:
                    text = redact_data.decode("utf-8", errors="strict")
                except UnicodeDecodeError:
                    scanner._error(str(redact_path), "--redact requires strict UTF-8 text")
                else:
                    redacted, _ = redact_text(text)
                    sys.stdout.write(redacted)

    if args.json:
        unique = sorted({(item.rule, item.location) for item in scanner.findings})
        json.dump(
            {
                "clean": not unique and not scanner.errors,
                "findings": [{"rule": rule, "location": location} for rule, location in unique],
                "errors": [issue.__dict__ for issue in scanner.errors],
                "files": scanner.file_count,
                "bytes": scanner.total_bytes,
            },
            sys.stdout,
            indent=2,
        )
        print()
    elif not args.redact:
        for rule, location in sorted({(item.rule, item.location) for item in scanner.findings}):
            print(f"[SECRET] {rule}: {location}", file=sys.stderr)
        for issue in scanner.errors:
            print(f"[ERROR] {issue.source}: {issue.detail}", file=sys.stderr)
        if not scanner.findings and not scanner.errors:
            print("No secrets detected in bounded artifact scan.")
    else:
        for issue in scanner.errors:
            print(f"[ERROR] {issue.source}: {issue.detail}", file=sys.stderr)

    if scanner.errors:
        return 2
    return 1 if scanner.findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
