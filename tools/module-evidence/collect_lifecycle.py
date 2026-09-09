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
        --image-manifest package/module-lifecycle-images.json \
        --out <absolute-repo-root>\\build\\module-evidence\\module-lifecycle.json
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
_FILE_READ_ATTRIBUTES = 0x0080
_FILE_SHARE_READ = 0x00000001
_OPEN_EXISTING = 3
_FILE_ATTRIBUTE_NORMAL = 0x00000080
_FILE_ATTRIBUTE_DIRECTORY = 0x00000010
_FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
_FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
_FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
_HANDLE_FLAG_INHERIT = 0x00000001
_FILE_BEGIN = 0


WINDOWS_IMAGE_LEASE_AVAILABLE = False
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

        _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
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
        _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
        WINDOWS_IMAGE_LEASE_AVAILABLE = True
    except (AttributeError, ImportError, OSError):
        # Production collection must reject rather than silently reducing this
        # to path-based hashing when a native lease cannot be established.
        WINDOWS_IMAGE_LEASE_AVAILABLE = False


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
    if not WINDOWS_IMAGE_LEASE_AVAILABLE:
        raise OSError("native Windows image lease support is unavailable")
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
    if not WINDOWS_IMAGE_LEASE_AVAILABLE:
        raise OSError("native Windows image lease support is unavailable")
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
        self._closed = True
        if not _CloseHandle(self._handle):  # type: ignore[name-defined]
            raise OSError(f"CloseHandle failed: {ctypes.get_last_error()}")  # type: ignore[name-defined]

    def __enter__(self) -> ImageLease:
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> bool:
        self.close()
        return False


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
    current = root
    for component in LIFECYCLE_OUTPUT_DIRECTORY:
        current, error = _require_exact_nonreparse_directory(
            current, component, create_if_missing=True,
        )
        if error:
            return None, error
        assert current is not None
    if current != output_directory:
        return None, "lifecycle output directory did not resolve to the fixed namespace"
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
    if os.name != "nt":
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
    manifest_lease: object | None = None
    tracked_temps: set[Path] = set()
    cleanup_authorized = False
    success = False
    result = 1
    failure: str | None = None
    try:
        if len(args.modules) != 1 or args.modules[0] != "SparkGameFPS":
            raise RuntimeError("stable-v1 lifecycle evidence accepts only module SparkGameFPS")
        if args.rhi_backend != "d3d11":
            raise RuntimeError("stable-v1 lifecycle evidence requires --rhi-backend d3d11")
        if os.name != "nt":
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
        # Re-check immediately before the first destructive operation.  This
        # guarantees _clear_artifacts never receives a caller-selected path or
        # a namespace which became a reparse/case alias during input checks.
        namespace, err = _validate_output_namespace(args.out, REPO_ROOT)
        if err or namespace != (out_path, log_path):
            raise RuntimeError(err or "lifecycle output namespace changed during validation")
        cleanup_authorized = True
        cleared = _clear_artifacts(out_path, log_path)
        if cleared:
            raise RuntimeError(cleared)
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
        manifest_lease = None
        if close_error:
            raise RuntimeError(f"cannot close image manifest lease: {close_error}")
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
        namespace, err = _validate_output_namespace(args.out, REPO_ROOT)
        if err or namespace != (out_path, log_path):
            raise RuntimeError(err or "lifecycle output namespace changed before publication")
        log_temp = _write_temp(log_path, captured.audit_log, tracked_temps=tracked_temps)
        json_temp = _write_temp(
            out_path, json.dumps(document, indent=2, sort_keys=True) + "\n",
            tracked_temps=tracked_temps,
        )
        os.replace(log_temp, log_path)
        tracked_temps.discard(log_temp)
        os.replace(json_temp, out_path)
        tracked_temps.discard(json_temp)
        # Flush now: a BrokenPipe must be treated as a failed publication.
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
        if not success:
            if cleanup_authorized and out_path is not None and log_path is not None:
                namespace, namespace_error = _validate_output_namespace(args.out, REPO_ROOT)
                if namespace_error or namespace != (out_path, log_path):
                    _report_failure(
                        "FATAL: lifecycle evidence cleanup skipped because the fixed "
                        f"output namespace is no longer safe: {namespace_error or 'changed'}"
                    )
                else:
                    cleanup_error = _clear_artifacts(out_path, log_path, *tracked_temps)
                    if cleanup_error:
                        _report_failure(f"FATAL: lifecycle evidence cleanup failed: {cleanup_error}")
            if failure is not None:
                _report_failure(failure)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
