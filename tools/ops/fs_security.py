#!/usr/bin/env python3
"""Handle-based filesystem confinement for OPS-100 validation tools."""

from __future__ import annotations

import contextlib
import os
import stat
import time
import unicodedata
from dataclasses import dataclass
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Iterator


MAX_FILENAME_BYTES = 255
_WINDOWS_RESERVED_NAMES = {
    "CON",
    "PRN",
    "AUX",
    "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}


class FilesystemPolicyError(OSError):
    """A path or file violates the confined-root policy."""

    def __init__(self, code: str, message: str, *, size: int | None = None) -> None:
        super().__init__(message)
        self.code = code
        self.size = size


@dataclass(frozen=True)
class FileMetadata:
    size: int
    mtime_ns: int
    device: int
    file_id: int
    links: int


def validate_portable_filename(value: str) -> str:
    """Accept exactly one portable filename under an already-authoritative root."""

    if not isinstance(value, str) or not value:
        raise FilesystemPolicyError("invalid-name", "filename must be a non-empty string")
    if "\x00" in value:
        raise FilesystemPolicyError("invalid-name", "filename contains a NUL byte")
    if unicodedata.normalize("NFC", value) != value:
        raise FilesystemPolicyError("path-alias", "filename must use NFC Unicode normalization")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise FilesystemPolicyError("invalid-name", "filename contains a control character")
    if any(character in '<>"|?*' for character in value):
        raise FilesystemPolicyError("invalid-name", "filename contains a Windows-forbidden character")
    try:
        encoded_size = len(value.encode("utf-8", errors="strict"))
    except UnicodeEncodeError as exc:
        raise FilesystemPolicyError("invalid-name", "filename contains an invalid Unicode scalar") from exc
    if encoded_size > MAX_FILENAME_BYTES:
        raise FilesystemPolicyError("invalid-name", f"filename exceeds {MAX_FILENAME_BYTES} UTF-8 bytes")

    posix = PurePosixPath(value)
    windows = PureWindowsPath(value)
    if posix.is_absolute() or windows.is_absolute() or windows.drive or windows.root:
        raise FilesystemPolicyError("absolute-path", f"absolute or drive-qualified path is forbidden: {value!r}")
    if posix.parts != (value,) or windows.parts != (value,):
        raise FilesystemPolicyError("path-depth", f"subpaths and traversal are forbidden: {value!r}")
    if value in {".", ".."} or "/" in value or "\\" in value or ":" in value:
        raise FilesystemPolicyError("path-alias", f"path aliases are forbidden: {value!r}")
    if value[-1] in {" ", "."}:
        raise FilesystemPolicyError("path-alias", f"Windows trailing-dot/space aliases are forbidden: {value!r}")

    stem = value.split(".", 1)[0].rstrip(" .").upper()
    if stem in _WINDOWS_RESERVED_NAMES:
        raise FilesystemPolicyError("path-alias", f"Windows device name is forbidden: {value!r}")
    return value


if os.name == "nt":
    import ctypes
    import msvcrt
    from ctypes import wintypes

    _FILE_LIST_DIRECTORY = 0x0001
    _FILE_READ_ATTRIBUTES = 0x0080
    _GENERIC_READ = 0x80000000
    _FILE_SHARE_READ = 0x00000001
    _FILE_SHARE_WRITE = 0x00000002
    _OPEN_EXISTING = 3
    _FILE_ATTRIBUTE_DIRECTORY = 0x00000010
    _FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
    _FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
    _FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
    _FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    _WINDOWS_TO_UNIX_100NS = 116444736000000000

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

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _CreateFileW = _kernel32.CreateFileW
    _CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    _CreateFileW.restype = wintypes.HANDLE
    _CloseHandle = _kernel32.CloseHandle
    _CloseHandle.argtypes = [wintypes.HANDLE]
    _CloseHandle.restype = wintypes.BOOL
    _GetFileInformationByHandle = _kernel32.GetFileInformationByHandle
    _GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(_BY_HANDLE_FILE_INFORMATION)]
    _GetFileInformationByHandle.restype = wintypes.BOOL
    _GetFinalPathNameByHandleW = _kernel32.GetFinalPathNameByHandleW
    _GetFinalPathNameByHandleW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
    _GetFinalPathNameByHandleW.restype = wintypes.DWORD


def _filetime_ticks(value: object) -> int:
    return (int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)  # type: ignore[attr-defined]


def _windows_metadata(info: object) -> FileMetadata:
    size = (int(info.nFileSizeHigh) << 32) | int(info.nFileSizeLow)  # type: ignore[attr-defined]
    file_id = (int(info.nFileIndexHigh) << 32) | int(info.nFileIndexLow)  # type: ignore[attr-defined]
    unix_ticks = max(0, _filetime_ticks(info.ftLastWriteTime) - _WINDOWS_TO_UNIX_100NS)  # type: ignore[attr-defined]
    return FileMetadata(
        size=size,
        mtime_ns=unix_ticks * 100,
        device=int(info.dwVolumeSerialNumber),  # type: ignore[attr-defined]
        file_id=file_id,
        links=int(info.nNumberOfLinks),  # type: ignore[attr-defined]
    )


def _windows_info(handle: int) -> object:
    info = _BY_HANDLE_FILE_INFORMATION()  # type: ignore[name-defined]
    if not _GetFileInformationByHandle(handle, ctypes.byref(info)):  # type: ignore[name-defined]
        raise FilesystemPolicyError("file-info", f"GetFileInformationByHandle failed: {ctypes.get_last_error()}")
    return info


def _read_descriptor(fd: int, max_bytes: int, deadline: float) -> bytes:
    chunks: list[bytes] = []
    total = 0
    while True:
        if time.monotonic() > deadline:
            raise FilesystemPolicyError("time-bound", "file read exceeded the validation deadline")
        chunk = os.read(fd, min(64 * 1024, max_bytes + 1 - total))
        if not chunk:
            break
        chunks.append(chunk)
        total += len(chunk)
        if total > max_bytes:
            raise FilesystemPolicyError("file-too-large", f"file grew beyond {max_bytes} bytes", size=total)
    return b"".join(chunks)


class SecureRoot:
    """Pin a non-reparse directory and open every child through that authority."""

    def __init__(self, path: Path | str) -> None:
        self.requested_path = Path(path)
        self.display_path = str(self.requested_path)
        self._absolute_path = os.path.abspath(self.requested_path)
        self._fd: int | None = None
        self._handle: int | None = None
        self._scan_path: str | None = None
        if os.name == "nt":
            self._open_windows()
        else:
            self._open_posix()

    def __enter__(self) -> "SecureRoot":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    def close(self) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None
        if self._handle is not None:
            _CloseHandle(self._handle)  # type: ignore[name-defined]
            self._handle = None

    def _open_posix(self) -> None:
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        flags |= getattr(os, "O_CLOEXEC", 0)
        try:
            fd = os.open(self._absolute_path, flags)
        except OSError as exc:
            raise FilesystemPolicyError(
                "root-open", f"cannot open confined root {self.display_path}: {exc}"
            ) from exc
        info = os.fstat(fd)
        if not stat.S_ISDIR(info.st_mode):
            os.close(fd)
            raise FilesystemPolicyError("root-type", f"confined root is not a directory: {self.display_path}")
        if hasattr(os, "geteuid") and info.st_uid != os.geteuid():
            os.close(fd)
            raise FilesystemPolicyError(
                "root-owner", f"confined root is not owned by the current user: {self.display_path}"
            )
        if info.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            os.close(fd)
            raise FilesystemPolicyError(
                "root-permissions", f"confined root is group/world-writable: {self.display_path}"
            )
        try:
            named = os.stat(self._absolute_path, follow_symlinks=False)
            canonical = os.path.realpath(self._absolute_path, strict=True)
        except OSError as exc:
            os.close(fd)
            raise FilesystemPolicyError("root-race", f"cannot revalidate confined root: {exc}") from exc
        if (
            not stat.S_ISDIR(named.st_mode)
            or named.st_dev != info.st_dev
            or named.st_ino != info.st_ino
            or os.path.normcase(os.path.normpath(canonical))
            != os.path.normcase(os.path.normpath(self._absolute_path))
        ):
            os.close(fd)
            raise FilesystemPolicyError(
                "root-alias", f"confined root uses a symlink/alias or changed while opening: {self.display_path}"
            )
        self._fd = fd

    def _open_windows(self) -> None:
        absolute = self._absolute_path
        handle = _CreateFileW(  # type: ignore[name-defined]
            absolute,
            _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES,  # type: ignore[name-defined]
            _FILE_SHARE_READ | _FILE_SHARE_WRITE,  # type: ignore[name-defined]
            None,
            _OPEN_EXISTING,  # type: ignore[name-defined]
            _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT,  # type: ignore[name-defined]
            None,
        )
        if handle == _INVALID_HANDLE_VALUE:  # type: ignore[name-defined]
            raise FilesystemPolicyError(
                "root-open", f"cannot open confined root {absolute}: {ctypes.get_last_error()}"
            )
        info = _windows_info(handle)
        if not info.dwFileAttributes & _FILE_ATTRIBUTE_DIRECTORY:  # type: ignore[attr-defined,name-defined]
            _CloseHandle(handle)  # type: ignore[name-defined]
            raise FilesystemPolicyError("root-type", f"confined root is not a directory: {absolute}")
        if info.dwFileAttributes & _FILE_ATTRIBUTE_REPARSE_POINT:  # type: ignore[attr-defined,name-defined]
            _CloseHandle(handle)  # type: ignore[name-defined]
            raise FilesystemPolicyError(
                "root-reparse", f"confined root is a junction, symlink, or reparse point: {absolute}"
            )

        needed = _GetFinalPathNameByHandleW(handle, None, 0, 0)  # type: ignore[name-defined]
        if not needed:
            error = ctypes.get_last_error()
            _CloseHandle(handle)  # type: ignore[name-defined]
            raise FilesystemPolicyError("root-final-path", f"cannot resolve pinned root {absolute}: {error}")
        buffer = ctypes.create_unicode_buffer(needed + 1)
        written = _GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)  # type: ignore[name-defined]
        if not written or written >= len(buffer):
            error = ctypes.get_last_error()
            _CloseHandle(handle)  # type: ignore[name-defined]
            raise FilesystemPolicyError("root-final-path", f"cannot resolve pinned root {absolute}: {error}")
        canonical = buffer.value
        if canonical.startswith("\\\\?\\UNC\\"):
            canonical = "\\\\" + canonical[8:]
        elif canonical.startswith("\\\\?\\"):
            canonical = canonical[4:]
        if os.path.normcase(os.path.normpath(canonical)) != os.path.normcase(os.path.normpath(absolute)):
            _CloseHandle(handle)  # type: ignore[name-defined]
            raise FilesystemPolicyError(
                "root-alias", f"confined root uses a junction, symlink, or path alias: {absolute}"
            )
        self._handle = int(handle)
        self._scan_path = buffer.value.rstrip("\\/")

    def iter_names(self, *, max_entries: int, deadline: float) -> Iterator[str]:
        if max_entries < 1:
            raise ValueError("max_entries must be positive")
        scan_target: object = self._scan_path if os.name == "nt" else self._fd
        count = 0
        try:
            with os.scandir(scan_target) as entries:  # type: ignore[arg-type]
                for entry in entries:
                    if time.monotonic() > deadline:
                        raise FilesystemPolicyError("time-bound", "directory scan exceeded the validation deadline")
                    count += 1
                    if count > max_entries:
                        raise FilesystemPolicyError(
                            "entry-count", f"directory contains more than {max_entries} entries"
                        )
                    yield validate_portable_filename(entry.name)
        except FilesystemPolicyError:
            raise
        except OSError as exc:
            raise FilesystemPolicyError("directory-read", f"cannot enumerate {self.display_path}: {exc}") from exc

    def read_file(self, name: str, *, max_bytes: int, deadline: float) -> tuple[bytes, FileMetadata]:
        validate_portable_filename(name)
        if max_bytes < 0:
            raise ValueError("max_bytes must be non-negative")
        if time.monotonic() > deadline:
            raise FilesystemPolicyError("time-bound", "validation deadline elapsed before file open")
        if os.name == "nt":
            return self._read_windows(name, max_bytes=max_bytes, deadline=deadline)
        return self._read_posix(name, max_bytes=max_bytes, deadline=deadline)

    def _read_posix(self, name: str, *, max_bytes: int, deadline: float) -> tuple[bytes, FileMetadata]:
        assert self._fd is not None
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NONBLOCK", 0)
        try:
            fd = os.open(name, flags, dir_fd=self._fd)
        except OSError as exc:
            raise FilesystemPolicyError("file-open", f"cannot safely open {name!r}: {exc}") from exc
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode):
                raise FilesystemPolicyError("file-type", f"{name!r} is not a regular file")
            if before.st_nlink != 1:
                raise FilesystemPolicyError("hardlink", f"{name!r} has {before.st_nlink} hard links")
            if before.st_size > max_bytes:
                raise FilesystemPolicyError(
                    "file-too-large", f"{name!r} is {before.st_size} bytes; limit is {max_bytes}", size=before.st_size
                )
            data = _read_descriptor(fd, max_bytes, deadline)
            after = os.fstat(fd)
            stable = (
                before.st_dev == after.st_dev
                and before.st_ino == after.st_ino
                and before.st_size == after.st_size == len(data)
                and before.st_mtime_ns == after.st_mtime_ns
                and before.st_ctime_ns == after.st_ctime_ns
                and after.st_nlink == 1
            )
            if not stable:
                raise FilesystemPolicyError("file-changed", f"{name!r} changed while it was being validated")
            return data, FileMetadata(
                size=after.st_size,
                mtime_ns=after.st_mtime_ns,
                device=after.st_dev,
                file_id=after.st_ino,
                links=after.st_nlink,
            )
        finally:
            os.close(fd)

    def _read_windows(self, name: str, *, max_bytes: int, deadline: float) -> tuple[bytes, FileMetadata]:
        assert self._scan_path is not None
        child = os.path.join(self._scan_path, name)
        handle = _CreateFileW(  # type: ignore[name-defined]
            child,
            _GENERIC_READ | _FILE_READ_ATTRIBUTES,  # type: ignore[name-defined]
            _FILE_SHARE_READ,  # type: ignore[name-defined]
            None,
            _OPEN_EXISTING,  # type: ignore[name-defined]
            _FILE_FLAG_OPEN_REPARSE_POINT | _FILE_FLAG_SEQUENTIAL_SCAN,  # type: ignore[name-defined]
            None,
        )
        if handle == _INVALID_HANDLE_VALUE:  # type: ignore[name-defined]
            raise FilesystemPolicyError("file-open", f"cannot safely open {name!r}: {ctypes.get_last_error()}")
        fd: int | None = None
        try:
            before_info = _windows_info(handle)
            if before_info.dwFileAttributes & _FILE_ATTRIBUTE_REPARSE_POINT:  # type: ignore[attr-defined,name-defined]
                raise FilesystemPolicyError("file-reparse", f"{name!r} is a junction, symlink, or reparse point")
            if before_info.dwFileAttributes & _FILE_ATTRIBUTE_DIRECTORY:  # type: ignore[attr-defined,name-defined]
                raise FilesystemPolicyError("file-type", f"{name!r} is a directory, not a regular file")
            before = _windows_metadata(before_info)
            if before.links != 1:
                raise FilesystemPolicyError("hardlink", f"{name!r} has {before.links} hard links")
            if before.size > max_bytes:
                raise FilesystemPolicyError(
                    "file-too-large", f"{name!r} is {before.size} bytes; limit is {max_bytes}", size=before.size
                )

            fd = msvcrt.open_osfhandle(  # type: ignore[name-defined]
                int(handle), os.O_RDONLY | getattr(os, "O_BINARY", 0)
            )
            handle = None
            data = _read_descriptor(fd, max_bytes, deadline)
            crt_handle = msvcrt.get_osfhandle(fd)  # type: ignore[name-defined]
            after_info = _windows_info(crt_handle)
            after = _windows_metadata(after_info)
            if before != after or after.size != len(data):
                raise FilesystemPolicyError("file-changed", f"{name!r} changed while it was being validated")
            return data, after
        finally:
            if fd is not None:
                os.close(fd)
            elif handle not in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
                _CloseHandle(handle)  # type: ignore[name-defined]


@contextlib.contextmanager
def open_secure_root(path: Path | str) -> Iterator[SecureRoot]:
    root = SecureRoot(path)
    try:
        yield root
    finally:
        root.close()
