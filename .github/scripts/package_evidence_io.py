#!/usr/bin/env python3
"""Fail-closed staging and private-copy primitives for package evidence."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import secrets
import tempfile

if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _CreateFileW = _kernel32.CreateFileW
    _CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    _CreateFileW.restype = wintypes.HANDLE
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
        wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD,
    ]
    _SetFileInformationByHandle.restype = wintypes.BOOL
    _CloseHandle = _kernel32.CloseHandle
    _CloseHandle.argtypes = [wintypes.HANDLE]
    _CloseHandle.restype = wintypes.BOOL
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    _GENERIC_WRITE = 0x40000000
    _DELETE = 0x00010000
    _FILE_SHARE_READ = 0x00000001
    _CREATE_NEW = 1
    _FILE_ATTRIBUTE_NORMAL = 0x00000080
    _FILE_DISPOSITION_INFO_EX = 21
    _FILE_DISPOSITION_FLAG_DELETE = 0x00000001
    _FILE_DISPOSITION_FLAG_POSIX_SEMANTICS = 0x00000002
    _FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE = 0x00000010

    class _FileDispositionInfoEx(ctypes.Structure):
        _fields_ = [("Flags", wintypes.DWORD)]


class PackageEvidenceIOError(ValueError):
    """A package-evidence file boundary could not be established safely."""


def _require_leaf_name(name: str, *, label: str) -> None:
    if not name or Path(name).name != name or name in {".", ".."}:
        raise PackageEvidenceIOError(f"{label} must be one plain file name")


def _publish_temp_no_replace(temporary: Path, destination: Path) -> None:
    """Atomically add the final name only if no entry already owns it."""
    try:
        os.link(temporary, destination)
    except FileExistsError as exc:
        raise PackageEvidenceIOError(
            f"package-evidence destination already exists: {destination}"
        ) from exc
    except OSError as exc:
        raise PackageEvidenceIOError(
            f"package-evidence no-replace publication failed for {destination}: {exc}"
        ) from exc


def _create_windows_staging(parent: Path, destination_name: str) -> tuple[Path, int]:
    """Create an unpredictable staging leaf whose handle denies write/delete sharing."""
    for _ in range(32):
        temporary = parent / f".{destination_name}.{secrets.token_hex(16)}.tmp"
        handle = _CreateFileW(
            str(temporary), _GENERIC_WRITE | _DELETE, _FILE_SHARE_READ, None,
            _CREATE_NEW, _FILE_ATTRIBUTE_NORMAL, None,
        )
        raw_handle = ctypes.cast(handle, ctypes.c_void_p).value
        if raw_handle not in (None, _INVALID_HANDLE_VALUE):
            return temporary, raw_handle
        error = ctypes.get_last_error()
        if error not in (80, 183):
            raise OSError(error, f"CreateFileW could not create staging leaf: {error}")
    raise OSError("could not allocate an exclusive Windows staging leaf")


def _write_windows_staged_payload(handle: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        chunk = payload[offset:offset + 64 * 1024]
        buffer = ctypes.create_string_buffer(chunk)
        written = wintypes.DWORD()
        if not _WriteFile(handle, buffer, len(chunk), ctypes.byref(written), None):
            error = ctypes.get_last_error()
            raise OSError(error, f"WriteFile failed: {error}")
        count = int(written.value)
        if count <= 0:
            raise OSError("WriteFile wrote no bytes")
        offset += count
    if not _FlushFileBuffers(handle):
        error = ctypes.get_last_error()
        raise OSError(error, f"FlushFileBuffers failed: {error}")


def _discard_windows_staging(handle: int) -> None:
    flags = (
        _FILE_DISPOSITION_FLAG_DELETE |
        _FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
        _FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
    )
    disposition = _FileDispositionInfoEx(flags)
    if not _SetFileInformationByHandle(
        handle, _FILE_DISPOSITION_INFO_EX, ctypes.byref(disposition), ctypes.sizeof(disposition),
    ):
        error = ctypes.get_last_error()
        raise OSError(error, f"SetFileInformationByHandle staging delete failed: {error}")


def _close_windows_staging(handle: int) -> None:
    if not _CloseHandle(handle):
        error = ctypes.get_last_error()
        raise OSError(error, f"CloseHandle staging failed: {error}")


def _write_staged_payload(temporary: Path, payload: bytes) -> None:
    """Write all bytes durably before the success name is made visible."""
    descriptor = os.open(temporary, os.O_WRONLY | os.O_TRUNC)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError:
        raise


def publish_bytes_no_replace(destination: Path, payload: bytes) -> None:
    """Stage bytes privately, then make the success name as the final operation."""
    if os.name != "nt":
        raise PackageEvidenceIOError("Windows is required for package evidence publication")
    destination = Path(destination)
    _require_leaf_name(destination.name, label="package-evidence destination")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(destination):
        raise PackageEvidenceIOError(
            f"package-evidence destination already exists: {destination}"
        )
    if os.name == "nt":
        try:
            temporary, handle = _create_windows_staging(destination.parent, destination.name)
        except OSError as exc:
            raise PackageEvidenceIOError(
                f"package-evidence staging create failed for {destination}: {exc}"
            ) from exc
        published = False
        failure: BaseException | None = None
        try:
            _write_windows_staged_payload(handle, payload)
            _publish_temp_no_replace(temporary, destination)
            published = True
        except BaseException as exc:
            failure = exc
        finally:
            try:
                _discard_windows_staging(handle)
            except OSError:
                pass
            try:
                _close_windows_staging(handle)
            except OSError:
                pass
        if failure is not None:
            raise PackageEvidenceIOError(
                f"package-evidence staged publication failed for {destination}: {failure}"
            ) from failure
        if not published:
            raise PackageEvidenceIOError(
                f"package-evidence staged publication did not publish {destination}"
            )
        return
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{destination.name}.", dir=destination.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    published = False
    try:
        try:
            _write_staged_payload(temporary, payload)
        except OSError as exc:
            raise PackageEvidenceIOError(
                f"package-evidence staged write failed for {destination}: {exc}"
            ) from exc
        _publish_temp_no_replace(temporary, destination)
        published = True
    finally:
        if not published:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        else:
            # This cleanup is deliberately best-effort: the success name is
            # already durably published, so a cleanup error must not turn a
            # successful producer into a failed one with a final evidence leaf.
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass


def copy_private_verified_file(source: Path, private_directory: Path, destination_name: str) -> tuple[Path, str]:
    """Copy one regular input to a fresh private directory and return its SHA-256."""
    if os.name != "nt":
        raise PackageEvidenceIOError("Windows is required for private MSI verification")
    source = Path(source)
    private_directory = Path(private_directory)
    _require_leaf_name(destination_name, label="private package copy destination")
    if not source.is_file() or source.is_symlink():
        raise PackageEvidenceIOError("package input must be a regular file without symlink traversal")
    if not private_directory.is_dir() or private_directory.is_symlink():
        raise PackageEvidenceIOError("private package-evidence directory must be a real directory")
    destination = private_directory / destination_name
    if os.path.lexists(destination):
        raise PackageEvidenceIOError(
            f"private package-evidence destination already exists: {destination}"
        )
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{destination.name}.", dir=private_directory)
    temporary = Path(temporary_name)
    digest = hashlib.sha256()
    published = False
    try:
        with source.open("rb") as input_stream, os.fdopen(descriptor, "wb") as output_stream:
            for chunk in iter(lambda: input_stream.read(1024 * 1024), b""):
                digest.update(chunk)
                output_stream.write(chunk)
            output_stream.flush()
            os.fsync(output_stream.fileno())
        _publish_temp_no_replace(temporary, destination)
        published = True
        os.chmod(destination, 0o444)
        return destination, digest.hexdigest()
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            if not published:
                raise
