#!/usr/bin/env python3
"""Validate one sanitizer test run and write provenance-bound evidence.

This verifier is intentionally separate from the shell runner.  The runner
preserves the producer and capture exit codes; this program performs bounded,
machine-readable validation without trusting a successful process exit.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import os
import re
import stat
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any


MAX_CONSOLE_BYTES = 16 * 1024 * 1024
MAX_REPORT_BYTES = 16 * 1024 * 1024
MAX_JUNIT_BYTES = 16 * 1024 * 1024
MAX_JSON_BYTES = 1024 * 1024
MAX_JUNIT_TESTS = 10_000
# testsuites/testsuite/testcase plus, for waived and zero-assertion cases, a
# <properties> block of up to four <property> children. The ceiling covers one
# element per testcase plus the bounded flaky/empty metadata the ratchet caps.
MAX_JUNIT_ELEMENTS = 20_050
MAX_JUNIT_DEPTH = 5

# Marks that a testcase has already opened a <properties> block, so a second one
# is an error even when the first carried no recognised property.
PROPERTY_BLOCK_SENTINEL = "#properties"
MAX_JUNIT_CHARACTERS = MAX_JUNIT_BYTES
MAX_RUNTIME_LOGS = 32
MAX_RUNTIME_LOG_BYTES = 4 * 1024 * 1024
MAX_RUNTIME_TOTAL_BYTES = 16 * 1024 * 1024
MAX_EVIDENCE_ERRORS = 128
FRESHNESS_TOLERANCE_NS = 2_000_000_000
QUIESCENCE_SECONDS = 0.05

SANITIZER_PATTERNS = {
    "asan": re.compile(
        r"(?:ERROR:[ \t]*(?:Address|Leak)Sanitizer:|"
        r"SUMMARY:[ \t]*(?:Address|Leak)Sanitizer:|"
        r"AddressSanitizer:DEADLYSIGNAL|runtime error:)",
        re.IGNORECASE,
    ),
    "tsan": re.compile(
        r"(?:(?:WARNING|ERROR):[ \t]*ThreadSanitizer:|SUMMARY:[ \t]*ThreadSanitizer:)",
        re.IGNORECASE,
    ),
    "msan": re.compile(
        r"(?:(?:WARNING|ERROR):[ \t]*MemorySanitizer:|SUMMARY:[ \t]*MemorySanitizer:)",
        re.IGNORECASE,
    ),
}
ANY_SANITIZER_PATTERN = re.compile(
    r"(?:ERROR:[ \t]*(?:Address|Leak|Thread|Memory)Sanitizer:|"
    r"WARNING:[ \t]*(?:Thread|Memory)Sanitizer:|"
    r"SUMMARY:[ \t]*(?:Address|Leak|Thread|Memory)Sanitizer:|"
    r"AddressSanitizer:DEADLYSIGNAL|runtime error:)",
    re.IGNORECASE,
)
WARNING_PATTERN = re.compile(
    r"(?:^\[\s*WARN\s*\]|Known flaky|::warning title=Flaky test:)",
    re.IGNORECASE | re.MULTILINE,
)
FAILURE_PATTERN = re.compile(
    r"(?:^\[\s*FAILED\s*\]|^Tests:[^\n]*\b[1-9][0-9]* failed\b|"
    r"^Assertions:[^\n]*\b[1-9][0-9]* failed\b)",
    re.IGNORECASE | re.MULTILINE,
)
CRASH_PATTERN = re.compile(
    r"(?:Segmentation fault|core dumped|AddressSanitizer:DEADLYSIGNAL|"
    r"terminate called|uncaught exception|\bAborted\b)",
    re.IGNORECASE,
)
LAST_STARTED_TEST_PATTERN = re.compile(
    r"^\[[ \t]*RUN[ \t]*\][ \t]+([A-Za-z0-9_.:<>-]{1,120})[ \t]*$",
    re.MULTILINE,
)
INFRASTRUCTURE_PATTERN = re.compile(
    r"(?:command not found|No such file or directory|cannot execute|Permission denied|"
    r"failed to start process)",
    re.IGNORECASE,
)
XML_DECLARATION_PATTERN = re.compile(
    r"\A<\?xml[ \t\r\n]+version=(?P<version_quote>['\"])1\.0(?P=version_quote)"
    r"(?:[ \t\r\n]+encoding=(?P<encoding_quote>['\"])"
    r"(?P<encoding>[A-Za-z][A-Za-z0-9._-]*)(?P=encoding_quote))?"
    r"(?:[ \t\r\n]+standalone=(?P<standalone_quote>['\"])(?:yes|no)"
    r"(?P=standalone_quote))?[ \t\r\n]*\?>"
)


class BoundedErrors(list[str]):
    """Keep adversarial evidence from expanding metadata or diagnostics without bound."""

    def append(self, value: str) -> None:
        if len(self) < MAX_EVIDENCE_ERRORS:
            super().append(value)
        elif len(self) == MAX_EVIDENCE_ERRORS:
            super().append("additional evidence errors omitted at the configured ceiling")


FileIdentity = tuple[int, ...]


@dataclass(frozen=True)
class StableFile:
    """One bounded file read plus the identity which produced those bytes."""

    data: bytes
    identity: FileIdentity
    sha256: str


@dataclass(frozen=True)
class DirectorySnapshot:
    """The directory itself and every no-follow entry observed within it."""

    identity: FileIdentity
    entries: dict[str, FileIdentity]

    def __contains__(self, name: str) -> bool:
        return name in self.entries

    def __iter__(self):
        return iter(self.entries)

    def __len__(self) -> int:
        return len(self.entries)


@dataclass(frozen=True)
class RuntimeInspection:
    """Runtime-log bytes and the directory state from which they were derived."""

    results: list[dict[str, Any]]
    observations: dict[str, StableFile]
    snapshot: DirectorySnapshot | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--console", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--junit", required=True, type=Path)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--sanitizer", required=True, choices=sorted(SANITIZER_PATTERNS))
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", required=True)
    parser.add_argument("--job", required=True)
    parser.add_argument("--expected-selector", required=True, choices=("all",))
    parser.add_argument("--minimum-tests", required=True, type=int)
    parser.add_argument("--process-exit", required=True, type=int)
    parser.add_argument("--capture-exit", required=True, type=int)
    parser.add_argument("--signature-scan-status", required=True, type=int)
    parser.add_argument("--warning-scan-status", required=True, type=int)
    parser.add_argument("--failure-scan-status", required=True, type=int)
    parser.add_argument("--crash-scan-status", required=True, type=int)
    parser.add_argument("--infrastructure-scan-status", required=True, type=int)
    parser.add_argument("--started-ns", required=True, type=int)
    parser.add_argument("--timeout-seconds", required=True, type=int)
    parser.add_argument("--command-sha256", required=True)
    parser.add_argument("--timeout-marker", required=True, type=Path)
    parser.add_argument("--capture-overflow-marker", required=True, type=Path)
    parser.add_argument("--timeout-token", required=True)
    parser.add_argument("--capture-token", required=True)
    return parser.parse_args()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalized_absolute(path: Path) -> str:
    """Return a comparison form without following filesystem indirection."""

    return os.path.normcase(os.path.abspath(os.fspath(path)))


def validate_unindirected_path(
    path: Path,
    *,
    label: str,
    errors: list[str],
    leaf_may_be_missing: bool = False,
) -> bool:
    """Reject symlinks/reparse points in every existing lexical path component.

    Comparing the lexical and resolved forms catches ancestor junctions that a
    final-component ``lstat`` cannot see.  Walking the existing components makes
    the rejection explicit and covers platforms whose ``realpath`` preserves a
    subset of reparse-point spellings.
    """

    absolute = Path(os.path.abspath(os.fspath(path)))
    candidates = list(reversed(absolute.parents)) + [absolute]
    for index, candidate in enumerate(candidates):
        is_leaf = index == len(candidates) - 1
        if is_leaf and leaf_may_be_missing and not os.path.lexists(candidate):
            continue
        try:
            info = os.lstat(candidate)
        except OSError as exc:
            errors.append(f"{label}: path component cannot be inspected ({exc})")
            return False
        if is_link_or_junction(candidate, info):
            errors.append(f"{label}: path ancestry contains a symlink, junction, or reparse point")
            return False

    try:
        resolved = path.resolve(strict=not leaf_may_be_missing)
    except (OSError, RuntimeError) as exc:
        errors.append(f"{label}: path cannot be resolved safely ({exc})")
        return False
    if normalized_absolute(resolved) != normalized_absolute(absolute):
        errors.append(f"{label}: lexical and resolved paths disagree")
        return False
    return True


def validate_exact_child(
    path: Path,
    *,
    parent: Path,
    label: str,
    errors: list[str],
    leaf_may_be_missing: bool = False,
) -> bool:
    """Require a lexical direct child of an unindirected trusted directory."""

    if normalized_absolute(path.parent) != normalized_absolute(parent):
        errors.append(f"{label}: path is outside the exact evidence directory")
        return False
    return validate_unindirected_path(
        path,
        label=label,
        errors=errors,
        leaf_may_be_missing=leaf_may_be_missing,
    )


def is_link_or_junction(path: Path, info: os.stat_result | None = None) -> bool:
    """Return true for any POSIX link or Windows reparse-point indirection."""

    if info is None:
        info = os.lstat(path)
    if stat.S_ISLNK(info.st_mode):
        return True
    reparse_attribute = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    if getattr(info, "st_file_attributes", 0) & reparse_attribute:
        return True
    is_junction = getattr(os.path, "isjunction", None)
    return bool(is_junction and is_junction(path))


def file_identity(info: os.stat_result) -> FileIdentity:
    """Return fields which must remain stable while evidence is consumed."""

    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_nlink,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
        getattr(info, "st_file_attributes", 0),
        getattr(info, "st_reparse_tag", 0),
    )


def identities_match_for_open(before: os.stat_result, opened: os.stat_result) -> bool:
    """Match the path to its handle despite Windows' lazy creation-time refresh."""

    before_identity = file_identity(before)
    opened_identity = file_identity(opened)
    if os.name != "nt":
        return before_identity == opened_identity
    # Native Windows can refresh st_ctime (historically creation time there)
    # when a just-closed MSYS producer is first reopened.  Keep ctime in every
    # stable before/after snapshot, but do not mistake this one transition for
    # a different file.  Object, link, size, mtime, and reparse fields still
    # have to agree, and the double content read begins from the opened handle.
    return before_identity[:6] == opened_identity[:6] and before_identity[7:] == opened_identity[7:]


def validate_regular_file(
    path: Path, info: os.stat_result, *, label: str, errors: list[str]
) -> bool:
    """Require a no-follow regular file with no alternate hard-link name."""

    if is_link_or_junction(path, info) or not stat.S_ISREG(info.st_mode):
        errors.append(f"{label}: must be a regular non-symlink/non-reparse file")
        return False
    if info.st_nlink != 1:
        errors.append(f"{label}: must have exactly one hard link (found {info.st_nlink})")
        return False
    return True


def write_all(descriptor: int, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        written = os.write(descriptor, data[offset:])
        if written <= 0:
            raise OSError("short write")
        offset += written


def read_regular_file(
    path: Path,
    *,
    evidence_dir: Path,
    maximum: int,
    started_ns: int,
    errors: list[str],
) -> StableFile | None:
    try:
        if not validate_exact_child(
            path,
            parent=evidence_dir,
            label=path.name,
            errors=errors,
        ):
            return None
        before = os.lstat(path)
    except (OSError, RuntimeError) as exc:
        errors.append(f"{path.name}: missing or unreadable ({exc})")
        return None
    if not validate_regular_file(path, before, label=path.name, errors=errors):
        return None
    if before.st_size <= 0:
        errors.append(f"{path.name}: file is empty")
        return None
    if before.st_size > maximum:
        errors.append(f"{path.name}: file exceeds {maximum} bytes")
        return None
    if before.st_mtime_ns + FRESHNESS_TOLERANCE_NS < started_ns:
        errors.append(f"{path.name}: file predates this run")
        return None

    flags = (
        os.O_RDONLY
        | getattr(os, "O_BINARY", 0)
        | getattr(os, "O_NOFOLLOW", 0)
        | getattr(os, "O_NONBLOCK", 0)
        | getattr(os, "O_NOINHERIT", 0)
    )
    descriptor = -1
    try:
        descriptor = os.open(path, flags)
        opened = os.fstat(descriptor)
        if (
            not validate_regular_file(path, opened, label=path.name, errors=errors)
            or not identities_match_for_open(before, opened)
        ):
            errors.append(f"{path.name}: identity changed before it was opened")
            return None

        def read_once() -> bytes | None:
            os.lseek(descriptor, 0, os.SEEK_SET)
            chunks: list[bytes] = []
            total = 0
            while True:
                chunk = os.read(descriptor, min(64 * 1024, maximum + 1 - total))
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
                if total > maximum:
                    errors.append(f"{path.name}: file grew beyond {maximum} bytes while reading")
                    return None
            return b"".join(chunks)

        data = read_once()
        if data is None:
            return None

        after_fd = os.fstat(descriptor)
        after_path = os.lstat(path)
        if (
            file_identity(after_fd) != file_identity(opened)
            or file_identity(after_path) != file_identity(before)
            or is_link_or_junction(path, after_path)
            or not validate_exact_child(
                path,
                parent=evidence_dir,
                label=path.name,
                errors=errors,
            )
            or after_fd.st_nlink != 1
            or len(data) != opened.st_size
        ):
            errors.append(f"{path.name}: identity changed while it was read")
            return None

        # st_ctime_ns catches a same-size/restored-mtime rewrite on POSIX.  Windows
        # historically exposes creation time as st_ctime, so compare a second
        # bounded read as well.  The caller also rechecks this observation after
        # semantic validation to cover mutations between individual file reads.
        time.sleep(QUIESCENCE_SECONDS)
        second_data = read_once()
        final_fd = os.fstat(descriptor)
        final_path = os.lstat(path)
        if (
            second_data is None
            or second_data != data
            or file_identity(final_fd) != file_identity(opened)
            or file_identity(final_path) != file_identity(before)
            or is_link_or_junction(path, final_path)
            or not validate_exact_child(
                path,
                parent=evidence_dir,
                label=path.name,
                errors=errors,
            )
            or final_fd.st_nlink != 1
        ):
            errors.append(f"{path.name}: identity or content changed during stable read")
            return None
        return StableFile(data=data, identity=file_identity(opened), sha256=sha256_bytes(data))
    except OSError as exc:
        errors.append(f"{path.name}: cannot read ({exc})")
        return None
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def verify_stable_file(
    path: Path,
    expected: StableFile,
    *,
    evidence_dir: Path,
    maximum: int,
    started_ns: int,
    errors: list[str],
) -> None:
    """Reconsume a file after validation and require the same object and bytes."""

    current = read_regular_file(
        path,
        evidence_dir=evidence_dir,
        maximum=maximum,
        started_ns=started_ns,
        errors=errors,
    )
    if current is not None and (
        current.identity != expected.identity
        or current.sha256 != expected.sha256
        or current.data != expected.data
    ):
        errors.append(f"{path.name}: identity or content changed after it was consumed")


def parse_nonnegative_int(value: str | None, *, field: str, errors: list[str]) -> int | None:
    if value is None or not re.fullmatch(r"[0-9]+", value):
        errors.append(f"JUnit {field} is missing or not a non-negative integer")
        return None
    try:
        parsed = int(value)
    except (ValueError, OverflowError):
        errors.append(f"JUnit {field} is outside the supported integer range")
        return None
    if parsed > MAX_JUNIT_TESTS:
        errors.append(f"JUnit {field} exceeds the {MAX_JUNIT_TESTS} test ceiling")
        return None
    return parsed


def validate_time(value: str | None, *, field: str, errors: list[str]) -> float | None:
    if value is None:
        return 0.0
    try:
        parsed = float(value)
    except ValueError:
        errors.append(f"JUnit {field} is not numeric")
        return None
    if not math.isfinite(parsed) or parsed < 0:
        errors.append(f"JUnit {field} must be finite and non-negative")
        return None
    return parsed


def check_attributes(
    attributes: dict[str, str], *, allowed: set[str], field: str, errors: list[str]
) -> None:
    unexpected = sorted(set(attributes) - allowed)
    if unexpected:
        errors.append(f"JUnit {field} has unsupported attributes: {', '.join(unexpected)}")


def decode_junit_xml(data: bytes, *, errors: list[str]) -> str | None:
    """Decode one bounded XML entity before any parser can interpret markup.

    XML's byte-level encoding detection is handled explicitly so declarations
    cannot hide DTD/entity/comment markup in UTF-16.  The parser receives a
    Unicode string with the validated declaration removed; it therefore never
    gets an opportunity to select a second, conflicting decoder.
    """

    if not data:
        errors.append("JUnit XML is empty")
        return None
    if len(data) > MAX_JUNIT_BYTES:
        errors.append(f"JUnit exceeds the {MAX_JUNIT_BYTES} byte ceiling")
        return None

    utf32_boms = (b"\x00\x00\xfe\xff", b"\xff\xfe\x00\x00")
    utf32_prefixes = (b"\x00\x00\x00<", b"<\x00\x00\x00")
    if data.startswith(utf32_boms) or data.startswith(utf32_prefixes):
        errors.append("JUnit UTF-32 is outside the supported encoding contract")
        return None

    had_bom = False
    if data.startswith(b"\xef\xbb\xbf"):
        codec = "utf-8-sig"
        actual_encoding = "utf-8"
        had_bom = True
    elif data.startswith(b"\xff\xfe"):
        codec = "utf-16-le"
        actual_encoding = "utf-16-le"
        had_bom = True
        data = data[2:]
    elif data.startswith(b"\xfe\xff"):
        codec = "utf-16-be"
        actual_encoding = "utf-16-be"
        had_bom = True
        data = data[2:]
    elif len(data) >= 4 and data[0] == 0x3C and data[1] == 0 and data[3] == 0:
        codec = "utf-16-le"
        actual_encoding = "utf-16-le"
    elif len(data) >= 4 and data[0] == 0 and data[1] == 0x3C and data[2] == 0:
        codec = "utf-16-be"
        actual_encoding = "utf-16-be"
    else:
        codec = "utf-8"
        actual_encoding = "utf-8"

    try:
        text = data.decode(codec, errors="strict")
    except UnicodeDecodeError as exc:
        errors.append(f"JUnit has invalid {actual_encoding} bytes ({exc})")
        return None
    if len(text) > MAX_JUNIT_CHARACTERS:
        errors.append(f"JUnit exceeds the {MAX_JUNIT_CHARACTERS} character ceiling")
        return None
    if "\x00" in text:
        errors.append("JUnit contains forbidden NUL characters")
        return None

    declaration = None
    body = text
    if text.startswith("<?xml"):
        declaration = XML_DECLARATION_PATTERN.match(text)
        if declaration is None:
            errors.append("JUnit XML declaration is malformed or ambiguous")
            return None
        body = text[declaration.end() :]

    declared = declaration.group("encoding") if declaration is not None else None
    normalized_declared = declared.lower().replace("_", "-") if declared else None
    aliases = {
        "utf-8": {"utf-8", "utf8"},
        "utf-16-le": {"utf-16", "utf-16le", "utf-16-le"},
        "utf-16-be": {"utf-16", "utf-16be", "utf-16-be"},
    }
    if normalized_declared is not None and normalized_declared not in aliases[actual_encoding]:
        errors.append(
            f"JUnit XML declaration {declared!r} conflicts with detected {actual_encoding} bytes"
        )
        return None
    if actual_encoding.startswith("utf-16") and not had_bom:
        if normalized_declared not in {"utf-16le", "utf-16-le", "utf-16be", "utf-16-be"}:
            errors.append("BOM-less UTF-16 JUnit must declare an explicit byte order")
            return None

    if re.search(r"<!\s*DOCTYPE\b", body, re.IGNORECASE):
        errors.append("JUnit contains a forbidden DTD declaration")
        return None
    if re.search(r"<!\s*ENTITY\b", body, re.IGNORECASE):
        errors.append("JUnit contains a forbidden entity declaration")
        return None
    if re.search(r"<!--", body, re.IGNORECASE):
        errors.append("JUnit comments are outside the SparkTests schema")
        return None
    if re.search(r"<!\[CDATA\[", body, re.IGNORECASE):
        errors.append("JUnit CDATA is outside the SparkTests schema")
        return None
    if "<?" in body:
        errors.append("JUnit processing instructions are outside the SparkTests schema")
        return None
    if "<!" in body:
        errors.append("JUnit markup declarations are outside the SparkTests schema")
        return None
    return body


def parse_junit(
    data: bytes,
    *,
    minimum_tests: int,
    errors: list[str],
    collect_cases: bool = False,
) -> dict[str, Any]:
    """Parse the exact bounded tree emitted by SparkTests, not generic JUnit."""

    xml_text = decode_junit_xml(data, errors=errors)
    if xml_text is None:
        return {}

    root_counts: dict[str, int | None] = {}
    suite_counts: dict[str, int | None] = {}
    stack: list[str] = []
    case_names: set[str] = set()
    case_records: list[dict[str, Any]] = []
    case_outcomes: list[int] = []
    case_properties: list[set[str]] = []
    tests = 0
    failures = 0
    test_errors = 0
    skipped = 0
    # A waived (known-flaky) test is reported two ways across the shapes this
    # verifier has to accept: the historical `<skipped message="Known flaky:">`
    # and the current `<flakyFailure>`. They are counted separately because they
    # sit in different buckets of the declared totals - the historical form is
    # inside `skipped`, the current one is not - and the terminal Results
    # reconciliation has to subtract each from the bucket it actually occupies.
    flaky_skips = 0
    flaky_outcomes = 0
    empty_cases = 0
    suites = 0
    elements = 0

    try:
        for event, element in ET.iterparse(io.StringIO(xml_text), events=("start", "end")):
            if not isinstance(element.tag, str) or "}" in element.tag or ":" in element.tag:
                errors.append("JUnit namespaces and non-element nodes are unsupported")
                return {}
            tag = element.tag
            if event == "start":
                stack.append(tag)
                depth = len(stack)
                elements += 1
                if elements > MAX_JUNIT_ELEMENTS:
                    errors.append(f"JUnit exceeds the {MAX_JUNIT_ELEMENTS} element ceiling")
                    return {}
                if depth > MAX_JUNIT_DEPTH:
                    errors.append(f"JUnit exceeds the depth ceiling of {MAX_JUNIT_DEPTH}")
                    return {}

                if depth == 1:
                    if tag != "testsuites":
                        errors.append("JUnit root must be exactly testsuites")
                        return {}
                    check_attributes(
                        element.attrib,
                        allowed={"tests", "failures", "errors", "skipped", "flaky", "empty", "time"},
                        field="testsuites",
                        errors=errors,
                    )
                    root_counts = {
                        name: parse_nonnegative_int(
                            element.attrib.get(name, "0" if name == "errors" else None),
                            field=f"testsuites.{name}",
                            errors=errors,
                        )
                        for name in ("tests", "failures", "errors", "skipped")
                    }
                    root_counts.update(
                        {
                            name: (
                                None
                                if element.attrib.get(name) is None
                                else parse_nonnegative_int(
                                    element.attrib[name], field=f"testsuites.{name}", errors=errors
                                )
                            )
                            for name in ("flaky", "empty")
                        }
                    )
                    validate_time(element.attrib.get("time"), field="testsuites.time", errors=errors)
                elif depth == 2:
                    if stack[-2] != "testsuites" or tag != "testsuite":
                        errors.append("JUnit testsuites may contain only one direct testsuite")
                        return {}
                    suites += 1
                    if suites > 1 or element.attrib.get("name") != "SparkEngine":
                        errors.append("JUnit must contain exactly one SparkEngine testsuite")
                    check_attributes(
                        element.attrib,
                        allowed={"name", "tests", "failures", "errors", "skipped", "flaky", "empty", "time"},
                        field="testsuite",
                        errors=errors,
                    )
                    suite_counts = {
                        name: parse_nonnegative_int(
                            element.attrib.get(name, "0" if name == "errors" else None),
                            field=f"testsuite.{name}",
                            errors=errors,
                        )
                        for name in ("tests", "failures", "errors", "skipped")
                    }
                    suite_counts.update(
                        {
                            name: (
                                None
                                if element.attrib.get(name) is None
                                else parse_nonnegative_int(
                                    element.attrib[name], field=f"testsuite.{name}", errors=errors
                                )
                            )
                            for name in ("flaky", "empty")
                        }
                    )
                    validate_time(element.attrib.get("time"), field="testsuite.time", errors=errors)
                elif depth == 3:
                    if stack[-2] != "testsuite" or tag != "testcase":
                        errors.append("JUnit testsuite may contain only direct testcase elements")
                        return {}
                    check_attributes(
                        element.attrib,
                        allowed={"name", "time"},
                        field="testcase",
                        errors=errors,
                    )
                    name = element.attrib.get("name", "")
                    if not name.strip() or len(name) > 512:
                        errors.append("JUnit testcase identity must be non-empty and at most 512 characters")
                    elif name in case_names:
                        errors.append(f"JUnit testcase identity is duplicated: {name!r}")
                    else:
                        case_names.add(name)
                    case_time = validate_time(
                        element.attrib.get("time"), field="testcase.time", errors=errors
                    )
                    case_records.append(
                        {
                            "name": name,
                            "durationSeconds": round(case_time or 0.0, 6),
                            "_rawDuration": case_time or 0.0,
                        }
                    )
                    tests += 1
                    if tests > MAX_JUNIT_TESTS:
                        errors.append(f"JUnit exceeds the {MAX_JUNIT_TESTS} testcase ceiling")
                        return {}
                    case_outcomes.append(0)
                    case_properties.append(set())
                elif depth == 4:
                    if stack[-2] != "testcase" or tag not in {
                        "failure",
                        "error",
                        "skipped",
                        "flakyFailure",
                        "properties",
                    }:
                        errors.append(
                            "JUnit testcase may contain only one failure, error, skipped, or flakyFailure "
                            "outcome and an optional properties block"
                        )
                        return {}
                    check_attributes(
                        element.attrib,
                        allowed=set() if tag == "properties" else {"message", "type"},
                        field=tag,
                        errors=errors,
                    )
                    if tag == "properties":
                        # A properties block is metadata, not an outcome: it
                        # accompanies the outcome element rather than replacing
                        # it, so it must not consume the one-outcome budget.
                        if PROPERTY_BLOCK_SENTINEL in case_properties[-1]:
                            errors.append("JUnit testcase contains multiple properties blocks")
                        case_properties[-1].add(PROPERTY_BLOCK_SENTINEL)
                    else:
                        case_outcomes[-1] += 1
                        if case_outcomes[-1] > 1:
                            errors.append("JUnit testcase contains multiple outcome elements")
                        if tag == "failure":
                            failures += 1
                        elif tag == "error":
                            test_errors += 1
                        elif tag == "flakyFailure":
                            flaky_outcomes += 1
                        else:
                            skipped += 1
                            # The runner emitted waived tests as a Known-flaky
                            # <skipped> before the flakyFailure shape landed;
                            # both are still recognised so archived evidence
                            # cannot silently reclassify as a genuine skip.
                            if (element.attrib.get("message") or "").startswith("Known flaky:"):
                                flaky_skips += 1
                elif depth == 5:
                    if stack[-2] != "properties" or tag != "property":
                        errors.append("JUnit properties may contain only property elements")
                        return {}
                    check_attributes(
                        element.attrib,
                        allowed={"name", "value"},
                        field="property",
                        errors=errors,
                    )
                    property_name = element.attrib.get("name", "")
                    if property_name not in {"flaky", "flaky-reason", "waived-assertions", "empty"}:
                        errors.append(f"JUnit property name is outside the SparkTests schema: {property_name!r}")
                    elif property_name in case_properties[-1]:
                        errors.append(f"JUnit testcase repeats the {property_name!r} property")
                    else:
                        case_properties[-1].add(property_name)
                        if property_name == "empty" and element.attrib.get("value") == "true":
                            empty_cases += 1
            else:
                if not stack or stack[-1] != tag:
                    errors.append("JUnit element nesting is inconsistent")
                    return {}
                if tag in {"testsuites", "testsuite", "testcase", "properties"} and (element.text or "").strip():
                    errors.append(f"JUnit {tag} contains unexpected direct text")
                if (element.tail or "").strip():
                    errors.append(f"JUnit {tag} contains unexpected tail text")
                if tag == "testcase":
                    case_outcomes.pop()
                    case_properties.pop()
                stack.pop()
                element.clear()
    except ET.ParseError as exc:
        errors.append(f"JUnit is not parseable XML ({exc})")
        return {}

    if stack or suites != 1:
        errors.append("JUnit must contain one complete SparkEngine testsuite")
    if tests < minimum_tests:
        errors.append(f"JUnit contains {tests} testcases; expected at least {minimum_tests}")

    actual = {"tests": tests, "failures": failures, "errors": test_errors, "skipped": skipped}
    # flaky/empty are declared only by the current runner shape, so they are
    # reconciled separately: absent means "old evidence", not "zero". Declared
    # counts that disagree with the elements actually present are the exact
    # shape a silently reclassified waiver would take, so they are errors.
    declared_derived = {"flaky": flaky_outcomes, "empty": empty_cases}
    for scope, declared in (("testsuites", root_counts), ("testsuite", suite_counts)):
        for name, count in actual.items():
            if declared.get(name) is not None and declared.get(name) != count:
                errors.append(
                    f"JUnit {scope}.{name} declares {declared.get(name)} but contains {count}"
                )
        for name, count in declared_derived.items():
            if declared.get(name) is not None and declared.get(name) != count:
                errors.append(
                    f"JUnit {scope}.{name} declares {declared.get(name)} but contains {count}"
                )

    result: dict[str, Any] = {
        **actual,
        # Waivers reported through either shape are one population; the buckets
        # they occupy differ, so both are published for the terminal-Results
        # reconciliation to subtract from the right total.
        "knownFlakyWarnings": flaky_skips + flaky_outcomes,
        "flakyOutcomes": flaky_outcomes,
        "flakySkips": flaky_skips,
        "empty": empty_cases,
        "suiteNames": ["SparkEngine"] if suites == 1 else [],
    }
    if collect_cases:
        result["_cases"] = case_records
    return result


def completion_count(
    text: str, *, label: str, errors: list[str]
) -> tuple[int | None, int | None, int | None, dict[str, int] | None]:
    running_matches = re.findall(r"^Running ([0-9]+) tests\.\.\.$", text, re.MULTILINE)
    result_matches = re.findall(
        r"^Tests:[ \t]+([0-9]+) passed,[ \t]+([0-9]+) failed"
        r"(?:,[ \t]+([0-9]+) warned)?(?:,[ \t]+([0-9]+) skipped)?"
        r",[ \t]+([0-9]+) total$",
        text,
        re.MULTILINE,
    )
    seed_matches = re.findall(r"^Shuffle seed:[ \t]+([0-9]+)$", text, re.MULTILINE)
    if len(running_matches) != 1:
        errors.append(f"{label}: expected exactly one Running test-count marker")
    if len(result_matches) != 1 or text.count("=== Results ===") != 1:
        errors.append(f"{label}: missing unique terminal Results marker")
    if seed_matches != ["123"]:
        errors.append(f"{label}: expected exactly one Shuffle seed: 123 marker")
    if re.search(r"(?im)^Retry policy:|^Retries:|^\[\s*RETRY\s*\]", text):
        errors.append(f"{label}: retry evidence is forbidden in required sanitizer lanes")
    running = None
    if len(running_matches) == 1:
        if len(running_matches[0]) > 5 or int(running_matches[0]) > MAX_JUNIT_TESTS:
            errors.append(f"{label}: Running test count exceeds the supported ceiling")
        else:
            running = int(running_matches[0])
    summary: dict[str, int] | None = None
    total = None
    if len(result_matches) == 1:
        passed, failed, warned, skipped, total_text = result_matches[0]
        raw_counts = (passed, failed, warned or "0", skipped or "0", total_text)
        if any(len(value) > 5 for value in raw_counts):
            errors.append(f"{label}: terminal Results count exceeds the supported ceiling")
        else:
            values = tuple(int(value) for value in raw_counts)
            if any(value > MAX_JUNIT_TESTS for value in values):
                errors.append(f"{label}: terminal Results count exceeds the supported ceiling")
            else:
                summary = dict(zip(("passed", "failed", "warned", "skipped"), values[:4]))
                total = values[4]
                if sum(summary.values()) != total:
                    errors.append(f"{label}: terminal Results arithmetic is inconsistent")
    seed = 123 if seed_matches == ["123"] else None
    return running, total, seed, summary


def verify_scan(status: int, present: bool, *, name: str, errors: list[str]) -> None:
    if status > 1 or status < 0:
        errors.append(f"{name} scanner failed with status {status}")
    elif (status == 0) != present:
        errors.append(f"{name} scanner result disagrees with verified content")


def snapshot_directory(
    path: Path, *, maximum_entries: int, label: str, errors: list[str]
) -> DirectorySnapshot | None:
    """Take a bounded, no-follow snapshot without materializing an unbounded directory."""

    snapshot: dict[str, FileIdentity] = {}
    try:
        if not validate_unindirected_path(path, label=label, errors=errors):
            return None
        before = os.lstat(path)
        if is_link_or_junction(path, before) or not stat.S_ISDIR(before.st_mode):
            errors.append(f"{label} must be a regular non-symlink/non-reparse directory")
            return None
        with os.scandir(path) as entries:
            for index, entry in enumerate(entries):
                if index >= maximum_entries:
                    errors.append(f"{label} contains more than {maximum_entries} entries")
                    return None
                try:
                    # DirEntry.stat() may return a zeroed inode/device/link count
                    # from cached WIN32_FIND_DATA.  os.stat(..., follow_symlinks=False)
                    # opens the entry and supplies the identity fields required by
                    # the hard-link and race checks on Windows.
                    info = os.stat(entry.path, follow_symlinks=False)
                except OSError as exc:
                    errors.append(f"{label} entry {entry.name!r} cannot be inspected ({exc})")
                    return None
                entry_path = Path(entry.path)
                if not validate_exact_child(
                    entry_path,
                    parent=path,
                    label=f"{label} entry {entry.name!r}",
                    errors=errors,
                ):
                    continue
                if is_link_or_junction(entry_path, info):
                    errors.append(
                        f"{label} entry {entry.name!r} must not be a symlink, junction, or reparse point"
                    )
                elif stat.S_ISREG(info.st_mode) and info.st_nlink != 1:
                    errors.append(
                        f"{label} entry {entry.name!r} must have exactly one hard link "
                        f"(found {info.st_nlink})"
                    )
                elif not stat.S_ISREG(info.st_mode) and not stat.S_ISDIR(info.st_mode):
                    errors.append(f"{label} entry {entry.name!r} has an unsupported file type")
                snapshot[entry.name] = file_identity(info)
        after = os.lstat(path)
        if (
            is_link_or_junction(path, after)
            or not stat.S_ISDIR(after.st_mode)
            or file_identity(after) != file_identity(before)
            or not validate_unindirected_path(path, label=label, errors=errors)
        ):
            errors.append(f"{label} identity changed while it was enumerated")
            return None
    except OSError as exc:
        errors.append(f"{label} cannot be enumerated ({exc})")
        return None
    return DirectorySnapshot(identity=file_identity(after), entries=snapshot)


def inspect_runtime_logs(
    runtime_dir: Path,
    *,
    sanitizer: str,
    started_ns: int,
    errors: list[str],
) -> RuntimeInspection:
    try:
        runtime_info = os.lstat(runtime_dir)
    except OSError as exc:
        errors.append(f"runtime directory is missing or unreadable ({exc})")
        return RuntimeInspection([], {}, None)
    if is_link_or_junction(runtime_dir, runtime_info) or not stat.S_ISDIR(runtime_info.st_mode):
        errors.append("runtime path must be a regular non-symlink directory")
        return RuntimeInspection([], {}, None)
    if os.name != "nt" and runtime_info.st_mode & 0o077:
        errors.append("runtime directory is not private (group/other permissions are set)")

    before = snapshot_directory(
        runtime_dir,
        maximum_entries=MAX_RUNTIME_LOGS + 1,
        label="runtime directory",
        errors=errors,
    )
    if before is None:
        return RuntimeInspection([], {}, None)
    if len(before) > MAX_RUNTIME_LOGS:
        errors.append(f"runtime log count exceeds {MAX_RUNTIME_LOGS}")
        return RuntimeInspection([], {}, before)

    results: list[dict[str, Any]] = []
    observations: dict[str, StableFile] = {}
    total_size = 0
    name_pattern = re.compile(r"sanitizer\.([1-9][0-9]*)")
    for name in sorted(before):
        entry = runtime_dir / name
        match = name_pattern.fullmatch(name)
        if not match:
            errors.append(f"runtime entry {name!r} does not match the private prefix")
            continue
        try:
            info = os.lstat(entry)
        except OSError as exc:
            errors.append(f"runtime entry {name!r} cannot be inspected ({exc})")
            continue
        if is_link_or_junction(entry, info) or not stat.S_ISREG(info.st_mode):
            errors.append(f"runtime entry {name!r} must be a regular non-symlink file")
            continue
        if info.st_size <= 0:
            errors.append(f"runtime entry {name!r} is empty")
            continue
        if info.st_size > MAX_RUNTIME_LOG_BYTES:
            errors.append(f"runtime entry {name!r} exceeds {MAX_RUNTIME_LOG_BYTES} bytes")
            continue
        if info.st_mtime_ns + FRESHNESS_TOLERANCE_NS < started_ns:
            errors.append(f"runtime entry {name!r} predates this run")
            continue
        total_size += info.st_size
        if total_size > MAX_RUNTIME_TOTAL_BYTES:
            errors.append(f"runtime logs exceed {MAX_RUNTIME_TOTAL_BYTES} total bytes")
            continue
        observation = read_regular_file(
            entry,
            evidence_dir=runtime_dir,
            maximum=MAX_RUNTIME_LOG_BYTES,
            started_ns=started_ns,
            errors=errors,
        )
        if observation is None:
            continue
        observations[name] = observation
        data = observation.data
        text = data.decode("utf-8", errors="replace")
        if not SANITIZER_PATTERNS[sanitizer].search(text):
            errors.append(f"runtime entry {name!r} lacks a parseable {sanitizer} signature")
            continue
        results.append(
            {
                "name": name,
                "pid": int(match.group(1)),
                "bytes": len(data),
                "sha256": sha256_bytes(data),
            }
        )

    time.sleep(QUIESCENCE_SECONDS)
    after = snapshot_directory(
        runtime_dir,
        maximum_entries=MAX_RUNTIME_LOGS + 1,
        label="runtime directory",
        errors=errors,
    )
    if after is not None and after != before:
        errors.append("runtime directory changed while evidence was verified")
    for name, observation in observations.items():
        verify_stable_file(
            runtime_dir / name,
            observation,
            evidence_dir=runtime_dir,
            maximum=MAX_RUNTIME_LOG_BYTES,
            started_ns=started_ns,
            errors=errors,
        )
    sealed = snapshot_directory(
        runtime_dir,
        maximum_entries=MAX_RUNTIME_LOGS + 1,
        label="runtime directory",
        errors=errors,
    )
    if after is not None and sealed is not None and sealed != after:
        errors.append("runtime directory changed during final content verification")
    return RuntimeInspection(results, observations, sealed or after or before)


def verify_runtime_inspection(
    runtime_dir: Path,
    inspection: RuntimeInspection,
    *,
    started_ns: int,
    errors: list[str],
) -> None:
    """Rebind runtime-log semantics to unchanged bytes through final use."""

    for name, observation in inspection.observations.items():
        verify_stable_file(
            runtime_dir / name,
            observation,
            evidence_dir=runtime_dir,
            maximum=MAX_RUNTIME_LOG_BYTES,
            started_ns=started_ns,
            errors=errors,
        )
    current = snapshot_directory(
        runtime_dir,
        maximum_entries=MAX_RUNTIME_LOGS + 1,
        label="runtime directory",
        errors=errors,
    )
    if inspection.snapshot is not None and current is not None and current != inspection.snapshot:
        errors.append("runtime directory changed after its evidence was consumed")


def write_json_exclusive(path: Path, value: dict[str, Any]) -> bytes:
    payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(payload) > MAX_JSON_BYTES:
        raise OSError("metadata exceeds its size ceiling")
    path_errors: list[str] = []
    if not validate_unindirected_path(
        path,
        label="metadata.json",
        errors=path_errors,
        leaf_may_be_missing=True,
    ):
        raise OSError("; ".join(path_errors))
    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_BINARY", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
            opened = os.fstat(stream.fileno())
            path_info = os.lstat(path)
            if (
                not validate_regular_file(
                    path,
                    opened,
                    label="metadata.json",
                    errors=path_errors,
                )
                or not identities_match_for_open(path_info, opened)
                or opened.st_size != len(payload)
                or not validate_unindirected_path(
                    path,
                    label="metadata.json",
                    errors=path_errors,
                )
            ):
                raise OSError("; ".join(path_errors) or "metadata identity changed while writing")
    except Exception:
        try:
            os.unlink(path)
        except OSError:
            pass
        raise
    return payload


def strict_json(data: bytes, *, label: str, errors: list[str]) -> Any:
    def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate object key {key!r}")
            result[key] = value
        return result

    def invalid_constant(value: str) -> None:
        raise ValueError(f"invalid numeric constant {value!r}")

    try:
        return json.loads(
            data.decode("utf-8"),
            object_pairs_hook=object_pairs,
            parse_constant=invalid_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
        errors.append(f"{label} is not strict JSON ({exc})")
        return None


def require_exact_keys(
    value: Any, expected: set[str], *, label: str, errors: list[str]
) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        errors.append(f"{label} must be a JSON object")
        return None
    actual = set(value)
    if actual != expected:
        errors.append(
            f"{label} keys differ: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )
        return None
    return value


def exact_json_value(actual: Any, expected: Any) -> bool:
    """Compare JSON scalars without Python's bool/int equality ambiguity."""

    if expected is None:
        return actual is None
    return type(actual) is type(expected) and actual == expected


def marker_state(
    path: Path,
    *,
    evidence_dir: Path,
    started_ns: int,
    expected: bytes,
    label: str,
    errors: list[str],
) -> tuple[bool, StableFile | None]:
    if not validate_exact_child(
        path,
        parent=evidence_dir,
        label=label,
        errors=errors,
        leaf_may_be_missing=True,
    ):
        return False, None
    if not os.path.lexists(path):
        return False, None
    observation = read_regular_file(
        path,
        evidence_dir=evidence_dir,
        maximum=512,
        started_ns=started_ns,
        errors=errors,
    )
    if observation is None:
        return False, None
    if observation.data != expected:
        errors.append(f"{label} has invalid wrapper authentication")
        return False, observation
    return True, observation


def check_directory_names(
    snapshot: DirectorySnapshot | None,
    *,
    allowed: set[str],
    required: set[str],
    label: str,
    errors: list[str],
) -> None:
    if snapshot is None:
        return
    actual = set(snapshot)
    unexpected = actual - allowed
    missing = required - actual
    if unexpected:
        errors.append(f"{label} has unexpected entries: {', '.join(sorted(unexpected))}")
    if missing:
        errors.append(f"{label} is missing required entries: {', '.join(sorted(missing))}")


def capture_main(argv: list[str]) -> int:
    """Bound both the persisted console and emitted job-log stream while draining stdin."""

    parser = argparse.ArgumentParser(description="bounded sanitizer console capture")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--overflow-marker", required=True, type=Path)
    parser.add_argument("--marker-token", required=True)
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-f]{64}", args.marker_token):
        print("error: invalid capture marker token", file=sys.stderr)
        return 2
    if args.output.parent != args.overflow_marker.parent:
        print("error: capture paths must share one directory", file=sys.stderr)
        return 2

    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_BINARY", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = -1
    overflow = False
    total = 0
    try:
        descriptor = os.open(args.output, flags, 0o600)
        while True:
            chunk = sys.stdin.buffer.read(64 * 1024)
            if not chunk:
                break
            remaining = max(0, MAX_CONSOLE_BYTES - total)
            kept = chunk[:remaining]
            if kept:
                write_all(descriptor, kept)
                write_all(sys.stdout.fileno(), kept)
                total += len(kept)
            if len(chunk) > remaining:
                overflow = True
        os.fsync(descriptor)
    except (BrokenPipeError, OSError) as exc:
        print(f"error: bounded console capture failed: {exc}", file=sys.stderr)
        return 75
    finally:
        if descriptor >= 0:
            os.close(descriptor)

    if overflow:
        marker = f"capture:{args.marker_token}\n".encode("ascii")
        marker_fd = -1
        try:
            marker_fd = os.open(args.overflow_marker, flags, 0o600)
            write_all(marker_fd, marker)
            os.fsync(marker_fd)
        except OSError as exc:
            print(f"error: cannot authenticate capture overflow: {exc}", file=sys.stderr)
            return 75
        finally:
            if marker_fd >= 0:
                os.close(marker_fd)
        print(
            f"error: sanitizer console exceeded {MAX_CONSOLE_BYTES} bytes and was truncated",
            file=sys.stderr,
        )
        return 74
    return 0


def parse_footer(data: bytes, *, errors: list[str]) -> dict[str, str]:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        errors.append(f"process footer is not UTF-8 ({exc})")
        return {}
    lines = text.splitlines()
    if not lines or lines[0] != "=== SparkEngine CI sanitizer process result ===":
        errors.append("process footer has an invalid or missing header")
        return {}
    fields: dict[str, str] = {}
    for line in lines[1:]:
        if "=" not in line:
            errors.append("process footer contains a malformed line")
            continue
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[a-z][a-z0-9_]*", key) or key in fields:
            errors.append(f"process footer contains duplicate/invalid field {key!r}")
            continue
        fields[key] = value
    expected = {
        "commit_sha",
        "run_id",
        "run_attempt",
        "job",
        "sanitizer",
        "expected_selector",
        "minimum_tests",
        "timeout_seconds",
        "test_exit_code",
        "capture_exit_code",
        "effective_exit_code",
        "signal_received",
        "evidence_classification",
        "metadata_file",
        "metadata_sha256",
        "command_sha256",
        "completed_utc",
    }
    if set(fields) != expected:
        errors.append(
            "process footer fields differ: "
            f"missing={sorted(expected - set(fields))}, "
            f"unexpected={sorted(set(fields) - expected)}"
        )
    return fields


def published_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="verify published sanitizer artifact evidence")
    parser.add_argument("--evidence-dir", required=True, type=Path)
    parser.add_argument("--stats", required=True, type=Path)
    parser.add_argument("--sanitizer", required=True, choices=("asan", "tsan"))
    parser.add_argument("--lane", required=True, choices=("linux-asan", "linux-tsan"))
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--run-attempt", required=True)
    parser.add_argument(
        "--allow-prior-attempt",
        action="store_true",
        help="accept evidence from an earlier successful attempt of the same exact workflow run",
    )
    parser.add_argument("--job", required=True)
    parser.add_argument("--minimum-tests", required=True, type=int)
    parser.add_argument("--timeout-seconds", required=True, type=int)
    return parser.parse_args(argv)


def published_main(argv: list[str]) -> int:
    args = published_args(argv)
    errors: list[str] = BoundedErrors()
    if not re.fullmatch(r"[0-9a-f]{40}", args.expected_sha):
        errors.append("external expected SHA is invalid")
    if not re.fullmatch(r"[1-9][0-9]*", args.run_id):
        errors.append("external run ID is invalid")
    if not re.fullmatch(r"[1-9][0-9]*", args.run_attempt):
        errors.append("external run attempt is invalid")
    if len(args.run_id) > 20 or len(args.run_attempt) > 10 or len(args.job) > 64:
        errors.append("external provenance identity exceeds its length ceiling")
    if args.job != f"build-linux-{args.sanitizer}" or args.lane != f"linux-{args.sanitizer}":
        errors.append("external job/lane/sanitizer identity is inconsistent")
    if not 1 <= args.minimum_tests <= MAX_JUNIT_TESTS:
        errors.append("external minimum test count is outside the supported range")
    if not 1 <= args.timeout_seconds <= 7200:
        errors.append("external timeout is outside the supported range")
    run_id_number = int(args.run_id) if re.fullmatch(r"[1-9][0-9]{0,19}", args.run_id) else None
    run_attempt_number = (
        int(args.run_attempt) if re.fullmatch(r"[1-9][0-9]{0,9}", args.run_attempt) else None
    )
    producer_attempt_number = run_attempt_number

    if not args.evidence_dir.is_absolute():
        errors.append("published evidence directory must be absolute")
    validate_unindirected_path(
        args.evidence_dir,
        label="published evidence directory",
        errors=errors,
    )
    try:
        evidence_dir = args.evidence_dir.resolve(strict=True)
        directory_info = os.lstat(args.evidence_dir)
        if is_link_or_junction(args.evidence_dir, directory_info) or not stat.S_ISDIR(directory_info.st_mode):
            errors.append("published evidence directory must be a regular non-symlink directory")
    except (OSError, RuntimeError) as exc:
        print(f"error: published evidence directory cannot be resolved ({exc})", file=sys.stderr)
        return 70

    expected_stats = f"test-stats-{args.lane}.json"
    if args.stats.name != expected_stats:
        errors.append("published statistics filename does not match the lane")
    validate_exact_child(
        args.stats,
        parent=evidence_dir,
        label="published statistics",
        errors=errors,
    )

    required_names = {
        "console.txt",
        "report.txt",
        "junit.xml",
        "metadata.json",
        "process-footer.txt",
        expected_stats,
    }
    allowed_names = required_names | {"runtime"}
    first = snapshot_directory(
        evidence_dir,
        maximum_entries=len(allowed_names) + 1,
        label="published evidence directory",
        errors=errors,
    )
    check_directory_names(
        first,
        allowed=allowed_names,
        required=required_names,
        label="published evidence directory",
        errors=errors,
    )

    runtime_path = evidence_dir / "runtime"
    if os.path.lexists(runtime_path):
        try:
            runtime_info = os.lstat(runtime_path)
            if is_link_or_junction(runtime_path, runtime_info) or not stat.S_ISDIR(runtime_info.st_mode):
                errors.append("published runtime entry must be a regular non-symlink directory")
            runtime_snapshot = snapshot_directory(
                runtime_path,
                maximum_entries=1,
                label="published runtime directory",
                errors=errors,
            )
            if runtime_snapshot:
                errors.append("clean published sanitizer evidence must have no runtime logs")
        except OSError as exc:
            errors.append(f"published runtime directory cannot be inspected ({exc})")

    file_specs = {
        "console": (evidence_dir / "console.txt", MAX_CONSOLE_BYTES),
        "report": (evidence_dir / "report.txt", MAX_REPORT_BYTES),
        "junit": (evidence_dir / "junit.xml", MAX_JUNIT_BYTES),
        "metadata": (evidence_dir / "metadata.json", MAX_JSON_BYTES),
        "footer": (evidence_dir / "process-footer.txt", 16 * 1024),
        "stats": (args.stats, MAX_JSON_BYTES),
    }
    payloads: dict[str, bytes] = {}
    observations: dict[str, StableFile] = {}
    for label, (path, maximum) in file_specs.items():
        observation = read_regular_file(
            path,
            evidence_dir=evidence_dir,
            maximum=maximum,
            started_ns=0,
            errors=errors,
        )
        if observation is not None:
            observations[label] = observation
            payloads[label] = observation.data

    junit = parse_junit(
        payloads.get("junit", b""),
        minimum_tests=args.minimum_tests,
        errors=errors,
        collect_cases=True,
    ) if payloads.get("junit") else {}
    console_text = payloads.get("console", b"").decode("utf-8", errors="replace")
    report_text = payloads.get("report", b"").decode("utf-8", errors="replace")
    console_counts = completion_count(console_text, label="published console", errors=errors)
    report_counts = completion_count(report_text, label="published report", errors=errors)
    if (
        any(value is None for value in (*console_counts[:3], *report_counts[:3], junit.get("tests")))
        or len(set((*console_counts[:2], *report_counts[:2], junit.get("tests")))) != 1
        or console_counts[2] != report_counts[2]
    ):
        errors.append("published console, report, seed, and JUnit counts do not agree")
    # A waived test is never a pass and never a genuine skip. The current runner
    # reports it as <flakyFailure>, which is counted in `tests` but in neither
    # `skipped` nor `failures`; archived evidence reports it as a Known-flaky
    # <skipped>, which is inside `skipped`. Subtract each from the bucket it
    # actually occupies so the console's warned/skipped split reconciles under
    # both shapes instead of silently agreeing under neither.
    expected_terminal = {
        "passed": (
            junit.get("tests", 0)
            - junit.get("failures", 0)
            - junit.get("errors", 0)
            - junit.get("skipped", 0)
            - junit.get("flakyOutcomes", 0)
        ) if junit else None,
        "failed": junit.get("failures") if junit else None,
        "warned": junit.get("knownFlakyWarnings") if junit else None,
        "skipped": (
            junit.get("skipped", 0) - junit.get("flakySkips", 0)
        ) if junit else None,
    }
    if console_counts[3] != report_counts[3] or console_counts[3] != expected_terminal:
        errors.append("published terminal Results fields do not match strict JUnit evidence")

    union_text = console_text + "\n" + report_text
    if ANY_SANITIZER_PATTERN.search(union_text):
        errors.append("published clean evidence contains a sanitizer signature")
    if WARNING_PATTERN.search(union_text) or FAILURE_PATTERN.search(union_text) or CRASH_PATTERN.search(union_text):
        errors.append("published clean evidence contains a warning/failure/crash signal")

    metadata_command_hash: str | None = None
    metadata = strict_json(payloads.get("metadata", b""), label="metadata", errors=errors)
    metadata_obj = require_exact_keys(
        metadata,
        {
            "schemaVersion",
            "provenance",
            "selector",
            "process",
            "completion",
            "signals",
            "runtimeLogs",
            "scannerExitCodes",
            "classification",
            "recommendedExitCode",
            "evidenceErrors",
            "startedUnixNanoseconds",
            "completedUtc",
        },
        label="metadata",
        errors=errors,
    )
    if metadata_obj is not None:
        if not exact_json_value(metadata_obj.get("schemaVersion"), 2):
            errors.append("metadata schemaVersion must be 2")
        provenance = require_exact_keys(
            metadata_obj.get("provenance"),
            {
                "commitSha",
                "runId",
                "runAttempt",
                "job",
                "sanitizer",
                "lane",
                "originEvidenceDirectory",
                "commandSha256",
            },
            label="metadata.provenance",
            errors=errors,
        )
        if provenance is not None:
            recorded_attempt = provenance.get("runAttempt")
            if args.allow_prior_attempt:
                if (
                    not isinstance(recorded_attempt, int)
                    or isinstance(recorded_attempt, bool)
                    or recorded_attempt < 1
                    or run_attempt_number is None
                    or recorded_attempt > run_attempt_number
                ):
                    errors.append(
                        "metadata provenance runAttempt is not a prior/current exact-run attempt"
                    )
                else:
                    producer_attempt_number = recorded_attempt
            elif not exact_json_value(recorded_attempt, run_attempt_number):
                errors.append("metadata provenance runAttempt does not match external identity")
            expected_origin = (
                f"spark-sanitizer-{args.sanitizer}-{args.expected_sha}-{args.run_id}-"
                f"{producer_attempt_number}-{args.job}"
            )
            expected_provenance = {
                "commitSha": args.expected_sha,
                "runId": run_id_number,
                "job": args.job,
                "sanitizer": args.sanitizer,
                "lane": args.lane,
                "originEvidenceDirectory": expected_origin,
            }
            for key, expected in expected_provenance.items():
                if not exact_json_value(provenance.get(key), expected):
                    errors.append(f"metadata provenance {key} does not match external identity")
            command_hash = provenance.get("commandSha256")
            if not isinstance(command_hash, str) or not re.fullmatch(
                r"[0-9a-f]{64}", command_hash
            ):
                errors.append("metadata command hash is invalid")
            else:
                metadata_command_hash = command_hash

        selector = require_exact_keys(
            metadata_obj.get("selector"), {"expected", "verified"}, label="metadata.selector", errors=errors
        )
        if selector is not None and (
            not exact_json_value(selector.get("expected"), "all")
            or selector.get("verified") is not True
        ):
            errors.append("metadata selector is not verified exact-all evidence")
        process = require_exact_keys(
            metadata_obj.get("process"),
            {"exitCode", "captureExitCode", "timeoutSeconds", "timedOut", "captureOverflow"},
            label="metadata.process",
            errors=errors,
        )
        if process is not None and (
            not exact_json_value(process.get("exitCode"), 0)
            or not exact_json_value(process.get("captureExitCode"), 0)
            or not exact_json_value(process.get("timeoutSeconds"), args.timeout_seconds)
            or process.get("timedOut") is not False
            or process.get("captureOverflow") is not False
        ):
            errors.append("metadata process state is not clean")
        completion = require_exact_keys(
            metadata_obj.get("completion"),
            {
                "valid",
                "tests",
                "failures",
                "errors",
                "skipped",
                "knownFlakyWarnings",
                "flakyOutcomes",
                "flakySkips",
                "empty",
                "suiteNames",
                "shuffleSeed",
                "junitSha256",
                "reportSha256",
                "consoleSha256",
            },
            label="metadata.completion",
            errors=errors,
        )
        if completion is not None:
            expected_completion = {
                "valid": True,
                "tests": junit.get("tests"),
                "failures": junit.get("failures"),
                "errors": junit.get("errors"),
                "skipped": junit.get("skipped"),
                "knownFlakyWarnings": junit.get("knownFlakyWarnings"),
                "flakyOutcomes": junit.get("flakyOutcomes"),
                "flakySkips": junit.get("flakySkips"),
                "empty": junit.get("empty"),
                "suiteNames": ["SparkEngine"],
                "shuffleSeed": 123,
                "junitSha256": sha256_bytes(payloads.get("junit", b"")),
                "reportSha256": sha256_bytes(payloads.get("report", b"")),
                "consoleSha256": sha256_bytes(payloads.get("console", b"")),
            }
            for key, expected in expected_completion.items():
                if not exact_json_value(completion.get(key), expected):
                    errors.append(f"metadata completion {key} does not match consumed evidence")
        if not exact_json_value(
            metadata_obj.get("classification"), "clean"
        ) or not exact_json_value(metadata_obj.get("recommendedExitCode"), 0):
            errors.append("metadata does not carry an exact clean classification")
        if metadata_obj.get("evidenceErrors") != [] or metadata_obj.get("runtimeLogs") != []:
            errors.append("metadata clean evidence contains errors or runtime logs")
        signals = require_exact_keys(
            metadata_obj.get("signals"),
            {
                "sanitizerSignature",
                "runtimeEvidence",
                "testFailure",
                "knownFlakyWarning",
                "crash",
                "infrastructure",
            },
            label="metadata.signals",
            errors=errors,
        )
        if signals is not None and any(value is not False for value in signals.values()):
            errors.append("metadata clean evidence contains asserted signals")
        scans = require_exact_keys(
            metadata_obj.get("scannerExitCodes"),
            {"sanitizerSignature", "warning", "testFailure", "crash", "infrastructure"},
            label="metadata.scannerExitCodes",
            errors=errors,
        )
        if scans is not None and any(value != 1 or isinstance(value, bool) for value in scans.values()):
            errors.append("metadata scanner exit codes are not exact clean no-match results")
        started = metadata_obj.get("startedUnixNanoseconds")
        if not isinstance(started, int) or isinstance(started, bool) or started <= 0:
            errors.append("metadata start timestamp is invalid")
        if not re.fullmatch(
            r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z",
            str(metadata_obj.get("completedUtc", "")),
        ):
            errors.append("metadata completion timestamp is invalid")

    stats = strict_json(payloads.get("stats", b""), label="statistics", errors=errors)
    stats_obj = require_exact_keys(
        stats,
        {
            "schemaVersion",
            "reports",
            "tests",
            "executed",
            "passed",
            "failures",
            "errors",
            "skipped",
            "durationSeconds",
            "slowest",
        },
        label="statistics",
        errors=errors,
    )
    if stats_obj is not None:
        expected_counts = {
            "tests": junit.get("tests"),
            "executed": (junit.get("tests", 0) - junit.get("skipped", 0)) if junit else None,
            "passed": (
                junit.get("tests", 0)
                - junit.get("failures", 0)
                - junit.get("errors", 0)
                - junit.get("skipped", 0)
            ) if junit else None,
            "failures": junit.get("failures"),
            "errors": junit.get("errors"),
            "skipped": junit.get("skipped"),
        }
        if not exact_json_value(stats_obj.get("schemaVersion"), 1):
            errors.append("statistics schemaVersion must be 1")
        for key, expected in expected_counts.items():
            if not exact_json_value(stats_obj.get(key), expected):
                errors.append(f"statistics {key} does not match strict JUnit evidence")
        reports = stats_obj.get("reports")
        if not isinstance(reports, list) or len(reports) != 1 or Path(str(reports[0])).name != "junit.xml":
            errors.append("statistics must identify exactly one junit.xml report")
        duration = stats_obj.get("durationSeconds")
        if not isinstance(duration, (int, float)) or isinstance(duration, bool) or not math.isfinite(duration) or duration < 0:
            errors.append("statistics durationSeconds is invalid")
        slowest = stats_obj.get("slowest")
        expected_slowest = [
            {"name": case["name"], "durationSeconds": case["durationSeconds"]}
            for case in sorted(
                junit.get("_cases", []), key=lambda case: case["_rawDuration"], reverse=True
            )[:10]
        ]
        if not isinstance(slowest, list) or len(slowest) > 10:
            errors.append("statistics slowest list is invalid")
        elif any(
            not isinstance(entry, dict)
            or set(entry) != {"name", "durationSeconds"}
            or not isinstance(entry.get("name"), str)
            or not entry.get("name")
            or not isinstance(entry.get("durationSeconds"), (int, float))
            or isinstance(entry.get("durationSeconds"), bool)
            or not math.isfinite(entry.get("durationSeconds"))
            or entry.get("durationSeconds") < 0
            for entry in slowest
        ):
            errors.append("statistics slowest entries are malformed")
        elif slowest != expected_slowest:
            errors.append("statistics slowest entries do not match strict JUnit evidence")
        expected_duration = round(
            sum(case["_rawDuration"] for case in junit.get("_cases", [])), 6
        )
        if isinstance(duration, (int, float)) and not isinstance(duration, bool) and duration != expected_duration:
            errors.append("statistics durationSeconds does not match strict JUnit evidence")

    footer = parse_footer(payloads.get("footer", b""), errors=errors) if payloads.get("footer") else {}
    footer_expected = {
        "commit_sha": args.expected_sha,
        "run_id": args.run_id,
        "run_attempt": str(producer_attempt_number),
        "job": args.job,
        "sanitizer": args.sanitizer,
        "expected_selector": "all",
        "minimum_tests": str(args.minimum_tests),
        "timeout_seconds": str(args.timeout_seconds),
        "test_exit_code": "0",
        "capture_exit_code": "0",
        "effective_exit_code": "0",
        "signal_received": "none",
        "evidence_classification": "clean",
        "metadata_file": "metadata.json",
        "metadata_sha256": sha256_bytes(payloads.get("metadata", b"")),
    }
    for key, expected in footer_expected.items():
        if footer.get(key) != expected:
            errors.append(f"process footer {key} does not match consumed/external evidence")
    if not re.fullmatch(r"[0-9a-f]{64}", footer.get("command_sha256", "")):
        errors.append("process footer command hash is invalid")
    elif metadata_command_hash is not None and footer.get("command_sha256") != metadata_command_hash:
        errors.append("process footer and metadata command hashes disagree")
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", footer.get("completed_utc", "")):
        errors.append("process footer completion time is invalid")

    time.sleep(QUIESCENCE_SECONDS)
    for label, observation in observations.items():
        path, maximum = file_specs[label]
        verify_stable_file(
            path,
            observation,
            evidence_dir=evidence_dir,
            maximum=maximum,
            started_ns=0,
            errors=errors,
        )
    second = snapshot_directory(
        evidence_dir,
        maximum_entries=len(allowed_names) + 1,
        label="published evidence directory",
        errors=errors,
    )
    if first is not None and second is not None and first != second:
        errors.append("published evidence directory changed while it was consumed")

    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if errors:
        return 70
    print(f"verified published sanitizer evidence: {args.lane}")
    return 0


def main() -> int:
    args = parse_args()
    errors: list[str] = BoundedErrors()
    if not re.fullmatch(r"[0-9a-f]{40}", args.expected_sha):
        errors.append("expected SHA is invalid")
    if not re.fullmatch(r"[1-9][0-9]*", args.run_id):
        errors.append("run ID is invalid")
    if not re.fullmatch(r"[1-9][0-9]*", args.run_attempt):
        errors.append("run attempt is invalid")
    if len(args.run_id) > 20 or len(args.run_attempt) > 10 or len(args.job) > 64:
        errors.append("provenance identity exceeds its length ceiling")
    if not re.fullmatch(r"[0-9a-f]{64}", args.command_sha256):
        errors.append("command SHA-256 is invalid")
    if not re.fullmatch(r"[0-9a-f]{64}", args.timeout_token):
        errors.append("timeout marker token is invalid")
    if not re.fullmatch(r"[0-9a-f]{64}", args.capture_token):
        errors.append("capture marker token is invalid")
    if args.timeout_token == args.capture_token:
        errors.append("timeout and capture marker tokens must be independent")
    if not 1 <= args.minimum_tests <= MAX_JUNIT_TESTS:
        errors.append("minimum test count is outside the supported range")
    if not 1 <= args.timeout_seconds <= 7200:
        errors.append("timeout is outside the supported range")
    if args.started_ns <= 0:
        errors.append("start timestamp must be positive")
    if not 0 <= args.process_exit <= 255 or not 0 <= args.capture_exit <= 255:
        errors.append("process/capture exit status is outside the supported range")
    run_id_number = int(args.run_id) if re.fullmatch(r"[1-9][0-9]{0,19}", args.run_id) else None
    run_attempt_number = (
        int(args.run_attempt) if re.fullmatch(r"[1-9][0-9]{0,9}", args.run_attempt) else None
    )

    if not args.evidence_dir.is_absolute():
        errors.append("evidence directory must be absolute")
    validate_unindirected_path(
        args.evidence_dir,
        label="evidence directory",
        errors=errors,
    )
    try:
        evidence_dir = args.evidence_dir.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        print(f"error: evidence directory cannot be resolved ({exc})", file=sys.stderr)
        return 70

    try:
        directory_info = os.lstat(args.evidence_dir)
        if is_link_or_junction(args.evidence_dir, directory_info) or not stat.S_ISDIR(directory_info.st_mode):
            errors.append("evidence directory must be a regular non-symlink directory")
        if os.name != "nt" and directory_info.st_mode & 0o077:
            errors.append("evidence directory is not private (group/other permissions are set)")
    except OSError as exc:
        errors.append(f"evidence directory cannot be inspected ({exc})")

    try:
        root_info = os.lstat(args.evidence_dir.parent)
        if is_link_or_junction(args.evidence_dir.parent, root_info):
            errors.append("evidence root must not be a symlink, junction, or reparse link")
    except OSError as exc:
        errors.append(f"evidence root cannot be inspected ({exc})")

    expected_leaf = (
        f"spark-sanitizer-{args.sanitizer}-{args.expected_sha}-{args.run_id}-"
        f"{args.run_attempt}-{args.job}"
    )
    if args.evidence_dir.name != expected_leaf:
        errors.append("evidence directory name does not bind the supplied provenance")
    expected_paths = {
        "console.txt": args.console,
        "report.txt": args.report,
        "junit.xml": args.junit,
        "metadata.json": args.metadata,
        ".wrapper-timeout": args.timeout_marker,
        ".capture-overflow": args.capture_overflow_marker,
    }
    for expected_name, path in expected_paths.items():
        if path.name != expected_name:
            errors.append(f"{expected_name}: verifier path has an unexpected basename")
    if args.runtime_dir.name != "runtime":
        errors.append("runtime directory has an unexpected basename")
    validate_exact_child(
        args.runtime_dir,
        parent=evidence_dir,
        label="runtime directory",
        errors=errors,
    )
    validate_exact_child(
        args.metadata,
        parent=evidence_dir,
        label="metadata.json",
        errors=errors,
        leaf_may_be_missing=True,
    )
    if os.path.lexists(args.metadata):
        errors.append("metadata path already exists")

    marker_names = {args.timeout_marker.name, args.capture_overflow_marker.name}
    # The console, private runtime directory, and opened footer always exist.
    # Report/JUnit are completion evidence and may legitimately be absent after
    # a crash or timeout; their bounded readers below record that distinction.
    required_names = {"console.txt", "runtime", "process-footer.txt"}
    allowed_names = required_names | {"report.txt", "junit.xml", "metadata.json"} | marker_names
    first_snapshot = snapshot_directory(
        evidence_dir,
        maximum_entries=len(allowed_names) + 1,
        label="evidence directory",
        errors=errors,
    )
    check_directory_names(
        first_snapshot,
        allowed=allowed_names,
        required=required_names,
        label="evidence directory",
        errors=errors,
    )

    console_observation = read_regular_file(
        args.console,
        evidence_dir=evidence_dir,
        maximum=MAX_CONSOLE_BYTES,
        started_ns=args.started_ns,
        errors=errors,
    )
    report_observation = read_regular_file(
        args.report,
        evidence_dir=evidence_dir,
        maximum=MAX_REPORT_BYTES,
        started_ns=args.started_ns,
        errors=errors,
    )
    junit_observation = read_regular_file(
        args.junit,
        evidence_dir=evidence_dir,
        maximum=MAX_JUNIT_BYTES,
        started_ns=args.started_ns,
        errors=errors,
    )

    console_data = console_observation.data if console_observation else None
    report_data = report_observation.data if report_observation else None
    junit_data = junit_observation.data if junit_observation else None
    console_text = console_data.decode("utf-8", errors="replace") if console_data else ""
    report_text = report_data.decode("utf-8", errors="replace") if report_data else ""
    junit = parse_junit(junit_data, minimum_tests=args.minimum_tests, errors=errors) if junit_data else {}

    console_running, console_total, console_seed, console_summary = completion_count(
        console_text, label="console", errors=errors
    )
    report_running, report_total, report_seed, report_summary = completion_count(
        report_text, label="report", errors=errors
    )
    junit_tests = junit.get("tests")
    counts = [console_running, console_total, report_running, report_total, junit_tests]
    if any(value is None for value in counts) or len(set(counts)) != 1:
        errors.append("console, report, and JUnit test counts do not agree")
    if console_seed != 123 or report_seed != 123 or console_seed != report_seed:
        errors.append("console and report shuffle seed evidence does not agree")
    # A waived test is never a pass and never a genuine skip. The current runner
    # reports it as <flakyFailure>, which is counted in `tests` but in neither
    # `skipped` nor `failures`; archived evidence reports it as a Known-flaky
    # <skipped>, which is inside `skipped`. Subtract each from the bucket it
    # actually occupies so the console's warned/skipped split reconciles under
    # both shapes instead of silently agreeing under neither.
    expected_terminal = {
        "passed": (
            junit.get("tests", 0)
            - junit.get("failures", 0)
            - junit.get("errors", 0)
            - junit.get("skipped", 0)
            - junit.get("flakyOutcomes", 0)
        ) if junit else None,
        "failed": junit.get("failures") if junit else None,
        "warned": junit.get("knownFlakyWarnings") if junit else None,
        "skipped": (
            junit.get("skipped", 0) - junit.get("flakySkips", 0)
        ) if junit else None,
    }
    if console_summary != report_summary or console_summary != expected_terminal:
        errors.append("console/report terminal Results fields do not match JUnit evidence")

    union_text = console_text + "\n" + report_text
    runtime_inspection = inspect_runtime_logs(
        args.runtime_dir,
        sanitizer=args.sanitizer,
        started_ns=args.started_ns,
        errors=errors,
    )
    runtime_logs = runtime_inspection.results
    runtime_finding = bool(runtime_logs)
    signature_present = bool(ANY_SANITIZER_PATTERN.search(union_text)) or runtime_finding
    warning_present = bool(WARNING_PATTERN.search(union_text)) or bool(
        junit.get("knownFlakyWarnings", 0)
    )
    failure_present = bool(FAILURE_PATTERN.search(union_text)) or bool(
        junit.get("failures", 0) or junit.get("errors", 0)
    )
    crash_present = bool(CRASH_PATTERN.search(union_text)) or (
        args.process_exit >= 128 and args.process_exit != 124
    )
    infrastructure_present = bool(INFRASTRUCTURE_PATTERN.search(union_text)) or (
        args.process_exit in {126, 127}
    )

    verify_scan(
        args.signature_scan_status,
        signature_present,
        name="sanitizer signature",
        errors=errors,
    )
    verify_scan(
        args.warning_scan_status,
        bool(WARNING_PATTERN.search(union_text)),
        name="warning",
        errors=errors,
    )
    verify_scan(
        args.failure_scan_status,
        bool(FAILURE_PATTERN.search(union_text)),
        name="test failure",
        errors=errors,
    )
    verify_scan(
        args.crash_scan_status,
        bool(CRASH_PATTERN.search(union_text)),
        name="crash",
        errors=errors,
    )
    verify_scan(
        args.infrastructure_scan_status,
        bool(INFRASTRUCTURE_PATTERN.search(union_text)),
        name="infrastructure",
        errors=errors,
    )

    sanitizer_finding = signature_present
    timeout_marker, timeout_observation = marker_state(
        args.timeout_marker,
        evidence_dir=evidence_dir,
        started_ns=args.started_ns,
        expected=f"timeout:{args.timeout_token}\n".encode("ascii"),
        label="timeout marker",
        errors=errors,
    )
    capture_overflow, capture_observation = marker_state(
        args.capture_overflow_marker,
        evidence_dir=evidence_dir,
        started_ns=args.started_ns,
        expected=f"capture:{args.capture_token}\n".encode("ascii"),
        label="capture overflow marker",
        errors=errors,
    )
    timed_out = args.process_exit == 124 and timeout_marker
    if timeout_marker and args.process_exit != 124:
        errors.append("authenticated timeout marker exists without GNU timeout status 124")
    if capture_overflow != (args.capture_exit == 74):
        errors.append("capture overflow marker and capture exit status disagree")

    stable_inputs = (
        (args.console, console_observation, MAX_CONSOLE_BYTES),
        (args.report, report_observation, MAX_REPORT_BYTES),
        (args.junit, junit_observation, MAX_JUNIT_BYTES),
        (args.timeout_marker, timeout_observation, 512),
        (args.capture_overflow_marker, capture_observation, 512),
    )
    for path, observation, maximum in stable_inputs:
        if observation is not None:
            verify_stable_file(
                path,
                observation,
                evidence_dir=evidence_dir,
                maximum=maximum,
                started_ns=args.started_ns,
                errors=errors,
            )
    verify_runtime_inspection(
        args.runtime_dir,
        runtime_inspection,
        started_ns=args.started_ns,
        errors=errors,
    )

    incomplete_run = not junit or console_summary is None
    started_tests = LAST_STARTED_TEST_PATTERN.findall(console_text)
    last_started_test = started_tests[-1] if started_tests else None

    completion_errors = [
        error
        for error in errors
        if error.startswith(("console.txt", "report.txt", "junit.xml", "console:", "report:"))
        or "test counts" in error
        or "shuffle seed" in error
        or "terminal Results" in error
    ]
    critical_verification_errors = [error for error in errors if error not in completion_errors]

    if critical_verification_errors:
        classification = "verification-failure"
        recommended_exit = args.process_exit or args.capture_exit or 70
    elif args.capture_exit != 0 or capture_overflow:
        classification = "capture-failure"
        recommended_exit = args.process_exit or args.capture_exit
    elif timed_out:
        classification = "timeout"
        recommended_exit = args.process_exit
    elif sanitizer_finding and incomplete_run:
        # A suite that died mid-run is materially different evidence from a
        # completed run that reported a finding: an unknown fraction of the
        # tests never ran and the --minimum-tests floor cannot bite, because
        # there is no JUnit to count.  Name the incompleteness, not a finding.
        classification = "incomplete-run"
        recommended_exit = args.process_exit or 70
    elif sanitizer_finding:
        classification = "sanitizer-finding"
        recommended_exit = args.process_exit or 1
    elif warning_present or junit.get("knownFlakyWarnings", 0):
        classification = "test-policy-failure"
        recommended_exit = args.process_exit or 1
    elif failure_present:
        classification = "test-failure"
        recommended_exit = args.process_exit or 1
    elif crash_present:
        classification = "crash"
        recommended_exit = args.process_exit or 1
    elif infrastructure_present:
        classification = "infrastructure-failure"
        recommended_exit = args.process_exit or 70
    elif args.process_exit != 0:
        classification = "process-failure"
        recommended_exit = args.process_exit
    elif completion_errors:
        classification = "verification-failure"
        recommended_exit = 70
    else:
        classification = "clean"
        recommended_exit = 0

    metadata: dict[str, Any] = {
        "schemaVersion": 2,
        "provenance": {
            "commitSha": args.expected_sha,
            "runId": run_id_number,
            "runAttempt": run_attempt_number,
            "job": args.job,
            "sanitizer": args.sanitizer,
            "lane": f"linux-{args.sanitizer}",
            "originEvidenceDirectory": expected_leaf,
            "commandSha256": args.command_sha256,
        },
        "selector": {
            "expected": args.expected_selector,
            "verified": not any("count" in error or "selector" in error for error in errors),
        },
        "process": {
            "exitCode": args.process_exit,
            "captureExitCode": args.capture_exit,
            "timeoutSeconds": args.timeout_seconds,
            "timedOut": timed_out,
            "captureOverflow": capture_overflow,
        },
        "completion": {
            "valid": bool(junit) and not any(
                error.startswith(("JUnit", "console", "report"))
                or "test counts" in error
                for error in errors
            ),
            **junit,
            "shuffleSeed": console_seed if console_seed == report_seed else None,
            "junitSha256": sha256_bytes(junit_data) if junit_data else None,
            "reportSha256": sha256_bytes(report_data) if report_data else None,
            "consoleSha256": sha256_bytes(console_data) if console_data else None,
        },
        "signals": {
            "sanitizerSignature": signature_present,
            "runtimeEvidence": runtime_finding,
            "testFailure": failure_present,
            "knownFlakyWarning": warning_present,
            "crash": crash_present,
            "infrastructure": infrastructure_present,
        },
        "runtimeLogs": runtime_logs,
        "scannerExitCodes": {
            "sanitizerSignature": args.signature_scan_status,
            "warning": args.warning_scan_status,
            "testFailure": args.failure_scan_status,
            "crash": args.crash_scan_status,
            "infrastructure": args.infrastructure_scan_status,
        },
        "classification": classification,
        "recommendedExitCode": recommended_exit,
        "evidenceErrors": list(errors),
        "startedUnixNanoseconds": args.started_ns,
        "completedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }

    time.sleep(QUIESCENCE_SECONDS)
    before_metadata = snapshot_directory(
        evidence_dir,
        maximum_entries=len(allowed_names) + 1,
        label="evidence directory",
        errors=errors,
    )
    check_directory_names(
        before_metadata,
        allowed=allowed_names - {"metadata.json"},
        required=required_names,
        label="evidence directory",
        errors=errors,
    )
    if first_snapshot is not None and before_metadata is not None and first_snapshot != before_metadata:
        errors.append("evidence directory changed while evidence was verified")
    if errors != metadata["evidenceErrors"]:
        metadata["evidenceErrors"] = list(errors)
        metadata["classification"] = "verification-failure"
        metadata["recommendedExitCode"] = args.process_exit or args.capture_exit or 70
        classification = "verification-failure"
        recommended_exit = metadata["recommendedExitCode"]

    metadata_payload: bytes | None = None
    try:
        metadata_payload = write_json_exclusive(args.metadata, metadata)
    except OSError as exc:
        print(f"error: cannot write sanitizer metadata: {exc}", file=sys.stderr)
        print("verification-failure")
        return args.process_exit or args.capture_exit or 70

    metadata_error_count = len(errors)
    metadata_observation = read_regular_file(
        args.metadata,
        evidence_dir=evidence_dir,
        maximum=MAX_JSON_BYTES,
        started_ns=args.started_ns,
        errors=errors,
    )
    if metadata_observation is None or metadata_observation.data != metadata_payload:
        errors.append("metadata.json: published bytes do not match the verifier payload")
    if len(errors) != metadata_error_count:
        classification = "verification-failure"
        recommended_exit = args.process_exit or args.capture_exit or 70

    post_write_error_count = len(errors)
    first_final_snapshot = snapshot_directory(
        evidence_dir,
        maximum_entries=len(allowed_names) + 1,
        label="evidence directory",
        errors=errors,
    )
    time.sleep(QUIESCENCE_SECONDS)
    final_snapshot = snapshot_directory(
        evidence_dir,
        maximum_entries=len(allowed_names) + 1,
        label="evidence directory",
        errors=errors,
    )
    check_directory_names(
        final_snapshot,
        allowed=allowed_names,
        required=required_names | {"metadata.json"},
        label="evidence directory",
        errors=errors,
    )
    if (
        first_final_snapshot is None
        or final_snapshot is None
        or first_final_snapshot != final_snapshot
        or "metadata.json" not in final_snapshot
        or len(errors) != post_write_error_count
    ):
        if first_final_snapshot is not None and final_snapshot is not None and first_final_snapshot != final_snapshot:
            errors.append("evidence directory changed after metadata publication")
        classification = "verification-failure"
        recommended_exit = args.process_exit or args.capture_exit or 70

    for path, observation, maximum in stable_inputs:
        if observation is not None:
            verify_stable_file(
                path,
                observation,
                evidence_dir=evidence_dir,
                maximum=maximum,
                started_ns=args.started_ns,
                errors=errors,
            )
    verify_runtime_inspection(
        args.runtime_dir,
        runtime_inspection,
        started_ns=args.started_ns,
        errors=errors,
    )
    if metadata_observation is not None:
        verify_stable_file(
            args.metadata,
            metadata_observation,
            evidence_dir=evidence_dir,
            maximum=MAX_JSON_BYTES,
            started_ns=args.started_ns,
            errors=errors,
        )
    sealed_snapshot = snapshot_directory(
        evidence_dir,
        maximum_entries=len(allowed_names) + 1,
        label="evidence directory",
        errors=errors,
    )
    if final_snapshot is not None and sealed_snapshot is not None and sealed_snapshot != final_snapshot:
        errors.append("evidence directory changed during final content verification")
    if len(errors) != post_write_error_count:
        classification = "verification-failure"
        recommended_exit = args.process_exit or args.capture_exit or 70

    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if incomplete_run:
        print(
            "error: sanitizer suite did not complete: "
            f"junit={'absent' if not junit else 'present'}, "
            f"terminalResults={'absent' if console_summary is None else 'present'}, "
            f"lastStartedTest={last_started_test or 'unknown'}",
            file=sys.stderr,
        )
    print(classification)
    return recommended_exit


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "capture":
        raise SystemExit(capture_main(sys.argv[2:]))
    if len(sys.argv) > 1 and sys.argv[1] == "verify-published":
        raise SystemExit(published_main(sys.argv[2:]))
    raise SystemExit(main())
