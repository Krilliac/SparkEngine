#!/usr/bin/env python3
"""Validate untouched production crash artifacts through both existing consumers."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OPS = ROOT / "tools" / "ops"
sys.path.insert(0, str(OPS))
from fs_security import FilesystemPolicyError, SecureRoot
from ops_strict_json import loads_strict
from validate_crash_package import (
    MAX_ARTIFACT_BYTES,
    MAX_COLLECTION_ENTRIES,
    MAX_DIRECTORY_ENTRIES,
    MAX_JSON_DEPTH,
    MAX_JSON_STRING_BYTES,
    MAX_MANIFEST_BYTES,
    MAX_PACKAGE_BYTES,
    READY_MANIFEST_PATTERN,
)

PRODUCER_TEST = "CrashHandler_UngatedReportWritesAnArtifactAndTheAssertGateDoesNot"
VALIDATOR = OPS / "validate_crash_package.py"
MAX_PROCESS_OUTPUT = 1024 * 1024
PROCESS_POLL_INTERVAL = 0.02
PROCESS_KILL_TIMEOUT = 2.0


class CaptureError(RuntimeError):
    pass


def directory_identity(path: Path) -> tuple[int, int]:
    info = path.lstat()
    if (not stat.S_ISDIR(info.st_mode) or path.is_symlink()
            or getattr(info, "st_file_attributes", 0) & 0x400):
        raise CaptureError("expected an owned, non-reparse directory")
    return info.st_dev, info.st_ino


def producer_environment(temp_root: Path) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith("SPARK_TEST_")}
    env.update(TEMP=str(temp_root), TMP=str(temp_root), TMPDIR=str(temp_root),
               SPARK_TEST_NAME=PRODUCER_TEST, SPARK_TEST_EXPECT_COUNT="1",
               SPARK_TEST_KEEP_CRASH_ARTIFACTS="1")
    return env


def run_process(command: list[str], cwd: Path, *, env: dict[str, str] | None = None,
                timeout: float = 20) -> tuple[int, int, str]:
    if timeout <= 0:
        raise CaptureError("subprocess timeout must be positive")
    # Redirect straight to an owned file: inherited output handles cannot make
    # us wait for pipe EOF, and no communicate() call buffers unlimited output.
    with tempfile.NamedTemporaryFile(mode="w+b", prefix="process-", suffix=".log",
                                     dir=cwd, delete=False) as diagnostic:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=diagnostic,
                                   stderr=subprocess.STDOUT)
        deadline = time.monotonic() + timeout
        failure = None
        try:
            while True:
                status = process.poll()
                if os.fstat(diagnostic.fileno()).st_size > MAX_PROCESS_OUTPUT:
                    failure = "crash-security subprocess exceeded its output limit"
                    break
                if status is not None:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    failure = "crash-security subprocess timed out"
                    break
                try:
                    process.wait(timeout=min(PROCESS_POLL_INTERVAL, remaining))
                except subprocess.TimeoutExpired:
                    pass
        finally:
            try:
                if process.poll() is None:
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    try:
                        process.wait(timeout=PROCESS_KILL_TIMEOUT)
                    except subprocess.TimeoutExpired as error:
                        raise CaptureError("subprocess did not exit within the bounded post-kill wait") from error
            finally:
                # A burst may exceed the cap between polls. Retain at most the
                # cap, and only ever read a bounded prefix into memory.
                size = os.fstat(diagnostic.fileno()).st_size
                if size > MAX_PROCESS_OUTPUT:
                    failure = failure or "crash-security subprocess exceeded its output limit"
                    diagnostic.truncate(MAX_PROCESS_OUTPUT)
        if failure:
            raise CaptureError(failure)
        diagnostic.seek(0)
        output = diagnostic.read(MAX_PROCESS_OUTPUT)
        return process.pid, process.returncode, output.decode("utf-8", errors="replace")


def discover_capture(temp_root: Path, producer_pid: int) -> Path:
    directory_identity(temp_root)
    pattern = re.compile(rf"spark_crash_{producer_pid}_[0-9a-f]{{32}}")
    candidates = []
    with os.scandir(temp_root) as entries:
        for index, entry in enumerate(entries):
            if index >= MAX_DIRECTORY_ENTRIES:
                raise CaptureError("producer temporary directory has too many entries")
            if pattern.fullmatch(entry.name):
                candidate = temp_root / entry.name
                directory_identity(candidate)
                candidates.append(candidate)
    if len(candidates) != 1:
        raise CaptureError("expected exactly one fresh crash root for the producer PID")
    return candidates[0]


def snapshot(root: Path) -> tuple[tuple[int, int], dict[str, tuple[int, int, int, int, str]]]:
    identity = directory_identity(root)
    files = {}
    total = 0
    deadline = time.monotonic() + 15
    with SecureRoot(root) as pinned:
        for name in sorted(pinned.iter_names(max_entries=MAX_DIRECTORY_ENTRIES, deadline=deadline)):
            data, metadata = pinned.read_file(name, max_bytes=MAX_ARTIFACT_BYTES, deadline=deadline)
            total += len(data)
            if total > MAX_PACKAGE_BYTES:
                raise CaptureError("captured package exceeds the offline aggregate limit")
            files[name] = (metadata.device, metadata.file_id, metadata.size,
                           metadata.mtime_ns, hashlib.sha256(data).hexdigest())
    if directory_identity(root) != identity:
        raise CaptureError("crash root changed while taking its snapshot")
    return identity, files


def read_manifest(root: Path, producer_pid: int) -> tuple[str, dict]:
    with SecureRoot(root) as pinned:
        deadline = time.monotonic() + 15
        names = [name for name in pinned.iter_names(max_entries=MAX_DIRECTORY_ENTRIES, deadline=deadline)
                 if READY_MANIFEST_PATTERN.fullmatch(name)]
        if len(names) != 1:
            raise CaptureError("expected exactly one canonical emitted manifest")
        data, _ = pinned.read_file(names[0], max_bytes=MAX_MANIFEST_BYTES, deadline=deadline)
    manifest = loads_strict(data, source=names[0], max_bytes=MAX_MANIFEST_BYTES, max_depth=MAX_JSON_DEPTH,
                            max_collection_entries=MAX_COLLECTION_ENTRIES,
                            max_string_bytes=MAX_JSON_STRING_BYTES)
    if not isinstance(manifest, dict) or manifest.get("enginePID") != str(producer_pid):
        raise CaptureError("manifest does not identify the producer process")
    if any(not isinstance(manifest.get(key), str) or not manifest[key] for key in ("logFile", "dumpFile")):
        raise CaptureError("real producer must emit both log and dump references")
    if (manifest.get("screenshotFile") != "" or manifest.get("zipFile") != ""
            or manifest.get("requireConsent") is not False
            or manifest.get("promptUserDescription") is not False
            or manifest.get("fullMemoryDump") is not False):
        raise CaptureError("producer fixture is not the bounded noninteractive capture")
    return names[0], manifest


def validate_package(root: Path, cwd: Path, *, expected_check: str | None = None) -> list[str]:
    _, status, output = run_process(
        [sys.executable, "-B", str(VALIDATOR), "--writer-output", "--check-names", "--json", str(root)], cwd)
    try:
        result = json.loads(output)
    except (ValueError, TypeError) as error:
        raise CaptureError("validator did not return its JSON contract") from error
    if not isinstance(result, dict) or not isinstance(result.get("errors"), list):
        raise CaptureError("validator did not return its result object")
    checks = sorted({error.get("check") for error in result.get("errors", [])
                     if isinstance(error, dict) and isinstance(error.get("check"), str)})
    if expected_check is None:
        if status != 0 or result.get("passed") is not True or result["errors"]:
            raise CaptureError(f"untouched production package rejected: {', '.join(checks) or 'invalid verdict'}")
    elif status != 1 or result.get("passed") is not False or expected_check not in checks:
        raise CaptureError(f"negative control did not fail with {expected_check}")
    return checks


def copy_case(source: Path, destination: Path, manifest_name: str, manifest: dict) -> None:
    # Only adverse copies are rewritten. The captured baseline is never edited.
    destination.mkdir(mode=0o700)
    deadline = time.monotonic() + 15
    with SecureRoot(source) as pinned:
        for name in pinned.iter_names(max_entries=MAX_DIRECTORY_ENTRIES, deadline=deadline):
            data, _ = pinned.read_file(name, max_bytes=MAX_ARTIFACT_BYTES, deadline=deadline)
            if name == manifest_name:
                data = json.dumps(manifest, ensure_ascii=False).encode("utf-8")
            with (destination / name).open("xb") as output:
                output.write(data)


def remove_owned_tree(root: Path, identity: tuple[int, int]) -> bool:
    """Retain the capsule until identity-bound recursive deletion is available."""
    if directory_identity(root) != identity or root.resolve(strict=True) != root:
        raise CaptureError("cleanup refused a replaced or aliased owned root")
    # SecureRoot provides pinned reads, not deletion. In particular, Windows
    # shutil.rmtree cannot bind every removal to those pinned identities. A
    # path precheck followed by unpinned recursive deletion is not a substitute.
    print(f"Retained crash-security evidence (identity-bound cleanup unavailable): {root}", file=sys.stderr)
    return False


def capture_security(producer: Path, reporter: Path, *, keep_work: bool = False) -> dict:
    producer, reporter = producer.resolve(strict=True), reporter.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix="spark-crash-security-")).resolve(strict=True)
    identity = directory_identity(work)
    try:
        temp_root = work / "tmp"
        temp_root.mkdir(mode=0o700)
        pid, status, _ = run_process([str(producer), "--warn-is-error", "--empty-is-error"], work,
                                    env=producer_environment(temp_root), timeout=40)
        if status != 0:
            raise CaptureError("isolated production capture test failed")
        artifact_root = discover_capture(temp_root, pid)
        manifest_name, manifest = read_manifest(artifact_root, pid)
        before = snapshot(artifact_root)
        _, status, _ = run_process([str(reporter), "--report", str(artifact_root / manifest_name)], work)
        if status != 0:
            raise CaptureError("native reporter rejected untouched producer output")
        validate_package(artifact_root, work)
        if any(before[1].get(manifest[key], (0, 0, 0))[2] == 0 for key in ("logFile", "dumpFile")):
            raise CaptureError("captured log or dump is empty or absent")
        if snapshot(artifact_root) != before:
            raise CaptureError("native/Python consumers changed captured identities or bytes")

        cases = work / "cases"
        cases.mkdir(mode=0o700)
        sentinel = cases / "outside.log"
        sentinel.write_bytes(b"outside crash-root sentinel\n")
        sentinel_info = sentinel.stat()
        sentinel_identity = (sentinel_info.st_dev, sentinel_info.st_ino)
        hostile = dict(manifest, logFile="../outside.log")
        traversal = cases / "traversal"
        copy_case(artifact_root, traversal, manifest_name, hostile)
        case_before = snapshot(traversal)
        _, status, _ = run_process([str(reporter), "--report", str(traversal / manifest_name)], work)
        if status != 1:
            raise CaptureError("native traversal control did not return LoadManifest rejection exit 1")
        validate_package(traversal, work, expected_check="artifact-path")
        sentinel_info = sentinel.stat()
        if (snapshot(traversal) != case_before
                or (sentinel_info.st_dev, sentinel_info.st_ino) != sentinel_identity
                or sentinel.read_bytes() != b"outside crash-root sentinel\n"):
            raise CaptureError("traversal rejection changed fixture identities or outside data")

        legacy = cases / "legacy-field"
        copy_case(artifact_root, legacy, manifest_name, dict(manifest, githubToken=""))
        case_before = snapshot(legacy)
        _, status, _ = run_process([str(reporter), "--report", str(legacy / manifest_name)], work)
        if status != 0:
            raise CaptureError("native legacy transport-field compatibility regressed")
        validate_package(legacy, work, expected_check="writer-field")
        if snapshot(legacy) != case_before or snapshot(artifact_root) != before:
            raise CaptureError("negative controls changed original or copied artifacts")
        result = {"passed": True, "producer_pid": pid, "artifact_count": len(before[1]),
                  "native_and_python_read_only": True, "traversal_rejected": True,
                  "legacy_field_native_accepted_python_rejected": True,
                  "work_root": str(work) if keep_work else None}
    except Exception:
        print(f"Retained crash-security evidence: {work}", file=sys.stderr)
        raise
    if not keep_work:
        try:
            if not remove_owned_tree(work, identity):
                result["work_root"] = str(work)
        except (CaptureError, OSError):
            print(f"Retained crash-security evidence: {work}", file=sys.stderr)
            raise
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--reporter", type=Path, required=True)
    parser.add_argument("--keep-work", action="store_true", help="retain this invocation's owned local evidence")
    args = parser.parse_args()
    try:
        print(json.dumps(capture_security(args.producer, args.reporter, keep_work=args.keep_work), sort_keys=True))
        return 0
    except (CaptureError, FilesystemPolicyError, OSError, ValueError) as error:
        print(f"crash-security failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
