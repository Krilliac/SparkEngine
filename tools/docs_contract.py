#!/usr/bin/env python3
"""Deterministic first-party documentation generation and validation."""

from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import io
import json
import os
import posixpath
import re
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO, Iterable, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = REPO_ROOT / "docs" / "generated-docs-manifest.json"
MAX_SOURCE_FILES = 6000
MAX_SOURCE_BYTES = 512 * 1024 * 1024
MAX_SOURCE_FILE_BYTES = 8 * 1024 * 1024
MAX_JSON_BYTES = 4 * 1024 * 1024
MAX_JSON_DEPTH = 64
MAX_JSON_NODES = 100_000
MAX_GENERATED_FILES = 2500
MAX_GENERATED_BYTES = 128 * 1024 * 1024
MAX_GENERATED_DEPTH = 32
PROCESS_CLEANUP_SECONDS = 2.0
MAX_PROCESS_OUTPUT_BYTES = 4 * 1024 * 1024
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
HEADER_GUARD_SUFFIXES = ("_H", "_HPP", "_HH", "_HXX", "_GUARD", "_INCLUDED")
TICK = chr(96)


class ContractError(RuntimeError):
    pass


def _remaining(deadline: float) -> float:
    return max(0.0, deadline - time.monotonic())


def _posix_descendants(pid: int, deadline: float) -> list[int]:
    """Return currently attached descendants without consuming the cleanup bound."""

    if os.name == "nt" or _remaining(deadline) <= 0:
        return []
    try:
        listing = subprocess.run(
            ["ps", "-eo", "pid=,ppid="],
            check=False,
            capture_output=True,
            text=True,
            timeout=min(1.0, max(0.05, _remaining(deadline))),
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    children: dict[int, list[int]] = {}
    for line in listing.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        try:
            child, parent = (int(value) for value in fields)
        except ValueError:
            continue
        children.setdefault(parent, []).append(child)
    descendants: list[int] = []
    seen: set[int] = set()
    pending = list(children.get(pid, []))
    while pending:
        child = pending.pop()
        if child in seen:
            continue
        seen.add(child)
        descendants.append(child)
        pending.extend(children.get(child, []))
    return descendants


class _WindowsProcessJob:
    """Own a Windows process tree when nested jobs are permitted.

    ``taskkill /T`` is a useful fallback, but it cannot recover a child once the
    direct parent has already exited.  A kill-on-close job gives an owned child
    tree one lifetime and lets the caller make normal completion as strict as a
    timeout.  Assignment can legitimately fail under a restrictive outer job,
    so callers retain the portable process-group fallback.
    """

    _KILL_ON_JOB_CLOSE = 0x00002000
    _EXTENDED_LIMIT_INFORMATION = 9

    def __init__(self) -> None:
        self._kernel32: Any | None = None
        self._handle: Any | None = None
        if os.name != "nt":
            return
        try:
            from ctypes import wintypes

            class IoCounters(ctypes.Structure):
                _fields_ = [(name, ctypes.c_ulonglong) for name in (
                    "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                    "ReadTransferCount", "WriteTransferCount", "OtherTransferCount",
                )]

            class BasicLimitInformation(ctypes.Structure):
                _fields_ = [
                    ("PerProcessUserTimeLimit", ctypes.c_longlong),
                    ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD),
                ]

            class ExtendedLimitInformation(ctypes.Structure):
                _fields_ = [
                    ("BasicLimitInformation", BasicLimitInformation),
                    ("IoInfo", IoCounters),
                    ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t),
                ]

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
            kernel32.CreateJobObjectW.restype = wintypes.HANDLE
            kernel32.SetInformationJobObject.argtypes = (
                wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
            )
            kernel32.SetInformationJobObject.restype = wintypes.BOOL
            kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
            kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
            kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
            kernel32.CloseHandle.restype = wintypes.BOOL
            handle = kernel32.CreateJobObjectW(None, None)
            if not handle:
                return
            configuration = ExtendedLimitInformation()
            configuration.BasicLimitInformation.LimitFlags = self._KILL_ON_JOB_CLOSE
            if not kernel32.SetInformationJobObject(
                handle,
                self._EXTENDED_LIMIT_INFORMATION,
                ctypes.byref(configuration),
                ctypes.sizeof(configuration),
            ):
                kernel32.CloseHandle(handle)
                return
            self._kernel32 = kernel32
            self._handle = handle
        except (AttributeError, OSError):
            self._kernel32 = None
            self._handle = None

    def assign(self, process: subprocess.Popen[bytes]) -> bool:
        if self._kernel32 is None or self._handle is None:
            return False
        try:
            return bool(self._kernel32.AssignProcessToJobObject(self._handle, process._handle))
        except (AttributeError, OSError):
            return False

    def close(self) -> None:
        if self._kernel32 is not None and self._handle is not None:
            try:
                self._kernel32.CloseHandle(self._handle)
            except (AttributeError, OSError):
                pass
        self._handle = None


@dataclass
class _BoundedCapture:
    payload: bytearray
    overflowed: bool = False


def _drain_process_stream(stream: BinaryIO, capture: _BoundedCapture) -> None:
    """Drain an inherited pipe without allowing child output to consume memory."""

    try:
        while chunk := stream.read(64 * 1024):
            remaining = MAX_PROCESS_OUTPUT_BYTES - len(capture.payload)
            if remaining > 0:
                capture.payload.extend(chunk[:remaining])
            if len(chunk) > remaining:
                capture.overflowed = True
    except OSError:
        # The owner may close a pipe during timeout cleanup.  The process result
        # is already terminal in that path, so a reader-side close is expected.
        pass


def _capture_text(capture: _BoundedCapture) -> str:
    return bytes(capture.payload).decode("utf-8", errors="replace")


def _close_streams(process: subprocess.Popen[bytes]) -> None:
    for stream in (process.stdout, process.stderr):
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass


def _join_readers(readers: Sequence[threading.Thread], deadline: float) -> bool:
    for reader in readers:
        remaining = _remaining(deadline)
        if remaining <= 0:
            break
        reader.join(remaining)
    return all(not reader.is_alive() for reader in readers)


def terminate_process_tree(
    process: subprocess.Popen[bytes], deadline: float, job: _WindowsProcessJob | None = None
) -> None:
    """Best-effort kill of an owned process group without exceeding ``deadline``.

    A timed-out wrapper can leave descendants holding stdout/stderr handles.  The
    caller closes those handles and returns at the deadline even if an escaped
    process resists cleanup, so the documented wall bound remains enforceable.
    """

    if os.name == "nt":
        remaining = _remaining(deadline)
        if remaining > 0:
            try:
                subprocess.run(
                    ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=min(1.0, max(0.05, remaining)),
                )
            except (OSError, subprocess.TimeoutExpired):
                pass
    else:
        descendants = _posix_descendants(process.pid, deadline)
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        for pid in reversed(descendants):
            try:
                os.kill(pid, signal.SIGKILL)
            except (OSError, ProcessLookupError):
                pass
    try:
        process.kill()
    except OSError:
        pass
    if job is not None:
        job.close()


def run_bounded_process(
    command: list[str],
    *,
    cwd: Path,
    environment: dict[str, str],
    timeout: float,
    label: str,
) -> subprocess.CompletedProcess[str]:
    """Run a child tree with a strict ``timeout + cleanup`` wall bound."""

    if timeout <= 0:
        raise ContractError(f"{label} timeout must be positive")
    options: dict[str, Any] = {
        "cwd": cwd,
        "env": environment,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "stdin": subprocess.DEVNULL,
    }
    if os.name == "nt":
        options["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        options["start_new_session"] = True
    try:
        process = subprocess.Popen(command, **options)
    except OSError as exc:
        raise ContractError(f"cannot start {label}: {exc}") from exc
    job = _WindowsProcessJob()
    job_assigned = job.assign(process)
    stdout_capture = _BoundedCapture(bytearray())
    stderr_capture = _BoundedCapture(bytearray())
    assert process.stdout is not None and process.stderr is not None
    readers = (
        threading.Thread(target=_drain_process_stream, args=(process.stdout, stdout_capture), daemon=True),
        threading.Thread(target=_drain_process_stream, args=(process.stderr, stderr_capture), daemon=True),
    )
    for reader in readers:
        reader.start()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        cleanup_deadline = time.monotonic() + PROCESS_CLEANUP_SECONDS
        terminate_process_tree(process, cleanup_deadline, job)
        _close_streams(process)
        remaining = _remaining(cleanup_deadline)
        if remaining > 0:
            try:
                process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                pass
        _join_readers(readers, cleanup_deadline)
        raise ContractError(
            f"{label} exceeded {timeout:g} seconds and was terminated as a process tree"
        ) from exc
    # A successful direct child is insufficient: an inherited descendant can
    # otherwise mutate output after validation.  Closing an assigned job kills
    # every still-live member before captured output is accepted.
    if job_assigned:
        job.close()
    completion_deadline = time.monotonic() + PROCESS_CLEANUP_SECONDS
    readers_finished = _join_readers(readers, completion_deadline)
    if not readers_finished:
        terminate_process_tree(process, completion_deadline, None if job_assigned else job)
        _close_streams(process)
        _join_readers(readers, completion_deadline)
        raise ContractError(f"{label} left descendant output handles open after completion")
    if stdout_capture.overflowed or stderr_capture.overflowed:
        raise ContractError(f"{label} exceeded the {MAX_PROCESS_OUTPUT_BYTES}-byte output capture limit")
    return subprocess.CompletedProcess(
        command, process.returncode, _capture_text(stdout_capture), _capture_text(stderr_capture)
    )


@dataclass(frozen=True)
class FileIdentity:
    """The stable identity and metadata of a regular file at one observation."""

    device: int
    inode: int
    mode: int
    size: int
    mtime_ns: int
    ctime_ns: int
    nlink: int
    attributes: int


@dataclass(frozen=True, order=True)
class Symbol:
    path: str
    line: int
    kind: str
    name: str
    brief: str = ""

    def tsv_row(self) -> tuple[str, str, str, str, str]:
        clean = self.brief.replace("\t", " ").replace("\r", " ").replace("\n", " ")
        return self.kind, self.name, self.path, str(self.line), clean


def absolute_path(path: Path) -> Path:
    """Return a normalized lexical path without resolving links or junctions."""

    return Path(os.path.abspath(os.fspath(path)))


def is_reparse_stat(metadata: os.stat_result) -> bool:
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    attributes = getattr(metadata, "st_file_attributes", 0)
    return stat.S_ISLNK(metadata.st_mode) or bool(reparse_flag and attributes & reparse_flag)


def assert_no_reparse_ancestors(path: Path, *, label: str) -> None:
    """Reject every existing lexical ancestor that could redirect a path operation."""

    current = absolute_path(path)
    while True:
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            pass
        except OSError as exc:
            raise ContractError(f"cannot inspect {label} ancestor {current}: {exc}") from exc
        else:
            if is_reparse_stat(metadata):
                raise ContractError(f"{label} crosses a symlink or reparse point: {current}")
        if current.parent == current:
            return
        current = current.parent


def assert_contained(path: Path, root: Path, *, label: str) -> Path:
    """Check lexical containment without silently resolving a reparse point."""

    candidate = absolute_path(path)
    container = absolute_path(root)
    try:
        candidate.relative_to(container)
    except ValueError as exc:
        raise ContractError(f"{label} escapes its declared root: {candidate}") from exc
    assert_no_reparse_ancestors(container, label=label)
    relative = candidate.relative_to(container)
    current = container
    for part in relative.parts[:-1]:
        current /= part
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise ContractError(f"cannot inspect {label} directory {current}: {exc}") from exc
        if is_reparse_stat(metadata) or not stat.S_ISDIR(metadata.st_mode):
            raise ContractError(f"{label} crosses an unsafe directory: {current}")
    return candidate


def regular_identity(path: Path, *, label: str) -> FileIdentity:
    """Require a direct, single-link regular file and capture its metadata."""

    assert_no_reparse_ancestors(path.parent, label=label)
    try:
        metadata = os.lstat(path)
    except OSError as exc:
        raise ContractError(f"cannot inspect {label}: {path}: {exc}") from exc
    if is_reparse_stat(metadata) or not stat.S_ISREG(metadata.st_mode):
        raise ContractError(f"{label} is not a regular non-reparse file: {path}")
    if metadata.st_nlink != 1:
        raise ContractError(f"{label} is hard-linked and cannot be trusted: {path}")
    return FileIdentity(
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
        metadata.st_nlink,
        getattr(metadata, "st_file_attributes", 0),
    )


def opened_identity_matches(expected: FileIdentity, observed: FileIdentity) -> bool:
    """Compare handle identity without Windows' handle-local ctime projection."""

    return (
        expected.device,
        expected.inode,
        expected.mode,
        expected.size,
        expected.nlink,
        expected.attributes,
    ) == (
        observed.device,
        observed.inode,
        observed.mode,
        observed.size,
        observed.nlink,
        observed.attributes,
    )


def read_regular_bytes(path: Path, *, label: str, maximum: int) -> bytes:
    """Read a bounded regular file while detecting replacement or mutation races."""

    before = regular_identity(path, label=label)
    if before.size > maximum:
        raise ContractError(f"{label} exceeds the {maximum}-byte limit: {path}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise ContractError(f"cannot open {label}: {path}: {exc}") from exc
    try:
        with os.fdopen(descriptor, "rb", closefd=True) as stream:
            opened = os.fstat(stream.fileno())
            opened_identity = FileIdentity(
                opened.st_dev,
                opened.st_ino,
                opened.st_mode,
                opened.st_size,
                opened.st_mtime_ns,
                opened.st_ctime_ns,
                opened.st_nlink,
                getattr(opened, "st_file_attributes", 0),
            )
            if not opened_identity_matches(before, opened_identity):
                raise ContractError(f"{label} changed before it could be opened: {path}")
            payload = stream.read(maximum + 1)
    except ContractError:
        raise
    except OSError as exc:
        raise ContractError(f"cannot read {label}: {path}: {exc}") from exc
    if len(payload) > maximum:
        raise ContractError(f"{label} exceeds the {maximum}-byte limit: {path}")
    after = regular_identity(path, label=label)
    if after != before or len(payload) != before.size:
        raise ContractError(f"{label} changed while it was read: {path}")
    return payload


def reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON object key: {key!r}")
        result[key] = value
    return result


def reject_nonfinite_json_constant(value: str) -> object:
    raise ValueError(f"non-finite JSON constant is forbidden: {value}")


def assert_json_bounds(value: object) -> None:
    """Reject deeply nested or excessively broad JSON after duplicate-key parsing."""

    nodes = 0
    pending: list[tuple[object, int]] = [(value, 1)]
    while pending:
        current, depth = pending.pop()
        nodes += 1
        if nodes > MAX_JSON_NODES:
            raise ContractError("JSON input exceeds node-count bound")
        if depth > MAX_JSON_DEPTH:
            raise ContractError("JSON input exceeds nesting-depth bound")
        if isinstance(current, dict):
            for key, child in current.items():
                if not isinstance(key, str):
                    raise ContractError("JSON object key is not a string")
                pending.append((child, depth + 1))
        elif isinstance(current, list):
            pending.extend((child, depth + 1) for child in current)


def load_bounded_json(path: Path, *, label: str, maximum: int = MAX_JSON_BYTES) -> object:
    payload = read_regular_bytes(path, label=label, maximum=maximum)
    try:
        value = json.loads(
            payload.decode("utf-8-sig"),
            object_pairs_hook=reject_duplicate_json_keys,
            parse_constant=reject_nonfinite_json_constant,
        )
        assert_json_bounds(value)
        return value
    except (UnicodeDecodeError, json.JSONDecodeError, ContractError, RecursionError, ValueError) as exc:
        raise ContractError(f"cannot parse {label}: {path}: {exc}") from exc


def atomic_write_bytes(path: Path, payload: bytes) -> None:
    path = absolute_path(path)
    assert_no_reparse_ancestors(path.parent, label="output")
    if os.path.lexists(path):
        regular_identity(path, label="existing output")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        regular_identity(Path(temp_name), label="temporary output")
        assert_no_reparse_ancestors(path.parent, label="output")
        if os.path.lexists(path):
            regular_identity(path, label="existing output")
        os.replace(temp_name, path)
    except BaseException:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


def atomic_write_text(path: Path, payload: str) -> None:
    atomic_write_bytes(path, payload.encode("utf-8"))


def safe_relative(raw: str) -> PurePosixPath:
    path = PurePosixPath(raw)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise ContractError(f"unsafe repository-relative path: {raw!r}")
    if "\\" in raw or "\x00" in raw:
        raise ContractError(f"non-canonical repository-relative path: {raw!r}")
    return path


def load_contract() -> dict:
    try:
        contract = load_bounded_json(CONTRACT_PATH, label="generated-docs manifest")
    except ContractError as exc:
        raise ContractError(f"cannot load {CONTRACT_PATH}: {exc}") from exc
    if not isinstance(contract, dict):
        raise ContractError("generated-docs manifest must be a JSON object")
    if contract.get("schemaVersion") != 1:
        raise ContractError("generated-docs manifest schemaVersion must be 1")
    source = contract.get("sourceContract")
    if not isinstance(source, dict):
        raise ContractError("sourceContract must be an object")
    for key in ("includeRoots", "extensions", "headerExtensions", "excludePrefixes"):
        values = source.get(key)
        if not isinstance(values, list) or not values or not all(isinstance(value, str) and value for value in values):
            raise ContractError(f"sourceContract.{key} must be a non-empty string array")
        if len(values) != len(set(values)):
            raise ContractError(f"sourceContract.{key} contains duplicates")
    generators = contract.get("generators")
    if not isinstance(generators, list) or not generators:
        raise ContractError("generators must be a non-empty array")
    ids = [entry.get("id") for entry in generators if isinstance(entry, dict)]
    scripts = [entry.get("script") for entry in generators if isinstance(entry, dict)]
    if len(ids) != len(generators) or any(not isinstance(value, str) or not value for value in ids):
        raise ContractError("each generator must have a non-empty string id")
    if len(ids) != len(set(ids)) or len(scripts) != len(set(scripts)):
        raise ContractError("generator ids and scripts must each be unique")
    return contract


def tracked_paths() -> set[str] | None:
    external = os.environ.get("SPARK_DOC_TRACKED_PATHS")
    if external:
        try:
            raw = read_regular_bytes(
                Path(external), label="tracked-path manifest", maximum=MAX_JSON_BYTES
            )
        except ContractError as exc:
            raise ContractError(f"cannot read tracked-path manifest: {exc}") from exc
        result: set[str] = set()
        for item in raw.split(b"\0"):
            if not item:
                continue
            try:
                value = item.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise ContractError("tracked-path manifest has a non-UTF-8 path") from exc
            result.add(safe_relative(value).as_posix())
        return result
    try:
        process = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "ls-files", "-z", "--cached"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    result: set[str] = set()
    for item in process.stdout.split(b"\0"):
        if item:
            result.add(safe_relative(item.decode("utf-8")).as_posix())
    return result


def source_inventory_snapshot() -> dict[PurePosixPath, FileIdentity]:
    """Return the declared source inventory with stable per-file identities."""

    source = load_contract()["sourceContract"]
    roots = tuple(safe_relative(value) for value in source["includeRoots"])
    extensions = {value.lower() for value in source["extensions"]}
    excluded = tuple(safe_relative(value.rstrip("/")).as_posix() + "/" for value in source["excludePrefixes"])
    tracked = tracked_paths()
    candidates: list[str] = []
    if tracked is not None:
        candidates.extend(sorted(tracked))
    else:
        for root in roots:
            full = REPO_ROOT.joinpath(*root.parts)
            if full.is_symlink():
                raise ContractError(f"source root is a symlink or reparse point: {root.as_posix()}")
            if not full.exists():
                continue
            for directory, dirnames, filenames in os.walk(full, followlinks=False):
                base = Path(directory)
                for name in dirnames:
                    if (base / name).is_symlink():
                        raise ContractError(f"source directory is a symlink or reparse point: {base / name}")
                candidates.extend((base / name).relative_to(REPO_ROOT).as_posix() for name in filenames)

    paths: dict[PurePosixPath, FileIdentity] = {}
    for raw in candidates:
        rel = safe_relative(raw)
        posix = rel.as_posix()
        if not any(rel == root or root in rel.parents for root in roots):
            continue
        if any(posix.startswith(prefix) for prefix in excluded):
            continue
        if rel.suffix.lower() not in extensions:
            continue
        full = REPO_ROOT.joinpath(*rel.parts)
        assert_contained(full, REPO_ROOT, label="source")
        paths[rel] = regular_identity(full, label=f"tracked source {posix}")

    ordered = sorted(paths, key=lambda value: value.as_posix())
    if not ordered or len(ordered) > MAX_SOURCE_FILES:
        raise ContractError(f"source inventory size {len(ordered)} is outside 1..{MAX_SOURCE_FILES}")
    total = 0
    for rel in ordered:
        size = paths[rel].size
        if size > MAX_SOURCE_FILE_BYTES:
            raise ContractError(f"source file exceeds byte limit: {rel.as_posix()}")
        total += size
        if total > MAX_SOURCE_BYTES:
            raise ContractError("source inventory exceeds total-byte limit")
    return {rel: paths[rel] for rel in ordered}


def source_inventory() -> list[PurePosixPath]:
    return list(source_inventory_snapshot())


def read_source(rel: PurePosixPath, expected: FileIdentity | None = None) -> str:
    full = REPO_ROOT.joinpath(*rel.parts)
    try:
        assert_contained(full, REPO_ROOT, label="source")
        payload = read_regular_bytes(
            full, label=f"source {rel.as_posix()}", maximum=MAX_SOURCE_FILE_BYTES
        )
        actual = regular_identity(full, label=f"source {rel.as_posix()}")
        if expected is not None and actual != expected:
            raise ContractError(f"source changed since inventory: {rel.as_posix()}")
        return payload.decode("utf-8-sig")
    except (ContractError, UnicodeDecodeError) as exc:
        raise ContractError(f"cannot read UTF-8 source {rel.as_posix()}: {exc}") from exc


def strip_cpp_comments(text: str) -> str:
    out: list[str] = []
    state = "normal"
    index = 0
    while index < len(text):
        ch = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if state == "normal":
            if ch == "/" and nxt == "/":
                out.extend((" ", " "))
                index += 2
                state = "line"
                continue
            if ch == "/" and nxt == "*":
                out.extend((" ", " "))
                index += 2
                state = "block"
                continue
            if ch == '"':
                out.append(" ")
                state = "string"
            elif ch == "'":
                out.append(" ")
                state = "char"
            else:
                out.append(ch)
            index += 1
            continue
        if state == "line":
            out.append("\n" if ch == "\n" else " ")
            if ch == "\n":
                state = "normal"
            index += 1
            continue
        if state == "block":
            if ch == "*" and nxt == "/":
                out.extend((" ", " "))
                index += 2
                state = "normal"
                continue
            out.append("\n" if ch == "\n" else " ")
            index += 1
            continue
        quote = '"' if state == "string" else "'"
        if ch == "\\" and nxt:
            out.extend((" ", "\n" if nxt == "\n" else " "))
            index += 2
        else:
            out.append("\n" if ch == "\n" else " ")
            if ch == quote:
                state = "normal"
            index += 1
    if state == "block":
        raise ContractError("unterminated block comment in first-party source")
    return "".join(out)


def brief_for_line(lines: Sequence[str], line_index: int) -> str:
    window = "\n".join(lines[max(0, line_index - 8):line_index + 1])
    matches = list(re.finditer(r"@brief\s+([^\r\n*]+)", window))
    return matches[-1].group(1).strip() if matches else ""


def class_name(fragment: str) -> str | None:
    prefix = re.split(r"[:{;]", fragment, maxsplit=1)[0]
    prefix = re.sub(r"\bfinal\b", " ", prefix)
    prefix = re.sub(r"\b(?:alignas|__declspec)\s*\([^)]*\)", " ", prefix)
    prefix = re.sub(r"\[\[[^\]]+\]\]", " ", prefix)
    names = IDENT_RE.findall(prefix)
    return names[-1] if names else None


def extract_symbols(rel: PurePosixPath, text: str) -> list[Symbol]:
    stripped = strip_cpp_comments(text)
    original = text.splitlines()
    symbols: list[Symbol] = []
    for line_no, line in enumerate(stripped.splitlines(), start=1):
        brief = brief_for_line(original, line_no - 1)
        macro = re.match(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)", line)
        if macro:
            name = macro.group(1)
            if not name.endswith(HEADER_GUARD_SUFFIXES):
                symbols.append(Symbol(rel.as_posix(), line_no, "macro", name, brief))
            continue
        declaration = re.match(r"^\s*(class|struct)\s+(.+)", line)
        if declaration:
            name = class_name(declaration.group(2))
            if name and name not in {"final", "public", "private", "protected"}:
                symbols.append(Symbol(rel.as_posix(), line_no, declaration.group(1), name, brief))
            continue
        enum = re.match(r"^\s*enum(?:\s+class)?\s+([A-Za-z_][A-Za-z0-9_]*)", line)
        if enum:
            symbols.append(Symbol(rel.as_posix(), line_no, "enum", enum.group(1), brief))
            continue
        using = re.match(r"^\s*using\s+([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        if using:
            symbols.append(Symbol(rel.as_posix(), line_no, "alias", using.group(1), brief))
            continue
        typedef = re.match(r"^\s*typedef\b.+\b([A-Za-z_][A-Za-z0-9_]*)\s*;", line)
        if typedef:
            symbols.append(Symbol(rel.as_posix(), line_no, "alias", typedef.group(1), brief))
            continue
        method = re.match(
            r"^\s*(?:template\s*<[^>]+>\s*)?(?:[A-Za-z_][A-Za-z0-9_:<>,*&\s]*\s+)"
            r"([A-Za-z_][A-Za-z0-9_:]*::[A-Za-z_~][A-Za-z0-9_]*)\s*\(",
            line,
        )
        if method:
            symbols.append(Symbol(rel.as_posix(), line_no, "method", method.group(1), brief))
            continue
        function = re.match(
            r"^\s*(?:template\s*<[^>]+>\s*)?(?:static\s+|inline\s+|constexpr\s+|virtual\s+|extern\s+)*"
            r"[A-Za-z_][A-Za-z0-9_:<>,*&\s]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(",
            line,
        )
        if function and function.group(1) not in {
            "if", "for", "while", "switch", "return", "sizeof", "alignof",
            "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast",
        }:
            symbols.append(Symbol(rel.as_posix(), line_no, "function", function.group(1), brief))
    unique = {(value.kind, value.name, value.path, value.line): value for value in symbols}
    return sorted(unique.values())


def scan_sources() -> tuple[list[PurePosixPath], list[Symbol], dict[str, str]]:
    snapshot = source_inventory_snapshot()
    paths = list(snapshot)
    texts: dict[str, str] = {}
    symbols: list[Symbol] = []
    for rel in paths:
        text = read_source(rel, snapshot[rel])
        texts[rel.as_posix()] = text
        symbols.extend(extract_symbols(rel, text))
    if source_inventory_snapshot() != snapshot:
        raise ContractError("source inventory changed during documentation generation")
    return paths, sorted(symbols), texts


def source_identity() -> tuple[str, str]:
    sha = os.environ.get("SPARKENGINE_DOC_SOURCE_SHA", "")
    committed_at = os.environ.get("SPARKENGINE_DOC_SOURCE_COMMITTED_AT", "")
    if not sha:
        try:
            sha = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, timeout=15,
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise ContractError("SPARKENGINE_DOC_SOURCE_SHA is required without Git") from exc
    if not SHA_RE.fullmatch(sha):
        raise ContractError("source SHA must be an exact 40-character lowercase Git SHA")
    if not committed_at:
        try:
            committed_at = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "show", "-s", "--format=%cI", sha],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, timeout=15,
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise ContractError("SPARKENGINE_DOC_SOURCE_COMMITTED_AT is required without Git") from exc
    try:
        datetime.fromisoformat(committed_at.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ContractError("source committed-at value must be RFC 3339") from exc
    return sha, committed_at


def relative_link(from_file: PurePosixPath, target: PurePosixPath) -> str:
    return posixpath.relpath(target.as_posix(), start=from_file.parent.as_posix())


def generated_tree_snapshot(
    output: Path,
    *,
    label: str = "generated API tree",
    max_files: int | None = None,
    max_bytes: int | None = None,
) -> dict[str, FileIdentity]:
    """Enumerate a bounded regular-file tree without following link-like entries."""

    output = absolute_path(output)
    max_files = MAX_GENERATED_FILES if max_files is None else max_files
    max_bytes = MAX_GENERATED_BYTES if max_bytes is None else max_bytes
    assert_no_reparse_ancestors(output.parent, label=label)
    try:
        root_metadata = os.lstat(output)
    except OSError as exc:
        raise ContractError(f"cannot inspect {label}: {output}: {exc}") from exc
    if is_reparse_stat(root_metadata) or not stat.S_ISDIR(root_metadata.st_mode):
        raise ContractError(f"{label} root is missing or unsafe: {output}")

    result: dict[str, FileIdentity] = {}
    total = 0
    pending: list[tuple[Path, PurePosixPath]] = [(output, PurePosixPath())]
    while pending:
        directory, relative_directory = pending.pop()
        try:
            entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as exc:
            raise ContractError(f"cannot enumerate {label}: {directory}: {exc}") from exc
        for entry in entries:
            relative = relative_directory / entry.name
            if len(relative.parts) > MAX_GENERATED_DEPTH:
                raise ContractError(f"{label} exceeds depth bound at {relative.as_posix()}")
            child = Path(entry.path)
            try:
                metadata = os.lstat(child)
            except OSError as exc:
                raise ContractError(f"cannot inspect {label} entry {child}: {exc}") from exc
            if is_reparse_stat(metadata):
                raise ContractError(f"{label} contains a symlink or reparse point: {child}")
            if stat.S_ISDIR(metadata.st_mode):
                pending.append((child, relative))
                continue
            if not stat.S_ISREG(metadata.st_mode):
                raise ContractError(f"{label} contains a non-regular entry: {child}")
            if metadata.st_nlink != 1:
                raise ContractError(f"{label} contains a hard-linked file: {child}")
            identity = FileIdentity(
                metadata.st_dev,
                metadata.st_ino,
                metadata.st_mode,
                metadata.st_size,
                metadata.st_mtime_ns,
                metadata.st_ctime_ns,
                metadata.st_nlink,
                getattr(metadata, "st_file_attributes", 0),
            )
            canonical = relative.as_posix()
            result[canonical] = identity
            total += identity.size
            if len(result) > max_files or total > max_bytes:
                raise ContractError(f"{label} exceeds resource bounds")
    return result


def remove_generated_tree(
    root: Path,
    *,
    label: str,
    max_files: int | None = None,
    max_bytes: int | None = None,
) -> None:
    """Remove only a fully revalidated ordinary generated tree."""

    root = absolute_path(root)
    snapshot = generated_tree_snapshot(root, label=label, max_files=max_files, max_bytes=max_bytes)
    for relative, expected in sorted(snapshot.items(), key=lambda item: len(item[0].split("/")), reverse=True):
        path = root.joinpath(*PurePosixPath(relative).parts)
        if regular_identity(path, label=label) != expected:
            raise ContractError(f"{label} changed before removal: {path}")
        os.unlink(path)
    directories: list[Path] = []
    for directory, names, _ in os.walk(root, topdown=False, followlinks=False):
        current = Path(directory)
        try:
            metadata = os.lstat(current)
        except OSError as exc:
            raise ContractError(f"cannot inspect {label} directory {current}: {exc}") from exc
        if is_reparse_stat(metadata) or not stat.S_ISDIR(metadata.st_mode):
            raise ContractError(f"{label} directory became unsafe: {current}")
        directories.append(current)
        for name in names:
            child = current / name
            try:
                child_metadata = os.lstat(child)
            except OSError as exc:
                raise ContractError(f"cannot inspect {label} entry {child}: {exc}") from exc
            if is_reparse_stat(child_metadata):
                raise ContractError(f"{label} contains a reparse point during removal: {child}")
    for directory in directories:
        os.rmdir(directory)


def publish_generated_tree(staging: Path, destination: Path) -> None:
    """Publish a complete staged tree without traversing an untrusted old output."""

    staging = absolute_path(staging)
    destination = absolute_path(destination)
    if staging.parent != destination.parent:
        raise ContractError("staged API output must share its destination parent")
    assert_no_reparse_ancestors(destination.parent, label="API output")
    generated_tree_snapshot(staging, label="staged API output")
    backup: Path | None = None
    if os.path.lexists(destination):
        generated_tree_snapshot(destination, label="existing API output")
        backup = Path(
            tempfile.mkdtemp(prefix=f".{destination.name}.previous-", dir=destination.parent)
        )
        os.rmdir(backup)
        os.replace(destination, backup)
        # Recheck after the rename: if a race substituted a malicious tree, leave
        # it quarantined under the generated parent and fail rather than deleting it.
        generated_tree_snapshot(backup, label="quarantined API output")
    try:
        assert_no_reparse_ancestors(destination.parent, label="API output")
        os.replace(staging, destination)
        generated_tree_snapshot(destination, label="published API output")
    except BaseException:
        if backup is not None and not os.path.lexists(destination):
            os.replace(backup, destination)
        raise
    if backup is not None:
        remove_generated_tree(backup, label="quarantined API output")


def write_api_manifest(output: Path, source_sha: str) -> None:
    files: list[dict[str, object]] = []
    total = 0
    snapshot = generated_tree_snapshot(output)
    for raw, identity in snapshot.items():
        if raw == ".manifest.json":
            continue
        path = output.joinpath(*PurePosixPath(raw).parts)
        payload = read_regular_bytes(path, label="generated API file", maximum=MAX_GENERATED_BYTES)
        if regular_identity(path, label="generated API file") != identity:
            raise ContractError(f"generated API file changed during manifest creation: {raw}")
        total += len(payload)
        if len(files) >= MAX_GENERATED_FILES or total > MAX_GENERATED_BYTES:
            raise ContractError("generated API manifest exceeds resource bounds")
        files.append({
            "path": raw,
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        })
    manifest = {
        "schemaVersion": 1,
        "sourceCommit": source_sha,
        "files": files,
        "fileCount": len(files),
        "totalBytes": total,
    }
    atomic_write_text(output / ".manifest.json", json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def validate_api_manifest(output: Path, expected_sha: str | None = None) -> list[str]:
    errors: list[str] = []
    try:
        tree = generated_tree_snapshot(output)
        manifest = load_bounded_json(output / ".manifest.json", label="generated API manifest")
    except ContractError as exc:
        return [f"cannot read generated API manifest: {exc}"]
    if not isinstance(manifest, dict):
        return ["generated API manifest must be a JSON object"]
    if manifest.get("schemaVersion") != 1:
        errors.append("generated API manifest schemaVersion must be 1")
    source_sha = manifest.get("sourceCommit")
    if not isinstance(source_sha, str) or not SHA_RE.fullmatch(source_sha):
        errors.append("generated API manifest sourceCommit must be an exact SHA")
    elif expected_sha and source_sha != expected_sha:
        errors.append(f"generated API sourceCommit {source_sha} does not match {expected_sha}")
    rows = manifest.get("files")
    if not isinstance(rows, list) or not rows:
        return errors + ["generated API manifest files must be a non-empty array"]
    if len(rows) > MAX_GENERATED_FILES:
        return errors + ["generated API manifest exceeds file-count bound"]
    seen: set[str] = set()
    total = 0
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("path"), str):
            errors.append("generated API manifest row is malformed")
            continue
        try:
            rel = safe_relative(row["path"])
        except ContractError as exc:
            errors.append(str(exc))
            continue
        canonical = rel.as_posix()
        if canonical in seen:
            errors.append(f"duplicate API manifest path: {canonical}")
            continue
        seen.add(canonical)
        full = output.joinpath(*rel.parts)
        identity = tree.get(canonical)
        if identity is None:
            errors.append(f"API manifest target is missing or unsafe: {canonical}")
            continue
        try:
            payload = read_regular_bytes(full, label="generated API manifest target", maximum=MAX_GENERATED_BYTES)
        except ContractError as exc:
            errors.append(str(exc))
            continue
        if regular_identity(full, label="generated API manifest target") != identity:
            errors.append(f"API manifest target changed during validation: {canonical}")
            continue
        total += len(payload)
        if total > MAX_GENERATED_BYTES:
            errors.append("generated API manifest exceeds total-byte bound")
            continue
        if row.get("bytes") != len(payload):
            errors.append(f"API manifest byte mismatch: {canonical}")
        if row.get("sha256") != hashlib.sha256(payload).hexdigest():
            errors.append(f"API manifest digest mismatch: {canonical}")
    actual = {path for path in tree if path != ".manifest.json"}
    if actual != seen:
        errors.append("API manifest file set does not exactly match generated tree")
    if manifest.get("fileCount") != len(seen) or manifest.get("totalBytes") != total:
        errors.append("API manifest aggregate counts are inconsistent")
    return errors


def generate_api(output: Path) -> None:
    destination = absolute_path(output)
    assert_no_reparse_ancestors(destination.parent, label="API output")
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}.staging-", dir=destination.parent))
    output = staging
    sha, committed_at = source_identity()
    try:
        paths, symbols, _ = scan_sources()
        header_extensions = {value.lower() for value in load_contract()["sourceContract"]["headerExtensions"]}
        headers = [rel for rel in paths if rel.suffix.lower() in header_extensions]
        by_path: dict[str, list[Symbol]] = {}
        for symbol in symbols:
            by_path.setdefault(symbol.path, []).append(symbol)
        modules: dict[str, int] = {}
        for rel in headers:
            modules[rel.parts[0]] = modules.get(rel.parts[0], 0) + 1
            output_rel = PurePosixPath("docs/api").joinpath(rel).with_suffix(".md")
            source_link = relative_link(output_rel, rel)
            api_link = relative_link(output_rel, PurePosixPath("docs/api/README.md"))
            lines = [
                f"# {TICK}{rel.as_posix()}{TICK}",
                "",
                f"[Back to API Reference]({api_link}) - [View source]({source_link})",
                "",
                "## Declarations",
                "",
            ]
            selected = by_path.get(rel.as_posix(), [])
            if selected:
                lines.extend(["| Symbol | Kind | Source | Brief |", "|--------|------|--------|-------|"])
                for value in selected:
                    brief = value.brief.replace("|", "\\|")
                    lines.append(
                        f"| {TICK}{value.name}{TICK} | {value.kind} | "
                        f"[L{value.line}]({source_link}#L{value.line}) | {brief} |"
                    )
            else:
                lines.append("_No indexed declarations in this header._")
            lines.append("")
            atomic_write_text(output.joinpath(*rel.parts).with_suffix(".md"), "\n".join(lines))

        readme = [
        "# SparkEngine API Reference",
        "",
        f"> Deterministically generated from exact source commit {TICK}{sha}{TICK}",
        f"> committed at {TICK}{committed_at}{TICK}.",
        "",
        f"**Coverage:** {len(paths)} first-party source files, {len(headers)} headers, {len(symbols)} symbol rows.",
        "",
        "## Modules",
        "",
        "| Module | Header pages |",
        "|--------|-------------:|",
    ]
        readme.extend(f"| {TICK}{module}{TICK} | {count} |" for module, count in sorted(modules.items()))
        readme.append("")
        atomic_write_text(output / "README.md", "\n".join(readme))
        rows = ["\t".join(value.tsv_row()) for value in symbols]
        atomic_write_text(output / ".symbols.tsv", ("\n".join(rows) + "\n") if rows else "")
        for filename, predicate, title in (
        ("ComponentIndex.md", lambda value: "Component" in value.name, "Component Index"),
        ("SystemIndex.md", lambda value: "System" in value.name, "System Index"),
        ):
            selected = [value for value in symbols if predicate(value)]
            lines = [f"# {title}", "", "| Symbol | Kind | Source |", "|--------|------|--------|"]
            lines.extend(
                f"| {TICK}{value.name}{TICK} | {value.kind} | "
                f"[{PurePosixPath(value.path).name}:L{value.line}](../../{value.path}#L{value.line}) |"
                for value in selected
            )
            lines.append("")
            atomic_write_text(output / filename, "\n".join(lines))
        metadata = {
        "schemaVersion": 1,
        "sourceCommit": sha,
        "sourceCommittedAt": committed_at,
        "sourcesScanned": len(paths),
        "headersScanned": len(headers),
        "cppFilesScanned": sum(rel.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".mm"} for rel in paths),
        "markdownPages": len(headers) + 3,
        "symbolRecords": len(symbols),
        }
        atomic_write_text(output / ".generation.json", json.dumps(metadata, indent=2, sort_keys=True) + "\n")
        write_api_manifest(output, sha)
        errors = validate_api_manifest(output, sha)
        if errors:
            raise ContractError("generated API output failed self-validation: " + "; ".join(errors[:3]))
        publish_generated_tree(output, destination)
    except BaseException:
        if os.path.lexists(staging):
            remove_generated_tree(staging, label="failed API staging output")
        raise


def load_symbols(path: Path) -> list[Symbol]:
    rows: list[Symbol] = []
    try:
        payload = read_regular_bytes(path, label="symbol TSV", maximum=MAX_GENERATED_BYTES)
        with io.StringIO(payload.decode("utf-8"), newline="") as stream:
            for line_no, row in enumerate(csv.reader(stream, delimiter="\t"), start=1):
                if line_no > MAX_GENERATED_FILES * 4096:
                    raise ContractError("symbol TSV exceeds row-count bound")
                if len(row) != 5:
                    raise ContractError(f"{path}:{line_no}: expected five TSV fields")
                kind, name, raw_path, raw_line, brief = row
                safe_relative(raw_path)
                try:
                    source_line = int(raw_line)
                except ValueError as exc:
                    raise ContractError(f"{path}:{line_no}: invalid source line") from exc
                if source_line <= 0:
                    raise ContractError(f"{path}:{line_no}: source line must be positive")
                rows.append(Symbol(raw_path, source_line, kind, name, brief))
    except (OSError, UnicodeDecodeError, ContractError) as exc:
        raise ContractError(f"cannot read symbol TSV: {exc}") from exc
    if len(rows) != len(set(rows)):
        raise ContractError("symbol TSV contains duplicate rows")
    return sorted(rows)


INDEX_SPECS = (
    ("Symbol-Index.md", "Symbol Index", "Every indexed first-party declaration.", None),
    ("Function-Index.md", "Function Index", "Free functions and out-of-line methods.", {"function", "method"}),
    ("Class-Index.md", "Class and Struct Index", "Every declared class and struct.", {"class", "struct"}),
    ("Enum-Index.md", "Enum Index", "Every declared enum.", {"enum"}),
    ("Macro-Index.md", "Macro and Alias Index", "Every indexed macro and type alias.", {"macro", "alias"}),
)


def render_index(title: str, description: str, symbols: Iterable[Symbol]) -> str:
    selected = sorted(symbols, key=lambda value: (value.name.casefold(), value.path, value.line, value.kind))
    lines = [
        f"# {title}",
        "",
        f"> {description}",
        ">",
        f"> **Total:** {len(selected)} symbols. Auto-generated by {TICK}docs/generate-symbol-index.sh{TICK}.",
        "",
        "| Symbol | Kind | Module | Source | Brief |",
        "|--------|------|--------|--------|-------|",
    ]
    for value in selected:
        module = PurePosixPath(value.path).parts[0]
        brief = value.brief.replace("|", "\\|")
        lines.append(
            f"| {TICK}{value.name}{TICK} | {value.kind} | {module} | "
            f"[{PurePosixPath(value.path).name}:L{value.line}](../../{value.path}#L{value.line}) | {brief} |"
        )
    lines.append("")
    return "\n".join(lines)


def generate_indexes(api_dir: Path, output_root: Path) -> None:
    actual = load_symbols(api_dir / ".symbols.tsv")
    _, expected, _ = scan_sources()
    if actual != expected:
        raise ContractError("symbol TSV is not an exact path, line, kind, name, and brief projection of source")
    reference = output_root / "reference"
    for filename, title, description, kinds in INDEX_SPECS:
        selected = actual if kinds is None else [value for value in actual if value.kind in kinds]
        atomic_write_text(reference / filename, render_index(title, description, selected))


def generate_file_tree(output: Path) -> None:
    paths, _, texts = scan_sources()
    total_loc = 0
    lines = [
        "# File Tree",
        "",
        "> Every tracked first-party native source file declared by the generated-docs manifest.",
        "",
        f"Auto-generated by {TICK}docs/generate-file-tree.sh{TICK}.",
        "",
        "## Hierarchy",
        "",
    ]
    current = None
    for rel in paths:
        directory = rel.parent.as_posix()
        if directory != current:
            lines.extend([f"### {TICK}{directory}/{TICK}", ""])
            current = directory
        loc = len(texts[rel.as_posix()].splitlines())
        total_loc += loc
        lines.append(f"- [{TICK}{rel.name}{TICK}](../../{rel.as_posix()}) - {loc} LOC")
    lines.extend([
        "",
        "---",
        "",
        "## Totals",
        "",
        "| Metric | Count |",
        "|--------|------:|",
        f"| Source files scanned | {len(paths)} |",
        f"| Total logical lines | {total_loc} |",
        "",
    ])
    atomic_write_text(output, "\n".join(lines))


def generate_hierarchy(output: Path) -> None:
    paths, _, texts = scan_sources()
    edges: set[tuple[str, str, str, str, int]] = set()
    for rel in paths:
        for line_no, line in enumerate(strip_cpp_comments(texts[rel.as_posix()]).splitlines(), start=1):
            declaration = re.match(
                r"^\s*(?:class|struct)\s+(.+?)\s*:\s*(?:public|protected|private)?\s*([A-Za-z_][A-Za-z0-9_:]*)",
                line,
            )
            if declaration:
                derived = class_name(declaration.group(1))
                if derived:
                    edges.add((rel.parts[0], derived, declaration.group(2).split("::")[-1], rel.as_posix(), line_no))
    lines = [
        "# Class Hierarchy",
        "",
        f"> **Total inheritance edges:** {len(edges)}. Derived from the exact first-party source contract.",
        "",
        f"Auto-generated by {TICK}docs/generate-class-hierarchy.sh{TICK}.",
        "",
    ]
    current = None
    for module, derived, base, _path, _line in sorted(edges):
        if module != current:
            if current is not None:
                lines.extend(["~~~", ""])
            current = module
            lines.extend([f"## {TICK}{module}{TICK}", "", "~~~mermaid", "classDiagram"])
        lines.append(f"    {base} <|-- {derived}")
    if current is not None:
        lines.extend(["~~~", ""])
    atomic_write_text(output, "\n".join(lines))


FILE_TREE_ROW = re.compile(r"^- \[[^]]+\]\(\.\./\.\./([^)]+)\) - ([0-9]+) LOC$")


def validate_source_contract(api_dir: Path, wiki_root: Path) -> list[str]:
    errors: list[str] = []
    paths, expected_symbols, texts = scan_sources()
    try:
        actual_symbols = load_symbols(api_dir / ".symbols.tsv")
    except ContractError as exc:
        errors.append(str(exc))
        actual_symbols = []
    if actual_symbols != expected_symbols:
        errors.append(
            f"symbol TSV differs from source: {len(set(expected_symbols) - set(actual_symbols))} missing, "
            f"{len(set(actual_symbols) - set(expected_symbols))} extra"
        )
    reference = wiki_root / "reference"
    for filename, title, description, kinds in INDEX_SPECS:
        selected = expected_symbols if kinds is None else [value for value in expected_symbols if value.kind in kinds]
        expected = render_index(title, description, selected).encode("utf-8")
        try:
            actual = read_regular_bytes(
                reference / filename,
                label=f"generated source index {filename}",
                maximum=MAX_GENERATED_BYTES,
            )
        except ContractError as exc:
            errors.append(str(exc))
            continue
        if actual != expected:
            errors.append(f"{filename} is not an exact source-derived index")

    rows: dict[str, int] = {}
    duplicates: set[str] = set()
    tree_path = reference / "File-Tree.md"
    try:
        tree_payload = read_regular_bytes(
            tree_path,
            label="generated File-Tree index",
            maximum=MAX_GENERATED_BYTES,
        )
        for line in tree_payload.decode("utf-8").splitlines():
            match = FILE_TREE_ROW.match(line)
            if match:
                if match.group(1) in rows:
                    duplicates.add(match.group(1))
                rows[match.group(1)] = int(match.group(2))
    except (ContractError, UnicodeDecodeError) as exc:
        errors.append(f"cannot read File-Tree.md: {exc}")
    expected_rows = {rel.as_posix(): len(texts[rel.as_posix()].splitlines()) for rel in paths}
    missing = set(expected_rows) - set(rows)
    deleted = set(rows) - set(expected_rows)
    wrong = {path for path in set(rows) & set(expected_rows) if rows[path] != expected_rows[path]}
    if duplicates or missing or deleted or wrong:
        errors.append(
            f"File-Tree is not bidirectionally exact: {len(missing)} missing, "
            f"{len(deleted)} deleted, {len(wrong)} wrong LOC, {len(duplicates)} duplicate"
        )
    try:
        metadata = load_bounded_json(api_dir / ".generation.json", label="API generation metadata")
        if not isinstance(metadata, dict):
            raise ContractError("API generation metadata must be a JSON object")
    except ContractError as exc:
        errors.append(f"cannot read API generation metadata: {exc}")
    else:
        header_extensions = {value.lower() for value in load_contract()["sourceContract"]["headerExtensions"]}
        expected_counts = {
            "sourcesScanned": len(paths),
            "headersScanned": sum(rel.suffix.lower() in header_extensions for rel in paths),
            "symbolRecords": len(expected_symbols),
        }
        for key, expected in expected_counts.items():
            if metadata.get(key) != expected:
                errors.append(f"API generation metadata {key} is incomplete or inconsistent")
    errors.extend(validate_api_manifest(api_dir))
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    api = subparsers.add_parser("generate-api")
    api.add_argument("--output", type=Path, required=True)
    indexes = subparsers.add_parser("generate-indexes")
    indexes.add_argument("--api-dir", type=Path, required=True)
    indexes.add_argument("--output-root", type=Path, required=True)
    tree = subparsers.add_parser("generate-file-tree")
    tree.add_argument("--output", type=Path, required=True)
    hierarchy = subparsers.add_parser("generate-hierarchy")
    hierarchy.add_argument("--output", type=Path, required=True)
    validate = subparsers.add_parser("validate")
    validate.add_argument("--api-dir", type=Path, required=True)
    validate.add_argument("--wiki-root", type=Path, required=True)
    api_manifest = subparsers.add_parser("validate-api-manifest")
    api_manifest.add_argument("--api-dir", type=Path, required=True)
    api_manifest.add_argument("--source-sha")
    args = parser.parse_args(argv)
    try:
        if args.command == "generate-api":
            generate_api(absolute_path(args.output))
        elif args.command == "generate-indexes":
            generate_indexes(absolute_path(args.api_dir), absolute_path(args.output_root))
        elif args.command == "generate-file-tree":
            generate_file_tree(absolute_path(args.output))
        elif args.command == "generate-hierarchy":
            generate_hierarchy(absolute_path(args.output))
        elif args.command == "validate":
            errors = validate_source_contract(absolute_path(args.api_dir), absolute_path(args.wiki_root))
            if errors:
                print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
                return 1
        elif args.command == "validate-api-manifest":
            errors = validate_api_manifest(absolute_path(args.api_dir), args.source_sha)
            if errors:
                print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
                return 1
    except ContractError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
