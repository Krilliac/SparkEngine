#!/usr/bin/env python3
"""SparkEngine repository secret scan (SEC-110).

Scans every git-tracked regular file with the OPS-100 detectors in
tools/ops/secret_policy.py (token formats, PEM private-key headers,
credential-bearing URLs, and the structured credential lexer) and fails on any
finding that is not covered by a reviewed exception.

Design rules this file is required to obey:

  * The inventory is the git index (``git ls-files -s -z``), not a directory
    walk, so ignored build output never enters the verdict and every tracked
    file does.  Symbolic links (mode 120000) are never followed, and gitlinks
    (mode 160000) are separate repositories with their own history.  A tracked
    regular file that has become a symlink on disk is an error.
  * Every read is bounded.  A file larger than MAX_FILE_BYTES cannot be
    established as clean and is an error, never a skip.
  * Findings print ``path:line: rule`` only.  The matched value is never
    written anywhere, so the gate cannot itself leak what it finds.
  * Exceptions reuse the supply-chain exception contract
    (tools/check-supply-chain.py validate_exception_records: named owner,
    16+ character justification, ISO expiry, case-insensitive unique ids) plus
    one required field, ``count``: the exact number of reviewed findings of
    that rule in that file.  A secret-scan scope is exactly
    ``<rule>:<tracked file path>``: no globs, no directories, no rule
    wildcards.  An exception covers its file only while the live finding count
    equals the reviewed count, so a second secret planted in an excepted file
    fails the gate instead of riding on the earlier review; a lower count is a
    stale exception that must be ratcheted down.  Expired, stale, or over-long
    (more than MAX_EXCEPTION_HORIZON_DAYS ahead) exceptions fail.

Detector scoping.  The OPS-100 structured-credential lexer was written for
operator artifacts (configs, logs, crash and telemetry payloads) in which the
right-hand side of ``password=...`` is always a value.  In source code it is
usually an expression (``password = GetPassword()``), so this scan narrows that
one detector without adding a detector of its own:

  * the last dotted segment of the key must itself name credential material
    (``secretPath.text`` is not a credential key; ``cfg.dbPassword`` is);
  * values that are references rather than literals (``${{ secrets.X }}``,
    ``$VAR``, ``%s``, ``{token}``, ``<redacted>``) are not findings; the same
    reference rule applies to the password part of credential-bearing URLs;
  * a ``::`` after the key is a scope operator (``PasswordHash::Create``), not
    a key/value separator;
  * in programming-language source (SOURCE_CODE_SUFFIXES) only a quoted string
    literal can carry a credential, so unquoted right-hand sides there are
    expressions.  Every other file -- configs, shell/batch/PowerShell scripts,
    Dockerfiles, suffixless or dotfile credential stores (.npmrc, .netrc, AWS
    ``credentials``), Markdown and text -- accepts an unquoted literal, except
    for a ``$name``/``@name`` key (a variable being read) and a YAML
    ``key = ...`` line (embedded script text).

Files containing NUL bytes are binary; only the self-identifying token and PEM
detectors (HIGH_SIGNAL_RULES) apply to them, because the lexer and URL rules
match arbitrary byte noise in compressed image data.

Exit codes:
    0  No unexcepted findings and every exception is valid and in use
    1  Findings, invalid or stale exceptions, or files that cannot be scanned
    2  The scan itself could not run (git missing, not a repository)

Usage:
    python3 tools/check-secret-scan.py
    python3 tools/check-secret-scan.py --json
    python3 tools/check-secret-scan.py --root <repo> --exceptions <file>
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import stat
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import date, datetime, timedelta, timezone
from pathlib import Path, PurePosixPath
from types import ModuleType
from typing import Any

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
SUPPLY_CHAIN_CHECKER = TOOLS_DIR / "check-supply-chain.py"
DEFAULT_EXCEPTIONS_REL = "tools/secret-scan-exceptions.json"

sys.path.insert(0, str(TOOLS_DIR / "ops"))
import secret_policy  # noqa: E402  (path set up above; tools/ops is not a package)

EXCEPTIONS_SCHEMA_VERSION = 1
MAX_TRACKED_ENTRIES = 200_000
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_TOTAL_BYTES = 4 * 1024 * 1024 * 1024
MAX_EXCEPTIONS_FILE_BYTES = 1024 * 1024
MAX_EXCEPTION_HORIZON_DAYS = 366
MAX_EXCEPTION_COUNT = 1000
MAX_REPORTED_FINDINGS = 500
GIT_TIMEOUT_SECONDS = 120

GIT_FILE_MODES = frozenset({"100644", "100755"})
GIT_SYMLINK_MODE = "120000"
GIT_GITLINK_MODE = "160000"

HIGH_SIGNAL_RULES = frozenset(
    {"private-key", "github-token", "openai-api-key", "anthropic-api-key", "aws-access-key-id"}
)
REFERENCE_CHECKED_RULES = frozenset({"structured-credential", "credential-in-url"})
# Programming-language sources in which ``key = <unquoted>`` is an expression,
# so only a quoted string literal can carry a credential.  Every other file
# (configs, shell/batch/PowerShell scripts, Dockerfiles, dotfile credential
# stores such as .npmrc/.netrc, suffixless files, Markdown, text) is treated as
# data: an unquoted right-hand side there is the value itself.
SOURCE_CODE_SUFFIXES = frozenset(
    {
        ".as", ".c", ".cc", ".cpp", ".cs", ".cxx", ".frag", ".gd", ".glsl", ".go", ".gradle", ".h", ".hh",
        ".hlsl", ".hlsli", ".hpp", ".inl", ".java", ".js", ".kt", ".lua", ".m", ".metal", ".mm", ".py",
        ".rs", ".swift", ".ts", ".vert",
    }
)
YAML_SUFFIXES = frozenset({".yaml", ".yml"})
REFERENCE_PREFIXES = ("$", "%", "{", "<", "@", "[")
QUOTES = "\"'"

if set(HIGH_SIGNAL_RULES | REFERENCE_CHECKED_RULES) - set(secret_policy.RULE_NAMES):
    raise ImportError("check-secret-scan rule sets name a rule tools/ops/secret_policy.py does not define")


class ScanSetupError(Exception):
    """The scan cannot run at all (exit 2)."""


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str

    def label(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}"


@dataclass
class ScanReport:
    scanned_files: int = 0
    scanned_bytes: int = 0
    skipped_symlinks: int = 0
    skipped_gitlinks: int = 0
    findings: list[Finding] = field(default_factory=list)
    excepted: list[Finding] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return not self.findings and not self.errors


# ── Inventory ─────────────────────────────────────────────────────────


def tracked_entries(root: Path) -> list[tuple[str, str]]:
    """Return (mode, path) for every index entry, bounded."""
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-s", "-z"],
            capture_output=True,
            timeout=GIT_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ScanSetupError(f"git ls-files failed: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ScanSetupError(f"git ls-files failed in {root}: {detail}")

    entries: dict[str, str] = {}
    for record in completed.stdout.split(b"\0"):
        if not record:
            continue
        meta, separator, raw_path = record.partition(b"\t")
        fields = meta.split()
        if not separator or len(fields) != 3:
            raise ScanSetupError(f"unparseable git index record: {record[:200]!r}")
        # Unmerged paths list up to three stages; the path is scanned once.
        entries.setdefault(raw_path.decode("utf-8", errors="surrogateescape"), fields[0].decode("ascii"))
        if len(entries) > MAX_TRACKED_ENTRIES:
            raise ScanSetupError(f"more than {MAX_TRACKED_ENTRIES} tracked entries")
    return sorted((mode, path) for path, mode in entries.items())


def read_tracked_file(root: Path, rel: str) -> bytes | None:
    """Read one tracked regular file without following links; None if absent.

    Raises ValueError with a policy message when the file cannot be scanned.
    """
    full = root / rel
    try:
        info = os.lstat(full)
    except FileNotFoundError:
        return None  # deleted in the working tree: there is no content to leak
    if stat.S_ISLNK(info.st_mode):
        raise ValueError("tracked as a regular file but is a symbolic link on disk; links are not followed")
    if not stat.S_ISREG(info.st_mode):
        raise ValueError("tracked as a regular file but is not a regular file on disk")
    if info.st_size > MAX_FILE_BYTES:
        raise ValueError(f"file exceeds the {MAX_FILE_BYTES}-byte scan bound and cannot be established clean")

    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    descriptor = os.open(full, flags)
    with os.fdopen(descriptor, "rb") as handle:
        if not stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
            raise ValueError("file changed type while it was opened")
        data = handle.read(MAX_FILE_BYTES + 1)
    if len(data) > MAX_FILE_BYTES:
        raise ValueError(f"file grew past the {MAX_FILE_BYTES}-byte scan bound while it was read")
    return data


# ── Detection ─────────────────────────────────────────────────────────


def _is_reference(value: str) -> bool:
    return value.startswith(REFERENCE_PREFIXES) or "${" in value or "{{" in value or "%s" in value


def _structured_is_literal_credential(text: str, match: secret_policy.SecretMatch, suffix: str) -> bool:
    prefix = text[match.start:match.value_start]
    key_part = prefix.rstrip(QUOTES + " \t")
    separator = key_part[-1:]
    key_part = key_part[:-1].rstrip(QUOTES + " \t\\")
    last_segment = key_part.rsplit(".", 1)[-1]
    if not secret_policy.is_sensitive_key(last_segment):
        return False

    value = text[match.value_start:match.value_end]
    if separator == ":" and value.startswith(":"):
        return False  # ``Name::Member`` is a scope operator, not ``key: value``
    if _is_reference(value):
        return False
    if match.value_start > 0 and text[match.value_start - 1] in QUOTES:
        return True

    # An unquoted value is an expression in programming-language source.  In
    # every other file it is a literal, except for a ``$name``/``@name`` key (a
    # shell or PowerShell variable being read, not assigned) and a YAML
    # ``key = ...`` line (embedded script text).
    if suffix in SOURCE_CODE_SUFFIXES:
        return False
    if match.start > 0 and text[match.start - 1] in "$@":
        return False
    return not (suffix in YAML_SUFFIXES and separator == "=")


def _url_password_is_literal(text: str, match: secret_policy.SecretMatch) -> bool:
    userinfo = text[match.start:match.end].split("://", 1)[-1].rstrip("@")
    password = userinfo.split(":", 1)[-1]
    return not _is_reference(password)


def scan_bytes(rel: str, data: bytes) -> list[Finding]:
    """Return the OPS-100 detector findings for one file's content."""
    binary = b"\0" in data
    text = data.decode("latin-1")
    suffix = PurePosixPath(rel).suffix.lower()

    # One finding per detector match (not per line), so a second value planted
    # on an already-excepted line still raises the reviewed count.
    found: list[Finding] = []
    for match in secret_policy.iter_secret_matches(text, HIGH_SIGNAL_RULES if binary else None):
        if match.rule == "structured-credential" and not _structured_is_literal_credential(text, match, suffix):
            continue
        if match.rule == "credential-in-url" and not _url_password_is_literal(text, match):
            continue
        found.append(Finding(rel, text.count("\n", 0, match.start) + 1, match.rule))
    return sorted(found, key=lambda item: (item.line, item.rule))


# ── Exceptions ────────────────────────────────────────────────────────


def _load_supply_chain_checker() -> ModuleType:
    """Import tools/check-supply-chain.py, the single exception-schema definition."""
    name = "spark_check_supply_chain"
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, SUPPLY_CHAIN_CHECKER)
    if spec is None or spec.loader is None:
        raise ScanSetupError(f"cannot load exception validator from {SUPPLY_CHAIN_CHECKER}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # dataclasses resolve annotations through sys.modules
    try:
        spec.loader.exec_module(module)
    except BaseException:
        del sys.modules[name]
        raise
    return module


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _parse_scope(scope: str) -> tuple[str, str] | str:
    """Split ``<rule>:<path>``; return an error string when it is not exact."""
    rule, separator, path = scope.partition(":")
    if not separator or rule not in secret_policy.RULE_NAMES:
        return f"scope must be '<rule>:<tracked file path>' with rule one of {sorted(secret_policy.RULE_NAMES)}"
    parts = path.split("/")
    if not path or path.endswith("/") or any(part in ("", ".", "..") for part in parts):
        return "scope path must be one exact tracked file path, not a directory or relative component"
    return rule, path


def load_exceptions(
    path: Path, tracked_files: set[str], today: date
) -> tuple[dict[tuple[str, str], tuple[str, int]], list[str]]:
    """Return ({(rule, path): (exception id, reviewed count)}, errors) for the exceptions file."""
    errors: list[str] = []
    try:
        size = path.stat().st_size
    except OSError as exc:
        return {}, [f"{path}: cannot read exceptions file: {exc}"]
    if size > MAX_EXCEPTIONS_FILE_BYTES:
        return {}, [f"{path}: exceptions file exceeds {MAX_EXCEPTIONS_FILE_BYTES} bytes"]
    try:
        document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except (OSError, UnicodeDecodeError, ValueError, RecursionError) as exc:
        return {}, [f"{path}: invalid exceptions JSON: {exc}"]

    if not isinstance(document, dict) or set(document) != {"schemaVersion", "exceptions"}:
        return {}, [f"{path}: expected exactly the keys 'schemaVersion' and 'exceptions'"]
    if document["schemaVersion"] != EXCEPTIONS_SCHEMA_VERSION:
        return {}, [f"{path}: schemaVersion must be {EXCEPTIONS_SCHEMA_VERSION}"]

    records = document["exceptions"]
    if not isinstance(records, list):
        return {}, [f"{path}: 'exceptions' must be a list"]
    # ``count`` is the one field this gate adds; the rest of each record must
    # satisfy the shared supply-chain contract exactly.
    counts: list[Any] = []
    shared_records: list[Any] = []
    for record in records:
        if isinstance(record, dict):
            counts.append(record.get("count"))
            shared_records.append({key: value for key, value in record.items() if key != "count"})
        else:
            counts.append(None)
            shared_records.append(record)
    schema_errors = _load_supply_chain_checker().validate_exception_records(shared_records)
    for index, count in enumerate(counts):
        if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_EXCEPTION_COUNT:
            schema_errors.append(
                f"exceptions[{index}].count: must be the exact reviewed finding count, 1..{MAX_EXCEPTION_COUNT}"
            )
    if schema_errors:
        return {}, [f"{path}: {message}" for message in schema_errors]

    horizon = today + timedelta(days=MAX_EXCEPTION_HORIZON_DAYS)
    covered: dict[tuple[str, str], tuple[str, int]] = {}
    for index, record in enumerate(records):
        label = f"{path}: exceptions[{index}] ({record['id']})"
        parsed = _parse_scope(record["scope"])
        if isinstance(parsed, str):
            errors.append(f"{label}: {parsed}")
            continue
        if parsed[1] not in tracked_files:
            errors.append(f"{label}: scope path {parsed[1]!r} is not a tracked regular file")
            continue
        if parsed in covered:
            errors.append(f"{label}: duplicate scope {record['scope']!r}")
            continue
        expires = date.fromisoformat(record["expires"])
        if expires < today:
            errors.append(f"{label}: expired on {record['expires']}")
            continue
        if expires > horizon:
            errors.append(f"{label}: expiry {record['expires']} is more than {MAX_EXCEPTION_HORIZON_DAYS} days ahead")
            continue
        covered[parsed] = (record["id"], record["count"])
    return covered, errors


# ── Driver ────────────────────────────────────────────────────────────


def run_scan(root: Path, exceptions_path: Path, today: date | None = None) -> ScanReport:
    today = today or datetime.now(timezone.utc).date()
    report = ScanReport()
    entries = tracked_entries(root)
    tracked_files = {path for mode, path in entries if mode in GIT_FILE_MODES}

    raw_findings: list[Finding] = []
    for mode, rel in entries:
        if mode == GIT_SYMLINK_MODE:
            report.skipped_symlinks += 1
            continue
        if mode == GIT_GITLINK_MODE:
            report.skipped_gitlinks += 1
            continue
        if mode not in GIT_FILE_MODES:
            report.errors.append(f"{rel}: unsupported git index mode {mode}")
            continue
        try:
            data = read_tracked_file(root, rel)
        except (OSError, ValueError) as exc:
            report.errors.append(f"{rel}: {exc}")
            continue
        if data is None:
            continue
        report.scanned_files += 1
        report.scanned_bytes += len(data)
        if report.scanned_bytes > MAX_TOTAL_BYTES:
            report.errors.append(f"tracked content exceeds the {MAX_TOTAL_BYTES}-byte total scan bound")
            break
        raw_findings.extend(scan_bytes(rel, data))

    covered, exception_errors = load_exceptions(exceptions_path, tracked_files, today)
    report.errors.extend(exception_errors)

    live_counts: dict[tuple[str, str], int] = {}
    for finding in raw_findings:
        key = (finding.rule, finding.path)
        live_counts[key] = live_counts.get(key, 0) + 1

    for finding in raw_findings:
        key = (finding.rule, finding.path)
        # A count above the review means at least one unreviewed finding; the
        # scan cannot tell which, so none of them are excepted.
        if key in covered and live_counts[key] <= covered[key][1]:
            report.excepted.append(finding)
        else:
            report.findings.append(finding)
    for key in sorted(covered):
        exception_id, reviewed = covered[key]
        live = live_counts.get(key, 0)
        if live == 0:
            report.errors.append(
                f"exception {exception_id!r} ({key[0]}:{key[1]}) matches no finding; remove the stale exception"
            )
        elif live > reviewed:
            report.errors.append(
                f"exception {exception_id!r} ({key[0]}:{key[1]}) was reviewed for {reviewed} finding(s) but "
                f"{live} are present; the new finding(s) are not covered"
            )
        elif live < reviewed:
            report.errors.append(
                f"exception {exception_id!r} ({key[0]}:{key[1]}) was reviewed for {reviewed} finding(s) but only "
                f"{live} remain; lower 'count' to {live}"
            )
    return report


def _print_text(report: ScanReport) -> None:
    for finding in report.findings[:MAX_REPORTED_FINDINGS]:
        print(f"SECRET  {finding.label()}")
    if len(report.findings) > MAX_REPORTED_FINDINGS:
        print(f"... and {len(report.findings) - MAX_REPORTED_FINDINGS} more findings")
    for error in report.errors[:MAX_REPORTED_FINDINGS]:
        print(f"ERROR   {error}")
    print(
        f"secret-scan: {report.scanned_files} files ({report.scanned_bytes} bytes) scanned, "
        f"{report.skipped_symlinks} symlinks and {report.skipped_gitlinks} gitlinks not followed, "
        f"{len(report.excepted)} excepted, {len(report.findings)} findings, {len(report.errors)} errors: "
        f"{'PASS' if report.passed else 'FAIL'}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--exceptions", type=Path, help=f"exceptions file (default: <root>/{DEFAULT_EXCEPTIONS_REL})")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    exceptions_path = args.exceptions or root / DEFAULT_EXCEPTIONS_REL
    try:
        report = run_scan(root, exceptions_path)
    except ScanSetupError as exc:
        print(f"secret-scan: cannot run: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(
            json.dumps(
                {
                    "passed": report.passed,
                    "scannedFiles": report.scanned_files,
                    "scannedBytes": report.scanned_bytes,
                    "skippedSymlinks": report.skipped_symlinks,
                    "skippedGitlinks": report.skipped_gitlinks,
                    "findings": [finding.__dict__ for finding in report.findings[:MAX_REPORTED_FINDINGS]],
                    "findingCount": len(report.findings),
                    "excepted": [finding.__dict__ for finding in report.excepted],
                    "errors": report.errors[:MAX_REPORTED_FINDINGS],
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        _print_text(report)
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
