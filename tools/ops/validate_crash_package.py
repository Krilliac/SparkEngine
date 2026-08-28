#!/usr/bin/env python3
"""Fail-closed, read-only validation of Spark crash packages."""

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

from fs_security import FilesystemPolicyError, SecureRoot, validate_portable_filename
from secret_policy import scan_json_values, scan_payload
from strict_json import StrictJsonError, loads_strict


# These five limits mirror CrashReporterApp.cpp.
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_JSON_STRING_BYTES = 256 * 1024
MAX_JSON_DEPTH = 16
MAX_COLLECTION_ENTRIES = 4096
MAX_READY_MANIFESTS = 32
MAX_CRASH_LOG_BYTES = 8 * 1024 * 1024

# Offline-tool resource limits. The current C++ runtime does not enforce them.
MAX_ARTIFACT_BYTES = 32 * 1024 * 1024
MAX_PACKAGE_BYTES = 96 * 1024 * 1024
MAX_DIRECTORY_ENTRIES = 256
VALIDATION_SECONDS = 15.0

READY_MANIFEST_PATTERN = re.compile(r"^crash_manifest_[0-9a-f]{16}\.json$")
MANIFEST_LIKE_PATTERN = re.compile(r"^crash_manifest_.*\.json$", re.IGNORECASE)
ARTIFACT_FIELDS = ("logFile", "dumpFile", "screenshotFile", "zipFile")
STRING_FIELDS = {
    "enginePID",
    "timestamp",
    "dumpFile",
    "logFile",
    "screenshotFile",
    "zipFile",
    "crashTitle",
    "uploadURL",
    "proxyURL",
    "githubRepo",
    "githubToken",
    "githubLabels",
    "smtpUser",
    "smtpPass",
    "emailTo",
    "emailFrom",
}
BOOLEAN_FIELDS = {
    "requireConsent",
    "allowScreenshotRefusal",
    "promptUserDescription",
    "fullMemoryDump",
}
TRANSPORT_FIELDS = {
    "uploadURL",
    "proxyURL",
    "githubRepo",
    "githubToken",
    "githubLabels",
    "smtpUser",
    "smtpPass",
    "emailTo",
    "emailFrom",
}


@dataclass(frozen=True)
class ValidationError:
    check: str
    message: str
    severity: str = "error"

    def __str__(self) -> str:
        return f"[{self.severity.upper()}] {self.check}: {self.message}"


class CrashPackageValidator:
    """Validate manifests and every referenced artifact from a pinned root."""

    def __init__(self, *, writer_output: bool = False, check_names: bool = False) -> None:
        self.errors: list[ValidationError] = []
        self.writer_output = writer_output
        self.check_names = check_names
        self._deadline = time.monotonic() + VALIDATION_SECONDS
        self._aggregate_bytes = 0
        self._cache: dict[str, bytes] = {}
        self._accounted_names: set[str] = set()

    def _error(self, check: str, message: str) -> None:
        self.errors.append(ValidationError(check, message))

    def _reset_package_state(self) -> None:
        self._deadline = time.monotonic() + VALIDATION_SECONDS
        self._aggregate_bytes = 0
        self._cache.clear()
        self._accounted_names.clear()

    def _read(self, root: SecureRoot, name: str, *, maximum: int, check: str) -> bytes | None:
        if name in self._cache:
            return self._cache[name]
        try:
            data, _ = root.read_file(name, max_bytes=maximum, deadline=self._deadline)
        except FilesystemPolicyError as exc:
            if exc.size is not None and name not in self._accounted_names:
                self._aggregate_bytes += exc.size
                self._accounted_names.add(name)
                if self._aggregate_bytes > MAX_PACKAGE_BYTES:
                    self._error(
                        "package-size",
                        f"validated entries exceed offline-tool limit {MAX_PACKAGE_BYTES} bytes",
                    )
            self._error(check, f"{name!r}: {exc}")
            return None
        if name not in self._accounted_names:
            self._aggregate_bytes += len(data)
            self._accounted_names.add(name)
        if self._aggregate_bytes > MAX_PACKAGE_BYTES:
            self._error(
                "package-size",
                f"validated content exceeds offline-tool limit {MAX_PACKAGE_BYTES} bytes",
            )
            return None
        self._cache[name] = data
        return data

    def validate_manifest_json(self, data: bytes, source: str = "<input>") -> dict[str, Any] | None:
        try:
            value = loads_strict(
                data,
                source=source,
                max_bytes=MAX_MANIFEST_BYTES,
                max_depth=MAX_JSON_DEPTH,
                max_collection_entries=MAX_COLLECTION_ENTRIES,
                max_string_bytes=MAX_JSON_STRING_BYTES,
            )
        except StrictJsonError as exc:
            self._error("manifest-json", str(exc))
            return None
        if not isinstance(value, dict):
            self._error("manifest-type", f"{source}: manifest root must be a JSON object")
            return None
        return value

    def validate_manifest_fields(self, manifest: dict[str, Any], source: str = "<input>") -> bool:
        valid = True
        for field in STRING_FIELDS:
            if field in manifest and not isinstance(manifest[field], str):
                self._error("field-type", f"{source}: {field!r} must be a string")
                valid = False
        for field in BOOLEAN_FIELDS:
            if field in manifest and type(manifest[field]) is not bool:
                self._error("field-type", f"{source}: {field!r} must be a boolean")
                valid = False
        if "timeoutSeconds" in manifest:
            timeout = manifest["timeoutSeconds"]
            if type(timeout) is not int or not -(2**31) <= timeout <= 2**31 - 1:
                self._error("field-type", f"{source}: 'timeoutSeconds' must be a signed 32-bit integer")
                valid = False
        log_name = manifest.get("logFile")
        if not isinstance(log_name, str) or not log_name:
            self._error("required-field", f"{source}: required string field 'logFile' is missing or empty")
            valid = False
        return valid

    def validate_no_transport_fields(self, manifest: dict[str, Any], source: str = "<input>") -> None:
        if not self.writer_output:
            return
        for field in sorted(TRANSPORT_FIELDS | {"timeoutSeconds", "artifactRoot"}):
            if field in manifest:
                self._error("writer-field", f"{source}: writer output must not contain legacy/local field {field!r}")

    def _validate_artifact_names(self, manifest: dict[str, Any], source: str) -> dict[str, str]:
        names: dict[str, str] = {}
        used: set[str] = set()
        for field in ARTIFACT_FIELDS:
            value = manifest.get(field)
            if value in (None, ""):
                continue
            if not isinstance(value, str):
                continue
            try:
                name = validate_portable_filename(value)
            except FilesystemPolicyError as exc:
                self._error("artifact-path", f"{source}: {field}: {exc}")
                continue
            alias = name.casefold()
            if alias in used:
                self._error("artifact-alias", f"{source}: duplicate/case-alias artifact filename {name!r}")
                continue
            used.add(alias)
            names[field] = name
        return names

    def _scan(self, data: bytes, *, source: str, suffix: str) -> None:
        if time.monotonic() > self._deadline:
            self._error("time-bound", f"{source}: validation deadline elapsed before secret inspection")
            return
        result = scan_payload(data, location=source, suffix=suffix)
        for finding in result.findings:
            self._error("secret-exposure", f"{finding.location}: reusable secret matches {finding.rule}")
        for error in result.errors:
            self._error("binary-policy", error)
        if time.monotonic() > self._deadline:
            self._error("time-bound", f"{source}: secret inspection exceeded the validation deadline")

    def _validate_manifest_from_root(self, root: SecureRoot, name: str) -> dict[str, Any] | None:
        if self.check_names and not READY_MANIFEST_PATTERN.fullmatch(name):
            self._error("manifest-name", f"{name!r} is not a canonical ready-manifest filename")
            return None
        data = self._read(root, name, maximum=MAX_MANIFEST_BYTES, check="manifest-open")
        if data is None:
            return None
        manifest = self.validate_manifest_json(data, name)
        if manifest is None:
            return None
        fields_valid = self.validate_manifest_fields(manifest, name)
        self.validate_no_transport_fields(manifest, name)
        for finding in scan_json_values(manifest, location=name):
            self._error("secret-exposure", f"{finding.location}: reusable secret matches {finding.rule}")
        if not fields_valid:
            return manifest

        for field, artifact_name in self._validate_artifact_names(manifest, name).items():
            maximum = MAX_CRASH_LOG_BYTES if field == "logFile" else MAX_ARTIFACT_BYTES
            artifact = self._read(root, artifact_name, maximum=maximum, check="artifact-open")
            if artifact is None:
                self._error("artifact-missing", f"{name}: referenced {field} {artifact_name!r} is unavailable")
                continue
            self._scan(artifact, source=artifact_name, suffix=Path(artifact_name).suffix.lower())
        return manifest

    def validate_manifest_file(self, path: Path) -> dict[str, Any] | None:
        self._reset_package_state()
        try:
            validate_portable_filename(path.name)
            with SecureRoot(path.parent or Path(".")) as root:
                return self._validate_manifest_from_root(root, path.name)
        except (FilesystemPolicyError, OSError) as exc:
            self._error("manifest-root", f"{path}: {exc}")
            return None

    def validate_written_manifest(self, path: Path) -> dict[str, Any] | None:
        previous = self.writer_output
        self.writer_output = True
        try:
            return self.validate_manifest_file(path)
        finally:
            self.writer_output = previous

    def validate_manifest_name(self, name: str) -> None:
        if not READY_MANIFEST_PATTERN.fullmatch(name):
            self._error("manifest-name", f"{name!r} is not a canonical ready-manifest filename")

    def validate_artifact_directory(self, directory: Path) -> None:
        self._reset_package_state()
        try:
            with SecureRoot(directory) as root:
                names = list(root.iter_names(max_entries=MAX_DIRECTORY_ENTRIES, deadline=self._deadline))
                manifest_names: list[str] = []
                for name in names:
                    if MANIFEST_LIKE_PATTERN.fullmatch(name):
                        if not READY_MANIFEST_PATTERN.fullmatch(name):
                            if self.check_names:
                                self._error("manifest-name", f"{name!r} is not a canonical ready-manifest filename")
                            continue
                        manifest_names.append(name)
                if len(manifest_names) > MAX_READY_MANIFESTS:
                    self._error(
                        "queue-bound",
                        f"{directory}: {len(manifest_names)} manifests exceed runtime bound {MAX_READY_MANIFESTS}",
                    )
                    manifest_names = manifest_names[:MAX_READY_MANIFESTS]
                for name in sorted(manifest_names):
                    self._validate_manifest_from_root(root, name)

                # Every otherwise-unreferenced file is still safely opened, bounded, and scanned.
                for name in sorted(names):
                    if name in self._cache:
                        continue
                    data = self._read(root, name, maximum=MAX_ARTIFACT_BYTES, check="package-entry")
                    if data is not None:
                        self._scan(data, source=name, suffix=Path(name).suffix.lower())
        except (FilesystemPolicyError, OSError) as exc:
            self._error("package-root", f"{directory}: {exc}")

    def has_errors(self) -> bool:
        return bool(self.errors)


def _is_reparse_or_link(path: Path) -> bool:
    try:
        info = os.lstat(path)
    except OSError:
        return False
    attributes = getattr(info, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return stat.S_ISLNK(info.st_mode) or bool(attributes & reparse)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate read-only crash packages (OPS-100)")
    parser.add_argument("paths", nargs="+", help="Manifest files or authoritative artifact roots")
    parser.add_argument("--writer-output", action="store_true", help="Reject fields the writer must not persist")
    parser.add_argument("--check-names", action="store_true", help="Check ready-manifest names in files and roots")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable findings")
    args = parser.parse_args(argv)

    validator = CrashPackageValidator(writer_output=args.writer_output, check_names=args.check_names)
    for text_path in args.paths:
        path = Path(text_path)
        try:
            info = os.lstat(path)
        except OSError as exc:
            validator._error("input-open", f"{path}: {exc}")
            continue
        if _is_reparse_or_link(path):
            validator._error("input-reparse", f"{path}: symlink, junction, or reparse input is forbidden")
        elif stat.S_ISDIR(info.st_mode):
            validator.validate_artifact_directory(path)
        elif stat.S_ISREG(info.st_mode):
            validator.validate_manifest_file(path)
        else:
            validator._error("input-type", f"{path}: input is not a regular file or directory")

    if args.json:
        json.dump(
            {
                "passed": not validator.has_errors(),
                "errors": [error.__dict__ for error in validator.errors],
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
            print("Crash-package validation passed (read-only tooling policy).")
    return 1 if validator.has_errors() else 0


if __name__ == "__main__":
    raise SystemExit(main())
