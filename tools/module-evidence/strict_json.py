#!/usr/bin/env python3
"""Strict, duplicate-aware, bounded JSON loading for module evidence documents.

`json.load` is unsuitable for validating a security-relevant manifest:

  - Duplicate object properties are silently accepted, last one winning.  A
    hostile document can therefore shadow a rejected value with an accepted
    one and the validator never sees the rejected value at all.
  - `NaN`, `Infinity` and `-Infinity` are accepted by default even though they
    are not valid JSON and cannot round-trip through other consumers.
  - There is no bound on document size, nesting depth or element count, so a
    small file can expand into an arbitrarily expensive parse.

This module provides a loader that rejects all of the above, at every object
and nested level, and reports the JSON pointer of the offending node.
"""

from __future__ import annotations

import json
import errno
import os
import re
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# Document limits.  These are deliberately far above any legitimate manifest
# (11 modules, 1 profile) and far below anything that could exhaust memory.
MAX_DOCUMENT_BYTES = 512 * 1024
MAX_DEPTH = 12
MAX_TOTAL_NODES = 20_000
MAX_STRING_LENGTH = 512
MAX_CONTAINER_ITEMS = 512


@dataclass(frozen=True)
class Limits:
    """Resource bounds applied to one document."""

    document_bytes: int = MAX_DOCUMENT_BYTES
    depth: int = MAX_DEPTH
    total_nodes: int = MAX_TOTAL_NODES
    string_length: int = MAX_STRING_LENGTH
    container_items: int = MAX_CONTAINER_ITEMS


DEFAULT_LIMITS = Limits()

# The readiness contract under docs/ is repository-controlled reference data
# rather than the artifact under adversarial validation, and it legitimately
# carries long prose fields.  Duplicate-key and non-finite rejection still
# apply — only the size bounds are relaxed.
CONTRACT_LIMITS = Limits(
    document_bytes=4 * 1024 * 1024,
    depth=24,
    total_nodes=200_000,
    string_length=8192,
    container_items=4096,
)

# CMake File API target documents and the normalized module-target evidence
# written from them can legitimately list every compilation source in a large
# target.  Keep that representation bounded against hostile artifacts while
# allowing the current engine target (526 sources) to be recorded faithfully.
MODULE_TARGET_LIMITS = Limits(container_items=2048)


class StrictJSONError(ValueError):
    """A document that must be rejected before any semantic validation runs."""


class NoFollowEvidenceMissing(StrictJSONError):
    """The exact no-follow evidence leaf does not exist.

    This is deliberately the only no-follow reader condition that a caller may
    map to a declared producer gap.  A caller must not treat a malformed,
    unreadable, reparse-bearing, or otherwise untrustworthy *present* document
    as though its producer simply never ran.
    """


class NoFollowAuthorityError(StrictJSONError):
    """No safe held authority could be established or retained for a path."""


def lexical_absolute_no_follow_path(
    path: Path | str, *, label: str = "path", allow_current_directory: bool = False,
) -> Path:
    """Make a lexical absolute path without erasing a raw reparse-bearing segment.

    ``abspath`` is deliberately used only after rejecting ``.`` and ``..`` in
    the caller's spelling.  Collapsing either component first could remove a
    directory that a platform would otherwise traverse as a reparse point.
    This is lexical preparation only: callers that need filesystem authority
    must still use the held no-follow open routines below.
    """
    raw = os.fspath(path)
    if not isinstance(raw, str) or not raw or "\0" in raw:
        raise NoFollowAuthorityError(
            f"{label} must be a non-empty text path without NUL"
        )
    if allow_current_directory and raw in {".", "./", ".\\"}:
        return Path(os.path.abspath(raw))
    _drive, tail = os.path.splitdrive(raw)
    separator = r"[\\/]+" if os.name == "nt" else r"/+"
    if any(component in {".", ".."} for component in re.split(separator, tail) if component):
        raise NoFollowAuthorityError(
            f"{label} contains a dot or traversal segment; no-follow paths must "
            "preserve every raw component"
        )
    return Path(os.path.abspath(raw))


def _validated_relative_components(relative: str) -> tuple[str, ...]:
    """Return one literal slash-delimited path below a held directory handle."""
    if not isinstance(relative, str) or not relative or "\0" in relative:
        raise NoFollowAuthorityError(
            "rooted relative path must be non-empty text without NUL"
        )
    drive, _tail = os.path.splitdrive(relative)
    if drive or relative.startswith(("/", "\\")):
        raise NoFollowAuthorityError(
            f"rooted relative path must not be absolute: {relative!r}"
        )
    if "\\" in relative:
        raise NoFollowAuthorityError(
            f"rooted relative path must use forward slashes only: {relative!r}"
        )
    components = tuple(relative.split("/"))
    for component in components:
        if component in {"", ".", ".."} or ":" in component or \
                "*" in component or "?" in component:
            raise NoFollowAuthorityError(
                f"rooted relative path has an unsafe component: {relative!r}"
            )
    return components


def _classify_no_follow_leaf_open_error(path: Path | str, exc: OSError) -> StrictJSONError:
    """Classify an error from opening only the fixed final evidence leaf.

    ``NtCreateFile`` reports missing names as NTSTATUS rather than a Win32
    ``errno``.  Keep that mapping here alongside POSIX ``ENOENT`` so lifecycle
    callers get a structured result instead of parsing messages.  Callers must
    never use this for root/ancestor, held-file, or read failures: those are
    authority failures even when their platform error code resembles absence.
    """
    status = getattr(exc, "status", None)
    if isinstance(status, int) and (status & 0xFFFFFFFF) in {
        0xC0000034,  # STATUS_OBJECT_NAME_NOT_FOUND
        0xC000003A,  # STATUS_OBJECT_PATH_NOT_FOUND
    }:
        return NoFollowEvidenceMissing(
            f"lifecycle evidence leaf is absent at {path}: {exc}"
        )
    error_numbers = {getattr(exc, "errno", None), getattr(exc, "winerror", None)}
    # ENOTDIR is deliberately not absence: an attacker can replace an expected
    # ancestor directory with a regular file.  Only a name/path that truly does
    # not exist may be softened by the declared-gap ledger.
    if error_numbers & {errno.ENOENT, 2, 3}:
        return NoFollowEvidenceMissing(
            f"lifecycle evidence leaf is absent at {path}: {exc}"
        )
    return NoFollowAuthorityError(
        f"unsafe no-follow lifecycle evidence read for {path}: {exc}"
    )


if os.name == "nt":
    try:
        import ctypes
        from ctypes import wintypes

        _GENERIC_READ = 0x80000000
        _FILE_LIST_DIRECTORY = 0x00000001
        _FILE_READ_ATTRIBUTES = 0x00000080
        _SYNCHRONIZE = 0x00100000
        _FILE_SHARE_READ = 0x00000001
        _OPEN_EXISTING = 3
        _FILE_ATTRIBUTE_NORMAL = 0x00000080
        _FILE_ATTRIBUTE_DIRECTORY = 0x00000010
        _FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
        _FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
        _FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
        _FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
        _FILE_OPEN = 1
        _FILE_DIRECTORY_FILE = 0x00000001
        _FILE_NON_DIRECTORY_FILE = 0x00000040
        _FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020
        _OBJ_CASE_INSENSITIVE = 0x00000040
        _HANDLE_FLAG_INHERIT = 0x00000001

        class _BY_HANDLE_FILE_INFORMATION(ctypes.Structure):
            _fields_ = [
                ("dwFileAttributes", wintypes.DWORD),
                ("ftCreationTime", wintypes.FILETIME),
                ("ftLastAccessTime", wintypes.FILETIME),
                ("ftLastWriteTime", wintypes.FILETIME),
                ("dwVolumeSerialNumber", wintypes.DWORD),
                ("nFileSizeHigh", wintypes.DWORD),
                ("nFileSizeLow", wintypes.DWORD),
                ("nNumberOfLinks", wintypes.DWORD),
                ("nFileIndexHigh", wintypes.DWORD),
                ("nFileIndexLow", wintypes.DWORD),
            ]

        class _UNICODE_STRING(ctypes.Structure):
            _fields_ = [
                ("Length", ctypes.c_ushort),
                ("MaximumLength", ctypes.c_ushort),
                ("Buffer", wintypes.LPWSTR),
            ]

        class _OBJECT_ATTRIBUTES(ctypes.Structure):
            _fields_ = [
                ("Length", wintypes.ULONG),
                ("RootDirectory", wintypes.HANDLE),
                ("ObjectName", ctypes.POINTER(_UNICODE_STRING)),
                ("Attributes", wintypes.ULONG),
                ("SecurityDescriptor", wintypes.LPVOID),
                ("SecurityQualityOfService", wintypes.LPVOID),
            ]

        class _IO_STATUS_BLOCK(ctypes.Structure):
            _fields_ = [
                ("Status", wintypes.LONG),
                ("Information", ctypes.c_size_t),
            ]

        _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        _ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
        _CreateFileW = _kernel32.CreateFileW
        _CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
        ]
        _CreateFileW.restype = wintypes.HANDLE
        _CloseHandle = _kernel32.CloseHandle
        _CloseHandle.argtypes = [wintypes.HANDLE]
        _CloseHandle.restype = wintypes.BOOL
        _SetHandleInformation = _kernel32.SetHandleInformation
        _SetHandleInformation.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD]
        _SetHandleInformation.restype = wintypes.BOOL
        _GetFileInformationByHandle = _kernel32.GetFileInformationByHandle
        _GetFileInformationByHandle.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(_BY_HANDLE_FILE_INFORMATION),
        ]
        _GetFileInformationByHandle.restype = wintypes.BOOL
        _ReadFile = _kernel32.ReadFile
        _ReadFile.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        ]
        _ReadFile.restype = wintypes.BOOL
        _NtCreateFile = _ntdll.NtCreateFile
        _NtCreateFile.argtypes = [
            ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD,
            ctypes.POINTER(_OBJECT_ATTRIBUTES), ctypes.POINTER(_IO_STATUS_BLOCK),
            ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD, wintypes.DWORD,
            wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.ULONG,
        ]
        _NtCreateFile.restype = wintypes.LONG
        _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
        _WINDOWS_NO_FOLLOW_READER_AVAILABLE = True

        class _WindowsNtCreateError(OSError):
            """Preserve an NTSTATUS so missing names can be classified safely."""

            def __init__(self, status: int) -> None:
                self.status = status & 0xFFFFFFFF
                super().__init__(f"NtCreateFile failed: 0x{self.status:08X}")
    except (AttributeError, ImportError, OSError):
        _WINDOWS_NO_FOLLOW_READER_AVAILABLE = False
else:
    _WINDOWS_NO_FOLLOW_READER_AVAILABLE = False


def _posix_snapshot(fd: int) -> tuple[int, int, int, int, int, int, int]:
    """Return the mutation-relevant identity for one already-open POSIX file."""
    info = os.fstat(fd)
    return (
        int(info.st_dev), int(info.st_ino), int(info.st_mode), int(info.st_size),
        int(getattr(info, "st_mtime_ns", 0)), int(getattr(info, "st_ctime_ns", 0)),
        int(info.st_nlink),
    )


def _read_open_descriptor(fd: int, max_bytes: int) -> bytes:
    """Read a bounded file through its already-authoritative descriptor."""
    chunks: list[bytes] = []
    total = 0
    while True:
        request = min(64 * 1024, max_bytes + 1 - total)
        if request <= 0:
            raise StrictJSONError(f"document exceeds the {max_bytes} byte limit")
        chunk = os.read(fd, request)
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)
        total += len(chunk)
        if total > max_bytes:
            raise StrictJSONError(f"document exceeds the {max_bytes} byte limit")


def _read_file_no_follow_posix(path: Path | str, max_bytes: int) -> bytes:
    """Open every POSIX path component below held parent descriptors."""
    no_follow = getattr(os, "O_NOFOLLOW", 0)
    directory = getattr(os, "O_DIRECTORY", 0)
    if not no_follow or not directory or os.open not in os.supports_dir_fd:
        raise NoFollowAuthorityError(
            "secure no-follow POSIX evidence reader is unavailable"
        )
    absolute = os.fspath(lexical_absolute_no_follow_path(
        path, label="lifecycle evidence path",
    ))
    components = [component for component in absolute.split(os.path.sep) if component]
    if not components:
        raise StrictJSONError(f"unsafe lifecycle evidence path is not a file: {path}")

    close_fds: list[int] = []
    data: bytes | None = None
    failure: StrictJSONError | None = None
    operation = "root or ancestor open"
    try:
        root_flags = os.O_RDONLY | directory | no_follow | getattr(os, "O_CLOEXEC", 0)
        current_fd = os.open(os.path.sep, root_flags)
        close_fds.append(current_fd)
        if not stat.S_ISDIR(os.fstat(current_fd).st_mode):
            raise StrictJSONError("unsafe lifecycle evidence root is not a directory")
        for component in components[:-1]:
            current_fd = os.open(
                component, root_flags, dir_fd=current_fd,
            )
            close_fds.append(current_fd)
            if not stat.S_ISDIR(os.fstat(current_fd).st_mode):
                raise StrictJSONError(
                    f"unsafe lifecycle evidence ancestor {component!r} is not a directory"
                )
        leaf_flags = os.O_RDONLY | no_follow | getattr(os, "O_CLOEXEC", 0)
        leaf_flags |= getattr(os, "O_NONBLOCK", 0)
        operation = "final lifecycle evidence leaf open"
        leaf_fd = os.open(components[-1], leaf_flags, dir_fd=current_fd)
        close_fds.append(leaf_fd)
        operation = "held lifecycle evidence leaf inspection or read"
        before = _posix_snapshot(leaf_fd)
        if not stat.S_ISREG(before[2]):
            raise StrictJSONError("unsafe lifecycle evidence leaf is not a regular file")
        if before[6] != 1:
            raise StrictJSONError("unsafe lifecycle evidence leaf has multiple hard links")
        if before[3] > max_bytes:
            raise StrictJSONError(
                f"{path}: document is {before[3]} bytes, limit is {max_bytes}"
            )
        data = _read_open_descriptor(leaf_fd, max_bytes)
        after = _posix_snapshot(leaf_fd)
        if before != after or after[3] != len(data):
            raise StrictJSONError("lifecycle evidence changed while its held file was read")
    except StrictJSONError as exc:
        failure = exc
    except OSError as exc:
        if operation == "final lifecycle evidence leaf open":
            failure = _classify_no_follow_leaf_open_error(path, exc)
        else:
            failure = NoFollowAuthorityError(
                f"unsafe no-follow lifecycle evidence {operation} for {path}: {exc}"
            )
    finally:
        close_errors: list[str] = []
        for fd in reversed(close_fds):
            try:
                os.close(fd)
            except OSError as exc:
                close_errors.append(str(exc))
    if failure is not None:
        if close_errors:
            raise NoFollowAuthorityError(
                f"{failure}; cannot close held evidence descriptors: "
                f"{'; '.join(close_errors)}"
            )
        raise failure
    if close_errors:
        raise NoFollowAuthorityError(
            "cannot close held lifecycle evidence descriptors: "
            f"{'; '.join(close_errors)}"
        )
    assert data is not None
    return data


if os.name == "nt":
    def _windows_file_snapshot(handle: int) -> tuple[int, int, int, int, int, int]:
        info = _BY_HANDLE_FILE_INFORMATION()  # type: ignore[name-defined]
        if not _GetFileInformationByHandle(handle, ctypes.byref(info)):  # type: ignore[name-defined]
            raise OSError(f"GetFileInformationByHandle failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        file_index = (int(info.nFileIndexHigh) << 32) | int(info.nFileIndexLow)
        size = (int(info.nFileSizeHigh) << 32) | int(info.nFileSizeLow)
        last_write = (int(info.ftLastWriteTime.dwHighDateTime) << 32) | int(info.ftLastWriteTime.dwLowDateTime)
        return (
            int(info.dwVolumeSerialNumber), file_index, size, last_write,
            int(info.dwFileAttributes), int(info.nNumberOfLinks),
        )


    def _windows_set_noninheritable(handle: int) -> None:
        if not _SetHandleInformation(handle, _HANDLE_FLAG_INHERIT, 0):  # type: ignore[name-defined]
            raise OSError(f"SetHandleInformation failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]


    def _windows_open_relative_no_follow(
        parent_handle: int, leaf_name: str, desired_access: int, file_attributes: int,
        create_options: int,
    ) -> int:
        name_bytes = leaf_name.encode("utf-16-le")
        if not name_bytes or len(name_bytes) > 0xFFFF:
            raise StrictJSONError("unsafe lifecycle evidence path component")
        buffer = ctypes.create_unicode_buffer(leaf_name)  # type: ignore[name-defined]
        object_name = _UNICODE_STRING(  # type: ignore[name-defined]
            len(name_bytes), len(name_bytes),
            ctypes.cast(buffer, wintypes.LPWSTR),  # type: ignore[name-defined]
        )
        attributes = _OBJECT_ATTRIBUTES(  # type: ignore[name-defined]
            ctypes.sizeof(_OBJECT_ATTRIBUTES), parent_handle, ctypes.pointer(object_name),  # type: ignore[name-defined]
            _OBJ_CASE_INSENSITIVE, None, None,
        )
        io_status = _IO_STATUS_BLOCK()  # type: ignore[name-defined]
        handle = wintypes.HANDLE()  # type: ignore[name-defined]
        status = int(_NtCreateFile(  # type: ignore[name-defined]
            ctypes.byref(handle), desired_access | _SYNCHRONIZE,
            ctypes.byref(attributes), ctypes.byref(io_status), None,
            file_attributes, _FILE_SHARE_READ, _FILE_OPEN,
            create_options | _FILE_SYNCHRONOUS_IO_NONALERT | _FILE_FLAG_OPEN_REPARSE_POINT,
            None, 0,
        ))
        if status < 0:
            raise _WindowsNtCreateError(status)  # type: ignore[name-defined]
        raw_handle = handle.value
        if raw_handle in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
            raise OSError("NtCreateFile returned an invalid lifecycle evidence handle")
        try:
            _windows_set_noninheritable(raw_handle)
        except BaseException:
            _CloseHandle(raw_handle)  # type: ignore[name-defined]
            raise
        return raw_handle


    def _read_windows_handle(handle: int, max_bytes: int) -> bytes:
        chunks: list[bytes] = []
        total = 0
        while True:
            request = min(64 * 1024, max_bytes + 1 - total)
            if request <= 0:
                raise StrictJSONError(f"document exceeds the {max_bytes} byte limit")
            buffer = ctypes.create_string_buffer(request)  # type: ignore[name-defined]
            read = wintypes.DWORD()  # type: ignore[name-defined]
            if not _ReadFile(  # type: ignore[name-defined]
                handle, buffer, request, ctypes.byref(read), None,  # type: ignore[name-defined]
            ):
                raise OSError(f"ReadFile failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
            count = int(read.value)
            if count == 0:
                return b"".join(chunks)
            chunks.append(buffer.raw[:count])
            total += count
            if total > max_bytes:
                raise StrictJSONError(f"document exceeds the {max_bytes} byte limit")


    def _read_file_no_follow_windows(path: Path | str, max_bytes: int) -> bytes:
        """Open a Windows file through a non-reparse RootDirectory HANDLE chain."""
        if not _WINDOWS_NO_FOLLOW_READER_AVAILABLE:
            raise NoFollowAuthorityError(
                "secure no-follow Windows evidence reader is unavailable"
            )
        absolute = os.fspath(lexical_absolute_no_follow_path(
            path, label="lifecycle evidence path",
        ))
        drive, tail = os.path.splitdrive(absolute)
        if not drive or drive.startswith("\\\\") or not tail.startswith("\\"):
            raise StrictJSONError("lifecycle evidence path must be an absolute drive-qualified path")
        components = [component for component in tail.split("\\") if component]
        if not components or any(component in {".", ".."} for component in components):
            raise StrictJSONError("unsafe lifecycle evidence path component")

        handles: list[int] = []
        data: bytes | None = None
        failure: StrictJSONError | None = None
        operation = "root or ancestor open"
        try:
            root_handle = _CreateFileW(  # type: ignore[name-defined]
                drive + "\\", _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES | _SYNCHRONIZE,
                _FILE_SHARE_READ, None, _OPEN_EXISTING,
                _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT, None,
            )
            if root_handle in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
                error = ctypes.get_last_error()  # type: ignore[name-defined]
                raise OSError(error, f"CreateFileW could not open evidence volume: {error}")
            root_handle = int(root_handle)
            handles.append(root_handle)
            _windows_set_noninheritable(root_handle)
            root_snapshot = _windows_file_snapshot(root_handle)
            if not root_snapshot[4] & _FILE_ATTRIBUTE_DIRECTORY or \
                    root_snapshot[4] & _FILE_ATTRIBUTE_REPARSE_POINT:
                raise StrictJSONError("unsafe lifecycle evidence root is not a real directory")
            current_handle = root_handle
            for component in components[:-1]:
                current_handle = _windows_open_relative_no_follow(
                    current_handle, component,
                    _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES,
                    _FILE_ATTRIBUTE_DIRECTORY,
                    _FILE_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
                )
                handles.append(current_handle)
                metadata = _windows_file_snapshot(current_handle)
                if not metadata[4] & _FILE_ATTRIBUTE_DIRECTORY or \
                        metadata[4] & _FILE_ATTRIBUTE_REPARSE_POINT:
                    raise StrictJSONError(
                        f"unsafe lifecycle evidence ancestor {component!r} is a reparse point or non-directory"
                    )
            operation = "final lifecycle evidence leaf open"
            leaf_handle = _windows_open_relative_no_follow(
                current_handle, components[-1],
                _GENERIC_READ | _FILE_READ_ATTRIBUTES,
                _FILE_ATTRIBUTE_NORMAL,
                _FILE_NON_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
            )
            handles.append(leaf_handle)
            operation = "held lifecycle evidence leaf inspection or read"
            before = _windows_file_snapshot(leaf_handle)
            if before[4] & (_FILE_ATTRIBUTE_REPARSE_POINT | _FILE_ATTRIBUTE_DIRECTORY):
                raise StrictJSONError("unsafe lifecycle evidence leaf is a reparse point or directory")
            if before[5] != 1:
                raise StrictJSONError("unsafe lifecycle evidence leaf has multiple hard links")
            if before[2] > max_bytes:
                raise StrictJSONError(
                    f"{path}: document is {before[2]} bytes, limit is {max_bytes}"
                )
            data = _read_windows_handle(leaf_handle, max_bytes)
            after = _windows_file_snapshot(leaf_handle)
            if before != after or after[2] != len(data):
                raise StrictJSONError("lifecycle evidence changed while its held file was read")
        except StrictJSONError as exc:
            failure = exc
        except OSError as exc:
            if operation == "final lifecycle evidence leaf open":
                failure = _classify_no_follow_leaf_open_error(path, exc)
            else:
                failure = NoFollowAuthorityError(
                    f"unsafe no-follow lifecycle evidence {operation} for {path}: {exc}"
                )
        finally:
            close_errors: list[str] = []
            for handle in reversed(handles):
                if not _CloseHandle(handle):  # type: ignore[name-defined]
                    close_errors.append(str(ctypes.get_last_error()))  # type: ignore[name-defined]
        if failure is not None:
            if close_errors:
                raise NoFollowAuthorityError(
                    f"{failure}; cannot close held evidence handles: "
                    f"{'; '.join(close_errors)}"
                )
            raise failure
        if close_errors:
            raise NoFollowAuthorityError(
                "cannot close held lifecycle evidence handles: "
                f"{'; '.join(close_errors)}"
            )
        assert data is not None
        return data


    def _windows_directory_snapshot(handle: int, label: str) -> tuple[int, int, int]:
        """Return stable identity/attributes for one held non-reparse directory."""
        snapshot = _windows_file_snapshot(handle)
        attributes = snapshot[4]
        if not attributes & _FILE_ATTRIBUTE_DIRECTORY or \
                attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
            raise NoFollowAuthorityError(
                f"unsafe held {label} directory is a reparse point or non-directory"
            )
        return snapshot[0], snapshot[1], attributes


    def _close_windows_directory_handles(lease: "NoFollowDirectoryLease") -> None:
        remaining: list[tuple[int, tuple[int, int, int]]] = []
        errors: list[str] = []
        for handle, snapshot in reversed(list(zip(lease._handles, lease._snapshots))):
            if not _CloseHandle(handle):  # type: ignore[name-defined]
                remaining.append((handle, snapshot))
                errors.append(str(ctypes.get_last_error()))  # type: ignore[name-defined]
        remaining.reverse()
        lease._handles = [handle for handle, _snapshot in remaining]
        lease._snapshots = [snapshot for _handle, snapshot in remaining]
        if errors:
            raise NoFollowAuthorityError(
                "cannot close held no-follow directory handles: "
                f"{'; '.join(errors)}"
            )
        lease._closed = True


    def _open_no_follow_directory_lease_windows(
        absolute: Path, label: str,
    ) -> "NoFollowDirectoryLease":
        """Hold every drive-rooted ancestor without following a reparse point."""
        if not _WINDOWS_NO_FOLLOW_READER_AVAILABLE:
            raise NoFollowAuthorityError(
                "secure no-follow Windows directory authority is unavailable"
            )
        raw = os.fspath(absolute)
        drive, tail = os.path.splitdrive(raw)
        if not drive or drive.startswith("\\\\") or not tail.startswith("\\"):
            raise NoFollowAuthorityError(
                f"{label} must be an absolute drive-qualified path"
            )
        components = [component for component in tail.split("\\") if component]
        if any(component in {".", ".."} for component in components):
            raise NoFollowAuthorityError(
                f"{label} contains an unsafe directory component"
            )

        handles: list[int] = []
        snapshots: list[tuple[int, int, int]] = []
        try:
            root_handle = _CreateFileW(  # type: ignore[name-defined]
                drive + "\\", _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES | _SYNCHRONIZE,
                _FILE_SHARE_READ, None, _OPEN_EXISTING,
                _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT, None,
            )
            if root_handle in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
                error = ctypes.get_last_error()  # type: ignore[name-defined]
                raise OSError(error, f"CreateFileW could not open {label} volume: {error}")
            root_handle = int(root_handle)
            handles.append(root_handle)
            _windows_set_noninheritable(root_handle)
            snapshots.append(_windows_directory_snapshot(root_handle, label))
            current_handle = root_handle
            for component in components:
                current_handle = _windows_open_relative_no_follow(
                    current_handle, component,
                    _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES,
                    _FILE_ATTRIBUTE_DIRECTORY,
                    _FILE_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
                )
                handles.append(current_handle)
                snapshots.append(_windows_directory_snapshot(current_handle, label))
        except BaseException as exc:
            close_errors: list[str] = []
            for handle in reversed(handles):
                if not _CloseHandle(handle):  # type: ignore[name-defined]
                    close_errors.append(str(ctypes.get_last_error()))  # type: ignore[name-defined]
            if isinstance(exc, NoFollowAuthorityError):
                raise
            if isinstance(exc, OSError):
                detail = f"cannot acquire held no-follow {label} authority at {absolute}: {exc}"
            else:
                detail = f"cannot acquire held no-follow {label} authority at {absolute}: {exc}"
            if close_errors:
                detail += f"; cannot close partial directory handles: {'; '.join(close_errors)}"
            raise NoFollowAuthorityError(detail) from exc
        return NoFollowDirectoryLease(absolute, label, handles, snapshots)


    def _verify_windows_directory_lease(lease: "NoFollowDirectoryLease") -> None:
        if not lease._handles or len(lease._handles) != len(lease._snapshots):
            raise NoFollowAuthorityError(
                f"held {lease.label} directory authority is incomplete"
            )
        for handle, expected in zip(lease._handles, lease._snapshots):
            try:
                actual = _windows_directory_snapshot(handle, lease.label)
            except OSError as exc:
                raise NoFollowAuthorityError(
                    f"cannot inspect held {lease.label} authority: {exc}"
                ) from exc
            if actual != expected:
                raise NoFollowAuthorityError(
                    f"held {lease.label} directory identity or attributes changed"
                )
        probe = _open_no_follow_directory_lease_windows(lease.path, lease.label)
        try:
            if probe.identity != lease.identity:
                raise NoFollowAuthorityError(
                    f"{lease.label} no longer names the held directory identity"
                )
        finally:
            probe.close()


    def _read_relative_bytes_windows(
        lease: "NoFollowDirectoryLease", components: tuple[str, ...], max_bytes: int,
    ) -> bytes:
        """Read a child file through the lease's final held directory HANDLE."""
        temporary_handles: list[int] = []
        failure: StrictJSONError | None = None
        data: bytes | None = None
        operation = "rooted relative ancestor open"
        try:
            current_handle = lease._handles[-1]
            for component in components[:-1]:
                current_handle = _windows_open_relative_no_follow(
                    current_handle, component,
                    _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES,
                    _FILE_ATTRIBUTE_DIRECTORY,
                    _FILE_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
                )
                temporary_handles.append(current_handle)
                _windows_directory_snapshot(current_handle, "rooted relative")

            operation = "rooted relative final leaf open"
            leaf_handle = _windows_open_relative_no_follow(
                current_handle, components[-1],
                _GENERIC_READ | _FILE_READ_ATTRIBUTES,
                _FILE_ATTRIBUTE_NORMAL,
                _FILE_NON_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
            )
            temporary_handles.append(leaf_handle)
            operation = "rooted relative held leaf inspection or read"
            before = _windows_file_snapshot(leaf_handle)
            if before[4] & (_FILE_ATTRIBUTE_REPARSE_POINT | _FILE_ATTRIBUTE_DIRECTORY):
                raise NoFollowAuthorityError(
                    "rooted relative leaf is a reparse point or directory"
                )
            if before[5] != 1:
                raise NoFollowAuthorityError(
                    "rooted relative leaf has multiple hard links"
                )
            if before[2] > max_bytes:
                raise StrictJSONError(
                    f"rooted relative leaf is {before[2]} bytes, limit is {max_bytes}"
                )
            data = _read_windows_handle(leaf_handle, max_bytes)
            after = _windows_file_snapshot(leaf_handle)
            if before != after or after[2] != len(data):
                raise NoFollowAuthorityError(
                    "rooted relative leaf changed while its held file was read"
                )
        except StrictJSONError as exc:
            failure = exc
        except OSError as exc:
            if operation == "rooted relative final leaf open":
                failure = _classify_no_follow_leaf_open_error("/".join(components), exc)
            else:
                failure = NoFollowAuthorityError(
                    f"unsafe {operation} for {'/'.join(components)}: {exc}"
                )
        finally:
            close_errors: list[str] = []
            for handle in reversed(temporary_handles):
                if not _CloseHandle(handle):  # type: ignore[name-defined]
                    close_errors.append(str(ctypes.get_last_error()))  # type: ignore[name-defined]
        if failure is not None:
            if close_errors:
                raise NoFollowAuthorityError(
                    f"{failure}; cannot close rooted relative handles: "
                    f"{'; '.join(close_errors)}"
                )
            raise failure
        if close_errors:
            raise NoFollowAuthorityError(
                "cannot close rooted relative handles: "
                f"{'; '.join(close_errors)}"
            )
        assert data is not None
        return data


class NoFollowDirectoryLease:
    """Hold a component-by-component no-follow directory authority.

    The lease retains every opened ancestor, not merely the requested final
    directory.  ``verify`` checks the held objects and then re-opens the raw
    lexical component chain to prove the caller's spelling still reaches the
    same terminal directory.  This detects a replacement around downstream
    legacy path-consuming validation phases; it is not a claim that those
    legacy readers become one atomic rooted transaction.
    """

    def __init__(
        self, path: Path, label: str, handles: list[int],
        snapshots: list[tuple[int, ...]],
    ) -> None:
        if not handles or len(handles) != len(snapshots):
            raise NoFollowAuthorityError(
                f"cannot establish empty or incomplete held {label} authority"
            )
        self.path = path
        self.label = label
        self._handles = handles
        self._snapshots = snapshots
        self.identity = tuple(snapshots[-1][:2])
        self._closed = False

    def __enter__(self) -> "NoFollowDirectoryLease":
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()

    def verify(self) -> None:
        """Fail if held identity or the raw component chain changed."""
        if self._closed:
            raise NoFollowAuthorityError(
                f"held {self.label} authority was already closed"
            )
        if os.name == "nt":
            _verify_windows_directory_lease(self)  # type: ignore[name-defined]
        else:
            _verify_posix_directory_lease(self)

    def read_relative_bytes(self, relative: str, *, max_bytes: int) -> bytes:
        """Read a literal child file through this held root without path reopen."""
        if self._closed:
            raise NoFollowAuthorityError(
                f"held {self.label} authority was already closed"
            )
        if not isinstance(max_bytes, int) or isinstance(max_bytes, bool) or max_bytes < 0:
            raise NoFollowAuthorityError("rooted relative byte limit must be non-negative")
        components = _validated_relative_components(relative)
        if os.name == "nt":
            return _read_relative_bytes_windows(self, components, max_bytes)  # type: ignore[name-defined]
        return _read_relative_bytes_posix(self, components, max_bytes)

    def posix_git_cwd(self) -> tuple[str, tuple[int, ...]]:
        """Return an inherited descriptor-rooted Git cwd on supported POSIX hosts."""
        if self._closed:
            raise NoFollowAuthorityError(
                f"held {self.label} authority was already closed"
            )
        if os.name == "nt" or not os.path.isdir("/proc/self/fd"):
            raise NoFollowAuthorityError(
                "descriptor-rooted Git cwd is unavailable on this platform"
            )
        fd = self._handles[-1]
        return f"/proc/self/fd/{fd}", (fd,)

    def close(self) -> None:
        """Release every held handle, retaining failed ones for a fatal retry."""
        if self._closed:
            return
        if os.name == "nt":
            _close_windows_directory_handles(self)  # type: ignore[name-defined]
        else:
            _close_posix_directory_handles(self)


def _posix_directory_snapshot(fd: int, label: str) -> tuple[int, int, int]:
    info = os.fstat(fd)
    if not stat.S_ISDIR(info.st_mode):
        raise NoFollowAuthorityError(
            f"unsafe held {label} directory is not a real directory"
        )
    return int(info.st_dev), int(info.st_ino), int(stat.S_IFMT(info.st_mode))


def _close_posix_directory_handles(lease: NoFollowDirectoryLease) -> None:
    remaining: list[tuple[int, tuple[int, ...]]] = []
    errors: list[str] = []
    for fd, snapshot in reversed(list(zip(lease._handles, lease._snapshots))):
        try:
            os.close(fd)
        except OSError as exc:
            remaining.append((fd, snapshot))
            errors.append(str(exc))
    remaining.reverse()
    lease._handles = [fd for fd, _snapshot in remaining]
    lease._snapshots = [snapshot for _fd, snapshot in remaining]
    if errors:
        raise NoFollowAuthorityError(
            "cannot close held no-follow directory descriptors: "
            f"{'; '.join(errors)}"
        )
    lease._closed = True


def _open_no_follow_directory_lease_posix(
    absolute: Path, label: str,
) -> NoFollowDirectoryLease:
    """Open every directory component through previously held descriptors."""
    no_follow = getattr(os, "O_NOFOLLOW", 0)
    directory = getattr(os, "O_DIRECTORY", 0)
    if not no_follow or not directory or os.open not in os.supports_dir_fd:
        raise NoFollowAuthorityError(
            "secure no-follow POSIX directory authority is unavailable"
        )
    components = [component for component in os.fspath(absolute).split(os.path.sep) if component]
    handles: list[int] = []
    snapshots: list[tuple[int, int, int]] = []
    flags = os.O_RDONLY | directory | no_follow | getattr(os, "O_CLOEXEC", 0)
    try:
        current_fd = os.open(os.path.sep, flags)
        handles.append(current_fd)
        snapshots.append(_posix_directory_snapshot(current_fd, label))
        for component in components:
            current_fd = os.open(component, flags, dir_fd=current_fd)
            handles.append(current_fd)
            snapshots.append(_posix_directory_snapshot(current_fd, label))
    except BaseException as exc:
        close_errors: list[str] = []
        for fd in reversed(handles):
            try:
                os.close(fd)
            except OSError as close_exc:
                close_errors.append(str(close_exc))
        if isinstance(exc, NoFollowAuthorityError):
            raise
        detail = f"cannot acquire held no-follow {label} authority at {absolute}: {exc}"
        if close_errors:
            detail += f"; cannot close partial directory descriptors: {'; '.join(close_errors)}"
        raise NoFollowAuthorityError(detail) from exc
    return NoFollowDirectoryLease(absolute, label, handles, snapshots)


def _verify_posix_directory_lease(lease: NoFollowDirectoryLease) -> None:
    if not lease._handles or len(lease._handles) != len(lease._snapshots):
        raise NoFollowAuthorityError(
            f"held {lease.label} directory authority is incomplete"
        )
    for fd, expected in zip(lease._handles, lease._snapshots):
        try:
            actual = _posix_directory_snapshot(fd, lease.label)
        except OSError as exc:
            raise NoFollowAuthorityError(
                f"cannot inspect held {lease.label} authority: {exc}"
            ) from exc
        if actual != expected:
            raise NoFollowAuthorityError(
                f"held {lease.label} directory identity or type changed"
            )
    probe = _open_no_follow_directory_lease_posix(lease.path, lease.label)
    try:
        if probe.identity != lease.identity:
            raise NoFollowAuthorityError(
                f"{lease.label} no longer names the held directory identity"
            )
    finally:
        probe.close()


def _read_relative_bytes_posix(
    lease: NoFollowDirectoryLease, components: tuple[str, ...], max_bytes: int,
) -> bytes:
    """Read a child file through the lease's final held directory descriptor."""
    no_follow = getattr(os, "O_NOFOLLOW", 0)
    directory = getattr(os, "O_DIRECTORY", 0)
    if not no_follow or not directory or os.open not in os.supports_dir_fd:
        raise NoFollowAuthorityError(
            "secure rooted relative POSIX reader is unavailable"
        )
    temporary_fds: list[int] = []
    failure: StrictJSONError | None = None
    data: bytes | None = None
    operation = "rooted relative ancestor open"
    directory_flags = os.O_RDONLY | directory | no_follow | getattr(os, "O_CLOEXEC", 0)
    try:
        current_fd = lease._handles[-1]
        for component in components[:-1]:
            current_fd = os.open(component, directory_flags, dir_fd=current_fd)
            temporary_fds.append(current_fd)
            _posix_directory_snapshot(current_fd, "rooted relative")

        leaf_flags = os.O_RDONLY | no_follow | getattr(os, "O_CLOEXEC", 0)
        leaf_flags |= getattr(os, "O_NONBLOCK", 0)
        operation = "rooted relative final leaf open"
        leaf_fd = os.open(components[-1], leaf_flags, dir_fd=current_fd)
        temporary_fds.append(leaf_fd)
        operation = "rooted relative held leaf inspection or read"
        before = _posix_snapshot(leaf_fd)
        if not stat.S_ISREG(before[2]):
            raise NoFollowAuthorityError("rooted relative leaf is not a regular file")
        if before[6] != 1:
            raise NoFollowAuthorityError("rooted relative leaf has multiple hard links")
        if before[3] > max_bytes:
            raise StrictJSONError(
                f"rooted relative leaf is {before[3]} bytes, limit is {max_bytes}"
            )
        data = _read_open_descriptor(leaf_fd, max_bytes)
        after = _posix_snapshot(leaf_fd)
        if before != after or after[3] != len(data):
            raise NoFollowAuthorityError(
                "rooted relative leaf changed while its held file was read"
            )
    except StrictJSONError as exc:
        failure = exc
    except OSError as exc:
        if operation == "rooted relative final leaf open":
            failure = _classify_no_follow_leaf_open_error("/".join(components), exc)
        else:
            failure = NoFollowAuthorityError(
                f"unsafe {operation} for {'/'.join(components)}: {exc}"
            )
    finally:
        close_errors: list[str] = []
        for fd in reversed(temporary_fds):
            try:
                os.close(fd)
            except OSError as exc:
                close_errors.append(str(exc))
    if failure is not None:
        if close_errors:
            raise NoFollowAuthorityError(
                f"{failure}; cannot close rooted relative descriptors: "
                f"{'; '.join(close_errors)}"
            )
        raise failure
    if close_errors:
        raise NoFollowAuthorityError(
            "cannot close rooted relative descriptors: "
            f"{'; '.join(close_errors)}"
        )
    assert data is not None
    return data


def open_no_follow_directory_lease(
    path: Path | str, *, label: str = "directory",
) -> NoFollowDirectoryLease:
    """Acquire a held, component-by-component no-follow directory authority.

    This API deliberately has no path-only fallback on Windows.  It is used for
    a repository root before non-policy lifecycle validation so that a final
    root *or any ancestor* reparse point is rejected before an explicit evidence
    file can cause later root-relative semantic/provenance work to run.
    """
    absolute = lexical_absolute_no_follow_path(path, label=label)
    if os.name == "nt":
        return _open_no_follow_directory_lease_windows(absolute, label)  # type: ignore[name-defined]
    return _open_no_follow_directory_lease_posix(absolute, label)


def _reject_constant(name: str) -> Any:
    raise StrictJSONError(
        f"non-finite JSON constant {name!r} is not permitted anywhere in the document"
    )


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    seen: set[str] = set()
    for key, _ in pairs:
        if key in seen:
            raise StrictJSONError(
                f"duplicate JSON property {key!r} — duplicate properties are "
                f"rejected because they let a hostile value shadow a valid one"
            )
        seen.add(key)
    return dict(pairs)


def _walk(node: Any, pointer: str, depth: int, counter: list[int], limits: Limits) -> None:
    """Enforce depth, node-count, string and container limits recursively."""
    counter[0] += 1
    if counter[0] > limits.total_nodes:
        raise StrictJSONError(
            f"document exceeds the {limits.total_nodes} node limit at {pointer or '/'}"
        )
    if depth > limits.depth:
        raise StrictJSONError(
            f"document nesting exceeds the depth limit of {limits.depth} "
            f"at {pointer or '/'}"
        )

    if isinstance(node, dict):
        if len(node) > limits.container_items:
            raise StrictJSONError(
                f"{pointer or '/'}: object has {len(node)} properties, "
                f"limit is {limits.container_items}"
            )
        for key, value in node.items():
            if len(key) > limits.string_length:
                raise StrictJSONError(
                    f"{pointer or '/'}: property name exceeds "
                    f"{limits.string_length} characters"
                )
            _walk(value, f"{pointer}/{key}", depth + 1, counter, limits)
    elif isinstance(node, list):
        if len(node) > limits.container_items:
            raise StrictJSONError(
                f"{pointer or '/'}: array has {len(node)} items, "
                f"limit is {limits.container_items}"
            )
        for i, value in enumerate(node):
            _walk(value, f"{pointer}/{i}", depth + 1, counter, limits)
    elif isinstance(node, str):
        if len(node) > limits.string_length:
            raise StrictJSONError(
                f"{pointer or '/'}: string exceeds {limits.string_length} characters"
            )
    elif isinstance(node, float):
        # json's parse_constant only covers the bare literals; a value such as
        # 1e400 parses to inf through parse_float and must also be rejected.
        if node != node or node in (float("inf"), float("-inf")):
            raise StrictJSONError(
                f"{pointer or '/'}: non-finite number is not permitted"
            )


def loads(text: str, *, origin: str = "<string>", limits: Limits = DEFAULT_LIMITS) -> Any:
    """Parse `text` under the strict rules above, or raise StrictJSONError."""
    encoded = text.encode("utf-8")
    if len(encoded) > limits.document_bytes:
        raise StrictJSONError(
            f"{origin}: document is {len(encoded)} bytes, "
            f"limit is {limits.document_bytes}"
        )
    try:
        parsed = json.loads(
            text,
            object_pairs_hook=_no_duplicates,
            parse_constant=_reject_constant,
        )
    except StrictJSONError as exc:
        raise StrictJSONError(f"{origin}: {exc}") from None
    except json.JSONDecodeError as exc:
        raise StrictJSONError(f"{origin}: invalid JSON: {exc}") from exc

    _walk(parsed, "", 1, [0], limits)
    return parsed


def load_file(path: Path | str, *, limits: Limits = DEFAULT_LIMITS) -> Any:
    """Read and strictly parse a JSON document from disk."""
    path = Path(path)
    if not path.is_file():
        raise StrictJSONError(f"file not found: {path}")
    size = path.stat().st_size
    if size > limits.document_bytes:
        raise StrictJSONError(
            f"{path}: document is {size} bytes, limit is {limits.document_bytes}"
        )
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise StrictJSONError(f"{path}: file is not valid UTF-8: {exc}") from exc
    return loads(text, origin=str(path), limits=limits)


def load_file_no_follow(path: Path | str, *, limits: Limits = DEFAULT_LIMITS) -> Any:
    """Read one strict JSON document through held, no-follow filesystem authority.

    Unlike :func:`load_file`, this routine never performs an ``is_file``/
    ``stat``/path-reopen sequence.  It opens every ancestor through already-held
    directory authority, rejects a reparse point at every component, reads the
    final regular file through that exact handle/descriptor, and verifies its
    identity and metadata did not change during the read.  Windows requires the
    native rooted ``NtCreateFile`` binding; there is no weaker path-based
    fallback when that binding is unavailable.
    """
    if os.name == "nt":
        data = _read_file_no_follow_windows(path, limits.document_bytes)
    else:
        data = _read_file_no_follow_posix(path, limits.document_bytes)
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise StrictJSONError(f"{path}: file is not valid UTF-8: {exc}") from exc
    return loads(text, origin=str(path), limits=limits)
