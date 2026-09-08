#!/usr/bin/env python3
"""Fail-closed filesystem access for PLT-200 certification evidence.

Every read in the certification path goes through this module.  The rules are
deliberately hostile, because an evidence bundle is attacker-influenced input:

  - Nothing is opened by a path that has been resolved through a link.  Each
    path component is lstat-ed and rejected if it is a symlink, junction, or
    any other reparse point.
  - Files are opened with O_NOFOLLOW where the platform has it, and the opened
    handle is re-stat-ed and compared against the pre-open lstat so a path
    swapped between the check and the open cannot be substituted (TOCTOU).
  - Sizes are taken from the validated handle and checked *before* any read,
    so an oversized file is never allocated.
  - Containment is canonical: the real path of the target must live beneath
    the real path of the bundle root.

Nothing here trusts a declared value.  Sizes and digests are always measured.
"""

from __future__ import annotations

import hashlib
import os
import stat
import sys
from pathlib import Path, PurePosixPath

IS_WINDOWS = sys.platform == "win32"

# Read granularity for hashing.  Small enough that a bogus huge file is
# refused on the size check long before this matters.
_CHUNK_BYTES = 64 * 1024

_FILE_ATTRIBUTE_REPARSE_POINT = 0x400

# Windows treats these as device names regardless of extension or directory.
_WINDOWS_RESERVED = frozenset(
    ["con", "prn", "aux", "nul"]
    + [f"com{i}" for i in range(1, 10)]
    + [f"lpt{i}" for i in range(1, 10)]
)

# Characters Windows forbids in a filename.  Rejected on every platform so a
# bundle validates identically on Linux CI and on a Windows host.
_ILLEGAL_CHARS = frozenset("<>:\"|?*\\")

# A declared artifact path longer than this is refused outright.
MAX_DECLARED_PATH_LENGTH = 512


class FileSecurityError(Exception):
    """A filesystem access that is unsafe or outside the evidence bundle."""


# -- Link / reparse detection ----------------------------------------------


def _is_link_like(st: os.stat_result) -> bool:
    """True if st describes a symlink, junction, or any other reparse point."""
    if stat.S_ISLNK(st.st_mode):
        return True
    attrs = getattr(st, "st_file_attributes", 0)
    return bool(attrs & _FILE_ATTRIBUTE_REPARSE_POINT)


def lstat_checked(path: Path, *, expect: str = "file") -> os.stat_result:
    """lstat a path, refusing links, reparse points, and wrong node types.

    `expect` is "file" or "dir".  Raises FileSecurityError on any violation.
    """
    try:
        st = path.lstat()
    except FileNotFoundError as exc:
        raise FileSecurityError(f"does not exist: {path}") from exc
    except OSError as exc:
        raise FileSecurityError(f"cannot stat {path}: {exc}") from exc

    if _is_link_like(st):
        raise FileSecurityError(
            f"refusing link/reparse point (symlink or junction): {path}"
        )
    if expect == "file" and not stat.S_ISREG(st.st_mode):
        raise FileSecurityError(f"not a regular file: {path}")
    if expect == "dir" and not stat.S_ISDIR(st.st_mode):
        raise FileSecurityError(f"not a directory: {path}")
    return st


def assert_path_chain_safe(root: Path, target: Path) -> None:
    """lstat every component from `root` down to `target`.

    realpath() alone is not enough: a junction pointing back *inside* the root
    would survive a containment check while still being a link, and a link is
    exactly what we are refusing.
    """
    lstat_checked(root, expect="dir")
    try:
        rel = target.relative_to(root)
    except ValueError as exc:
        raise FileSecurityError(f"{target} is not beneath {root}") from exc

    walked = root
    parts = list(rel.parts)
    for part in parts[:-1]:
        walked = walked / part
        lstat_checked(walked, expect="dir")
    if parts:
        lstat_checked(root / rel, expect="file")


# -- Containment -----------------------------------------------------------


def _reject_bad_component(component: str, declared: str) -> None:
    if not component:
        raise FileSecurityError(f"empty path component in {declared!r}")
    if component in (".", ".."):
        raise FileSecurityError(f"relative path component in {declared!r}")
    if any(c in _ILLEGAL_CHARS or ord(c) < 32 for c in component):
        raise FileSecurityError(
            f"illegal character in path component {component!r} of {declared!r}"
        )
    # Windows silently strips trailing dots and spaces, so "a.txt." and
    # "a.txt" name the same file while comparing unequal.
    if component != component.rstrip(" ."):
        raise FileSecurityError(
            f"trailing dot or space in path component {component!r} "
            f"of {declared!r}"
        )
    if component.split(".")[0].lower() in _WINDOWS_RESERVED:
        raise FileSecurityError(
            f"reserved Windows device name {component!r} in {declared!r}"
        )


def resolve_under(root: Path, declared: str) -> Path:
    """Resolve a declared relative artifact path beneath `root`.

    Rejects absolute paths, drive letters, UNC paths, backslashes, traversal,
    and Windows filename aliases.  The returned path is canonical and proven
    to live beneath the canonical root.
    """
    if not isinstance(declared, str) or not declared:
        raise FileSecurityError("artifact path must be a non-empty string")
    if len(declared) > MAX_DECLARED_PATH_LENGTH:
        raise FileSecurityError(f"artifact path too long ({len(declared)})")
    if "\x00" in declared:
        raise FileSecurityError("NUL byte in artifact path")
    if declared.startswith("/") or declared.startswith("\\"):
        raise FileSecurityError(f"absolute path not allowed: {declared!r}")
    if len(declared) >= 2 and declared[1] == ":":
        raise FileSecurityError(f"drive-letter path not allowed: {declared!r}")

    # Split the raw string, not PurePosixPath: it silently folds away "" and
    # "." components, so "a//b", "./a/b" and "a/./b" would all reach the same
    # file as three different strings and defeat duplicate detection.
    # Backslash is an illegal component character, which also catches UNC.
    for component in declared.split("/"):
        _reject_bad_component(component, declared)

    root_real = Path(os.path.realpath(root))
    candidate = root / PurePosixPath(declared)
    candidate_real = Path(os.path.realpath(candidate))

    try:
        common = os.path.commonpath([str(root_real), str(candidate_real)])
    except ValueError as exc:  # different drives
        raise FileSecurityError(
            f"artifact {declared!r} escapes the evidence bundle root"
        ) from exc
    if Path(common) != root_real:
        raise FileSecurityError(
            f"artifact {declared!r} escapes the evidence bundle root"
        )
    return candidate


def case_collision_key(declared: str) -> str:
    """The identity a case-insensitive filesystem would consider the same."""
    return declared.replace("\\", "/").casefold()


# -- Bounded reads ---------------------------------------------------------


def _open_no_follow(path: Path, pre: os.stat_result) -> int:
    """Open `path` without following links, verifying handle identity."""
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise FileSecurityError(f"cannot open {path}: {exc}") from exc

    try:
        post = os.fstat(fd)
        if not stat.S_ISREG(post.st_mode):
            raise FileSecurityError(f"opened handle is not a regular file: {path}")
        # The handle must be the very node we vetted.  st_ino is a real file
        # index on Windows for Python 3.8+, so this holds cross-platform.
        if (post.st_dev, post.st_ino) != (pre.st_dev, pre.st_ino):
            raise FileSecurityError(
                f"file changed identity between check and open: {path}"
            )
    except Exception:
        os.close(fd)
        raise
    return fd


def read_bounded(path: Path, *, max_bytes: int) -> bytes:
    """Read a whole file, refusing it on size *before* allocating anything."""
    pre = lstat_checked(path, expect="file")
    if pre.st_size > max_bytes:
        raise FileSecurityError(
            f"{path.name}: file is {pre.st_size} bytes (max {max_bytes})"
        )
    fd = _open_no_follow(path, pre)
    try:
        if os.fstat(fd).st_size > max_bytes:
            raise FileSecurityError(f"{path.name}: file grew past {max_bytes} bytes")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(fd, _CHUNK_BYTES)
            if not chunk:
                break
            total += len(chunk)
            if total > max_bytes:
                raise FileSecurityError(
                    f"{path.name}: file grew past {max_bytes} bytes during read"
                )
            chunks.append(chunk)
    finally:
        os.close(fd)
    return b"".join(chunks)


def measure_file(path: Path, *, max_bytes: int) -> tuple[str, int]:
    """Return (sha256_hex, size_bytes) measured from the validated handle.

    The digest and the size come from the same open handle, so a declared
    value can never stand in for a measured one.
    """
    pre = lstat_checked(path, expect="file")
    if pre.st_size > max_bytes:
        raise FileSecurityError(
            f"{path.name}: artifact is {pre.st_size} bytes (max {max_bytes})"
        )
    fd = _open_no_follow(path, pre)
    digest = hashlib.sha256()
    size = 0
    try:
        while True:
            chunk = os.read(fd, _CHUNK_BYTES)
            if not chunk:
                break
            size += len(chunk)
            if size > max_bytes:
                raise FileSecurityError(
                    f"{path.name}: artifact grew past {max_bytes} bytes"
                )
            digest.update(chunk)
    finally:
        os.close(fd)
    return digest.hexdigest(), size


# -- Bundle enumeration ----------------------------------------------------


def iter_bundle_files(
    root: Path,
    *,
    max_files: int,
    max_depth: int,
    max_total_bytes: int,
) -> list[Path]:
    """Enumerate every regular file beneath `root`, bounded and link-free.

    Raises rather than skipping: a bundle containing something we refuse to
    walk is a bundle we refuse to certify.
    """
    lstat_checked(root, expect="dir")
    found: list[Path] = []
    total = 0
    pending: list[tuple[Path, int]] = [(root, 0)]

    while pending:
        directory, depth = pending.pop()
        if depth > max_depth:
            raise FileSecurityError(
                f"evidence bundle nests deeper than {max_depth} levels "
                f"at {directory}"
            )
        try:
            entries = sorted(os.scandir(directory), key=lambda e: e.name)
        except OSError as exc:
            raise FileSecurityError(f"cannot list {directory}: {exc}") from exc

        for entry in entries:
            child = Path(entry.path)
            st = child.lstat()
            if _is_link_like(st):
                raise FileSecurityError(
                    f"refusing link/reparse point in evidence bundle: {child}"
                )
            if stat.S_ISDIR(st.st_mode):
                pending.append((child, depth + 1))
                continue
            if not stat.S_ISREG(st.st_mode):
                raise FileSecurityError(
                    f"refusing non-regular file in evidence bundle: {child}"
                )
            found.append(child)
            if len(found) > max_files:
                raise FileSecurityError(
                    f"evidence bundle holds more than {max_files} files"
                )
            total += st.st_size
            if total > max_total_bytes:
                raise FileSecurityError(
                    f"evidence bundle exceeds {max_total_bytes} bytes"
                )

    return sorted(found)
