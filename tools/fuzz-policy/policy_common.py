#!/usr/bin/env python3
"""Shared fail-closed primitives for the SEC-120 fuzz policy tools."""

from __future__ import annotations

import json
import os
import stat
import sys
import time
import unicodedata
from datetime import date
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Any, Iterator


MAX_MANIFEST_BYTES = 1024 * 1024
MAX_JSON_DEPTH = 32
MAX_JSON_OBJECT_MEMBERS = 256
MAX_JSON_ARRAY_ITEMS = 20_000
MAX_JSON_STRING_CHARS = 64 * 1024
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
MAX_DIRECTORY_ENTRIES = 20_000
WINDOWS_RESERVED_NAMES = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}


class Deadline:
    """A single wall-clock budget shared by every phase of one policy run."""

    def __init__(self, seconds: int, label: str) -> None:
        self._deadline = time.monotonic() + seconds
        self._seconds = seconds
        self._label = label

    def check(self) -> None:
        # >= so that a zero-second budget is already spent on its first check
        # rather than depending on clock granularity.
        if time.monotonic() >= self._deadline:
            raise PolicyError(f"{self._label} exceeded {self._seconds} seconds")


class PolicyError(ValueError):
    """A policy input is malformed, unsafe, or outside its declared root."""


def require_exact_int(value: Any, field: str, *, minimum: int, maximum: int) -> int:
    if type(value) is not int:  # bool is intentionally rejected.
        raise PolicyError(f"{field} must be an integer")
    if not minimum <= value <= maximum:
        raise PolicyError(f"{field} must be between {minimum} and {maximum}")
    return value


def require_string(value: Any, field: str, *, maximum: int = 4096) -> str:
    if not isinstance(value, str) or not value.strip():
        raise PolicyError(f"{field} must be a non-empty string")
    if len(value) > maximum:
        raise PolicyError(f"{field} exceeds {maximum} characters")
    if "\x00" in value:
        raise PolicyError(f"{field} contains NUL")
    return value


def require_exact_keys(value: Any, field: str, required: set[str], optional: set[str] | None = None) -> dict:
    if not isinstance(value, dict):
        raise PolicyError(f"{field} must be an object")
    optional = optional or set()
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    if missing:
        raise PolicyError(f"{field} is missing keys: {', '.join(sorted(missing))}")
    if unknown:
        raise PolicyError(f"{field} has unknown keys: {', '.join(sorted(unknown))}")
    return value


def require_list(value: Any, field: str, *, maximum: int = 4096) -> list:
    if not isinstance(value, list):
        raise PolicyError(f"{field} must be an array")
    if len(value) > maximum:
        raise PolicyError(f"{field} exceeds {maximum} entries")
    return value


def normalized_relative_path(raw: Any, field: str) -> str:
    value = require_string(raw, field, maximum=1024).replace("\\", "/")
    windows = PureWindowsPath(value)
    posix = PurePosixPath(value)
    if windows.drive or windows.root or posix.is_absolute():
        raise PolicyError(f"{field} must be repository-relative: {raw!r}")
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise PolicyError(f"{field} contains an empty, dot, or parent component: {raw!r}")
    if any(":" in part for part in parts):
        raise PolicyError(f"{field} contains a drive/stream alias: {raw!r}")
    for part in parts:
        if part != part.rstrip(" ."):
            raise PolicyError(f"{field} contains a Windows-normalized alias: {raw!r}")
        if any(ord(char) < 32 or char in '<>"|?*' for char in part):
            raise PolicyError(f"{field} contains a Windows-invalid character: {raw!r}")
        if part.split(".", 1)[0].upper() in WINDOWS_RESERVED_NAMES:
            raise PolicyError(f"{field} contains a Windows device alias: {raw!r}")
    return "/".join(parts)


def _is_reparse(stat_result: os.stat_result) -> bool:
    return bool(getattr(stat_result, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT)


def _validate_identity(stat_result: os.stat_result, field: str, *, expect: str) -> None:
    if _is_reparse(stat_result):
        raise PolicyError(f"{field} is a reparse point")
    if expect == "file":
        if not stat.S_ISREG(stat_result.st_mode):
            raise PolicyError(f"{field} is not a regular file")
        if stat_result.st_nlink != 1:
            raise PolicyError(f"{field} must not be a hard link")
    elif expect == "dir" and not stat.S_ISDIR(stat_result.st_mode):
        raise PolicyError(f"{field} is not a directory")


def canonical_root(root: Path) -> Path:
    lexical = Path(os.path.abspath(root))
    try:
        root_identity = os.lstat(lexical)
    except OSError as exc:
        raise PolicyError(f"source_root is missing or unreadable: {exc}") from exc
    _validate_identity(root_identity, "source_root", expect="dir")
    resolved = lexical.resolve(strict=True)
    if os.path.normcase(str(lexical)) != os.path.normcase(str(resolved)):
        raise PolicyError("source_root must not use a symlink or reparse alias")
    return resolved


def _opened_final_path(descriptor: int) -> Path | None:
    """Return the path of the pinned handle on Windows/Linux when available."""
    if os.name == "nt":
        import ctypes
        import msvcrt
        from ctypes import wintypes

        get_final_path = ctypes.WinDLL("kernel32", use_last_error=True).GetFinalPathNameByHandleW
        get_final_path.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
        get_final_path.restype = wintypes.DWORD
        buffer = ctypes.create_unicode_buffer(32_768)
        length = get_final_path(msvcrt.get_osfhandle(descriptor), buffer, len(buffer), 0)
        if length == 0 or length >= len(buffer):
            raise PolicyError("cannot resolve the opened file handle")
        value = buffer.value
        if value.startswith("\\\\?\\UNC\\"):
            value = "\\\\" + value[8:]
        elif value.startswith("\\\\?\\"):
            value = value[4:]
        return Path(value)
    if sys.platform.startswith("linux"):
        try:
            value = os.readlink(f"/proc/self/fd/{descriptor}")
        except OSError as exc:
            raise PolicyError(f"cannot resolve the opened file descriptor: {exc}") from exc
        if value.endswith(" (deleted)"):
            raise PolicyError("opened file was unlinked during validation")
        return Path(value)
    if sys.platform == "darwin":
        import fcntl

        f_getpath = 50  # <sys/fcntl.h> F_GETPATH
        buffer = bytearray(1024)  # PATH_MAX
        try:
            fcntl.fcntl(descriptor, f_getpath, buffer)
        except OSError as exc:
            raise PolicyError(f"cannot resolve the opened file descriptor: {exc}") from exc
        return Path(bytes(buffer).split(b"\0", 1)[0].decode("utf-8", "surrogateescape"))
    # Returning None here would make the post-open confinement check a silent
    # no-op, so an unsupported platform fails closed instead.
    raise PolicyError(f"opened-handle confinement is unavailable on {sys.platform}")


def _require_opened_confinement(root: Path, descriptor: int, field: str, relative: str) -> None:
    opened_path = _opened_final_path(descriptor)
    root_text = os.path.normcase(str(root))
    opened_text = os.path.normcase(str(opened_path.resolve(strict=True)))
    try:
        if os.path.commonpath((root_text, opened_text)) != root_text:
            raise PolicyError(f"{field} opened outside source_root: {relative}")
    except ValueError as exc:
        raise PolicyError(f"{field} opened on a different volume: {relative}") from exc


def confined_path(
    root: Path, raw: Any, field: str, *, expect: str, root_is_canonical: bool = False
) -> tuple[str, Path, os.stat_result]:
    relative = normalized_relative_path(raw, field)
    # Re-canonicalizing per file costs several stat/resolve syscalls each time;
    # callers that already hold a canonical root say so.
    if not root_is_canonical:
        root = canonical_root(root)

    current = root
    for index, part in enumerate(relative.split("/")):
        current = current / part
        try:
            identity = os.lstat(current)
        except OSError as exc:
            raise PolicyError(f"{field} is missing or unreadable: {relative}: {exc}") from exc
        _validate_identity(identity, field, expect=expect if index == len(relative.split("/")) - 1 else "dir")

    resolved = current.resolve(strict=True)
    try:
        if os.path.commonpath((os.path.normcase(str(root)), os.path.normcase(str(resolved)))) != os.path.normcase(str(root)):
            raise PolicyError(f"{field} escapes source_root: {relative}")
    except ValueError as exc:
        raise PolicyError(f"{field} is on a different volume: {relative}") from exc
    return relative, current, identity


def read_confined_file(
    root: Path, raw: Any, field: str, *, max_bytes: int, root_is_canonical: bool = False
) -> bytes:
    if not root_is_canonical:
        root = canonical_root(root)
    relative, path, expected = confined_path(root, raw, field, expect="file", root_is_canonical=True)
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise PolicyError(f"{field} cannot be opened: {relative}: {exc}") from exc
    try:
        opened = os.fstat(descriptor)
        _validate_identity(opened, field, expect="file")
        _require_opened_confinement(root, descriptor, field, relative)
        if (opened.st_dev, opened.st_ino) != (expected.st_dev, expected.st_ino):
            raise PolicyError(f"{field} changed before it was opened: {relative}")
        if opened.st_size > max_bytes:
            raise PolicyError(f"{field} exceeds {max_bytes} bytes: {relative}")
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = os.read(descriptor, min(64 * 1024, max_bytes + 1 - total))
            if not chunk:
                break
            total += len(chunk)
            if total > max_bytes:
                raise PolicyError(f"{field} grew beyond {max_bytes} bytes: {relative}")
            chunks.append(chunk)
        final = os.fstat(descriptor)
        if (final.st_dev, final.st_ino, final.st_size) != (opened.st_dev, opened.st_ino, opened.st_size):
            raise PolicyError(f"{field} changed while it was read: {relative}")
        return b"".join(chunks)
    finally:
        os.close(descriptor)


def _check_json_lexical_bounds(text: str, field: str) -> None:
    depth = 0
    in_string = False
    escaped = False
    string_chars = 0
    for char in text:
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            else:
                string_chars += 1
                if string_chars > MAX_JSON_STRING_CHARS:
                    raise PolicyError(f"{field} contains an oversized string")
            continue
        if char == '"':
            in_string = True
            string_chars = 0
        elif char in "[{":
            depth += 1
            if depth > MAX_JSON_DEPTH:
                raise PolicyError(f"{field} exceeds JSON depth {MAX_JSON_DEPTH}")
        elif char in "]}":
            depth -= 1
            if depth < 0:
                raise PolicyError(f"{field} has unbalanced JSON delimiters")
    if in_string or depth != 0:
        raise PolicyError(f"{field} has unterminated JSON structure")


def load_json_document(root: Path, raw: str, field: str) -> Any:
    payload = read_confined_file(root, raw, field, max_bytes=MAX_MANIFEST_BYTES)
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PolicyError(f"{field} must be strict UTF-8") from exc
    if text.startswith("\ufeff"):
        raise PolicyError(f"{field} must not contain a UTF-8 BOM")
    _check_json_lexical_bounds(text, field)

    def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        if len(pairs) > MAX_JSON_OBJECT_MEMBERS:
            raise PolicyError(f"{field} object exceeds {MAX_JSON_OBJECT_MEMBERS} members")
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise PolicyError(f"{field} contains duplicate key: {key}")
            result[key] = value
        return result

    def reject_float(value: str) -> None:
        raise PolicyError(f"{field} contains non-integer number: {value}")

    def reject_constant(value: str) -> None:
        raise PolicyError(f"{field} contains non-finite number: {value}")

    try:
        document = json.loads(
            text,
            object_pairs_hook=object_pairs,
            parse_float=reject_float,
            parse_constant=reject_constant,
        )
    except PolicyError:
        raise
    except (json.JSONDecodeError, RecursionError, ValueError) as exc:
        raise PolicyError(f"{field} is malformed JSON: {exc}") from exc

    def check_collections(value: Any, path: str) -> None:
        if isinstance(value, list):
            if len(value) > MAX_JSON_ARRAY_ITEMS:
                raise PolicyError(f"{path} exceeds {MAX_JSON_ARRAY_ITEMS} array entries")
            for index, item in enumerate(value):
                check_collections(item, f"{path}[{index}]")
        elif isinstance(value, dict):
            for key, item in value.items():
                if not isinstance(key, str) or len(key) > 256:
                    raise PolicyError(f"{path} contains an invalid object key")
                check_collections(item, f"{path}.{key}")

    check_collections(document, field)
    return document


def bounded_scandir(directory: Path, field: str, *, max_entries: int = MAX_DIRECTORY_ENTRIES) -> list[os.DirEntry]:
    """Enumerate a directory with the entry cap enforced *before* the list is materialized."""
    entries: list[os.DirEntry] = []
    try:
        with os.scandir(directory) as iterator:
            for entry in iterator:
                if len(entries) >= max_entries:
                    raise PolicyError(f"{field} exceeds {max_entries} directory entries: {directory}")
                entries.append(entry)
    except PolicyError:
        raise
    except OSError as exc:
        raise PolicyError(f"{field} is unreadable: {directory}: {exc}") from exc
    entries.sort(key=lambda entry: entry.name)
    return entries


def require_token(value: Any, field: str, pattern, *, maximum: int = 128) -> str:
    """Require an exact, unicode-canonical token so case and homoglyph aliases cannot slip in."""
    text = require_string(value, field, maximum=maximum)
    if unicodedata.normalize("NFC", text) != text:
        raise PolicyError(f"{field} must be unicode-NFC normalized")
    if not text.isascii():
        raise PolicyError(f"{field} must be ASCII")
    if text != text.strip():
        raise PolicyError(f"{field} must not have surrounding whitespace")
    if not pattern.fullmatch(text):
        raise PolicyError(f"{field} does not match {pattern.pattern}")
    return text


def require_iso_date(value: Any, field: str) -> date:
    text = require_string(value, field, maximum=10)
    try:
        parsed = date.fromisoformat(text)
    except ValueError as exc:
        raise PolicyError(f"{field} must be YYYY-MM-DD") from exc
    if parsed.isoformat() != text:
        raise PolicyError(f"{field} must use canonical YYYY-MM-DD")
    return parsed


def casefold_duplicates(values: Iterator[str] | list[str] | tuple[str, ...], field: str) -> None:
    """Reject values that differ only by case, so aliases cannot double-register."""
    seen: dict[str, str] = {}
    for value in values:
        key = value.casefold()
        if key in seen:
            raise PolicyError(f"{field} contains case-aliased duplicates: {seen[key]!r} and {value!r}")
        seen[key] = value
