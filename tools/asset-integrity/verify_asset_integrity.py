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


MANIFEST_SCHEMA_VERSION = 1
HASH_ALGORITHM = "sha256"
MANIFEST_FILENAME = "assets.integrity.json"
REPO_ROOT = Path(__file__).resolve().parents[2]

MAX_MANIFEST_BYTES = 64 * 1024 * 1024
MAX_ENTRY_COUNT = 100_000
MAX_DIRECTORY_ENTRY_COUNT = 200_000
MAX_PATH_BYTES = 1024
MAX_FILE_BYTES = 4 * 1024 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024 * 1024
MAX_DIRECTORY_DEPTH = 128
HASH_CHUNK_BYTES = 1024 * 1024

KNOWN_MANIFESTS = (("Assets/assets.integrity.json", "Assets"),)
ROOT_IGNORES = frozenset({MANIFEST_FILENAME})
TEMPLATE_ROOT_METADATA = frozenset({"README.md", "manifest.json"})
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


def generate_manifest(root: Path) -> tuple[dict[str, Any], list[IntegrityError]]:
    entries, errors = scan_directory(root)
    manifest = {
        "version": MANIFEST_SCHEMA_VERSION,
        "algorithm": HASH_ALGORITHM,
        "root": _absolute_lexical(root).name,
        "fileCount": len(entries),
        "entries": entries,
    }
    return manifest, errors


def manifest_bytes(manifest: dict[str, Any]) -> bytes:
    return (json.dumps(manifest, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def write_manifest(manifest: dict[str, Any], output: Path) -> None:
    """Write deterministic bytes. Callers must already have a clean snapshot."""
    output.write_bytes(manifest_bytes(manifest))


def load_manifest(path: Path) -> dict[str, Any]:
    data = _read_bounded_json(path)
    if not isinstance(data, dict):
        raise ManifestFormatError("manifest root must be an object")
    if data.get("version") != MANIFEST_SCHEMA_VERSION:
        raise ManifestFormatError(f"unsupported manifest version: {data.get('version')!r}")
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
        if set(entry) != {"path", "sha256", "size"}:
            raise ManifestFormatError(f"entry {index} must contain exactly path, sha256, and size")
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


def verify_manifest(manifest_path: Path, root: Path) -> list[IntegrityError]:
    """Verify a manifest against a caller-authorized root in one disk scan."""
    try:
        manifest = load_manifest(_absolute_lexical(manifest_path))
    except (ManifestFormatError, OSError) as exc:
        return [IntegrityError(str(manifest_path), "manifest-load", str(exc))]

    absolute_root = _absolute_lexical(root)
    if manifest["root"] != absolute_root.name:
        return [IntegrityError(
            str(manifest_path), "root-metadata",
            f"manifest root {manifest['root']!r} does not exactly match caller root {absolute_root.name!r}")]

    disk_entries, scan_errors = scan_directory(absolute_root)
    if scan_errors:
        return scan_errors

    declared = {entry["path"]: entry for entry in manifest["entries"]}
    disk = {entry["path"]: entry for entry in disk_entries}
    errors: list[IntegrityError] = []
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
        if not stat.S_ISDIR(info.st_mode):
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
    for manifest_relative, root_relative in KNOWN_MANIFESTS:
        errors.extend(verify_manifest(repo_root / manifest_relative, repo_root / root_relative))
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
    manifest, errors = generate_manifest(root)
    if errors:
        print(f"Refusing generation after {len(errors)} scan error(s):", file=sys.stderr)
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
    errors = verify_manifest(Path(args.manifest), Path(args.root))
    if errors:
        print(f"FAILED: {len(errors)} error(s)", file=sys.stderr)
        _print_errors(errors)
        return 1
    manifest = load_manifest(_absolute_lexical(Path(args.manifest)))
    print(f"OK: {manifest['fileCount']} entries verified")
    return 0


def cmd_check_all(args: argparse.Namespace) -> int:
    repo_root = Path(args.repo_root) if args.repo_root else REPO_ROOT
    errors = verify_repository(repo_root)
    if errors:
        print(f"FAILED: {len(errors)} error(s)", file=sys.stderr)
        _print_errors(errors)
        return 1
    print("OK: first-party and template asset integrity checks passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="SparkEngine asset integrity tool")
    commands = parser.add_subparsers(dest="command", required=True)

    generate = commands.add_parser("generate", help="Generate the fixed-root manifest")
    generate.add_argument("root", help="Caller-authorized asset root")
    generate.add_argument("-o", "--output", help="Must equal <root>/assets.integrity.json")
    generate.set_defaults(handler=cmd_generate)

    verify = commands.add_parser("verify", help="Verify a manifest and explicit root")
    verify.add_argument("manifest", help="Manifest JSON path")
    verify.add_argument("--root", required=True, help="Caller-authorized asset root")
    verify.set_defaults(handler=cmd_verify)

    check_all = commands.add_parser("check-all", help="Verify all repository asset contracts")
    check_all.add_argument("--repo-root", help="Repository root (default: auto-detect)")
    check_all.set_defaults(handler=cmd_check_all)

    args = parser.parse_args()
    return int(args.handler(args))


if __name__ == "__main__":
    raise SystemExit(main())
