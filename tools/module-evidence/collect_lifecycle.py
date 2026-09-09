#!/usr/bin/env python3
"""Produce runtime lifecycle evidence by really running a module.

This is the *producer* half of the lifecycle contract. It launches the exact
stable-v1 Windows command against its built game DLL and accepts one direct,
host-owned terminal record emitted after teardown:

    SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=4 \
    fixed=2 render=4 unload=1 destroy=1 faults=0

If the engine binary is missing, the run fails, or the trace contains none of
the required markers, this program writes **no evidence file** and exits
non-zero.  A partial or empty document is never emitted: absent evidence must
present as absent, not as a module that ran and did nothing.

Usage:
    python tools/module-evidence/collect_lifecycle.py \
        --engine package/SparkEngine.exe --module SparkGameFPS \
        --module-image package/SparkGameFPS.dll --working-directory package \
        --rhi-backend d3d11 \
        --image-manifest <ABSOLUTE_WORKING_DIRECTORY>\\module-lifecycle-images.json \
        --out <ABSOLUTE_REPO_ROOT>\\build\\module-evidence\\module-lifecycle.json

``<ABSOLUTE_WORKING_DIRECTORY>`` and ``<ABSOLUTE_REPO_ROOT>`` are literal
placeholders: release automation must supply those exact absolute paths.  The
collector accepts no alternate manifest filename or output namespace.

Completion contract:
    The ``OK: wrote ...`` line is progress, not a commit signal. Automation
    must wait for this collector process to exit successfully. On success the
    collector deliberately retains final-file guards until Windows ExitProcess
    closes them; it makes no claim of a durable lock after the process exits.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import ntpath
import os
import re
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lifecycle as lifecycle_mod  # noqa: E402
import paths as paths_mod  # noqa: E402
import strict_json  # noqa: E402
from provenance import resolve_head_sha  # noqa: E402
from schema import expected_library_names  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

_ENGINE_NAME = "SparkEngine.exe"
_SCRIPT_EXTENSIONS = frozenset({".cmd", ".bat", ".sh", ".ps1", ".py", ".pl", ".rb"})
_SCRIPT_SIGNATURES = (b"#!", b"@echo", b"@ECHO", b"@rem", b"@REM")
_MIN_ENGINE_SIZE = 4096
IMAGE_MANIFEST_SCHEMA = "spark-image-manifest-v1"
IMAGE_MANIFEST_FILENAME = "module-lifecycle-images.json"
LIFECYCLE_OUTPUT_DIRECTORY = ("build", "module-evidence")
LIFECYCLE_OUTPUT_FILENAME = "module-lifecycle.json"
LIFECYCLE_AUDIT_LOG_FILENAME = "module-lifecycle-SparkGameFPS.log"
LIFECYCLE_TOKEN = "SPARK_MODULE_LIFECYCLE"

_WINDOWS_RESERVED_NAMES = frozenset(
    {"con", "prn", "aux", "nul"}
    | {f"com{i}" for i in range(1, 10)}
    | {f"lpt{i}" for i in range(1, 10)}
)

_GENERIC_READ = 0x80000000
_GENERIC_WRITE = 0x40000000
_DELETE = 0x00010000
_SYNCHRONIZE = 0x00100000
_FILE_LIST_DIRECTORY = 0x00000001
_FILE_READ_ATTRIBUTES = 0x0080
_FILE_WRITE_ATTRIBUTES = 0x0100
_FILE_SHARE_READ = 0x00000001
_FILE_SHARE_WRITE = 0x00000002
_CREATE_NEW = 1
_OPEN_EXISTING = 3
_FILE_OPEN = 1
_FILE_CREATE = 2
_FILE_OPEN_IF = 3
_FILE_ATTRIBUTE_NORMAL = 0x00000080
_FILE_ATTRIBUTE_DIRECTORY = 0x00000010
_FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
_FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
_FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
_FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
_FILE_DIRECTORY_FILE = 0x00000001
_FILE_NON_DIRECTORY_FILE = 0x00000040
_FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020
_OBJ_CASE_INSENSITIVE = 0x00000040
_HANDLE_FLAG_INHERIT = 0x00000001
_DUPLICATE_SAME_ACCESS = 0x00000002
_FILE_BEGIN = 0
_FILE_DISPOSITION_INFO = 4
_ERROR_FILE_NOT_FOUND = 2
_ERROR_PATH_NOT_FOUND = 3
_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034
_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A


WINDOWS_IMAGE_LEASE_AVAILABLE = False
WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE = False
# Successful CLI publication keeps one exact file HANDLE per final leaf alive
# until Windows tears down this collector process.  The integers are retained
# explicitly because OutputArtifactHandle intentionally has no destructor: a
# Python GC cycle must never close an accepted evidence guard early.
_PROCESS_LIFETIME_OUTPUT_RESERVE_HANDLES: list[int] = []


def _stable_v1_windows_platform() -> bool:
    """Keep the CLI platform gate injectable without mutating global ``os.name``."""
    return os.name == "nt"


if os.name == "nt":
    try:
        import ctypes
        from ctypes import wintypes

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

        class _FILE_DISPOSITION_INFORMATION(ctypes.Structure):
            _fields_ = [("DeleteFile", ctypes.c_ubyte)]

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
        _GetCurrentProcess = _kernel32.GetCurrentProcess
        _GetCurrentProcess.argtypes = []
        _GetCurrentProcess.restype = wintypes.HANDLE
        _DuplicateHandle = _kernel32.DuplicateHandle
        _DuplicateHandle.argtypes = [
            wintypes.HANDLE, wintypes.HANDLE, wintypes.HANDLE,
            ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD, wintypes.BOOL,
            wintypes.DWORD,
        ]
        _DuplicateHandle.restype = wintypes.BOOL
        _GetFileInformationByHandle = _kernel32.GetFileInformationByHandle
        _GetFileInformationByHandle.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(_BY_HANDLE_FILE_INFORMATION),
        ]
        _GetFileInformationByHandle.restype = wintypes.BOOL
        _GetFinalPathNameByHandleW = _kernel32.GetFinalPathNameByHandleW
        _GetFinalPathNameByHandleW.argtypes = [
            wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD,
        ]
        _GetFinalPathNameByHandleW.restype = wintypes.DWORD
        _SetFilePointerEx = _kernel32.SetFilePointerEx
        _SetFilePointerEx.argtypes = [
            wintypes.HANDLE, ctypes.c_longlong, ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD,
        ]
        _SetFilePointerEx.restype = wintypes.BOOL
        _ReadFile = _kernel32.ReadFile
        _ReadFile.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        ]
        _ReadFile.restype = wintypes.BOOL
        _WriteFile = _kernel32.WriteFile
        _WriteFile.argtypes = [
            wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
        ]
        _WriteFile.restype = wintypes.BOOL
        _FlushFileBuffers = _kernel32.FlushFileBuffers
        _FlushFileBuffers.argtypes = [wintypes.HANDLE]
        _FlushFileBuffers.restype = wintypes.BOOL
        _SetFileInformationByHandle = _kernel32.SetFileInformationByHandle
        _SetFileInformationByHandle.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
        ]
        _SetFileInformationByHandle.restype = wintypes.BOOL
        _NtCreateFile = _ntdll.NtCreateFile
        _NtCreateFile.argtypes = [
            ctypes.POINTER(wintypes.HANDLE), wintypes.DWORD,
            ctypes.POINTER(_OBJECT_ATTRIBUTES), ctypes.POINTER(_IO_STATUS_BLOCK),
            ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD, wintypes.DWORD,
            wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.ULONG,
        ]
        _NtCreateFile.restype = wintypes.LONG
        _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
        WINDOWS_IMAGE_LEASE_AVAILABLE = True
        WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE = True
    except (AttributeError, ImportError, OSError):
        # Production collection must reject rather than silently reducing this
        # to path-based hashing when a native lease cannot be established.
        WINDOWS_IMAGE_LEASE_AVAILABLE = False
        WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE = False


@dataclass(frozen=True)
class EngineOutput:
    stdout: str
    stderr: str
    engine_image: ImageVerification | None = None
    module_image: ImageVerification | None = None

    @property
    def audit_log(self) -> str:
        separator = "" if self.stdout.endswith("\n") else "\n"
        return f"--- stdout ---\n{self.stdout}{separator}--- stderr ---\n{self.stderr}"


@dataclass(frozen=True)
class ImageIdentity:
    """Windows handle identity and mutation-relevant metadata for one image."""

    volume_serial: int
    file_index: int
    size: int
    last_write_time: int


@dataclass(frozen=True)
class ImageVerification:
    """A manifest-matched digest and final path read from a live file handle."""

    digest: str
    final_path: str
    identity: ImageIdentity
    attributes: int = 0


def _filetime_ticks(value: object) -> int:
    return (int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)  # type: ignore[attr-defined]


def _native_identity(handle: int) -> tuple[ImageIdentity, int]:
    if not (WINDOWS_IMAGE_LEASE_AVAILABLE or WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE):
        raise OSError("native Windows handle identity support is unavailable")
    info = _BY_HANDLE_FILE_INFORMATION()  # type: ignore[name-defined]
    if not _GetFileInformationByHandle(handle, ctypes.byref(info)):  # type: ignore[name-defined]
        raise OSError(f"GetFileInformationByHandle failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
    identity = ImageIdentity(
        volume_serial=int(info.dwVolumeSerialNumber),
        file_index=(int(info.nFileIndexHigh) << 32) | int(info.nFileIndexLow),
        size=(int(info.nFileSizeHigh) << 32) | int(info.nFileSizeLow),
        last_write_time=_filetime_ticks(info.ftLastWriteTime),
    )
    return identity, int(info.dwFileAttributes)


def _native_final_path(handle: int) -> str:
    if not (WINDOWS_IMAGE_LEASE_AVAILABLE or WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE):
        raise OSError("native Windows handle final-path support is unavailable")
    size = 512
    while True:
        buffer = ctypes.create_unicode_buffer(size)  # type: ignore[name-defined]
        length = _GetFinalPathNameByHandleW(handle, buffer, size, 0)  # type: ignore[name-defined]
        if length == 0:
            raise OSError(f"GetFinalPathNameByHandleW failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        if length < size:
            return _display_windows_final_path(buffer.value)
        size = int(length) + 1


def _native_read_at(handle: int, offset: int, size: int) -> bytes:
    if not WINDOWS_IMAGE_LEASE_AVAILABLE:
        raise OSError("native Windows image lease support is unavailable")
    if offset < 0 or size < 0:
        raise OSError("invalid native image read range")
    if size == 0:
        return b""
    if not _SetFilePointerEx(handle, ctypes.c_longlong(offset), None, _FILE_BEGIN):  # type: ignore[name-defined]
        raise OSError(f"SetFilePointerEx failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
    buffer = ctypes.create_string_buffer(size)  # type: ignore[name-defined]
    read = wintypes.DWORD()  # type: ignore[name-defined]
    if not _ReadFile(handle, buffer, size, ctypes.byref(read), None):  # type: ignore[name-defined]
        raise OSError(f"ReadFile failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
    return buffer.raw[:int(read.value)]


class ImageLease:
    """An owning, non-inheritable Windows image handle held across execution."""

    def __init__(self, path: Path, handle: int, identity: ImageIdentity,
                 attributes: int, final_path: str) -> None:
        self.path = path
        self._handle = handle
        self.identity = identity
        self.attributes = attributes
        self.final_path = final_path
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    def snapshot(self) -> tuple[ImageIdentity, int]:
        if self._closed:
            raise OSError("image lease is already closed")
        return _native_identity(self._handle)

    def read_at(self, offset: int, size: int) -> bytes:
        if self._closed:
            raise OSError("image lease is already closed")
        return _native_read_at(self._handle, offset, size)

    def sha256(self) -> str:
        if self._closed:
            raise OSError("image lease is already closed")
        before, _ = self.snapshot()
        h = hashlib.sha256()
        offset = 0
        while offset < before.size:
            chunk = self.read_at(offset, min(1 << 16, before.size - offset))
            if not chunk:
                raise OSError("image lease reached EOF before its recorded size")
            h.update(chunk)
            offset += len(chunk)
        after, _ = self.snapshot()
        if after != before:
            raise OSError("image changed while it was hashed through its live lease")
        return h.hexdigest()

    def close(self) -> None:
        if self._closed:
            return
        if not _CloseHandle(self._handle):  # type: ignore[name-defined]
            raise OSError(f"CloseHandle failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        # A failed CloseHandle leaves ownership with this object.  Keep the
        # lease live so the caller can retry or retain its fail-closed cleanup
        # authority; marking it closed first would both lose that authority and
        # misreport a still-open native handle.
        self._closed = True

    def __enter__(self) -> ImageLease:
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> bool:
        self.close()
        return False


class OutputDirectoryLease:
    """A Windows HANDLE pinning one directory object for rooted child operations."""

    def __init__(self, path: Path, handle: int, identity: ImageIdentity,
                 attributes: int, final_path: str) -> None:
        self.path = path
        self._handle = handle
        self.identity = identity
        self.attributes = attributes
        self.final_path = final_path
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    def snapshot(self) -> tuple[ImageIdentity, int]:
        if self._closed:
            raise OSError("output-directory lease is already closed")
        return _native_identity(self._handle)

    def close(self) -> None:
        if self._closed:
            return
        if not _CloseHandle(self._handle):  # type: ignore[name-defined]
            raise OSError(f"CloseHandle failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        self._closed = True

    def __enter__(self) -> OutputDirectoryLease:
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> bool:
        self.close()
        return False


class OutputArtifactHandle:
    """One fixed output leaf owned through a native handle."""

    def __init__(self, handle: int, directory: OutputDirectoryLease, leaf_name: str,
                 *, identity: ImageIdentity | None = None,
                 attributes: int | None = None,
                 final_path: str | None = None) -> None:
        self._handle = handle
        self._directory = directory
        self._leaf_name = leaf_name
        self._identity = identity
        self._attributes = attributes
        self._final_path = final_path
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def leaf_name(self) -> str:
        return self._leaf_name

    def verify(self, expected_path: Path) -> None:
        """Ensure this retained deletion authority still names its fixed leaf."""
        if self._closed:
            raise OSError("lifecycle output artifact is already closed")
        if self._identity is None or self._attributes is None or self._final_path is None:
            raise OSError("lifecycle output artifact was not sealed after publication")
        identity, attributes = _native_identity(self._handle)
        if identity != self._identity:
            raise OSError("lifecycle output artifact identity changed after publication")
        if attributes != self._attributes:
            raise OSError("lifecycle output artifact attributes changed after publication")
        _native_handle_matches_output_path(self._handle, expected_path)
        final_path = _native_final_path(self._handle)
        if _normalise_windows_final_path(final_path) != _normalise_windows_final_path(self._final_path):
            raise OSError("lifecycle output artifact final path changed after publication")

    def seal(self, expected_path: Path) -> None:
        """Record the verified identity for an exact newly-created final leaf."""
        if self._closed:
            raise OSError("lifecycle output artifact is already closed")
        identity, attributes = _native_identity(self._handle)
        _native_handle_matches_output_path(self._handle, expected_path)
        final_path = _native_final_path(self._handle)
        self._identity = identity
        self._attributes = attributes
        self._final_path = final_path

    def duplicate(self) -> OutputArtifactHandle:
        """Retain a second path-independent deletion authority for this leaf."""
        if self._closed:
            raise OSError("lifecycle output artifact is already closed")
        duplicate = wintypes.HANDLE()  # type: ignore[name-defined]
        current_process = _GetCurrentProcess()  # type: ignore[name-defined]
        if not _DuplicateHandle(  # type: ignore[name-defined]
            current_process, self._handle, current_process, ctypes.byref(duplicate),  # type: ignore[name-defined]
            0, False, _DUPLICATE_SAME_ACCESS,
        ):
            raise OSError(f"DuplicateHandle failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        handle = duplicate.value
        if handle in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
            raise OSError("DuplicateHandle returned an invalid lifecycle output handle")
        try:
            if not _SetHandleInformation(handle, _HANDLE_FLAG_INHERIT, 0):  # type: ignore[name-defined]
                raise OSError(f"SetHandleInformation failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
            return OutputArtifactHandle(
                handle, self._directory, self._leaf_name, identity=self._identity,
                attributes=self._attributes, final_path=self._final_path,
            )
        except BaseException:
            _CloseHandle(handle)  # type: ignore[name-defined]
            raise

    def close(self) -> None:
        if self._closed:
            return
        if not _CloseHandle(self._handle):  # type: ignore[name-defined]
            raise OSError(f"CloseHandle failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        self._closed = True

    def discard(self) -> None:
        """Mark this exact open file object for deletion, then release it."""
        if self._closed:
            return
        disposition = _FILE_DISPOSITION_INFORMATION(1)  # type: ignore[name-defined]
        if not _SetFileInformationByHandle(  # type: ignore[name-defined]
            self._handle, _FILE_DISPOSITION_INFO, ctypes.byref(disposition),  # type: ignore[name-defined]
            ctypes.sizeof(disposition),  # type: ignore[name-defined]
        ):
            raise OSError(
                "SetFileInformationByHandle(FileDispositionInfo) failed: "
                f"{ctypes.get_last_error()}"  # type: ignore[name-defined]
            )
        self.close()


@dataclass(frozen=True)
class OutputNamespaceLease:
    """One identity-checked directory anchor in the fixed output namespace."""

    path: Path
    lease: object
    identity: object


@dataclass(frozen=True)
class OutputPublicationOperations:
    """Native publication authority, or an explicitly supplied test double.

    main never chooses a weaker path-based implementation itself. Production
    obtains this object only from _output_publication_operations_for_main,
    which requires the complete native Windows binding. Platform-independent
    transaction tests may inject a complete fake object at that one boundary.
    """

    lease_factory: Callable[[Path], object]
    open_namespace: Callable[
        [Path, Path],
        tuple[tuple[OutputNamespaceLease, ...] | None, str | None],
    ]
    validate_directory: Callable[[object], object]
    clear: Callable[..., str | None]
    write: Callable[[object, Path, str], object]
    discard: Callable[[list[object]], str | None]
    close_in_order: Callable[[list[object]], str | None]
    finalize_reserves: Callable[[list[object]], str | None]


def open_image_lease(path: Path) -> ImageLease:
    """Open a Windows image read lease that denies subsequent writes/deletes."""
    if os.name != "nt" or not WINDOWS_IMAGE_LEASE_AVAILABLE:
        raise OSError("native Windows image leases are required but unavailable")
    # Windows has read/write/delete share flags only.  Sharing read access lets
    # the loader map the image while intentionally excluding new writers and
    # replacement/deletion opens for this evidence epoch.
    handle = _CreateFileW(  # type: ignore[name-defined]
        str(path),
        _GENERIC_READ | _FILE_READ_ATTRIBUTES,
        _FILE_SHARE_READ,
        None,
        _OPEN_EXISTING,
        _FILE_ATTRIBUTE_NORMAL | _FILE_FLAG_OPEN_REPARSE_POINT | _FILE_FLAG_SEQUENTIAL_SCAN,
        None,
    )
    if handle in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
        raise OSError(f"CreateFileW could not lease {path}: {ctypes.get_last_error()}")  # type: ignore[name-defined]
    try:
        if not _SetHandleInformation(handle, _HANDLE_FLAG_INHERIT, 0):  # type: ignore[name-defined]
            raise OSError(f"SetHandleInformation failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        identity, attributes = _native_identity(handle)
        final_path = _native_final_path(handle)
        return ImageLease(path, handle, identity, attributes, final_path)
    except BaseException:
        _CloseHandle(handle)  # type: ignore[name-defined]
        raise


def open_output_directory_lease(path: Path) -> OutputDirectoryLease:
    """Open a Windows directory HANDLE used only as a rooted child authority."""
    if os.name != "nt" or not WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
        raise OSError("native Windows output-directory leases are required but unavailable")
    # Keep the conservative FILE_SHARE_READ mode, but do not treat a share
    # mode as reparse protection: FILE_WRITE_ATTRIBUTES can still authorize an
    # in-place metadata mutation. Safety comes from every child open below
    # being rooted at this specific HANDLE and using FILE_OPEN_REPARSE_POINT.
    handle = _CreateFileW(  # type: ignore[name-defined]
        str(path),
        _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES | _SYNCHRONIZE,
        _FILE_SHARE_READ,
        None,
        _OPEN_EXISTING,
        _FILE_FLAG_BACKUP_SEMANTICS | _FILE_FLAG_OPEN_REPARSE_POINT,
        None,
    )
    if handle in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
        raise OSError(f"CreateFileW could not lease output directory {path}: {ctypes.get_last_error()}")  # type: ignore[name-defined]
    try:
        if not _SetHandleInformation(handle, _HANDLE_FLAG_INHERIT, 0):  # type: ignore[name-defined]
            raise OSError(f"SetHandleInformation failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        identity, attributes = _native_identity(handle)
        final_path = _native_final_path(handle)
        return OutputDirectoryLease(path, handle, identity, attributes, final_path)
    except BaseException:
        _CloseHandle(handle)  # type: ignore[name-defined]
        raise


def _ntstatus_text(status: int) -> str:
    """Format an NTSTATUS without incorrectly consulting GetLastError."""
    return f"0x{int(status) & 0xFFFFFFFF:08X}"


def _native_relative_output_name(
    directory: OutputDirectoryLease, path: Path, allowed_leaves: frozenset[str],
) -> str:
    """Bind an NT-rooted operation to one exact direct child of a held directory."""
    if os.name != "nt" or not WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
        raise OSError("native Windows output operations are required but unavailable")
    if not isinstance(directory, OutputDirectoryLease) or directory.closed:
        raise OSError("lifecycle output directory is not held by a live native lease")
    expected_parent = _absolute_raw(directory.path)
    if _absolute_raw(path.parent) != expected_parent:
        raise OSError("lifecycle output operation escaped its held directory")
    leaf_name = path.name
    if leaf_name not in allowed_leaves:
        raise OSError("lifecycle output operation used a non-fixed namespace leaf")
    if any(separator in leaf_name for separator in ("\\", "/")) or leaf_name in {".", ".."}:
        raise OSError("lifecycle output operation used an unsafe relative leaf")
    return leaf_name


def _nt_create_file_relative(
    directory: OutputDirectoryLease, leaf_name: str, desired_access: int,
    create_disposition: int, file_attributes: int, create_options: int,
    *, missing_ok: bool = False,
) -> int | None:
    """Open one child through a held directory HANDLE, never through its pathname."""
    name_bytes = leaf_name.encode("utf-16-le")
    if not name_bytes or len(name_bytes) > 0xFFFF:
        raise OSError("lifecycle output relative leaf is not representable as UNICODE_STRING")
    buffer = ctypes.create_unicode_buffer(leaf_name)  # type: ignore[name-defined]
    object_name = _UNICODE_STRING(  # type: ignore[name-defined]
        len(name_bytes), len(name_bytes),
        ctypes.cast(buffer, wintypes.LPWSTR),  # type: ignore[name-defined]
    )
    attributes = _OBJECT_ATTRIBUTES(  # type: ignore[name-defined]
        ctypes.sizeof(_OBJECT_ATTRIBUTES), directory._handle, ctypes.pointer(object_name),  # type: ignore[name-defined]
        _OBJ_CASE_INSENSITIVE, None, None,
    )
    io_status = _IO_STATUS_BLOCK()  # type: ignore[name-defined]
    handle = wintypes.HANDLE()  # type: ignore[name-defined]
    status = _NtCreateFile(  # type: ignore[name-defined]
        ctypes.byref(handle),
        desired_access | _SYNCHRONIZE,
        ctypes.byref(attributes),
        ctypes.byref(io_status),
        None,
        file_attributes,
        _FILE_SHARE_READ,
        create_disposition,
        create_options | _FILE_SYNCHRONOUS_IO_NONALERT | _FILE_FLAG_OPEN_REPARSE_POINT,
        None,
        0,
    )
    status_value = int(status)
    if status_value < 0:
        if missing_ok and (status_value & 0xFFFFFFFF) in {
            _STATUS_OBJECT_NAME_NOT_FOUND, _STATUS_OBJECT_PATH_NOT_FOUND,
        }:
            return None
        raise OSError(
            f"NtCreateFile could not open lifecycle output child {leaf_name!r}: "
            f"{_ntstatus_text(status_value)}"
        )
    raw_handle = handle.value
    if raw_handle in (None, _INVALID_HANDLE_VALUE):  # type: ignore[name-defined]
        raise OSError("NtCreateFile returned an invalid lifecycle output handle")
    try:
        if not _SetHandleInformation(raw_handle, _HANDLE_FLAG_INHERIT, 0):  # type: ignore[name-defined]
            raise OSError(f"SetHandleInformation failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
    except BaseException:
        _CloseHandle(raw_handle)  # type: ignore[name-defined]
        raise
    return raw_handle


def _open_relative_output_directory(
    parent: OutputDirectoryLease, path: Path, component: str,
) -> OutputDirectoryLease:
    """Open or create one fixed output ancestor through its held parent HANDLE."""
    leaf_name = _native_relative_output_name(parent, path, frozenset({component}))
    handle = _nt_create_file_relative(
        parent, leaf_name,
        _FILE_LIST_DIRECTORY | _FILE_READ_ATTRIBUTES,
        _FILE_OPEN_IF,
        _FILE_ATTRIBUTE_DIRECTORY,
        _FILE_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
    )
    assert handle is not None
    try:
        identity, attributes = _native_identity(handle)
        final_path = _native_final_path(handle)
        return OutputDirectoryLease(path, handle, identity, attributes, final_path)
    except BaseException:
        _CloseHandle(handle)  # type: ignore[name-defined]
        raise


def _validate_image_manifest_document(
    document: object, commit_sha: str,
) -> tuple[dict[str, str] | None, str | None]:
    """Validate the narrowly-scoped upstream Release image manifest object."""
    if not isinstance(document, dict) or set(document) != {"schemaVersion", "commitSHA", "images"}:
        return None, "image manifest has an invalid top-level schema"
    if document["schemaVersion"] != IMAGE_MANIFEST_SCHEMA or document["commitSHA"] != commit_sha:
        return None, "image manifest schemaVersion or commitSHA does not match this collection"
    images = document["images"]
    if not isinstance(images, list) or len(images) != 2:
        return None, "image manifest must declare exactly two images"
    expected = {"SparkEngine.exe", "SparkGameFPS.dll"}
    values: dict[str, str] = {}
    for image in images:
        if not isinstance(image, dict) or set(image) != {"path", "sha256"}:
            return None, "image manifest image schema is invalid"
        artifact_path, digest = image["path"], image["sha256"]
        if not isinstance(artifact_path, str) or artifact_path not in expected or \
           Path(artifact_path).name != artifact_path or not isinstance(digest, str) or \
           not lifecycle_mod.ENGINE_SHA256_RE.fullmatch(digest) or artifact_path in values:
            return None, "image manifest image path or SHA-256 is invalid"
        values[artifact_path] = digest
    if set(values) != expected:
        return None, "image manifest must declare both required stable-v1 images"
    return values, None


def validate_engine_binary(engine: Path) -> str | None:
    """Return an error string if `engine` is not a plausible SparkEngine binary."""
    if not engine.is_file():
        return (
            f"engine executable not found at {engine} — lifecycle evidence "
            f"requires a real build; there is nothing to run"
        )
    if paths_mod._is_reparse_point(engine):
        return (
            f"engine path {engine} is a symlink, junction, or reparse point — "
            f"lifecycle evidence must be produced by a real engine binary, not "
            f"an indirection that can point anywhere"
        )
    if engine.suffix.lower() in _SCRIPT_EXTENSIONS:
        return (
            f"engine path {engine.name!r} is a script ({engine.suffix}) — "
            f"lifecycle evidence requires a compiled engine binary, not a "
            f"script that can print arbitrary lifecycle markers"
        )
    if engine.name != _ENGINE_NAME:
        return (
            f"engine filename {engine.name!r} is not the required stable-v1 "
            f"engine {_ENGINE_NAME!r}"
        )
    try:
        size = engine.stat().st_size
    except OSError as exc:
        return f"cannot stat engine binary {engine}: {exc}"
    if size < _MIN_ENGINE_SIZE:
        return (
            f"engine binary {engine} is only {size} bytes — a compiled engine "
            f"executable is orders of magnitude larger; this looks like a stub "
            f"or script masquerading as a binary"
        )
    try:
        with open(engine, "rb") as f:
            header = f.read(64)
    except OSError as exc:
        return f"cannot read engine binary header: {exc}"
    for sig in _SCRIPT_SIGNATURES:
        if header.lstrip().startswith(sig):
            return (
                f"engine binary {engine} starts with script signature "
                f"{sig!r} — lifecycle evidence requires a compiled executable"
            )
    if not _is_pe_image(header, engine):
        return f"engine binary {engine} is not a valid PE image"
    return None


_TERMINAL_COUNTS = (
    ("create", "CreateModule"), ("load", "OnLoad"), ("update", "OnUpdate"),
    ("fixed", "OnFixedUpdate"), ("render", "OnRender"),
    ("unload", "OnUnload"), ("destroy", "DestroyModule"),
)


def _is_pe_image(header: bytes, path: Path) -> bool:
    """Check the inexpensive PE identity properties needed before launch."""
    if len(header) < 64 or header[:2] != b"MZ":
        return False
    pe_offset = int.from_bytes(header[0x3C:0x40], "little")
    try:
        with path.open("rb") as image:
            image.seek(pe_offset)
            return image.read(4) == b"PE\0\0"
    except OSError:
        return False


def _display_windows_final_path(value: str | Path) -> str:
    """Normalize a handle-derived path for publication without changing its casing."""
    path = str(value)
    if path[:8].casefold() == "\\\\?\\unc\\":
        path = "\\\\" + path[8:]
    elif path[:4].casefold() == "\\\\?\\":
        path = path[4:]
    return ntpath.normpath(path)


def _normalise_windows_final_path(value: str | Path) -> str:
    """Case-fold a handle-derived or canonical path for security comparisons only."""
    return ntpath.normcase(_display_windows_final_path(value))


def _validate_leased_output_directory(
    lease: object, expected_path: Path, *, expected_identity: object | None = None,
) -> tuple[object | None, str | None]:
    """Require a live lease for the exact fixed publication directory."""
    expected = _absolute_raw(expected_path)
    try:
        identity, attributes = _lease_snapshot(lease)
        final_path = _lease_final_path(lease)
    except OSError as exc:
        return None, f"cannot inspect lifecycle output-directory lease: {exc}"
    if attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
        return None, "lifecycle output-directory lease refers to a reparse point"
    if not attributes & _FILE_ATTRIBUTE_DIRECTORY:
        return None, "lifecycle output-directory lease does not refer to a directory"
    if _normalise_windows_final_path(final_path) != _normalise_windows_final_path(expected):
        return None, "lifecycle output-directory handle path does not match the fixed namespace"
    expected_leaf = ntpath.basename(_display_windows_final_path(expected))
    if ntpath.basename(_display_windows_final_path(final_path)) != expected_leaf:
        return None, "lifecycle output-directory handle leaf is not the fixed namespace"
    if expected_identity is not None and not _same_file_identity(identity, expected_identity):
        return None, "lifecycle output-directory handle changed identity while held"
    return identity, None


def _lease_snapshot(lease: object) -> tuple[object, int]:
    """Read image metadata from an injected or native lease without paths."""
    snapshot = getattr(lease, "snapshot", None)
    if callable(snapshot):
        value = snapshot()
        if not isinstance(value, tuple) or len(value) != 2:
            raise OSError("image lease returned malformed handle metadata")
        identity, attributes = value
    else:
        identity = getattr(lease, "identity", None)
        attributes = getattr(lease, "attributes", None)
    if identity is None or not isinstance(attributes, int):
        raise OSError("image lease cannot report native metadata")
    for field in ("volume_serial", "file_index", "size", "last_write_time"):
        if not isinstance(getattr(identity, field, None), int):
            raise OSError("image lease returned malformed file identity")
    return identity, attributes


def _lease_final_path(lease: object) -> str:
    final_path = getattr(lease, "final_path", None)
    if not isinstance(final_path, str) or not final_path:
        raise OSError("image lease cannot report its final handle path")
    return final_path


def _lease_read_at(lease: object, offset: int, size: int) -> bytes:
    reader = getattr(lease, "read_at", None)
    if not callable(reader):
        raise OSError("image lease cannot read through its live handle")
    value = reader(offset, size)
    if not isinstance(value, bytes):
        raise OSError("image lease returned a non-bytes image read")
    return value


def _lease_sha256(lease: object) -> str:
    hasher = getattr(lease, "sha256", None)
    if not callable(hasher):
        raise OSError("image lease cannot hash through its live handle")
    before, _ = _lease_snapshot(lease)
    digest = hasher()
    after, _ = _lease_snapshot(lease)
    if before != after:
        raise OSError("image changed while it was hashed through its live lease")
    if not isinstance(digest, str) or not lifecycle_mod.ENGINE_SHA256_RE.fullmatch(digest):
        raise OSError("image lease returned an invalid SHA-256 digest")
    return digest


def _validate_leased_manifest(lease: object, expected_path: Path,
                              root: Path) -> str | None:
    """Require the fixed manifest to be the regular file behind this lease."""
    expected = _absolute_raw(root / IMAGE_MANIFEST_FILENAME)
    if expected_path != expected:
        return "image manifest path is not the fixed artifact-root filename"
    try:
        _identity, attributes = _lease_snapshot(lease)
        final_path = _lease_final_path(lease)
    except OSError as exc:
        return f"cannot inspect image manifest lease: {exc}"
    if attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
        return "image manifest lease refers to a reparse point"
    if attributes & _FILE_ATTRIBUTE_DIRECTORY:
        return "image manifest lease refers to a directory"
    if _normalise_windows_final_path(final_path) != _normalise_windows_final_path(expected):
        return "image manifest handle path does not match the fixed artifact-root file"
    if Path(final_path).name.casefold() != IMAGE_MANIFEST_FILENAME.casefold():
        return "image manifest handle filename is not the required fixed filename"
    return None


def _read_manifest_text_from_lease(lease: object, origin: str) -> tuple[str | None, str | None]:
    """Read a bounded UTF-8 manifest from its held handle, never its pathname."""
    try:
        before, _attributes = _lease_snapshot(lease)
        size = getattr(before, "size")
        if size > strict_json.DEFAULT_LIMITS.document_bytes:
            return None, (
                f"image manifest is {size} bytes, limit is "
                f"{strict_json.DEFAULT_LIMITS.document_bytes}"
            )
        chunks: list[bytes] = []
        offset = 0
        while offset < size:
            chunk = _lease_read_at(lease, offset, min(1 << 16, size - offset))
            if not chunk:
                return None, "image manifest lease reached EOF before its recorded size"
            if len(chunk) > size - offset:
                return None, "image manifest lease returned bytes beyond its recorded size"
            chunks.append(chunk)
            offset += len(chunk)
        after, _attributes = _lease_snapshot(lease)
        if after != before:
            return None, "image manifest changed while it was read through its live lease"
    except OSError as exc:
        return None, f"cannot read image manifest through its live lease: {exc}"
    try:
        return b"".join(chunks).decode("utf-8"), None
    except UnicodeDecodeError as exc:
        return None, f"image manifest is not valid UTF-8: {exc}"


def load_image_manifest_from_lease(
    lease: object, path: Path, root: Path, commit_sha: str,
) -> tuple[dict[str, str] | None, str | None]:
    """Parse the fixed manifest bytes from a live, non-replaceable file lease."""
    error = _validate_leased_manifest(lease, path, root)
    if error:
        return None, error
    final_path = _lease_final_path(lease)
    text, error = _read_manifest_text_from_lease(lease, final_path)
    if error:
        return None, error
    assert text is not None
    try:
        document = strict_json.loads(text, origin=final_path)
    except strict_json.StrictJSONError as exc:
        return None, f"image manifest is unusable: {exc}"
    return _validate_image_manifest_document(document, commit_sha)


def _same_file_identity(left: object, right: object) -> bool:
    return (
        getattr(left, "volume_serial", None), getattr(left, "file_index", None)
    ) == (
        getattr(right, "volume_serial", None), getattr(right, "file_index", None)
    )


def _lease_is_pe_image(lease: object, header: bytes, size: int) -> bool:
    if len(header) < 64 or header[:2] != b"MZ":
        return False
    pe_offset = int.from_bytes(header[0x3C:0x40], "little")
    if pe_offset < 0 or pe_offset + 4 > size:
        return False
    try:
        return _lease_read_at(lease, pe_offset, 4) == b"PE\0\0"
    except OSError:
        return False


def _validate_leased_image(lease: object, expected_path: Path, *, role: str,
                           expected_name: str, min_size: int = 0) -> str | None:
    """Validate type, final path, and PE header through a held native handle."""
    try:
        identity, attributes = _lease_snapshot(lease)
        final_path = _lease_final_path(lease)
    except OSError as exc:
        return f"cannot inspect {role} image lease: {exc}"
    if attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
        return f"{role} image lease refers to a reparse point"
    if attributes & _FILE_ATTRIBUTE_DIRECTORY:
        return f"{role} image lease refers to a directory"
    if getattr(identity, "size") < min_size:
        return f"{role} image lease is smaller than the required compiled image size"
    if _normalise_windows_final_path(final_path) != _normalise_windows_final_path(expected_path):
        return f"{role} image handle path does not match the canonical expected image"
    if Path(final_path).name.casefold() != expected_name.casefold():
        return f"{role} image handle filename is not the required {expected_name!r}"
    try:
        header = _lease_read_at(lease, 0, 64)
    except OSError as exc:
        return f"cannot read {role} image through its lease: {exc}"
    for signature in _SCRIPT_SIGNATURES:
        if header.lstrip().startswith(signature):
            return f"{role} image lease starts with a script signature"
    if not _lease_is_pe_image(lease, header, getattr(identity, "size")):
        return f"{role} image lease is not a valid PE image"
    return None


def _verify_leased_image(lease: object, expected_path: Path, *, role: str,
                         expected_name: str, expected_digest: str,
                         min_size: int = 0) -> tuple[ImageVerification | None, str | None]:
    """Return one handle-derived, manifest-matched image verification."""
    error = _validate_leased_image(
        lease, expected_path, role=role, expected_name=expected_name,
        min_size=min_size,
    )
    if error:
        return None, error
    try:
        digest = _lease_sha256(lease)
        identity, attributes = _lease_snapshot(lease)
        final_path = _lease_final_path(lease)
    except OSError as exc:
        return None, f"cannot hash {role} image through its lease: {exc}"
    if digest != expected_digest:
        return None, f"{role} image digest does not match the trusted image manifest"
    return ImageVerification(
        digest=digest, final_path=final_path, identity=identity,
        attributes=attributes,
    ), None


def _absolute_raw(path: Path) -> Path:
    return Path(os.path.abspath(path))


def _windows_path_alias_error(value: str | Path, label: str) -> str | None:
    """Reject lexical Windows aliases before pathlib or Win32 can normalize them."""
    raw = os.fspath(value)
    if not isinstance(raw, str) or not raw:
        return f"{label} must be a non-empty path"
    if "\0" in raw:
        return f"{label} contains a NUL byte"
    drive_seen = False
    for segment in re.split(r"[\\/]+", raw):
        if not segment:
            continue
        if not drive_seen and re.fullmatch(r"[A-Za-z]:", segment):
            drive_seen = True
            continue
        if segment in {".", ".."}:
            return f"{label} contains a traversal or dot segment"
        if ":" in segment:
            return f"{label} contains a drive-relative or alternate-data-stream alias"
        if segment.endswith((".", " ")):
            return f"{label} contains a trailing-dot or trailing-space alias"
        if any(character in segment for character in '<>"|?*'):
            return f"{label} contains a Windows device or wildcard alias"
        stem = segment.split(".", 1)[0].casefold()
        if stem in _WINDOWS_RESERVED_NAMES:
            return f"{label} contains the reserved Windows device name {segment!r}"
    return None


def _require_exact_nonreparse_directory(
    parent: Path, name: str, *, create_if_missing: bool,
) -> tuple[Path | None, str | None]:
    """Walk one fixed namespace segment without case or reparse aliases."""
    candidate = parent / name
    try:
        entries = {entry.name for entry in os.scandir(parent)}
    except OSError as exc:
        return None, f"cannot enumerate required evidence namespace parent {parent}: {exc}"
    if name not in entries:
        aliases = [entry for entry in entries if entry.casefold() == name.casefold()]
        if aliases:
            return None, (
                f"required evidence namespace segment {name!r} is case-aliased "
                f"by {aliases[0]!r}"
            )
        if create_if_missing:
            try:
                candidate.mkdir()
            except FileExistsError:
                # A competing creator is safe only if the fresh directory
                # check below accepts the exact non-reparse entry.
                pass
            except OSError as exc:
                return None, f"cannot create required evidence namespace directory {candidate}: {exc}"
            return _require_exact_nonreparse_directory(
                parent, name, create_if_missing=False,
            )
        return None, f"required evidence namespace directory {candidate} does not exist"
    if paths_mod._is_reparse_point(candidate) or not candidate.is_dir():
        return None, f"required evidence namespace directory {candidate} is not a real non-reparse directory"
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as exc:
        return None, f"cannot resolve required evidence namespace directory {candidate}: {exc}"
    if _normalise_windows_final_path(resolved) != _normalise_windows_final_path(candidate):
        return None, f"required evidence namespace directory {candidate} resolves through an alias"
    return candidate, None


def _validate_output_leaf(path: Path) -> str | None:
    """Ensure a fixed output leaf cannot be a directory, reparse point, or device."""
    if paths_mod._is_reparse_point(path):
        return f"lifecycle output {path} is a reparse point"
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return None
    except OSError as exc:
        return f"cannot inspect lifecycle output {path}: {exc}"
    if not stat.S_ISREG(mode):
        return f"lifecycle output {path} is not a regular file"
    return None


def _same_output_identity(left: Path, right: Path) -> bool:
    """Detect both lexical aliases and pre-existing hard-link collisions."""
    if _normalise_windows_final_path(left) == _normalise_windows_final_path(right):
        return True
    try:
        return left.exists() and right.exists() and os.path.samefile(left, right)
    except OSError:
        return False


def _validate_output_namespace(raw_output: str, repo_root: Path) -> tuple[tuple[Path, Path] | None, str | None]:
    """Authorize the one fixed, non-reparse release-evidence publication pair."""
    alias_error = _windows_path_alias_error(raw_output, "--out")
    if alias_error:
        return None, alias_error
    root = _absolute_raw(repo_root)
    output_directory = root.joinpath(*LIFECYCLE_OUTPUT_DIRECTORY)
    output = output_directory / LIFECYCLE_OUTPUT_FILENAME
    log = output_directory / LIFECYCLE_AUDIT_LOG_FILENAME
    # Do not accept relative, case-folded, short-name, dot, ADS, or any other
    # spelling.  The producer clears files only after this exact comparison.
    if raw_output != str(output):
        return None, (
            "--out must exactly name the fixed release evidence file "
            f"{output}"
        )
    if paths_mod._is_reparse_point(root) or not root.is_dir():
        return None, f"repository root {root} must be a real non-reparse directory"
    try:
        resolved_root = root.resolve(strict=True)
    except OSError as exc:
        return None, f"cannot resolve repository root for lifecycle evidence: {exc}"
    if _normalise_windows_final_path(resolved_root) != _normalise_windows_final_path(root):
        return None, "repository root for lifecycle evidence resolves through an alias"
    # Do not inspect, create, or resolve descendants by their paths here. The
    # production namespace walk opens the root once and then acquires build and
    # module-evidence relative to those held parent handles, so a concurrent
    # reparse change cannot redirect a pre-lease mkdir or a later acquisition.
    if _same_output_identity(output, log):
        return None, "lifecycle JSON and audit-log paths must be distinct"
    for path in (output, log):
        error = _validate_output_leaf(path)
        if error:
            return None, error
    return (output, log), None


def _output_collides_with_input(output: Path, log: Path,
                                inputs: tuple[tuple[str, Path], ...]) -> str | None:
    for label, path in inputs:
        if _same_output_identity(output, path) or _same_output_identity(log, path):
            return f"fixed lifecycle output collides with the {label} input path"
    return None


def _open_output_namespace_leases(
    repo_root: Path, output_directory: Path,
    *, lease_factory: Callable[[Path], object] | None = None,
) -> tuple[tuple[OutputNamespaceLease, ...] | None, str | None]:
    """Lease every mutable output anchor, rooted from one absolute repository open.

    A supplied factory is an explicit test seam only. Production opens the
    repository root once by absolute path, then opens or creates build and
    module-evidence relative to the already-held parent HANDLE. That prevents a
    post-root-open reparse replacement from redirecting a descendant acquisition
    before its own handle exists.
    """
    root = _absolute_raw(repo_root)
    expected_paths = (
        root,
        root / LIFECYCLE_OUTPUT_DIRECTORY[0],
        root.joinpath(*LIFECYCLE_OUTPUT_DIRECTORY),
    )
    if output_directory != expected_paths[-1]:
        return None, "lifecycle output directory is not the fixed namespace anchor"
    acquired: list[OutputNamespaceLease] = []
    opened: list[object] = []
    try:
        if lease_factory is not None:
            # Test doubles deliberately provide all three anchors. This path
            # is reachable only through OutputPublicationOperations injection;
            # the production provider always follows the native relative walk.
            for path in expected_paths:
                lease = lease_factory(path)
                opened.append(lease)
                identity, error = _validate_leased_output_directory(lease, path)
                if error:
                    raise OSError(error)
                assert identity is not None
                acquired.append(OutputNamespaceLease(path, lease, identity))
        else:
            root_lease = open_output_directory_lease(expected_paths[0])
            opened.append(root_lease)
            root_identity, error = _validate_leased_output_directory(
                root_lease, expected_paths[0],
            )
            if error:
                raise OSError(error)
            assert root_identity is not None
            acquired.append(OutputNamespaceLease(expected_paths[0], root_lease, root_identity))

            build_lease = _open_relative_output_directory(
                root_lease, expected_paths[1], LIFECYCLE_OUTPUT_DIRECTORY[0],
            )
            opened.append(build_lease)
            build_identity, error = _validate_leased_output_directory(
                build_lease, expected_paths[1],
            )
            if error:
                raise OSError(error)
            assert build_identity is not None
            acquired.append(OutputNamespaceLease(expected_paths[1], build_lease, build_identity))

            output_lease = _open_relative_output_directory(
                build_lease, expected_paths[2], LIFECYCLE_OUTPUT_DIRECTORY[1],
            )
            opened.append(output_lease)
            output_identity, error = _validate_leased_output_directory(
                output_lease, expected_paths[2],
            )
            if error:
                raise OSError(error)
            assert output_identity is not None
            acquired.append(OutputNamespaceLease(expected_paths[2], output_lease, output_identity))
    except OSError as exc:
        close_error = _close_image_leases(opened)
        suffix = f"; cannot close partial output namespace leases: {close_error}" if close_error else ""
        return None, f"cannot acquire lifecycle output namespace leases: {exc}{suffix}"
    return tuple(acquired), None


def _validate_output_pair_under_namespace_leases(
    leases: tuple[OutputNamespaceLease, ...], output: Path, log: Path,
) -> str | None:
    """Validate the fixed pair while each ancestor directory remains leased."""
    expected_paths = (output.parent.parent.parent, output.parent.parent, output.parent)
    if len(leases) != len(expected_paths):
        return "lifecycle output namespace lease chain is incomplete"
    for anchor, expected_path in zip(leases, expected_paths, strict=True):
        if anchor.path != expected_path:
            return "lifecycle output namespace lease chain does not match the fixed path"
        _identity, error = _validate_leased_output_directory(
            anchor.lease, expected_path, expected_identity=anchor.identity,
        )
        if error:
            return error
    if output.parent != leases[-1].path or log.parent != leases[-1].path:
        return "lifecycle output pair escaped the held fixed namespace"
    if output.name != LIFECYCLE_OUTPUT_FILENAME or \
       log.name != LIFECYCLE_AUDIT_LOG_FILENAME:
        return "lifecycle output pair does not use the fixed artifact leaves"
    if output == log:
        return "lifecycle JSON and audit-log paths must be distinct"
    return None


def _canonicalize_images(engine: Path, module_image: Path,
                         working_directory: Path) -> tuple[tuple[Path, Path, Path] | None, str | None]:
    """Reject raw reparse paths before producing stable canonical identities."""
    root_raw = _absolute_raw(working_directory)
    images_raw = (("engine", _absolute_raw(engine)), ("module", _absolute_raw(module_image)))
    if not root_raw.is_dir() or paths_mod._is_reparse_point(root_raw):
        return None, f"working directory {root_raw} must be a real non-reparse directory"
    anchor = Path(root_raw.anchor)
    current = anchor
    for component in root_raw.relative_to(anchor).parts:
        current /= component
        if paths_mod._is_reparse_point(current):
            return None, f"working directory ancestor {current} is a reparse point"
    for role, raw_image in images_raw:
        try:
            relative = raw_image.relative_to(root_raw)
        except ValueError:
            return None, f"{role} image {raw_image} lies outside working directory {root_raw}"
        current = root_raw
        if paths_mod._is_reparse_point(current):
            return None, f"working directory {current} is a reparse point"
        for component in relative.parts:
            current /= component
            if paths_mod._is_reparse_point(current):
                return None, f"{role} image component {current} is a reparse point"
    try:
        root = root_raw.resolve(strict=True)
        canonical_engine = images_raw[0][1].resolve(strict=True)
        canonical_module = images_raw[1][1].resolve(strict=True)
    except OSError as exc:
        return None, f"cannot resolve lifecycle image paths: {exc}"
    if root not in canonical_engine.parents or root not in canonical_module.parents:
        return None, "canonical lifecycle image path escaped the working directory"
    return (root, canonical_engine, canonical_module), None


def _canonical_manifest(path: str | Path, root: Path) -> tuple[Path | None, str | None]:
    alias_error = _windows_path_alias_error(path, "image manifest path")
    if alias_error:
        return None, alias_error
    raw_value = os.fspath(path)
    raw = _absolute_raw(Path(raw_value))
    expected_raw = _absolute_raw(root / IMAGE_MANIFEST_FILENAME)
    if raw_value != str(expected_raw) or raw != expected_raw:
        return None, (
            "image manifest must be the fixed artifact-root file "
            f"{IMAGE_MANIFEST_FILENAME!r}"
        )
    if paths_mod._is_reparse_point(raw):
        return None, "image manifest must not be a reparse point"
    # Do not read or resolve the leaf by pathname here.  The native held lease
    # below is the authority for its type, final path, and bytes.
    return expected_raw, None


def resolve_collection_sha(requested_sha: str | None) -> tuple[str | None, str | None]:
    """Bind evidence to checkout HEAD; CLI input cannot invent a revision."""
    head_sha, error = resolve_head_sha(REPO_ROOT)
    if head_sha is None:
        return None, error
    if requested_sha is not None and requested_sha != head_sha:
        return None, "--commit-sha must exactly equal the checked-out HEAD"
    github_sha = os.environ.get("GITHUB_SHA")
    if github_sha is not None and github_sha != head_sha:
        return None, "GITHUB_SHA must exactly equal the checked-out HEAD"
    return head_sha, None


def _validate_image(path: Path, root: Path, *, role: str,
                    expected_name: str | None = None) -> str | None:
    """Reject an image that is not a real, in-package PE file."""
    if root not in path.parents:
        return f"{role} image {path} lies outside working directory {root}"
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        return f"cannot stat {role} image {path}: {exc}"
    if not stat.S_ISREG(mode) or paths_mod._is_reparse_point(path):
        return f"{role} image {path} must be a regular non-reparse file"
    if expected_name is not None and path.name != expected_name:
        return f"{role} image {path.name!r} must be the Windows module {expected_name!r}"
    if path.suffix.lower() in _SCRIPT_EXTENSIONS:
        return f"{role} image {path} is a script, not a compiled binary"
    try:
        with path.open("rb") as image:
            header = image.read(64)
    except OSError as exc:
        return f"cannot read {role} image {path}: {exc}"
    if not _is_pe_image(header, path):
        return f"{role} image {path} is not a PE binary"
    return None


def validate_image_pair(engine: Path, module_image: Path, module: str,
                        working_directory: Path) -> str | None:
    """Validate the exact engine/module image pair that will be launched."""
    prepared, error = _canonicalize_images(engine, module_image, working_directory)
    if error:
        return error
    assert prepared is not None
    root, canonical_engine, canonical_module = prepared
    error = validate_engine_binary(canonical_engine)
    if error:
        return error
    return _validate_image(
        canonical_module, root, role="module",
        expected_name=expected_library_names(module)["windows"],
    )


def parse_terminal_record(text: str, module: str) -> dict[str, int]:
    """Parse exactly one direct, standalone host lifecycle record."""
    fields = " ".join(
        f"{name}=(?P<{name}>[0-9]+)" for name, _ in _TERMINAL_COUNTS
    ) + " faults=(?P<faults>[0-9]+)"
    record_re = re.compile(
        rf"^SPARK_MODULE_LIFECYCLE module={re.escape(module)} {fields}$"
    )
    matching = [line for line in text.splitlines() if LIFECYCLE_TOKEN in line]
    if len(matching) != 1:
        raise ValueError(f"expected exactly one standalone lifecycle record for {module}")
    match = record_re.fullmatch(matching[0])
    if match is None:
        raise ValueError(f"malformed, wrong-module, or unknown-key lifecycle record for {module}")
    values = {name: int(match.group(name)) for name, _ in _TERMINAL_COUNTS}
    faults = int(match.group("faults"))
    if any(value == 0 for value in values.values()) or faults != 0:
        raise ValueError(f"incomplete or faulted lifecycle record for {module}")
    return {phase: values[name] for name, phase in _TERMINAL_COUNTS}


def parse_terminal_streams(stdout: str, stderr: str, module: str) -> dict[str, int]:
    """Admit one direct host record only from stdout; stderr is never evidence."""
    if LIFECYCLE_TOKEN in stderr:
        raise ValueError("lifecycle marker appeared on stderr")
    return parse_terminal_record(stdout, module)


def _close_image_leases(leases: list[object]) -> str | None:
    """Close every acquired lease, even if a preceding close fails."""
    errors: list[str] = []
    for lease in reversed(leases):
        closer = getattr(lease, "close", None)
        if not callable(closer):
            errors.append("image lease has no close operation")
            continue
        try:
            closer()
        except Exception as exc:
            errors.append(str(exc))
    return "; ".join(errors) if errors else None


def _native_output_leaf_name(directory: OutputDirectoryLease, path: Path) -> str:
    """Bind a native output operation to one fixed leaf below its held directory."""
    return _native_relative_output_name(
        directory, path,
        frozenset({LIFECYCLE_OUTPUT_FILENAME, LIFECYCLE_AUDIT_LOG_FILENAME}),
    )


def _native_handle_matches_output_path(handle: int, expected_path: Path) -> None:
    """Reject a file handle that does not still name the intended fixed leaf."""
    _identity, attributes = _native_identity(handle)
    if attributes & _FILE_ATTRIBUTE_REPARSE_POINT:
        raise OSError("lifecycle output artifact is a reparse point")
    if attributes & _FILE_ATTRIBUTE_DIRECTORY:
        raise OSError("lifecycle output artifact is a directory")
    final_path = _native_final_path(handle)
    expected = _absolute_raw(expected_path)
    if _normalise_windows_final_path(final_path) != _normalise_windows_final_path(expected):
        raise OSError("lifecycle output handle path does not match the fixed artifact leaf")
    expected_leaf = ntpath.basename(_display_windows_final_path(expected))
    if ntpath.basename(_display_windows_final_path(final_path)) != expected_leaf:
        raise OSError("lifecycle output handle leaf is not the fixed artifact leaf")


def _open_existing_output_artifact(
    directory: OutputDirectoryLease, path: Path,
) -> OutputArtifactHandle | None:
    """Open one existing fixed output leaf without traversing a leaf reparse point."""
    leaf_name = _native_output_leaf_name(directory, path)
    handle = _nt_create_file_relative(
        directory, leaf_name,
        _GENERIC_READ | _FILE_READ_ATTRIBUTES | _DELETE,
        _FILE_OPEN,
        _FILE_ATTRIBUTE_NORMAL,
        _FILE_NON_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
        missing_ok=True,
    )
    if handle is None:
        return None
    try:
        identity, attributes = _native_identity(handle)
        _native_handle_matches_output_path(handle, path)
        final_path = _native_final_path(handle)
        return OutputArtifactHandle(
            handle, directory, leaf_name, identity=identity,
            attributes=attributes, final_path=final_path,
        )
    except BaseException:
        _CloseHandle(handle)  # type: ignore[name-defined]
        raise


def _clear_output_artifacts(directory: OutputDirectoryLease, *paths: Path) -> str | None:
    """Delete fixed stale outputs through leaf handles, never Path.unlink()."""
    errors: list[str] = []
    seen: set[str] = set()
    for path in paths:
        try:
            leaf_name = _native_output_leaf_name(directory, path)
        except OSError as exc:
            errors.append(str(exc))
            continue
        if leaf_name in seen:
            continue
        seen.add(leaf_name)
        artifact: OutputArtifactHandle | None = None
        try:
            artifact = _open_existing_output_artifact(directory, path)
            if artifact is not None:
                artifact.discard()
        except OSError as exc:
            errors.append(f"cannot clear lifecycle artifact {path}: {exc}")
        finally:
            if artifact is not None and not artifact.closed:
                try:
                    artifact.close()
                except OSError as exc:
                    errors.append(f"cannot close lifecycle artifact {path}: {exc}")
    return "; ".join(errors) if errors else None


def _write_output_bytes(handle: int, content: str) -> None:
    """Write and flush UTF-8 output through a native file handle."""
    payload = content.encode("utf-8")
    for offset in range(0, len(payload), 64 * 1024):
        chunk = payload[offset:offset + 64 * 1024]
        buffer = ctypes.create_string_buffer(chunk)  # type: ignore[name-defined]
        written = wintypes.DWORD()  # type: ignore[name-defined]
        if not _WriteFile(  # type: ignore[name-defined]
            handle, buffer, len(chunk), ctypes.byref(written), None,  # type: ignore[name-defined]
        ):
            raise OSError(f"WriteFile failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]
        if written.value != len(chunk):
            raise OSError("WriteFile performed a partial lifecycle output write")
    if not _FlushFileBuffers(handle):  # type: ignore[name-defined]
        raise OSError(f"FlushFileBuffers failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]


def _write_output_artifact(
    directory: OutputDirectoryLease, final_path: Path, content: str,
) -> OutputArtifactHandle:
    """Create one fixed final leaf, flush it, close its writer, then retain deletion authority.

    This intentionally does not stage/rename. The directory HANDLE, rather
    than its mutable pathname, is the RootDirectory for the FILE_CREATE open;
    FILE_OPEN_REPARSE_POINT prevents a fixed leaf reparse point from being
    followed, while FILE_CREATE prevents an existing final leaf from being
    overwritten. The audit log is called first by
    :func:`main`; only a fully written and closed audit log permits creation of
    the authoritative JSON leaf.
    """
    leaf_name = _native_output_leaf_name(directory, final_path)
    content.encode("utf-8")  # Fail before CREATE_NEW if the payload is invalid.
    handle = _nt_create_file_relative(
        directory, leaf_name,
        _GENERIC_READ | _GENERIC_WRITE | _FILE_READ_ATTRIBUTES | _DELETE,
        _FILE_CREATE,
        _FILE_ATTRIBUTE_NORMAL,
        _FILE_NON_DIRECTORY_FILE | _FILE_FLAG_OPEN_REPARSE_POINT,
    )
    assert handle is not None
    writer = OutputArtifactHandle(handle, directory, leaf_name)
    guard: OutputArtifactHandle | None = None
    try:
        _native_handle_matches_output_path(handle, final_path)
        _write_output_bytes(handle, content)
        _native_handle_matches_output_path(handle, final_path)
        # Duplicate the exact file object before closing its writer.  Reopening
        # by pathname after close would create a replacement window for a
        # malicious handle that predates the namespace lease.
        guard = writer.duplicate()
        writer.close()
        guard.seal(final_path)
        return guard
    except BaseException as exc:
        artifacts = [artifact for artifact in (guard, writer) if artifact is not None]
        cleanup_error = _discard_output_artifacts(artifacts)
        if cleanup_error:
            raise OSError(
                f"cannot clean incomplete lifecycle output {final_path}: {cleanup_error}"
            ) from exc
        raise


def _discard_output_artifacts(artifacts: list[object]) -> str | None:
    """Delete every staged/published output through the handles that own them."""
    errors: list[str] = []
    for artifact in reversed(artifacts):
        if getattr(artifact, "closed", False):
            continue
        try:
            discard = getattr(artifact, "discard", None)
            if not callable(discard):
                raise OSError("lifecycle output artifact has no discard operation")
            discard()
        except OSError as exc:
            errors.append(str(exc))
            try:
                close = getattr(artifact, "close", None)
                if not callable(close):
                    raise OSError("lifecycle output artifact has no close operation")
                close()
            except OSError as close_exc:
                errors.append(str(close_exc))
    return "; ".join(errors) if errors else None


def _close_output_artifacts_in_order(artifacts: list[object]) -> str | None:
    """Close fixed leaves in commit order, preserving later cleanup authority on error."""
    for artifact in artifacts:
        if getattr(artifact, "closed", False):
            continue
        try:
            close = getattr(artifact, "close", None)
            if not callable(close):
                raise OSError("lifecycle output artifact has no close operation")
            close()
        except OSError as exc:
            # Stop immediately: the remaining handle(s) are the only
            # path-independent deletion authority after namespace release.
            return str(exc)
    return None


def _handoff_output_reserves_to_process_exit(artifacts: list[object]) -> str | None:
    """Retain one raw native HANDLE per final leaf until ExitProcess closes it.

    This is deliberately the production CLI's final commit handoff, not an
    ``atexit`` close sequence.  Closing the final audit and JSON guards one at
    a time would recreate a window where one leaf is no longer protected while
    the collector still has work left to do.  After a successful handoff the
    process exit itself is the commit boundary; callers must not treat the
    earlier ``OK`` stdout line as a completed publication signal.
    """
    if os.name != "nt" or not WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
        return "native Windows output reserve handoff is unavailable"
    expected_leaves = (
        LIFECYCLE_AUDIT_LOG_FILENAME,
        LIFECYCLE_OUTPUT_FILENAME,
    )
    if len(artifacts) != len(expected_leaves):
        return "lifecycle output reserve handoff received an incomplete leaf pair"
    handles: list[int] = []
    for artifact, expected_leaf in zip(artifacts, expected_leaves, strict=True):
        if not isinstance(artifact, OutputArtifactHandle) or artifact.closed:
            return "lifecycle output reserve handoff received a closed or non-native handle"
        if artifact.leaf_name != expected_leaf:
            return "lifecycle output reserve handoff received a non-fixed leaf"
        handles.append(artifact._handle)
    try:
        # OutputArtifactHandle has no __del__; after callers clear their
        # wrappers these raw values deliberately remain valid in the process
        # handle table until Windows ExitProcess releases them together with
        # the rest of this short-lived CLI's handles.
        _PROCESS_LIFETIME_OUTPUT_RESERVE_HANDLES.extend(handles)
    except MemoryError:
        return "cannot retain lifecycle output reserve handles until process exit"
    return None


def _release_output_namespace_leases(
    leases: tuple[OutputNamespaceLease, ...],
) -> str | None:
    """Release root, build, then output while retaining rooted cleanup on failure.

    The output directory is deliberately last. If closing root or build fails,
    its output HANDLE remains a safe relative authority; if closing that final
    HANDLE fails, the native CloseHandle contract leaves it usable for rollback.
    A caller must perform rollback before the ordinary best-effort final close.
    """
    for anchor in leases:
        closer = getattr(anchor.lease, "close", None)
        if not callable(closer):
            return "lifecycle output namespace lease has no close operation"
        try:
            closer()
        except Exception as exc:
            return str(exc)
    return None


def _validate_native_output_directory_authority(directory: object) -> OutputDirectoryLease:
    """Reject anything but the held native directory handle in production."""
    if os.name != "nt" or not WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
        raise OSError("native Windows output operations are required but unavailable")
    if not isinstance(directory, OutputDirectoryLease) or directory.closed:
        raise OSError("lifecycle output namespace requires a live native Windows directory lease")
    return directory


def _output_publication_operations_for_main() -> OutputPublicationOperations:
    """Return the production-only output authority; tests must inject explicitly."""
    if os.name != "nt" or not WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
        raise OSError("native Windows output-directory leases are required but unavailable")
    return OutputPublicationOperations(
        lease_factory=open_output_directory_lease,
        open_namespace=_open_output_namespace_leases,
        validate_directory=_validate_native_output_directory_authority,
        clear=_clear_output_artifacts,
        write=_write_output_artifact,
        discard=_discard_output_artifacts,
        close_in_order=_close_output_artifacts_in_order,
        finalize_reserves=_handoff_output_reserves_to_process_exit,
    )


def run_engine(
    engine: Path,
    module_image: Path,
    module: str,
    working_directory: Path,
    rhi_backend: str,
    timeout: int,
    expected_digests: tuple[str, str] | None = None,
    *,
    lease_factory: Callable[[Path], object] | None = None,
) -> tuple[EngineOutput, str | None]:
    """Run stable-v1 while native handles pin both manifest-bound images."""
    if not _stable_v1_windows_platform():
        return EngineOutput("", ""), "stable-v1 lifecycle evidence is Windows-only"
    if module != "SparkGameFPS":
        return EngineOutput("", ""), "stable-v1 lifecycle evidence accepts only module SparkGameFPS"
    if rhi_backend != "d3d11":
        return EngineOutput("", ""), "stable-v1 lifecycle evidence requires --rhi-backend d3d11"
    if expected_digests is None or any(
        not isinstance(digest, str) or not lifecycle_mod.ENGINE_SHA256_RE.fullmatch(digest)
        for digest in expected_digests
    ):
        return EngineOutput("", ""), "stable-v1 lifecycle evidence requires manifest-bound image digests"
    prepared, error = _canonicalize_images(engine, module_image, working_directory)
    if error:
        return EngineOutput("", ""), error
    assert prepared is not None
    root, engine, module_image = prepared
    factory = lease_factory or open_image_lease
    leases: list[object] = []
    captured = EngineOutput("", "")
    error = None
    try:
        engine_lease = factory(engine)
        leases.append(engine_lease)
        module_lease = factory(module_image)
        leases.append(module_lease)

        engine_verified, error = _verify_leased_image(
            engine_lease, engine, role="engine", expected_name=_ENGINE_NAME,
            expected_digest=expected_digests[0], min_size=_MIN_ENGINE_SIZE,
        )
        if error is None:
            module_verified, error = _verify_leased_image(
                module_lease, module_image, role="module",
                expected_name=expected_library_names(module)["windows"],
                expected_digest=expected_digests[1],
            )
        else:
            module_verified = None
        if error is None and engine_verified is not None and module_verified is not None:
            if _same_file_identity(engine_verified.identity, module_verified.identity):
                error = "engine and module image leases refer to the same file identity"

        if error is None:
            cmd = [
                str(engine), "-game", str(module_image), "-require-game",
                "-test-seconds", "1.0", "-threads", "2", "-window-size", "640x360",
                "-no-subprocess",
            ]
            print(f"[collect_lifecycle] {' '.join(cmd)}", flush=True)
            try:
                proc = subprocess.run(
                    cmd, capture_output=True, text=True, timeout=timeout,
                    check=False, cwd=str(root),
                    env={**os.environ, "SPARK_RHI_BACKEND": "d3d11",
                         "SPARK_D3D11_DRIVER": "warp"},
                )
            except subprocess.TimeoutExpired:
                error = f"engine run exceeded {timeout}s without completing"
            except OSError as exc:
                error = f"cannot launch engine: {exc}"
            else:
                captured = EngineOutput(proc.stdout or "", proc.stderr or "")
                if proc.returncode != 0:
                    error = (
                        f"engine exited {proc.returncode}; a failed run is not evidence of "
                        "a working lifecycle"
                    )

        if error is None and engine_verified is not None and module_verified is not None:
            post_engine, post_error = _verify_leased_image(
                engine_lease, engine, role="engine", expected_name=_ENGINE_NAME,
                expected_digest=expected_digests[0], min_size=_MIN_ENGINE_SIZE,
            )
            post_module, module_post_error = _verify_leased_image(
                module_lease, module_image, role="module",
                expected_name=expected_library_names(module)["windows"],
                expected_digest=expected_digests[1],
            )
            if post_error or module_post_error or post_engine != engine_verified or post_module != module_verified:
                error = "engine or module image changed while the lifecycle run executed"

        if error is None and engine_verified is not None and module_verified is not None:
            reopened_engine_lease = factory(engine)
            leases.append(reopened_engine_lease)
            reopened_module_lease = factory(module_image)
            leases.append(reopened_module_lease)
            reopened_engine, reopen_error = _verify_leased_image(
                reopened_engine_lease, engine, role="engine", expected_name=_ENGINE_NAME,
                expected_digest=expected_digests[0], min_size=_MIN_ENGINE_SIZE,
            )
            reopened_module, module_reopen_error = _verify_leased_image(
                reopened_module_lease, module_image, role="module",
                expected_name=expected_library_names(module)["windows"],
                expected_digest=expected_digests[1],
            )
            if reopen_error or module_reopen_error or reopened_engine != engine_verified or reopened_module != module_verified:
                error = "canonical image path changed identity while the lifecycle run executed"
            else:
                captured = EngineOutput(
                    captured.stdout, captured.stderr,
                    engine_image=engine_verified, module_image=module_verified,
                )
    except OSError as exc:
        error = f"cannot acquire or inspect native lifecycle image leases: {exc}"
    finally:
        close_error = _close_image_leases(leases)
        if close_error:
            error = f"{error}; " if error else ""
            error += f"cannot close lifecycle image leases: {close_error}"
    return captured, error


def _clear_artifacts(*paths: Path) -> str | None:
    """Remove only declared artifact files, attempting every path on failure."""
    errors: list[str] = []
    seen: set[Path] = set()
    for path in paths:
        if path in seen:
            continue
        seen.add(path)
        try:
            if path.is_dir() and not paths_mod._is_reparse_point(path):
                errors.append(
                    f"artifact path {path} is a directory and cannot be safely cleared"
                )
                continue
            if path.exists() or paths_mod._is_reparse_point(path):
                path.unlink()
        except FileNotFoundError:
            # A concurrent remover already achieved the required absent state.
            continue
        except (OSError, ValueError) as exc:
            errors.append(f"cannot clear lifecycle artifact {path}: {exc}")
    return "; ".join(errors) if errors else None


def _write_temp(final_path: Path, content: str, *, tracked_temps: set[Path]) -> Path:
    """Stage content beside its destination and retain it for transaction cleanup."""
    if paths_mod._is_reparse_point(final_path.parent) or not final_path.parent.is_dir():
        raise OSError(f"lifecycle output parent {final_path.parent} is not a real directory")
    descriptor, temp_name = tempfile.mkstemp(prefix=f".{final_path.name}.", suffix=".tmp",
                                             dir=final_path.parent, text=True)
    temporary = Path(temp_name)
    tracked_temps.add(temporary)
    descriptor_owned = True
    try:
        stream = os.fdopen(descriptor, "w", encoding="utf-8", newline="\n")
        descriptor_owned = False
        with stream:
            stream.write(content)
            stream.flush()
        return temporary
    except BaseException as exc:
        descriptor_close_error: OSError | None = None
        if descriptor_owned:
            try:
                os.close(descriptor)
            except OSError as close_exc:
                descriptor_close_error = close_exc
        cleanup_error = _clear_artifacts(temporary)
        if cleanup_error is None:
            tracked_temps.discard(temporary)
        else:
            raise OSError(
                f"cannot clean temporary lifecycle artifact {temporary}: {cleanup_error}"
            ) from exc
        if descriptor_close_error is not None:
            raise OSError(
                f"cannot close temporary lifecycle descriptor for {temporary}: "
                f"{descriptor_close_error}"
            ) from exc
        raise


def _report_failure(message: str) -> None:
    """Report failure without allowing a broken output stream to skip cleanup."""
    try:
        print(message, file=sys.stderr, flush=True)
    except (OSError, ValueError):
        try:
            sys.stderr.write(message + "\n")
            sys.stderr.flush()
        except (OSError, ValueError):
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    # Keep externally supplied paths as raw strings until alias rejection has
    # completed.  pathlib intentionally normalizes ``.``/``..`` on Windows,
    # which would otherwise erase exactly the evidence needed to reject them.
    parser.add_argument("--engine", required=True)
    parser.add_argument("--module", action="append", required=True, dest="modules")
    parser.add_argument("--module-image", required=True)
    parser.add_argument("--working-directory", required=True)
    parser.add_argument("--rhi-backend", required=True)
    parser.add_argument("--image-manifest", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--commit-sha", default=None)
    args = parser.parse_args()
    out_path: Path | None = None
    log_path: Path | None = None
    output_namespace_leases: tuple[OutputNamespaceLease, ...] | None = None
    output_directory_lease: object | None = None
    output_operations: OutputPublicationOperations | None = None
    manifest_lease: object | None = None
    output_primary_artifacts: list[object] = []
    output_backup_artifacts: list[object] = []
    cleanup_authorized = False
    success = False
    result = 1
    failure: str | None = None
    try:
        if len(args.modules) != 1 or args.modules[0] != "SparkGameFPS":
            raise RuntimeError("stable-v1 lifecycle evidence accepts only module SparkGameFPS")
        if args.rhi_backend != "d3d11":
            raise RuntimeError("stable-v1 lifecycle evidence requires --rhi-backend d3d11")
        if not _stable_v1_windows_platform():
            raise RuntimeError("stable-v1 lifecycle evidence is Windows-only")
        for label, value in (
            ("--engine", args.engine),
            ("--module-image", args.module_image),
            ("--working-directory", args.working_directory),
            ("--image-manifest", args.image_manifest),
            ("--out", args.out),
        ):
            alias_error = _windows_path_alias_error(value, label)
            if alias_error:
                raise RuntimeError(alias_error)
        namespace, err = _validate_output_namespace(args.out, REPO_ROOT)
        if err:
            raise RuntimeError(err)
        assert namespace is not None
        out_path, log_path = namespace
        engine_arg = Path(args.engine)
        module_image_arg = Path(args.module_image)
        working_directory_arg = Path(args.working_directory)
        raw_collision_error = _output_collides_with_input(
            out_path, log_path,
            (("engine", _absolute_raw(engine_arg)),
             ("module image", _absolute_raw(module_image_arg)),
             ("image manifest", _absolute_raw(Path(args.image_manifest)))),
        )
        if raw_collision_error:
            raise RuntimeError(raw_collision_error)
        # This is the only publication abstraction boundary.  Production
        # obtains a strictly native Windows provider; tests must deliberately
        # inject their own complete provider instead of inducing a fallback.
        output_operations = _output_publication_operations_for_main()
        output_namespace_leases, err = output_operations.open_namespace(
            _absolute_raw(REPO_ROOT), out_path.parent,
        )
        if err:
            raise RuntimeError(err)
        assert output_namespace_leases is not None
        candidate_output_directory = output_namespace_leases[-1].lease
        output_directory_lease = output_operations.validate_directory(
            candidate_output_directory,
        )
        err = _validate_output_pair_under_namespace_leases(
            output_namespace_leases, out_path, log_path,
        )
        if err:
            raise RuntimeError(err)
        # From this point onward, cleanup targets are fixed leaf names opened
        # relative to a live directory HANDLE. Sharing flags alone do not
        # prevent metadata/reparse mutations; rooted no-follow opens do.
        cleanup_authorized = True
        cleared = output_operations.clear(output_directory_lease, out_path, log_path)
        if cleared:
            raise RuntimeError(cleared)
        sha, err = resolve_collection_sha(args.commit_sha)
        if sha is None:
            raise RuntimeError(err)
        prepared, err = _canonicalize_images(
            engine_arg, module_image_arg, working_directory_arg,
        )
        if err:
            raise RuntimeError(err)
        assert prepared is not None
        root, engine, module_image = prepared
        manifest_path, err = _canonical_manifest(args.image_manifest, root)
        if err:
            raise RuntimeError(err)
        assert manifest_path is not None
        collision_error = _output_collides_with_input(
            out_path, log_path,
            (("engine", engine), ("module image", module_image),
             ("image manifest", manifest_path)),
        )
        if collision_error:
            raise RuntimeError(collision_error)
        manifest_lease = open_image_lease(manifest_path)
        trusted_images, err = load_image_manifest_from_lease(
            manifest_lease, manifest_path, root, sha,
        )
        if err:
            raise RuntimeError(err)
        assert trusted_images is not None
        source_dir = "GameModules/SparkGameFPS/Source"
        tree_sha, err = lifecycle_mod.source_tree_sha(REPO_ROOT, sha, source_dir)
        if tree_sha is None:
            raise RuntimeError(err)
        err = _validate_output_pair_under_namespace_leases(
            output_namespace_leases, out_path, log_path,
        )
        if err:
            raise RuntimeError(err)
        captured, err = run_engine(engine, module_image, "SparkGameFPS", root,
                                   args.rhi_backend, args.timeout,
                                   (trusted_images["SparkEngine.exe"],
                                    trusted_images["SparkGameFPS.dll"]))
        if err:
            raise RuntimeError(err)
        if captured.engine_image is None or captured.module_image is None:
            raise RuntimeError("native image lease verification returned no image metadata")
        # The manifest remains held until the image leases have been acquired,
        # the child has exited, and run_engine has completed its post-run
        # identity checks.  A close failure is fatal before publication.
        close_error = _close_image_leases([manifest_lease])
        if close_error:
            raise RuntimeError(f"cannot close image manifest lease: {close_error}")
        manifest_lease = None
        phases = parse_terminal_streams(captured.stdout, captured.stderr, "SparkGameFPS")
        record = {
            "module": "SparkGameFPS",
            "sharedLibrary": expected_library_names("SparkGameFPS")["windows"],
            "sourceDirectory": source_dir,
            "sourceTreeSHA": tree_sha,
            "runner": "headless-exec",
            "phases": phases,
            "engineSHA256": captured.engine_image.digest,
            "enginePath": captured.engine_image.final_path,
            "moduleSHA256": captured.module_image.digest,
            "modulePath": captured.module_image.final_path,
        }
        document = {
            "schemaVersion": lifecycle_mod.LIFECYCLE_SCHEMA_VERSION,
            "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "commitSHA": sha,
            "records": [record],
        }
        err = _validate_output_pair_under_namespace_leases(
            output_namespace_leases, out_path, log_path,
        )
        if err:
            raise RuntimeError(err)
        assert output_directory_lease is not None
        # The audit log is intentionally the first, non-authoritative final
        # leaf.  It is flushed and its writer closed before CREATE_NEW can
        # create the JSON evidence leaf, so no valid JSON can outlive an
        # incomplete audit trail.
        audit_guard = output_operations.write(
            output_directory_lease, log_path, captured.audit_log,
        )
        output_primary_artifacts.append(audit_guard)
        duplicate = getattr(audit_guard, "duplicate", None)
        if not callable(duplicate):
            raise RuntimeError("lifecycle output artifact has no duplicate operation")
        output_backup_artifacts.append(duplicate())
        json_guard = output_operations.write(
            output_directory_lease, out_path,
            json.dumps(document, indent=2, sort_keys=True) + "\n",
        )
        output_primary_artifacts.append(json_guard)
        duplicate = getattr(json_guard, "duplicate", None)
        if not callable(duplicate):
            raise RuntimeError("lifecycle output artifact has no duplicate operation")
        output_backup_artifacts.append(duplicate())
        for artifact, final_path in (
            (audit_guard, log_path), (json_guard, out_path),
            (output_backup_artifacts[0], log_path),
            (output_backup_artifacts[1], out_path),
        ):
            verify = getattr(artifact, "verify", None)
            if not callable(verify):
                raise RuntimeError("lifecycle output artifact has no verify operation")
            verify(final_path)
        err = _validate_output_pair_under_namespace_leases(
            output_namespace_leases, out_path, log_path,
        )
        if err:
            raise RuntimeError(err)
        # Flush now: a BrokenPipe must be treated as a failed publication.
        # This is progress only, not a commit notification. The exact-file
        # guards remain live until namespace release and process-exit handoff
        # below have both succeeded.
        print(f"OK: wrote {out_path} with 1 lifecycle record", flush=True)
        success = True
        result = 0
    except Exception as exc:
        failure = f"FATAL: lifecycle evidence could not be produced: {exc}"
    finally:
        if manifest_lease is not None:
            close_error = _close_image_leases([manifest_lease])
            if close_error:
                close_failure = f"cannot close image manifest lease: {close_error}"
                failure = f"{failure}; {close_failure}" if failure else f"FATAL: {close_failure}"
                success = False
                result = 1
        if success and output_namespace_leases is not None:
            # The exact audit/JSON guards remain live across namespace release.
            # They retain exact-file rollback authority if release fails and
            # exclude newly opened ordinary write-data handles.  Do not treat
            # their share mode as metadata/reparse protection: a
            # FILE_WRITE_ATTRIBUTES handle (or a malicious writer opened
            # beforehand) is outside that guarantee.  Consumers must await
            # process exit and perform their own no-follow validation.
            close_error = _release_output_namespace_leases(
                output_namespace_leases,
            )
            if close_error:
                close_failure = f"cannot close lifecycle output namespace leases: {close_error}"
                failure = f"FATAL: {close_failure}"
                success = False
                result = 1
            else:
                output_namespace_leases = None
        if success:
            assert output_operations is not None
            # Redundant primary handles can close only after namespace release:
            # the backup pair still locks both exact leaves, and any close
            # failure can delete both through those still-live backups.
            close_error = output_operations.close_in_order(output_primary_artifacts)
            if close_error:
                close_failure = f"cannot close primary lifecycle output handles: {close_error}"
                failure = f"FATAL: {close_failure}"
                success = False
                result = 1
        if success:
            assert output_operations is not None
            # Production transfers one raw backup HANDLE per leaf to the
            # process-lifetime registry. Test providers instead synchronously
            # release their fakes. Do not close the real final pair one at a
            # time: Windows ExitProcess is the CLI commit boundary.
            handoff_error = output_operations.finalize_reserves(output_backup_artifacts)
            if handoff_error:
                failure = f"FATAL: cannot finalize lifecycle output reserves: {handoff_error}"
                success = False
                result = 1
            else:
                output_primary_artifacts.clear()
                output_backup_artifacts.clear()
        if not success:
            artifact_cleanup_error = (
                output_operations.discard(
                    output_primary_artifacts + output_backup_artifacts,
                ) if output_operations is not None else None
            )
            if artifact_cleanup_error:
                cleanup_failure = (
                    "cannot delete published lifecycle output through retained handles: "
                    f"{artifact_cleanup_error}"
                )
                failure = f"{failure}; {cleanup_failure}" if failure else f"FATAL: {cleanup_failure}"
            # A rooted output HANDLE remains sufficient even after root/build
            # release has started. Never re-resolve the full output pathname
            # during rollback; if an in-place reparse mutation makes rooted
            # opens fail, report the containment failure rather than following
            # the attacker-controlled target.
            if cleanup_authorized and out_path is not None and log_path is not None and \
               output_directory_lease is not None and \
               not getattr(output_directory_lease, "closed", True):
                assert output_operations is not None
                cleanup_error = output_operations.clear(
                    output_directory_lease, out_path, log_path,
                )
                if cleanup_error:
                    _report_failure(f"FATAL: lifecycle evidence cleanup failed: {cleanup_error}")
            if output_namespace_leases is not None:
                close_error = _close_image_leases(
                    [anchor.lease for anchor in output_namespace_leases],
                )
                if close_error:
                    close_failure = f"cannot close lifecycle output namespace leases: {close_error}"
                    failure = f"{failure}; {close_failure}" if failure else f"FATAL: {close_failure}"
                else:
                    output_namespace_leases = None
            if failure is not None:
                _report_failure(failure)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
