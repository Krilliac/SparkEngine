#!/usr/bin/env python3
"""Deterministic, fail-closed integrity checks for packaged SparkEngine assets.

The caller, never manifest data, selects the root being verified. A verification
walks the directory tree once, opens each accepted regular file, hashes from the
descriptor, and checks fingerprint stability around the read.

TOCTOU limitations (honest boundary):
- The fingerprint uses (dev, ino, size, mtime_ns, mode). A same-inode rewrite
  that restores the original mtime and size is not detected.
- The walk is non-atomic: files added or removed between scandir() and read are
  missed or stale. A directory swapped after scandir() but before the leaf stat
  may expose unexpected content.
- This is a repository-content gate, not proof that the verified snapshot was
  consumed atomically by package assembly. Immutable handoff requires a separate
  mechanism (e.g. content-addressed staging or sealed archive).

Provenance (manifest schema v2):
- Every v2 entry carries ``license`` and ``provenance``. Their values are never
  typed into the manifest; ``generate`` resolves them from the reviewed policy
  (tools/asset-integrity/provenance.json by default) and refuses to write when a
  file is unclaimed, claimed ambiguously, or its hash-bound claim is stale.
- ``NOASSERTION`` is a truthful "no tracked license record" value. The policy
  must name the work item that owns closing each such gap.
- Schema v1 (integrity only) still loads so that staged package fixtures remain
  verifiable; any caller that passes ``require_provenance=True`` rejects it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import ntpath
import os
import re
import stat
import sys
import tempfile
import unicodedata
from pathlib import Path
from typing import Any, Iterable


LEGACY_MANIFEST_SCHEMA_VERSION = 1
MANIFEST_SCHEMA_VERSION = 2
SUPPORTED_MANIFEST_VERSIONS = frozenset({LEGACY_MANIFEST_SCHEMA_VERSION, MANIFEST_SCHEMA_VERSION})
HASH_ALGORITHM = "sha256"
MANIFEST_FILENAME = "assets.integrity.json"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PROVENANCE_POLICY = REPO_ROOT / "tools" / "asset-integrity" / "provenance.json"
PROVENANCE_POLICY_VERSION = 1
NOASSERTION = "NOASSERTION"
LEGACY_ENTRY_KEYS = frozenset({"path", "sha256", "size"})
ENTRY_KEYS = frozenset({"path", "sha256", "size", "license", "provenance"})
LICENSE_ID_RE = re.compile(r"(?:NOASSERTION|LicenseRef-[A-Za-z0-9.-]+|[A-Za-z0-9][A-Za-z0-9.+-]*)\Z")
RULE_ID_RE = re.compile(r"[a-z0-9][a-z0-9-]*\Z")
GAP_ID_RE = re.compile(r"[A-Z]+-[0-9]{3}\Z")
MAX_PROVENANCE_BYTES = 1024
RULE_KEYS = frozenset({"id", "license", "provenance", "evidence", "gap", "files", "prefixes", "records"})
RECORD_KEYS = frozenset({"path", "format", "exclude"})
RECORD_FORMATS = frozenset({"terrafront-asset-manifest", "blender-provenance", "starter-model-manifest"})

MAX_MANIFEST_BYTES = 64 * 1024 * 1024
MAX_ENTRY_COUNT = 100_000
MAX_DIRECTORY_ENTRY_COUNT = 200_000
MAX_PATH_BYTES = 1024
MAX_FILE_BYTES = 4 * 1024 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024 * 1024
MAX_DIRECTORY_DEPTH = 128
HASH_CHUNK_BYTES = 1024 * 1024

# (manifest, asset root, provenance policy) — all repository-relative.
KNOWN_MANIFESTS = (
    ("Assets/assets.integrity.json", "Assets", "tools/asset-integrity/provenance.json"),
)
ROOT_IGNORES = frozenset({MANIFEST_FILENAME})
# Package profiles. OD-09: the stable-v1 package ships no asset whose license is
# NOASSERTION. Those files stay in the repository and in the default package;
# their provenance remains open outside stable-v1.
PACKAGE_PROFILES = frozenset({"default", "stable-v1"})
NOASSERTION_EXCLUDED_PROFILES = frozenset({"stable-v1"})
# RDY-020: these profiles ship only the runtime asset closure of their
# in-profile modules (tools/asset-integrity/package_closure.py), not every root.
CLOSURE_PROFILES = frozenset({"stable-v1"})
TEMPLATE_ROOT_METADATA = frozenset({"README.md", "manifest.json"})
TEMPLATE_COLLECTION_METADATA = frozenset({"README.md", "assets.lock.json"})
INVALID_WINDOWS_CHARS = frozenset('<>:"|?*')
RESERVED_WINDOWS_NAMES = frozenset(
    {"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"}
    | {f"COM{i}" for i in range(1, 10)}
    | {f"LPT{i}" for i in range(1, 10)}
)
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
FILE_ATTRIBUTE_REPARSE_POINT = 0x400


class IntegrityError:
    """A stable, printable integrity failure."""

    __slots__ = ("path", "category", "message")

    def __init__(self, path: str, category: str, message: str) -> None:
        self.path = path
        self.category = category
        self.message = message

    def __repr__(self) -> str:
        return f"IntegrityError({self.category}: {self.path}: {self.message})"

    def __str__(self) -> str:
        return f"[{self.category}] {self.path}: {self.message}"


class ManifestFormatError(ValueError):
    """Raised when bounded manifest parsing or schema validation fails."""


def _duplicate_rejecting_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ManifestFormatError(f"duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def _read_bounded_json(path: Path, *, limit: int = MAX_MANIFEST_BYTES) -> Any:
    path = _absolute_lexical(path)
    for component in _path_chain(path):
        try:
            component_info = component.lstat()
        except OSError as exc:
            raise ManifestFormatError(f"cannot inspect path component {component}: {exc}") from exc
        if _stat_is_reparse(component_info):
            raise ManifestFormatError(f"path component is a symlink/junction/reparse point: {component}")
    try:
        before_path = path.lstat()
    except OSError as exc:
        raise ManifestFormatError(f"cannot inspect file: {exc}") from exc
    if _stat_is_reparse(before_path) or not stat.S_ISREG(before_path.st_mode):
        raise ManifestFormatError("must be a non-reparse regular file")
    if before_path.st_size > limit:
        raise ManifestFormatError(
            f"file is {before_path.st_size} bytes; limit is {limit} bytes")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd: int | None = None
    try:
        fd = os.open(path, flags)
        before_fd = os.fstat(fd)
        if not stat.S_ISREG(before_fd.st_mode) or _fingerprint(before_path) != _fingerprint(before_fd):
            raise ManifestFormatError("file changed between inspection and open")
        chunks: list[bytes] = []
        consumed = 0
        while True:
            chunk = os.read(fd, min(HASH_CHUNK_BYTES, limit + 1 - consumed))
            if not chunk:
                break
            chunks.append(chunk)
            consumed += len(chunk)
            if consumed > limit:
                raise ManifestFormatError(f"file exceeds {limit}-byte limit")
        after_fd = os.fstat(fd)
    except OSError as exc:
        raise ManifestFormatError(f"cannot read file: {exc}") from exc
    finally:
        if fd is not None:
            os.close(fd)
    try:
        after_path = path.lstat()
    except OSError as exc:
        raise ManifestFormatError(f"file changed after read: {exc}") from exc
    if _stat_is_reparse(after_path) or not (
        _fingerprint(before_path)
        == _fingerprint(before_fd)
        == _fingerprint(after_fd)
        == _fingerprint(after_path)
    ):
        raise ManifestFormatError("file changed or was replaced during read")
    raw = b"".join(chunks)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ManifestFormatError(f"not valid UTF-8: {exc}") from exc
    try:
        return json.loads(text, object_pairs_hook=_duplicate_rejecting_object)
    except (json.JSONDecodeError, ManifestFormatError) as exc:
        raise ManifestFormatError(str(exc)) from exc


def _canonical_relative_path(value: Any) -> tuple[str | None, str | None]:
    """Return (canonical_path, error), using Windows-portable path semantics."""
    if not isinstance(value, str):
        return None, "path must be a string"
    if not value:
        return None, "empty path"
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        return None, f"path is not valid Unicode: {exc}"
    if len(encoded) > MAX_PATH_BYTES:
        return None, f"UTF-8 path exceeds {MAX_PATH_BYTES} bytes"
    if value != unicodedata.normalize("NFC", value):
        return None, "path is not NFC-normalized"
    if value.startswith("/") or ntpath.isabs(value) or ntpath.splitdrive(value)[0]:
        return None, "absolute or drive-qualified path"
    if "\\" in value:
        return None, "backslash separator"
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        return None, "empty, dot, or traversal path segment"
    for part in parts:
        if part != part.strip() or part.endswith((".", " ")):
            return None, f"non-canonical whitespace or trailing dot in segment: {part!r}"
        for char in part:
            cp = ord(char)
            if cp < 0x20 or cp == 0x7F:
                return None, f"control character U+{cp:04X}"
            if char in INVALID_WINDOWS_CHARS:
                return None, f"Windows-invalid character {char!r}"
        # Windows resolves device names after trimming and before the first dot.
        device_stem = part.split(".", 1)[0].rstrip(" .").upper()
        if device_stem in RESERVED_WINDOWS_NAMES:
            return None, f"reserved Windows device name: {part}"
    canonical = "/".join(parts)
    if canonical != value:
        return None, "path is not canonical"
    return canonical, None


def _path_identity(path: str) -> str:
    return unicodedata.normalize("NFC", path).casefold()


def _stat_is_reparse(info: os.stat_result) -> bool:
    attrs = getattr(info, "st_file_attributes", 0)
    return stat.S_ISLNK(info.st_mode) or bool(attrs & FILE_ATTRIBUTE_REPARSE_POINT)


def _absolute_lexical(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def _path_chain(path: Path) -> Iterable[Path]:
    absolute = _absolute_lexical(path)
    anchor = Path(absolute.anchor)
    current = anchor
    for part in absolute.parts[1:]:
        current = current / part
        yield current


def _prepare_root(root: Path) -> tuple[Path | None, Path | None, list[IntegrityError]]:
    """Validate every existing lexical component before resolving the root."""
    absolute = _absolute_lexical(root)
    for component in _path_chain(absolute):
        try:
            info = component.lstat()
        except OSError as exc:
            return None, None, [IntegrityError(str(component), "root", str(exc))]
        if _stat_is_reparse(info):
            return None, None, [IntegrityError(
                str(component), "reparse", "root ancestry contains a symlink/junction/reparse point")]
    try:
        info = absolute.lstat()
        if not stat.S_ISDIR(info.st_mode):
            return None, None, [IntegrityError(str(absolute), "root", "root is not a directory")]
        resolved = absolute.resolve(strict=True)
    except OSError as exc:
        return None, None, [IntegrityError(str(absolute), "root", str(exc))]
    return absolute, resolved, []


def _checked_candidate(
    root: Path, root_resolved: Path, relative: str, *, want_directory: bool
) -> tuple[Path | None, os.stat_result | None, IntegrityError | None]:
    """Check each component without following it, then prove containment."""
    current = root
    parts = relative.split("/") if relative else []
    for index, part in enumerate(parts):
        current = current / part
        try:
            info = current.lstat()
        except OSError as exc:
            return None, None, IntegrityError(relative, "io-error", str(exc))
        if _stat_is_reparse(info):
            return None, None, IntegrityError(
                relative, "reparse", f"component is a symlink/junction/reparse point: {current}")
        is_leaf = index == len(parts) - 1
        if not is_leaf and not stat.S_ISDIR(info.st_mode):
            return None, None, IntegrityError(relative, "non-regular", "parent component is not a directory")
    try:
        resolved = current.resolve(strict=True)
        resolved.relative_to(root_resolved)
    except (OSError, ValueError) as exc:
        return None, None, IntegrityError(relative, "traversal", f"path escapes or cannot resolve: {exc}")
    try:
        leaf_info = current.lstat()
    except OSError as exc:
        return None, None, IntegrityError(relative, "io-error", str(exc))
    expected = stat.S_ISDIR if want_directory else stat.S_ISREG
    if not expected(leaf_info.st_mode):
        kind = "directory" if want_directory else "regular file"
        return None, None, IntegrityError(relative, "non-regular", f"expected {kind}")
    return current, leaf_info, None


def _fingerprint(info: os.stat_result) -> tuple[int, int, int, int, int]:
    return (
        info.st_dev,
        info.st_ino,
        info.st_size,
        getattr(info, "st_mtime_ns", int(info.st_mtime * 1_000_000_000)),
        info.st_mode,
    )


def _hash_fd(fd: int, *, max_bytes: int = MAX_FILE_BYTES) -> str:
    digest = hashlib.sha256()
    consumed = 0
    while True:
        remaining = max_bytes + 1 - consumed
        chunk = os.read(fd, min(HASH_CHUNK_BYTES, remaining))
        if not chunk:
            return digest.hexdigest()
        consumed += len(chunk)
        if consumed > max_bytes:
            raise OSError(f"file grew past {max_bytes}-byte limit during hash")
        digest.update(chunk)


def _read_regular_file(
    root: Path, root_resolved: Path, relative: str
) -> tuple[dict[str, Any] | None, IntegrityError | None]:
    candidate, before_path, error = _checked_candidate(
        root, root_resolved, relative, want_directory=False)
    if error is not None or candidate is None or before_path is None:
        return None, error
    if before_path.st_size > MAX_FILE_BYTES:
        return None, IntegrityError(relative, "resource-limit", f"file exceeds {MAX_FILE_BYTES} bytes")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd: int | None = None
    try:
        fd = os.open(candidate, flags)
        before_fd = os.fstat(fd)
        if not stat.S_ISREG(before_fd.st_mode):
            return None, IntegrityError(relative, "non-regular", "opened object is not a regular file")
        if _fingerprint(before_path) != _fingerprint(before_fd):
            return None, IntegrityError(relative, "io-race", "file changed between inspection and open")
        _, confirmed_path, confirmed_error = _checked_candidate(
            root, root_resolved, relative, want_directory=False)
        if confirmed_error is not None or confirmed_path is None:
            return None, IntegrityError(
                relative,
                "io-race",
                f"path components changed before read: {confirmed_error or 'missing path'}",
            )
        if _fingerprint(confirmed_path) != _fingerprint(before_fd):
            return None, IntegrityError(relative, "io-race", "path changed before read")
        if before_fd.st_size > MAX_FILE_BYTES:
            return None, IntegrityError(relative, "resource-limit", f"file exceeds {MAX_FILE_BYTES} bytes")
        digest = _hash_fd(fd, max_bytes=before_fd.st_size)
        after_fd = os.fstat(fd)
    except OSError as exc:
        return None, IntegrityError(relative, "io-error", str(exc))
    finally:
        if fd is not None:
            os.close(fd)
    try:
        after_path = candidate.lstat()
    except OSError as exc:
        return None, IntegrityError(relative, "io-race", f"path changed after read: {exc}")
    if _stat_is_reparse(after_path):
        return None, IntegrityError(relative, "io-race", "path became a reparse point during read")
    if not (_fingerprint(before_path) == _fingerprint(before_fd) == _fingerprint(after_fd) == _fingerprint(after_path)):
        return None, IntegrityError(relative, "io-race", "file changed or was replaced during read")
    return {"path": relative, "sha256": digest, "size": before_fd.st_size}, None


def scan_directory(
    root: Path, *, ignored_root_paths: frozenset[str] = ROOT_IGNORES
) -> tuple[list[dict[str, Any]], list[IntegrityError]]:
    """Capture one bounded, deterministic snapshot without following reparses."""
    root, root_resolved, errors = _prepare_root(root)
    if errors or root is None or root_resolved is None:
        return [], errors

    entries: list[dict[str, Any]] = []
    identities: dict[str, str] = {}
    total_bytes = 0
    node_count = 0
    stopped = False

    def register(relative: str) -> bool:
        canonical, problem = _canonical_relative_path(relative)
        if problem is not None or canonical is None:
            errors.append(IntegrityError(relative, "path-safety", problem or "invalid path"))
            return False
        identity = _path_identity(canonical)
        previous = identities.get(identity)
        if previous is not None and previous != canonical:
            errors.append(IntegrityError(
                canonical, "case-collision", f"portable alias collides with {previous!r}"))
            return False
        identities[identity] = canonical
        return True

    def walk(directory: Path, relative_dir: str, depth: int) -> None:
        nonlocal node_count, total_bytes, stopped
        if stopped:
            return
        if depth > MAX_DIRECTORY_DEPTH:
            errors.append(IntegrityError(relative_dir, "resource-limit", "directory depth limit exceeded"))
            stopped = True
            return
        if relative_dir:
            _, _, error = _checked_candidate(
                root, root_resolved, relative_dir, want_directory=True)
            if error is not None:
                errors.append(error)
                return
        try:
            children = []
            with os.scandir(directory) as iterator:
                for child in iterator:
                    node_count += 1
                    if node_count > MAX_DIRECTORY_ENTRY_COUNT:
                        errors.append(IntegrityError(
                            relative_dir or ".",
                            "resource-limit",
                            f"filesystem node count exceeds {MAX_DIRECTORY_ENTRY_COUNT}",
                        ))
                        stopped = True
                        return
                    children.append(child)
            children.sort(key=lambda item: item.name)
        except OSError as exc:
            errors.append(IntegrityError(relative_dir or ".", "io-error", str(exc)))
            return
        for child in children:
            relative = f"{relative_dir}/{child.name}" if relative_dir else child.name
            try:
                info = child.stat(follow_symlinks=False)
            except OSError as exc:
                if relative not in ignored_root_paths:
                    errors.append(IntegrityError(relative, "io-error", str(exc)))
                continue
            if relative in ignored_root_paths:
                if _stat_is_reparse(info) or not stat.S_ISREG(info.st_mode):
                    errors.append(IntegrityError(
                        relative, "concealed-payload",
                        "ignored root path is not a regular file"))
                continue
            if not register(relative):
                continue
            if _stat_is_reparse(info):
                errors.append(IntegrityError(
                    relative, "reparse", "symlink/junction/reparse point is forbidden"))
                continue
            if stat.S_ISDIR(info.st_mode):
                walk(Path(child.path), relative, depth + 1)
                continue
            if not stat.S_ISREG(info.st_mode):
                errors.append(IntegrityError(relative, "non-regular", "filesystem object is not a regular file"))
                continue
            if len(entries) >= MAX_ENTRY_COUNT:
                errors.append(IntegrityError(relative, "resource-limit", f"entry count exceeds {MAX_ENTRY_COUNT}"))
                stopped = True
                return
            entry, error = _read_regular_file(root, root_resolved, relative)
            if error is not None or entry is None:
                errors.append(error or IntegrityError(relative, "io-error", "unknown read failure"))
                continue
            if total_bytes + entry["size"] > MAX_TOTAL_BYTES:
                errors.append(IntegrityError(relative, "resource-limit", f"snapshot exceeds {MAX_TOTAL_BYTES} bytes"))
                stopped = True
                return
            total_bytes += entry["size"]
            entries.append(entry)

    walk(root, "", 0)
    entries.sort(key=lambda entry: entry["path"])
    errors.sort(key=lambda error: (error.path, error.category, error.message))
    return entries, errors


def _text_problem(value: Any, *, limit: int = MAX_PROVENANCE_BYTES) -> str | None:
    if not isinstance(value, str) or not value:
        return "must be a non-empty string"
    if value != value.strip():
        return "must not have leading or trailing whitespace"
    if len(value.encode("utf-8")) > limit:
        return f"exceeds {limit} UTF-8 bytes"
    for char in value:
        cp = ord(char)
        if cp < 0x20 or cp == 0x7F:
            return f"contains control character U+{cp:04X}"
    return None


def _require_canonical(value: Any, where: str) -> str:
    canonical, problem = _canonical_relative_path(value)
    if problem is not None or canonical is None:
        raise ManifestFormatError(f"{where}: unsafe path {value!r}: {problem}")
    return canonical


def _validate_policy_rule(rule: Any, index: int, licenses: dict[str, Any], seen_ids: set[str]) -> None:
    where = f"rules[{index}]"
    if not isinstance(rule, dict):
        raise ManifestFormatError(f"{where} must be an object")
    unknown = set(rule) - RULE_KEYS
    if unknown:
        raise ManifestFormatError(f"{where} has unknown keys: {sorted(unknown)}")
    for required in ("id", "license", "provenance", "evidence"):
        if required not in rule:
            raise ManifestFormatError(f"{where} is missing {required!r}")
    rule_id = rule["id"]
    if not isinstance(rule_id, str) or RULE_ID_RE.fullmatch(rule_id) is None:
        raise ManifestFormatError(f"{where} id must match {RULE_ID_RE.pattern}")
    if rule_id in seen_ids:
        raise ManifestFormatError(f"duplicate rule id: {rule_id}")
    seen_ids.add(rule_id)
    where = f"rule {rule_id!r}"
    if rule["license"] not in licenses:
        raise ManifestFormatError(f"{where} license {rule['license']!r} is not declared in licenses")
    problem = _text_problem(rule["provenance"])
    if problem:
        raise ManifestFormatError(f"{where} provenance {problem}")
    evidence = rule["evidence"]
    if not isinstance(evidence, list):
        raise ManifestFormatError(f"{where} evidence must be an array")
    canonical_evidence = [_require_canonical(item, f"{where} evidence") for item in evidence]
    if len(set(canonical_evidence)) != len(canonical_evidence):
        raise ManifestFormatError(f"{where} evidence has duplicates")
    if rule["license"] == NOASSERTION:
        gap = rule.get("gap")
        if not isinstance(gap, str) or GAP_ID_RE.fullmatch(gap) is None:
            raise ManifestFormatError(
                f"{where} asserts no license and must name the owning work item in 'gap'")
    else:
        if "gap" in rule:
            raise ManifestFormatError(f"{where} asserts a license and must not declare a gap")
        if not canonical_evidence:
            raise ManifestFormatError(f"{where} asserts a license and must cite tracked evidence")

    files = rule.get("files", {})
    if not isinstance(files, dict):
        raise ManifestFormatError(f"{where} files must be an object of path -> sha256")
    for path, digest in files.items():
        _require_canonical(path, f"{where} files")
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            raise ManifestFormatError(f"{where} files[{path!r}] must be a lowercase sha256")
    prefixes = rule.get("prefixes", [])
    if not isinstance(prefixes, list):
        raise ManifestFormatError(f"{where} prefixes must be an array")
    for prefix in prefixes:
        if not isinstance(prefix, str) or not prefix.endswith("/"):
            raise ManifestFormatError(f"{where} prefix {prefix!r} must be a directory ending in '/'")
        _require_canonical(prefix[:-1], f"{where} prefixes")
    records = rule.get("records", [])
    if not isinstance(records, list):
        raise ManifestFormatError(f"{where} records must be an array")
    for record in records:
        if not isinstance(record, dict) or set(record) - RECORD_KEYS or not {"path", "format"} <= set(record):
            raise ManifestFormatError(f"{where} record must contain path, format, and optional exclude")
        _require_canonical(record["path"], f"{where} record")
        if record["format"] not in RECORD_FORMATS:
            raise ManifestFormatError(f"{where} record format {record['format']!r} is not supported")
        exclude = record.get("exclude", [])
        if not isinstance(exclude, list):
            raise ManifestFormatError(f"{where} record exclude must be an array")
        excluded = [_require_canonical(item, f"{where} record exclude") for item in exclude]
        if len(set(excluded)) != len(excluded):
            raise ManifestFormatError(f"{where} record exclude has duplicates")
    if not (files or prefixes or records):
        raise ManifestFormatError(f"{where} claims no files, prefixes, or records")


def load_provenance_policy(path: Path) -> dict[str, Any]:
    """Load and structurally validate a provenance policy; filesystem checks come later."""
    data = _read_bounded_json(path, limit=MAX_MANIFEST_BYTES)
    if not isinstance(data, dict):
        raise ManifestFormatError("provenance policy root must be an object")
    if set(data) != {"version", "root", "licenses", "rules"}:
        raise ManifestFormatError("provenance policy must contain exactly version, root, licenses, and rules")
    if data["version"] != PROVENANCE_POLICY_VERSION:
        raise ManifestFormatError(f"unsupported provenance policy version: {data['version']!r}")
    if not isinstance(data["root"], str) or not data["root"]:
        raise ManifestFormatError("provenance policy root must be a non-empty string")
    licenses = data["licenses"]
    if not isinstance(licenses, dict) or not licenses:
        raise ManifestFormatError("provenance policy licenses must be a non-empty object")
    for license_id, details in licenses.items():
        if LICENSE_ID_RE.fullmatch(license_id) is None:
            raise ManifestFormatError(f"invalid license identifier: {license_id!r}")
        if not isinstance(details, dict) or set(details) != {"name"} or _text_problem(details["name"]):
            raise ManifestFormatError(f"license {license_id!r} must be an object with exactly a non-empty name")
    rules = data["rules"]
    if not isinstance(rules, list) or not rules:
        raise ManifestFormatError("provenance policy rules must be a non-empty array")
    seen_ids: set[str] = set()
    for index, rule in enumerate(rules):
        _validate_policy_rule(rule, index, licenses, seen_ids)
    return data


def _record_claims(
    fmt: str, data: Any, root_name: str
) -> list[tuple[str, str | None, str | None, str | None]]:
    """Return (root-relative path, detail, license, sha256) rows for one record file."""
    rows: list[tuple[str, str | None, str | None, str | None]] = []
    prefix = f"{root_name}/"
    if not isinstance(data, dict):
        raise ManifestFormatError("record root must be an object")
    if fmt == "terrafront-asset-manifest":
        files = data.get("files")
        if not isinstance(files, list):
            raise ManifestFormatError("record files must be an array")
        for index, row in enumerate(files):
            if not isinstance(row, dict):
                raise ManifestFormatError(f"record files[{index}] must be an object")
            pack, author, license_id = (row.get(key) for key in ("source_pack", "author", "license"))
            if any(_text_problem(part) for part in (pack, author, license_id)):
                raise ManifestFormatError(
                    f"record files[{index}] must record source_pack, author, and license")
            url = row.get("url", "")
            if url != "" and _text_problem(url):
                raise ManifestFormatError(f"record files[{index}] url must be a string")
            path = _require_canonical(row.get("path"), f"record files[{index}]")
            detail = f"{pack} by {author}" + (f" <{url}>" if url else "")
            rows.append((path, detail, license_id, None))
    elif fmt == "blender-provenance":
        license_info = data.get("license")
        license_name = license_info.get("name") if isinstance(license_info, dict) else None
        if _text_problem(license_name):
            raise ManifestFormatError("record must name its license")
        assets = data.get("assets")
        if not isinstance(assets, list):
            raise ManifestFormatError("record assets must be an array")
        for index, row in enumerate(assets):
            if not isinstance(row, dict) or _text_problem(row.get("name")):
                raise ManifestFormatError(f"record assets[{index}] must be an object with a name")
            for path_key, hash_key in (("obj_path", "obj_sha256"), ("mtl_path", "mtl_sha256")):
                value = row.get(path_key)
                digest = row.get(hash_key)
                if not isinstance(value, str) or not value.startswith(prefix):
                    raise ManifestFormatError(f"record assets[{index}].{path_key} must be under {prefix}")
                if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
                    raise ManifestFormatError(f"record assets[{index}].{hash_key} must be a sha256")
                path = _require_canonical(value[len(prefix):], f"record assets[{index}]")
                rows.append((path, f"Blender-authored {row['name']}", license_name, digest))
    elif fmt == "starter-model-manifest":
        assets = data.get("assets")
        if not isinstance(assets, list):
            raise ManifestFormatError("record assets must be an array")
        for index, row in enumerate(assets):
            if not isinstance(row, dict) or not isinstance(row.get("path"), str):
                raise ManifestFormatError(f"record assets[{index}] must be an object with a path")
            value = row["path"]
            if not value.startswith(prefix):
                continue  # rows for other trees (e.g. Templates) are verified by their own manifests
            if not value.endswith(".obj"):
                raise ManifestFormatError(f"record assets[{index}] path must name an .obj model")
            label = f"{row.get('pack')}/{row.get('name')}"
            if _text_problem(label):
                raise ManifestFormatError(f"record assets[{index}] must name its pack and model")
            obj = _require_canonical(value[len(prefix):], f"record assets[{index}]")
            for path, hash_key in ((obj, "sha256"), (obj[:-4] + ".mtl", "mtlSha256")):
                digest = row.get(hash_key)
                if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
                    raise ManifestFormatError(f"record assets[{index}].{hash_key} must be a sha256")
                rows.append((path, f"starter model {label}", None, digest))
    else:  # pragma: no cover - load_provenance_policy rejects unknown formats first
        raise ManifestFormatError(f"unsupported record format {fmt!r}")
    seen: set[str] = set()
    for path, *_ in rows:
        if path in seen:
            raise ManifestFormatError(f"record declares {path!r} twice")
        seen.add(path)
    return rows


def resolve_provenance(
    policy_path: Path,
    root: Path,
    entries: list[dict[str, Any]],
    repo_root: Path | None = None,
) -> tuple[dict[str, tuple[str, str]], list[IntegrityError]]:
    """Resolve (license, provenance) for every scanned entry, failing closed.

    Precedence: an exact claim (``files`` or a record row) beats any prefix, and
    the longest matching prefix beats shorter ones. Two exact claims on one
    path, or one prefix claimed twice, is ambiguous. Every file must be claimed
    and every claim must govern at least one file.
    """
    policy_label = str(policy_path)
    root = _absolute_lexical(root)
    repo_root = _absolute_lexical(repo_root) if repo_root is not None else root.parent
    try:
        policy = load_provenance_policy(_absolute_lexical(policy_path))
    except (ManifestFormatError, OSError) as exc:
        return {}, [IntegrityError(policy_label, "provenance-policy", str(exc))]
    if policy["root"] != root.name:
        return {}, [IntegrityError(
            policy_label, "provenance-policy",
            f"policy root {policy['root']!r} does not exactly match asset root {root.name!r}")]
    repo, repo_resolved, repo_errors = _prepare_root(repo_root)
    if repo_errors or repo is None or repo_resolved is None:
        return {}, [IntegrityError(policy_label, "provenance-policy", str(error)) for error in repo_errors]

    errors: list[IntegrityError] = []
    licenses: dict[str, Any] = policy["licenses"]
    disk = {entry["path"]: entry["sha256"] for entry in entries}
    exact: dict[str, tuple[dict[str, Any], str | None]] = {}
    prefixes: dict[str, dict[str, Any]] = {}

    def claim(path: str, rule: dict[str, Any], detail: str | None) -> None:
        previous = exact.get(path)
        if previous is not None:
            errors.append(IntegrityError(
                path, "provenance-ambiguous",
                f"claimed by both rule {previous[0]['id']!r} and rule {rule['id']!r}"))
            return
        exact[path] = (rule, detail)

    for rule in policy["rules"]:
        rule_id = rule["id"]
        for evidence in rule["evidence"]:
            _, _, error = _checked_candidate(repo, repo_resolved, evidence, want_directory=False)
            if error is not None:
                errors.append(IntegrityError(
                    evidence, "provenance-policy",
                    f"rule {rule_id!r} evidence is missing or unsafe: [{error.category}] {error.message}"))
        for path, digest in rule.get("files", {}).items():
            if path not in disk:
                errors.append(IntegrityError(
                    path, "provenance-record-missing", f"rule {rule_id!r} claims a file that is absent"))
                continue
            if disk[path] != digest:
                errors.append(IntegrityError(
                    path, "provenance-stale",
                    f"rule {rule_id!r} is bound to sha256 {digest}, actual {disk[path]}"))
                continue
            claim(path, rule, None)
        for record in rule.get("records", []):
            record_path = record["path"]
            candidate, _, error = _checked_candidate(repo, repo_resolved, record_path, want_directory=False)
            if error is not None or candidate is None:
                errors.append(IntegrityError(
                    record_path, "provenance-policy",
                    f"rule {rule_id!r} record is missing or unsafe: {error.message if error else 'missing'}"))
                continue
            try:
                rows = _record_claims(record["format"], _read_bounded_json(candidate), root.name)
            except (ManifestFormatError, OSError) as exc:
                errors.append(IntegrityError(record_path, "provenance-policy", f"rule {rule_id!r}: {exc}"))
                continue
            row_paths = {row[0] for row in rows}
            excluded = set(record.get("exclude", []))
            for path in sorted(excluded - row_paths):
                errors.append(IntegrityError(
                    path, "provenance-policy",
                    f"rule {rule_id!r} excludes a path that {record_path} does not record"))
            allowed_licenses = {rule["license"], licenses[rule["license"]]["name"]}
            for path, detail, record_license, digest in rows:
                if path in excluded:
                    continue
                if path not in disk:
                    errors.append(IntegrityError(
                        path, "provenance-record-missing", f"{record_path} records a file that is absent"))
                    continue
                if digest is not None and disk[path] != digest:
                    errors.append(IntegrityError(
                        path, "provenance-stale",
                        f"{record_path} is bound to sha256 {digest}, actual {disk[path]}"))
                    continue
                if record_license is not None and record_license not in allowed_licenses:
                    errors.append(IntegrityError(
                        path, "provenance-record-conflict",
                        f"{record_path} records license {record_license!r}; rule {rule_id!r} asserts "
                        f"{rule['license']!r}"))
                    continue
                claim(path, rule, detail)
        for prefix in rule.get("prefixes", []):
            previous = prefixes.get(prefix)
            if previous is not None:
                errors.append(IntegrityError(
                    prefix, "provenance-ambiguous",
                    f"prefix claimed by both rule {previous['id']!r} and rule {rule_id!r}"))
                continue
            prefixes[prefix] = rule

    ordered_prefixes = sorted(prefixes, key=lambda item: (-len(item), item))
    used_prefixes: set[str] = set()
    resolved: dict[str, tuple[str, str]] = {}
    for path in sorted(disk):
        governing = exact.get(path)
        if governing is None:
            match = next((prefix for prefix in ordered_prefixes if path.startswith(prefix)), None)
            if match is None:
                errors.append(IntegrityError(path, "provenance-unclaimed", "no provenance rule claims this file"))
                continue
            used_prefixes.add(match)
            governing = (prefixes[match], None)
        rule, detail = governing
        text = rule["provenance"] if detail is None else f"{rule['provenance']}: {detail}"
        if rule["license"] == NOASSERTION:
            text += f" (license unasserted; gap {rule['gap']})"
        text += f" [{rule['id']}]"
        problem = _text_problem(text)
        if problem:
            errors.append(IntegrityError(path, "provenance-policy", f"resolved provenance {problem}"))
            continue
        resolved[path] = (rule["license"], text)
    for prefix in sorted(set(prefixes) - used_prefixes):
        errors.append(IntegrityError(
            prefix, "provenance-unused-claim",
            f"rule {prefixes[prefix]['id']!r} prefix governs no file"))
    errors.sort(key=lambda error: (error.path, error.category, error.message))
    return resolved, errors


def generate_manifest(
    root: Path,
    provenance_policy: Path | None = None,
    repo_root: Path | None = None,
) -> tuple[dict[str, Any], list[IntegrityError]]:
    """Snapshot ``root``. With a policy, emit schema v2 with per-entry provenance.

    Without a policy the result is a legacy v1 integrity-only manifest, suitable
    only for staged fixtures; repository gates require v2.
    """
    entries, errors = scan_directory(root)
    version = LEGACY_MANIFEST_SCHEMA_VERSION
    if provenance_policy is not None:
        version = MANIFEST_SCHEMA_VERSION
        if not errors:
            resolved, provenance_errors = resolve_provenance(provenance_policy, root, entries, repo_root)
            errors.extend(provenance_errors)
            entries = [
                {**entry, "license": resolved[entry["path"]][0], "provenance": resolved[entry["path"]][1]}
                for entry in entries
                if entry["path"] in resolved
            ]
    manifest = {
        "version": version,
        "algorithm": HASH_ALGORITHM,
        "root": _absolute_lexical(root).name,
        "fileCount": len(entries),
        "entries": entries,
    }
    return manifest, errors


def provenance_summary(manifest: dict[str, Any]) -> dict[str, Any]:
    entries = manifest.get("entries", [])
    unasserted = [entry for entry in entries if entry.get("license") == NOASSERTION]
    gaps = sorted({
        match.group(1)
        for entry in unasserted
        for match in [re.search(r"\(license unasserted; gap ([A-Z]+-[0-9]{3})\)", entry.get("provenance", ""))]
        if match
    })
    return {
        "entries": len(entries),
        "asserted": sum(1 for entry in entries if entry.get("license") not in (None, NOASSERTION)),
        "unasserted": len(unasserted),
        "gaps": gaps,
    }


def _package_closure_module() -> Any:
    """Load the sibling closure module; this verifier is also loaded by file path."""
    import importlib.util

    path = Path(__file__).resolve().with_name("package_closure.py")
    spec = importlib.util.spec_from_file_location("spark_asset_package_closure", path)
    if spec is None or spec.loader is None:
        raise ManifestFormatError(f"cannot load the package closure module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def derive_profile_closure(
    repo_root: Path, manifest: dict[str, Any], profile: str
) -> tuple[dict[str, list[str]] | None, list[Path]]:
    """Return ``(closure, inputs)`` for a closure profile, or ``(None, [])`` for a whole-tree profile.

    Raises ``ManifestFormatError`` when the closure cannot be derived: an
    unresolved reference, a NOASSERTION entry the profile needs, or a profile
    definition that disagrees with the module inventory.
    """
    if profile not in PACKAGE_PROFILES:
        raise ManifestFormatError(f"unknown package profile: {profile!r}")
    if profile not in CLOSURE_PROFILES:
        return None, []
    closure_module = _package_closure_module()
    try:
        return closure_module.derive_closure(repo_root, manifest, profile)
    except closure_module.ClosureError as exc:
        raise ManifestFormatError(f"package profile {profile!r} asset closure:\n{exc}") from exc


def derive_package_manifest(
    manifest: dict[str, Any], profile: str, closure: Iterable[str] | None = None
) -> tuple[dict[str, Any], list[str]]:
    """Return ``(package manifest, excluded paths)`` for one package profile.

    The input must already have passed ``load_manifest``. A profile in
    ``NOASSERTION_EXCLUDED_PROFILES`` drops every NOASSERTION entry (OD-09) and
    requires schema v2, because a v1 manifest cannot show which files lack a
    license record. With ``closure`` (see ``derive_profile_closure``) only the
    closure entries are kept; a closure path the manifest does not declare, or
    one whose license is NOASSERTION, is an error rather than a silent drop.
    Entry objects are copied unchanged, so the result stays sorted and
    verifiable against an installed root.
    """
    if profile not in PACKAGE_PROFILES:
        raise ManifestFormatError(f"unknown package profile: {profile!r}")
    entries = manifest["entries"]
    excluded: list[str] = []
    if profile in NOASSERTION_EXCLUDED_PROFILES:
        if manifest["version"] != MANIFEST_SCHEMA_VERSION:
            raise ManifestFormatError(
                f"package profile {profile!r} needs a schema v{MANIFEST_SCHEMA_VERSION} manifest "
                "that records each entry's license")
    if closure is not None:
        kept = set(closure)
        by_path = {entry["path"]: entry for entry in entries}
        undeclared = sorted(kept - by_path.keys())
        if undeclared:
            raise ManifestFormatError(f"package profile {profile!r} closure names undeclared files: {undeclared}")
        if profile in NOASSERTION_EXCLUDED_PROFILES:
            unasserted = sorted(path for path in kept if by_path[path]["license"] == NOASSERTION)
            if unasserted:
                raise ManifestFormatError(
                    f"package profile {profile!r} closure needs NOASSERTION files (OD-09): {unasserted}")
        excluded = [entry["path"] for entry in entries if entry["path"] not in kept]
        entries = [entry for entry in entries if entry["path"] in kept]
    elif profile in NOASSERTION_EXCLUDED_PROFILES:
        excluded = [entry["path"] for entry in entries if entry["license"] == NOASSERTION]
        entries = [entry for entry in entries if entry["license"] != NOASSERTION]
    derived = {
        "version": manifest["version"],
        "algorithm": manifest["algorithm"],
        "root": manifest["root"],
        "fileCount": len(entries),
        "entries": [dict(entry) for entry in entries],
    }
    return derived, excluded


def package_exclusion_prefixes(all_paths: Iterable[str], excluded: Iterable[str]) -> list[str]:
    """Collapse excluded files into the fewest directory prefixes and file paths.

    A directory (returned with a trailing ``/``) is used only when no kept file
    lives anywhere beneath it, so an install rule built from the result cannot
    drop a kept file. Files in mixed directories are returned individually.
    """
    excluded_set = set(excluded)
    kept_directories: set[str] = set()
    for path in all_paths:
        if path in excluded_set:
            continue
        parts = path.split("/")
        for depth in range(1, len(parts)):
            kept_directories.add("/".join(parts[:depth]))
    result: set[str] = set()
    for path in excluded_set:
        parts = path.split("/")
        chosen = path
        for depth in range(1, len(parts)):
            directory = "/".join(parts[:depth])
            if directory not in kept_directories:
                chosen = f"{directory}/"
                break
        result.add(chosen)
    return sorted(result)


def package_profile_errors(
    manifest: dict[str, Any],
    profile: str,
    label: str,
    source_manifest: dict[str, Any] | None = None,
) -> list[IntegrityError]:
    """Reject a package manifest that ships content its profile excludes.

    For a NOASSERTION-excluding profile every packaged entry must assert a
    license. With ``source_manifest`` (the reviewed repository manifest) every
    packaged entry must also equal a source entry exactly, so a package cannot
    relabel an excluded file or add a file whose provenance was never reviewed.
    """
    if profile not in PACKAGE_PROFILES:
        return [IntegrityError(label, "package-profile", f"unknown package profile {profile!r}")]
    if profile not in NOASSERTION_EXCLUDED_PROFILES:
        return []
    for candidate, name in ((manifest, "package"), (source_manifest, "source")):
        if candidate is not None and candidate["version"] != MANIFEST_SCHEMA_VERSION:
            return [IntegrityError(
                label, "provenance-missing",
                f"{name} manifest version {candidate['version']} records no license; package profile "
                f"{profile!r} needs schema v{MANIFEST_SCHEMA_VERSION}")]
    source = None if source_manifest is None else {entry["path"]: entry for entry in source_manifest["entries"]}
    errors: list[IntegrityError] = []
    for entry in manifest["entries"]:
        path = entry["path"]
        if entry["license"] == NOASSERTION:
            errors.append(IntegrityError(
                path, "profile-excluded",
                f"package profile {profile!r} must not ship an asset whose license is NOASSERTION (OD-09)"))
            continue
        if source is None:
            continue
        reviewed = source.get(path)
        if reviewed is None:
            errors.append(IntegrityError(
                path, "profile-unreviewed",
                f"package profile {profile!r} ships a file the source manifest does not declare"))
        elif reviewed["license"] == NOASSERTION:
            errors.append(IntegrityError(
                path, "profile-excluded",
                f"source manifest records NOASSERTION for this file; package profile {profile!r} excludes it"))
        elif reviewed != entry:
            errors.append(IntegrityError(
                path, "profile-mismatch",
                "packaged entry differs from the source manifest entry"))
    return errors


def verify_package_manifest(
    manifest_path: Path,
    root: Path,
    profile: str,
    *,
    source_manifest_path: Path | None = None,
    provenance_policy: Path | None = None,
    require_provenance: bool = False,
) -> list[IntegrityError]:
    """Verify an installed package root, then apply its package profile.

    For a NOASSERTION-excluding profile the source manifest defaults to the
    repository manifest beside this verifier.
    """
    if profile not in PACKAGE_PROFILES:
        return [IntegrityError(str(manifest_path), "package-profile", f"unknown package profile {profile!r}")]
    excludes = profile in NOASSERTION_EXCLUDED_PROFILES
    errors = verify_manifest(
        manifest_path, root,
        provenance_policy=provenance_policy,
        require_provenance=require_provenance or excludes)
    if not excludes:
        return errors
    # A manifest that did not load, or records no license, already failed.
    if any(error.category in ("manifest-load", "provenance-missing") for error in errors):
        return errors
    source_path = source_manifest_path or (REPO_ROOT / KNOWN_MANIFESTS[0][0])
    try:
        manifest = load_manifest(_absolute_lexical(manifest_path))
        source = load_manifest(_absolute_lexical(source_path))
    except (ManifestFormatError, OSError) as exc:
        return errors + [IntegrityError(str(source_path), "manifest-load", str(exc))]
    errors += package_profile_errors(manifest, profile, str(manifest_path), source)
    return errors + _package_closure_errors(manifest, profile, _absolute_lexical(source_path), source)


def _package_closure_errors(
    manifest: dict[str, Any], profile: str, source_path: Path, source: dict[str, Any]
) -> list[IntegrityError]:
    """Require a closure profile's package to ship exactly its derived asset closure.

    The closure is derived from the repository that holds the source manifest
    (``<repo>/<root>/assets.integrity.json``) when that tree defines the
    package profiles, and otherwise from the checkout holding this verifier.
    It never fails open: when neither defines the profiles the package is
    rejected with a ``profile-closure`` error.
    """
    if profile not in CLOSURE_PROFILES:
        return []
    closure_module = _package_closure_module()
    definitions = closure_module.PROFILE_DEFINITIONS_RELATIVE
    repo_root = source_path.parent.parent
    if source_path.name != MANIFEST_FILENAME or not (repo_root / definitions).is_file():
        repo_root = REPO_ROOT
    if not (repo_root / definitions).is_file():
        return [IntegrityError(
            str(source_path), "profile-closure",
            f"package profile {profile!r} needs {definitions.as_posix()} to derive its asset closure, "
            f"but neither the source manifest's tree nor {REPO_ROOT} provides it")]
    try:
        closure, _ = derive_profile_closure(repo_root, source, profile)
    except ManifestFormatError as exc:
        return [IntegrityError(str(source_path), "profile-closure", str(exc))]
    assert closure is not None
    shipped = {entry["path"] for entry in manifest["entries"]}
    errors = [
        IntegrityError(path, "profile-outside-closure",
                       f"package profile {profile!r} ships a file outside its runtime asset closure")
        for path in sorted(shipped - closure.keys())
    ]
    errors += [
        IntegrityError(path, "profile-incomplete",
                       f"package profile {profile!r} omits a file its runtime asset closure needs "
                       f"(first needed by {closure[path][0]})")
        for path in sorted(closure.keys() - shipped)
    ]
    return errors


def manifest_bytes(manifest: dict[str, Any]) -> bytes:
    return (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def write_manifest(manifest: dict[str, Any], output: Path) -> None:
    """Write deterministic bytes. Callers must already have a clean snapshot."""
    output.write_bytes(manifest_bytes(manifest))


def load_manifest(path: Path) -> dict[str, Any]:
    data = _read_bounded_json(path)
    if not isinstance(data, dict):
        raise ManifestFormatError("manifest root must be an object")
    version = data.get("version")
    if isinstance(version, bool) or version not in SUPPORTED_MANIFEST_VERSIONS:
        raise ManifestFormatError(f"unsupported manifest version: {version!r}")
    entry_keys = ENTRY_KEYS if version == MANIFEST_SCHEMA_VERSION else LEGACY_ENTRY_KEYS
    if data.get("algorithm") != HASH_ALGORITHM:
        raise ManifestFormatError(f"unsupported algorithm: {data.get('algorithm')!r}")
    root_metadata = data.get("root")
    if not isinstance(root_metadata, str) or not root_metadata:
        raise ManifestFormatError("root must be a non-empty string")
    entries = data.get("entries")
    if not isinstance(entries, list):
        raise ManifestFormatError("entries must be an array")
    if len(entries) > MAX_ENTRY_COUNT:
        raise ManifestFormatError(f"entry count exceeds {MAX_ENTRY_COUNT}")
    file_count = data.get("fileCount")
    if isinstance(file_count, bool) or not isinstance(file_count, int) or file_count < 0:
        raise ManifestFormatError("fileCount must be a non-negative integer")
    if file_count != len(entries):
        raise ManifestFormatError(f"fileCount={file_count} but entries has {len(entries)} items")

    paths: set[str] = set()
    identities: dict[str, str] = {}
    previous_path: str | None = None
    total_bytes = 0
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ManifestFormatError(f"entry {index} must be an object")
        if set(entry) != entry_keys:
            raise ManifestFormatError(
                f"entry {index} must contain exactly {', '.join(sorted(entry_keys))} "
                f"for manifest version {version}")
        if version == MANIFEST_SCHEMA_VERSION:
            license_id = entry.get("license")
            if not isinstance(license_id, str) or LICENSE_ID_RE.fullmatch(license_id) is None:
                raise ManifestFormatError(f"entry {index} license must be a license identifier")
            problem = _text_problem(entry.get("provenance"))
            if problem:
                raise ManifestFormatError(f"entry {index} provenance {problem}")
        relative, problem = _canonical_relative_path(entry.get("path"))
        if problem is not None or relative is None:
            raise ManifestFormatError(f"entry {index} has unsafe path: {problem}")
        if relative in paths:
            raise ManifestFormatError(f"duplicate entry path: {relative}")
        identity = _path_identity(relative)
        if identity in identities:
            raise ManifestFormatError(
                f"portable path alias: {relative!r} collides with {identities[identity]!r}")
        if previous_path is not None and relative <= previous_path:
            raise ManifestFormatError("entries must be strictly sorted by path")
        digest = entry.get("sha256")
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            raise ManifestFormatError(f"entry {index} sha256 must be 64 lowercase hex characters")
        size = entry.get("size")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise ManifestFormatError(f"entry {index} size must be a non-negative integer")
        if size > MAX_FILE_BYTES:
            raise ManifestFormatError(f"entry {index} exceeds per-file size limit")
        total_bytes += size
        if total_bytes > MAX_TOTAL_BYTES:
            raise ManifestFormatError("declared total size exceeds limit")
        paths.add(relative)
        identities[identity] = relative
        previous_path = relative
    return data


def verify_manifest(
    manifest_path: Path,
    root: Path,
    *,
    provenance_policy: Path | None = None,
    repo_root: Path | None = None,
    require_provenance: bool = False,
) -> list[IntegrityError]:
    """Verify a manifest against a caller-authorized root in one disk scan.

    ``require_provenance`` rejects a legacy v1 manifest. ``provenance_policy``
    additionally proves every entry's license/provenance equals what the policy
    resolves for the bytes on disk.
    """
    try:
        manifest = load_manifest(_absolute_lexical(manifest_path))
    except (ManifestFormatError, OSError) as exc:
        return [IntegrityError(str(manifest_path), "manifest-load", str(exc))]

    absolute_root = _absolute_lexical(root)
    if manifest["root"] != absolute_root.name:
        return [IntegrityError(
            str(manifest_path), "root-metadata",
            f"manifest root {manifest['root']!r} does not exactly match caller root {absolute_root.name!r}")]

    errors: list[IntegrityError] = []
    has_provenance = manifest["version"] == MANIFEST_SCHEMA_VERSION
    if (require_provenance or provenance_policy is not None) and not has_provenance:
        errors.append(IntegrityError(
            str(manifest_path), "provenance-missing",
            f"manifest version {manifest['version']} carries no license/provenance; "
            f"regenerate schema v{MANIFEST_SCHEMA_VERSION} with the provenance policy"))

    disk_entries, scan_errors = scan_directory(absolute_root)
    if scan_errors:
        return errors + scan_errors

    declared = {entry["path"]: entry for entry in manifest["entries"]}
    disk = {entry["path"]: entry for entry in disk_entries}
    if provenance_policy is not None and has_provenance:
        resolved, provenance_errors = resolve_provenance(
            provenance_policy, absolute_root, disk_entries, repo_root)
        errors.extend(provenance_errors)
        for relative in sorted(declared.keys() & resolved.keys()):
            expected_license, expected_provenance = resolved[relative]
            entry = declared[relative]
            if entry["license"] != expected_license:
                errors.append(IntegrityError(
                    relative, "provenance-mismatch",
                    f"manifest license {entry['license']!r}, policy resolves {expected_license!r}"))
            if entry["provenance"] != expected_provenance:
                errors.append(IntegrityError(
                    relative, "provenance-mismatch",
                    f"manifest provenance {entry['provenance']!r}, policy resolves {expected_provenance!r}"))
    for relative in sorted(declared.keys() - disk.keys()):
        errors.append(IntegrityError(relative, "missing", "declared file is absent from snapshot"))
    for relative in sorted(disk.keys() - declared.keys()):
        errors.append(IntegrityError(relative, "undeclared", "snapshot file is absent from manifest"))
    for relative in sorted(declared.keys() & disk.keys()):
        expected = declared[relative]
        actual = disk[relative]
        if expected["size"] != actual["size"]:
            errors.append(IntegrityError(
                relative, "size-mismatch", f"expected {expected['size']}, actual {actual['size']}"))
        if expected["sha256"] != actual["sha256"]:
            errors.append(IntegrityError(
                relative, "hash-mismatch", f"expected {expected['sha256']}, actual {actual['sha256']}"))
    return errors


def _validate_lock(lock_path: Path) -> dict[str, str]:
    data = _read_bounded_json(lock_path)
    if not isinstance(data, dict):
        raise ManifestFormatError("template lock root must be an object")
    if data.get("version") != 1 or data.get("algorithm") != HASH_ALGORITHM:
        raise ManifestFormatError("template lock requires version 1 and sha256")
    assets = data.get("assets")
    if not isinstance(assets, dict):
        raise ManifestFormatError("template lock assets must be an object")
    if len(assets) > MAX_ENTRY_COUNT:
        raise ManifestFormatError(f"template lock exceeds {MAX_ENTRY_COUNT} entries")
    result: dict[str, str] = {}
    identities: dict[str, str] = {}
    for key, digest in assets.items():
        canonical, problem = _canonical_relative_path(key)
        if problem is not None or canonical is None:
            raise ManifestFormatError(f"unsafe template lock path {key!r}: {problem}")
        identity = _path_identity(canonical)
        if identity in identities:
            raise ManifestFormatError(
                f"template lock path alias: {canonical!r} collides with {identities[identity]!r}")
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            raise ManifestFormatError(f"invalid sha256 for template lock path {canonical!r}")
        identities[identity] = canonical
        result[canonical] = digest
    return result


def _validate_template_manifest(path: Path, template_name: str) -> dict[str, str]:
    data = _read_bounded_json(path)
    if not isinstance(data, dict):
        raise ManifestFormatError("template manifest root must be an object")
    if data.get("manifestVersion") != 1:
        raise ManifestFormatError("template manifest requires manifestVersion 1")
    if data.get("package") != template_name:
        raise ManifestFormatError(
            f"package {data.get('package')!r} does not exactly match {template_name!r}")
    assets = data.get("assets")
    if not isinstance(assets, list) or not assets:
        raise ManifestFormatError("template manifest assets must be a non-empty array")
    if len(assets) > MAX_ENTRY_COUNT:
        raise ManifestFormatError(f"template manifest exceeds {MAX_ENTRY_COUNT} entries")
    result: dict[str, str] = {}
    identities: dict[str, str] = {}
    for index, row in enumerate(assets):
        if not isinstance(row, dict):
            raise ManifestFormatError(f"template asset row {index} must be an object")
        relative, problem = _canonical_relative_path(row.get("path"))
        if problem is not None or relative is None:
            raise ManifestFormatError(f"template asset row {index} has unsafe path: {problem}")
        digest = row.get("sha256")
        if not isinstance(digest, str) or SHA256_RE.fullmatch(digest) is None:
            raise ManifestFormatError(f"template asset row {index} has invalid sha256")
        identity = _path_identity(relative)
        if relative in result:
            raise ManifestFormatError(f"duplicate template asset path: {relative}")
        if identity in identities:
            raise ManifestFormatError(
                f"template asset path alias: {relative!r} collides with {identities[identity]!r}")
        identities[identity] = relative
        result[relative] = digest
    return result


def verify_template_manifests(repo_root: Path) -> list[IntegrityError]:
    """Require exact manifest == lock == disk equality for every template."""
    repo, repo_resolved, root_errors = _prepare_root(repo_root)
    if root_errors or repo is None or repo_resolved is None:
        return root_errors
    templates, _, error = _checked_candidate(
        repo, repo_resolved, "Templates", want_directory=True)
    if error is not None or templates is None:
        return [error or IntegrityError("Templates", "missing", "template root missing")]
    lock_path, _, error = _checked_candidate(
        repo, repo_resolved, "Templates/assets.lock.json", want_directory=False)
    if error is not None or lock_path is None:
        return [error or IntegrityError("Templates/assets.lock.json", "missing", "lock missing")]
    try:
        lock = _validate_lock(lock_path)
    except ManifestFormatError as exc:
        return [IntegrityError("Templates/assets.lock.json", "manifest-load", str(exc))]

    errors: list[IntegrityError] = []
    declared_global: dict[str, str] = {}
    disk_global: dict[str, str] = {}
    try:
        template_children = sorted(os.scandir(templates), key=lambda item: item.name)
    except OSError as exc:
        return [IntegrityError("Templates", "io-error", str(exc))]

    templates_seen = 0
    for child in template_children:
        try:
            info = child.stat(follow_symlinks=False)
        except OSError as exc:
            errors.append(IntegrityError(f"Templates/{child.name}", "io-error", str(exc)))
            continue
        relative = f"Templates/{child.name}"
        if child.name in TEMPLATE_COLLECTION_METADATA:
            if _stat_is_reparse(info) or not stat.S_ISREG(info.st_mode):
                errors.append(IntegrityError(
                    relative,
                    "concealed-payload",
                    "template collection metadata is not a regular file",
                ))
            continue
        if _stat_is_reparse(info):
            errors.append(IntegrityError(
                relative, "reparse", "template directory is a reparse point"))
            continue
        if not stat.S_ISDIR(info.st_mode):
            errors.append(IntegrityError(
                relative, "undeclared", "template root entry is not a template directory"))
            continue
        template_name, problem = _canonical_relative_path(child.name)
        if problem is not None or template_name is None:
            errors.append(IntegrityError(f"Templates/{child.name}", "path-safety", problem or "invalid path"))
            continue
        if _stat_is_reparse(info):
            errors.append(IntegrityError(
                f"Templates/{template_name}", "reparse", "template directory is a reparse point"))
            continue
        assets_relative = f"Templates/{template_name}/Assets"
        assets_dir, _, assets_error = _checked_candidate(
            repo, repo_resolved, assets_relative, want_directory=True)
        if assets_error is not None or assets_dir is None:
            errors.append(assets_error or IntegrityError(assets_relative, "missing", "Assets directory missing"))
            continue
        templates_seen += 1
        manifest_relative = f"{assets_relative}/manifest.json"
        manifest_path, _, manifest_error = _checked_candidate(
            repo, repo_resolved, manifest_relative, want_directory=False)
        if manifest_error is not None or manifest_path is None:
            errors.append(manifest_error or IntegrityError(manifest_relative, "missing", "manifest missing"))
            continue
        try:
            declared = _validate_template_manifest(manifest_path, template_name)
        except ManifestFormatError as exc:
            errors.append(IntegrityError(manifest_relative, "manifest-load", str(exc)))
            continue
        disk_entries, scan_errors = scan_directory(
            assets_dir, ignored_root_paths=TEMPLATE_ROOT_METADATA)
        if scan_errors:
            errors.extend(IntegrityError(
                f"{template_name}/Assets/{item.path}", item.category, item.message)
                for item in scan_errors)
            continue
        disk = {item["path"]: item["sha256"] for item in disk_entries}
        for relative, digest in declared.items():
            declared_global[f"{template_name}/Assets/{relative}"] = digest
        for relative, digest in disk.items():
            disk_global[f"{template_name}/Assets/{relative}"] = digest

    if templates_seen == 0:
        errors.append(IntegrityError("Templates", "missing", "no template Assets directories found"))
    if errors:
        return sorted(errors, key=lambda item: (item.path, item.category, item.message))

    for key in sorted(declared_global.keys() - disk_global.keys()):
        errors.append(IntegrityError(key, "missing", "template manifest entry is absent from disk"))
    for key in sorted(disk_global.keys() - declared_global.keys()):
        errors.append(IntegrityError(key, "undeclared", "template disk asset is absent from manifest"))
    for key in sorted(declared_global.keys() & disk_global.keys()):
        if declared_global[key] != disk_global[key]:
            errors.append(IntegrityError(
                key, "hash-mismatch", f"manifest {declared_global[key]}, disk {disk_global[key]}"))
    for key in sorted(declared_global.keys() - lock.keys()):
        errors.append(IntegrityError(key, "lock-missing", "template manifest entry is absent from lock"))
    for key in sorted(lock.keys() - declared_global.keys()):
        errors.append(IntegrityError(key, "lock-extra", "lock entry is absent from template manifests"))
    for key in sorted(declared_global.keys() & lock.keys()):
        if declared_global[key] != lock[key]:
            errors.append(IntegrityError(
                key, "lock-mismatch", f"manifest {declared_global[key]}, lock {lock[key]}"))
    return errors


def verify_repository(repo_root: Path) -> list[IntegrityError]:
    repo_root = _absolute_lexical(repo_root)
    errors: list[IntegrityError] = []
    for manifest_relative, root_relative, policy_relative in KNOWN_MANIFESTS:
        errors.extend(verify_manifest(
            repo_root / manifest_relative,
            repo_root / root_relative,
            provenance_policy=repo_root / policy_relative,
            repo_root=repo_root,
            require_provenance=True,
        ))
    errors.extend(verify_template_manifests(repo_root))
    return errors


def _print_errors(errors: list[IntegrityError]) -> None:
    for error in errors:
        print(f"  {error}", file=sys.stderr)


def cmd_generate(args: argparse.Namespace) -> int:
    root = _absolute_lexical(Path(args.root))
    expected_output = root / MANIFEST_FILENAME
    output = _absolute_lexical(Path(args.output)) if args.output else expected_output
    if output != expected_output:
        print(
            f"Refusing output {output}: generation may only replace {expected_output}",
            file=sys.stderr,
        )
        return 1
    policy_arg = getattr(args, "provenance", None)
    policy = _absolute_lexical(Path(policy_arg)) if policy_arg else DEFAULT_PROVENANCE_POLICY
    manifest, errors = generate_manifest(root, provenance_policy=policy)
    if errors:
        print(f"Refusing generation after {len(errors)} scan/provenance error(s):", file=sys.stderr)
        _print_errors(errors)
        return 1
    payload = manifest_bytes(manifest)
    _, _, root_errors = _prepare_root(root)
    if root_errors:
        print("Refusing generation because the root changed after scanning:", file=sys.stderr)
        _print_errors(root_errors)
        return 1
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=root.parent,
            prefix=f".{root.name}-asset-integrity-",
            suffix=".tmp",
            delete=False,
        ) as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
            temporary = Path(stream.name)
        os.replace(temporary, output)
    except OSError as exc:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"Cannot write manifest: {exc}", file=sys.stderr)
        return 1
    print(f"Generated {output} with {manifest['fileCount']} entries")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    policy = getattr(args, "provenance", None)
    profile = getattr(args, "profile", None)
    source = getattr(args, "source_manifest", None)
    if source and not profile:
        print("--source-manifest requires --profile", file=sys.stderr)
        return 1
    options = {
        "provenance_policy": Path(policy) if policy else None,
        "require_provenance": bool(getattr(args, "require_provenance", False)),
    }
    if profile:
        errors = verify_package_manifest(
            Path(args.manifest), Path(args.root), profile,
            source_manifest_path=Path(source) if source else None, **options)
    else:
        errors = verify_manifest(Path(args.manifest), Path(args.root), **options)
    if errors:
        print(f"FAILED: {len(errors)} error(s)", file=sys.stderr)
        _print_errors(errors)
        return 1
    manifest = load_manifest(_absolute_lexical(Path(args.manifest)))
    suffix = f", package profile {profile}" if profile else ""
    print(f"OK: {manifest['fileCount']} entries verified (manifest v{manifest['version']}{suffix})")
    return 0


def cmd_package_profile(args: argparse.Namespace) -> int:
    """Write the package manifest and install exclusion list for one profile."""
    source = _absolute_lexical(Path(args.manifest))
    output = _absolute_lexical(Path(args.output))
    exclusions_output = _absolute_lexical(Path(args.exclusions))
    if len({source, output, exclusions_output}) != 3:
        print("Refusing: source, --output, and --exclusions must be three different files", file=sys.stderr)
        return 1
    repo_root = _absolute_lexical(Path(args.repo_root)) if args.repo_root else REPO_ROOT
    try:
        manifest = load_manifest(source)
        closure, inputs = derive_profile_closure(repo_root, manifest, args.profile)
        derived, excluded = derive_package_manifest(manifest, args.profile, closure)
    except (ManifestFormatError, OSError) as exc:
        print(f"Cannot derive package profile {args.profile!r}: {exc}", file=sys.stderr)
        return 1
    # Defense in depth: the derived manifest must pass the same check a
    # staged package will face.
    errors = package_profile_errors(derived, args.profile, str(output), manifest)
    if errors:
        print(f"FAILED: derived {args.profile} manifest violates its profile", file=sys.stderr)
        _print_errors(errors)
        return 1
    prefixes = package_exclusion_prefixes((entry["path"] for entry in manifest["entries"]), excluded)
    try:
        output.write_bytes(manifest_bytes(derived))
        exclusions_output.write_bytes("".join(f"{prefix}\n" for prefix in prefixes).encode("utf-8"))
        if args.inputs:
            inputs_output = _absolute_lexical(Path(args.inputs))
            inputs_output.write_bytes("".join(f"{path.as_posix()}\n" for path in inputs).encode("utf-8"))
    except OSError as exc:
        print(f"Cannot write package profile outputs: {exc}", file=sys.stderr)
        return 1
    print(f"Package profile {args.profile}: {derived['fileCount']} of {manifest['fileCount']} entries kept, "
          f"{_exclusion_summary(manifest, excluded)} excluded as {len(prefixes)} install exclusion(s)")
    return 0


def _exclusion_summary(manifest: dict[str, Any], excluded: list[str]) -> str:
    excluded_set = set(excluded)
    unasserted = sum(
        1 for entry in manifest["entries"]
        if entry["path"] in excluded_set and entry.get("license") == NOASSERTION)
    return (f"{len(excluded)} entries ({unasserted} NOASSERTION, "
            f"{len(excluded) - unasserted} outside the profile closure)")


def _format_summary(summary: dict[str, Any]) -> str:
    gaps = ", ".join(summary["gaps"]) or "none"
    return (
        f"provenance: {summary['entries']} entries, {summary['asserted']} license-asserted, "
        f"{summary['unasserted']} NOASSERTION (owning gaps: {gaps})"
    )


def cmd_check_all(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root) if args.repo_root else REPO_ROOT
    errors = verify_repository(repo_root)
    if errors:
        print(f"FAILED: {len(errors)} error(s)", file=sys.stderr)
        _print_errors(errors)
        return 1
    print("OK: first-party and template asset integrity checks passed")
    unasserted = 0
    for manifest_relative, _, _ in KNOWN_MANIFESTS:
        summary = provenance_summary(load_manifest(_absolute_lexical(Path(repo_root) / manifest_relative)))
        unasserted += summary["unasserted"]
        print(f"{manifest_relative} {_format_summary(summary)}")
    source_manifest = load_manifest(_absolute_lexical(Path(repo_root) / KNOWN_MANIFESTS[0][0]))
    selected = getattr(args, "profile", None)
    profiles = [selected] if selected else sorted(NOASSERTION_EXCLUDED_PROFILES | CLOSURE_PROFILES)
    for profile in profiles:
        try:
            closure, _ = derive_profile_closure(Path(repo_root), source_manifest, profile)
            derived, excluded = derive_package_manifest(source_manifest, profile, closure)
        except ManifestFormatError as exc:
            print(f"FAILED: cannot derive package profile {profile}: {exc}", file=sys.stderr)
            return 1
        profile_errors = package_profile_errors(derived, profile, profile, source_manifest)
        if profile_errors:
            print(f"FAILED: derived {profile} package manifest violates its profile", file=sys.stderr)
            _print_errors(profile_errors)
            return 1
        print(f"package profile {profile}: {derived['fileCount']} entries packaged, "
              f"{_exclusion_summary(source_manifest, excluded)} excluded")
        if selected:
            # With --profile, --strict-provenance judges only what that profile ships.
            unasserted = sum(1 for entry in derived["entries"] if entry.get("license") in (None, NOASSERTION))
            print(f"package profile {profile} {_format_summary(provenance_summary(derived))}")
    if getattr(args, "strict_provenance", False) and unasserted:
        scope = f"package profile {selected} ships" if selected else "the repository manifest has"
        print(f"FAILED: --strict-provenance and {scope} {unasserted} entries that assert no license",
              file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="SparkEngine asset integrity tool")
    commands = parser.add_subparsers(dest="command", required=True)

    generate = commands.add_parser("generate", help="Generate the fixed-root manifest")
    generate.add_argument("root", help="Caller-authorized asset root")
    generate.add_argument("-o", "--output", help="Must equal <root>/assets.integrity.json")
    generate.add_argument(
        "--provenance",
        help=f"Provenance policy (default: {DEFAULT_PROVENANCE_POLICY.relative_to(REPO_ROOT).as_posix()})")
    generate.set_defaults(handler=cmd_generate)

    verify = commands.add_parser("verify", help="Verify a manifest and explicit root")
    verify.add_argument("manifest", help="Manifest JSON path")
    verify.add_argument("--root", required=True, help="Caller-authorized asset root")
    verify.add_argument("--provenance", help="Also prove license/provenance against this policy")
    verify.add_argument(
        "--require-provenance", action="store_true",
        help="Reject a legacy v1 manifest that carries no license/provenance")
    verify.set_defaults(handler=cmd_verify)

    verify.add_argument(
        "--profile", choices=sorted(PACKAGE_PROFILES),
        help="Also enforce this package profile; stable-v1 rejects NOASSERTION entries (OD-09)")
    verify.add_argument(
        "--source-manifest",
        help="Reviewed repository manifest every packaged entry must match "
             f"(default for NOASSERTION-excluding profiles: {KNOWN_MANIFESTS[0][0]})")

    package_profile = commands.add_parser(
        "package-profile", help="Derive a package manifest and install exclusions for a profile")
    package_profile.add_argument("manifest", help="Reviewed source manifest (schema v2)")
    package_profile.add_argument("--profile", required=True, choices=sorted(PACKAGE_PROFILES))
    package_profile.add_argument("--output", required=True, help="Derived package manifest path")
    package_profile.add_argument(
        "--exclusions", required=True,
        help="Root-relative excluded files and directories (trailing '/'), one per line")
    package_profile.add_argument(
        "--repo-root",
        help="Repository whose module sources and profile definitions derive the asset closure "
             "(default: auto-detect)")
    package_profile.add_argument(
        "--inputs",
        help="Also write the closure's re-derivation inputs, one absolute path per line: every data file "
             "read plus each scanned source directory")
    package_profile.set_defaults(handler=cmd_package_profile)

    check_all = commands.add_parser("check-all", help="Verify all repository asset contracts")
    check_all.add_argument("--repo-root", help="Repository root (default: auto-detect)")
    check_all.add_argument(
        "--profile", choices=sorted(PACKAGE_PROFILES),
        help="Derive only this package profile; --strict-provenance then judges only the entries it ships")
    check_all.add_argument(
        "--strict-provenance", action="store_true",
        help="Also fail while any entry asserts NOASSERTION (release-promotion gate)")
    check_all.set_defaults(handler=cmd_check_all)

    args = parser.parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
