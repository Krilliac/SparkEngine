#!/usr/bin/env python3
"""OPS-100 Linux symbolication canary (local build-id symbol store, no relay).

Runs CrashSymbolicationProbe, which installs the production crash handler and
faults inside SparkSymbolicationCanaryCrashSite(). The canary then:

1. requires the process to die by SIGSEGV and leave exactly one crash log in
   its private spark_crash_<pid>_* directory (under an isolated TMPDIR, so the
   user's real temp directory is never touched);
2. splits the probe's debug info into a fresh .build-id store with
   ``symbolicate_crash.py store``;
3. resolves the log with ``symbolicate_crash.py resolve --json`` and requires
   frame 0 (the exact faulting pc) to name the crash-site function at the
   marked source line, and a later return-address frame to resolve to main;
4. proves fail-closed behaviour: a store entry whose build-id differs from the
   recorded one is refused (exit 2), and a symlinked store entry is refused;
5. proves the fatal-signal handler cannot hang or skip a report (SEC2 finding
   12): with its best-effort stage blocked (``--stall-report``) the report
   watchdog still ends the probe by SIGSEGV within the budget and the
   async-signal-safe log already holds the symbolic frames; and a stack
   overflow (``--stack-overflow``) is still reported from the alternate stack.
6. requires full or closed stderr pipes to retain the report and original
   fatal signal, including the stalled-report watchdog, without changing the
   launcher's shared stderr descriptor flags.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYMBOLICATE = ROOT / "tools" / "ops" / "symbolicate_crash.py"
CRASH_SITE = "SparkSymbolicationCanaryCrashSite"
FAULT_MARKER = "SPARK_SYMBOLICATION_CANARY_FAULT_LINE"
PROBE_TIMEOUT_SECONDS = 60
# CrashHandler.cpp kSignalReportBudgetSeconds is 10; allow scheduling slack.
STALLED_REPORT_DEADLINE_SECONDS = 30


class CanaryFailure(Exception):
    pass


def fault_line(source: Path) -> int:
    matches = [number for number, line in enumerate(source.read_text().splitlines(), 1) if FAULT_MARKER in line]
    if len(matches) != 1:
        raise CanaryFailure(f"{source} must mark exactly one fault line with {FAULT_MARKER}")
    return matches[0]


def run_probe(probe: Path, temp_root: Path, *mode: str) -> Path:
    env = dict(os.environ, TMPDIR=str(temp_root))
    completed = subprocess.run(
        [str(probe), *mode],
        env=env,
        cwd=temp_root,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        timeout=PROBE_TIMEOUT_SECONDS,
    )
    if completed.returncode != -signal.SIGSEGV:
        raise CanaryFailure(
            f"probe exited with {completed.returncode}, expected death by SIGSEGV\n"
            f"stderr:\n{completed.stderr.decode(errors='replace')[-2000:]}"
        )
    logs = [path for path in temp_root.glob("spark_crash_*/SymbolicationCanary_*.log") if path.is_file()]
    if len(logs) != 1:
        raise CanaryFailure(f"expected exactly one crash log under {temp_root}, found {logs}")
    return logs[0]


def check_stalled_report(probe: Path, temp_root: Path) -> None:
    """A report blocked after its signal-safe stage still ends the process."""
    started = time.monotonic()
    env = dict(os.environ, TMPDIR=str(temp_root))
    try:
        completed = subprocess.run(
            [str(probe), "--stall-report"],
            env=env,
            cwd=temp_root,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=STALLED_REPORT_DEADLINE_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise CanaryFailure(
            f"a stalled crash report hung the probe for {STALLED_REPORT_DEADLINE_SECONDS}s; "
            "the report watchdog did not terminate it"
        ) from exc
    elapsed = time.monotonic() - started
    stderr = completed.stderr.decode(errors="replace")
    require(
        completed.returncode == -signal.SIGSEGV,
        f"stalled report ended with {completed.returncode} after {elapsed:.1f}s, expected SIGSEGV\n{stderr[-2000:]}",
    )
    require("Crash report timed out" in stderr, f"stalled report did not report its timeout:\n{stderr[-2000:]}")
    logs = [path for path in temp_root.glob("spark_crash_*/SymbolicationCanary_*.log") if path.is_file()]
    require(len(logs) == 1, f"stalled report left {logs}, expected exactly one log")
    text = logs[0].read_text(errors="replace")
    for marker in ("Signal     : 11 - SIGSEGV", "*** STACK TRACE ***", "*** SYMBOLIC FRAMES ***"):
        require(marker in text, f"signal-safe stage did not write {marker!r} before the stall")
    manifests = list(temp_root.glob("spark_crash_*/crash_manifest_*.json"))
    require(not manifests, f"the stalled best-effort stage should not have published {manifests}")


def check_stack_overflow_report(probe: Path, temp_root: Path) -> None:
    """A stack-overflow SIGSEGV is handled on the alternate signal stack."""
    log = run_probe(probe, temp_root, "--stack-overflow")
    text = log.read_text(errors="replace")
    require("Signal     : 11 - SIGSEGV" in text, "stack-overflow report does not name SIGSEGV")
    require("*** SYMBOLIC FRAMES ***" in text, "stack-overflow report has no symbolic-frame section")


def check_unavailable_stderr_report(probe: Path, temp_root: Path, *, closed: bool, stalled: bool) -> None:
    """Real fatal reporting cannot depend on a supervisor draining stderr."""
    read_fd, write_fd = os.pipe()
    process = None
    try:
        original_flags = fcntl.fcntl(write_fd, fcntl.F_GETFL)
        if closed:
            os.close(read_fd)
            read_fd = -1
        else:
            fcntl.fcntl(write_fd, fcntl.F_SETFL, original_flags | os.O_NONBLOCK)
            while True:
                try:
                    os.write(write_fd, b"x" * 4096)
                except BlockingIOError:
                    break
            fcntl.fcntl(write_fd, fcntl.F_SETFL, original_flags)
        mode = ["--stall-report"] if stalled else []
        started = time.monotonic()
        process = subprocess.Popen(
            [str(probe), *mode], env=dict(os.environ, TMPDIR=str(temp_root)), cwd=temp_root,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=write_fd,
        )
        try:
            process.wait(timeout=STALLED_REPORT_DEADLINE_SECONDS)
        except subprocess.TimeoutExpired as exc:
            raise CanaryFailure("unavailable stderr hung fatal reporting past its watchdog budget") from exc
        elapsed = time.monotonic() - started
        require(process.returncode == -signal.SIGSEGV,
                f"unavailable stderr changed fatal termination to {process.returncode} after {elapsed:.1f}s")
        require(fcntl.fcntl(write_fd, fcntl.F_GETFL) == original_flags,
                "fatal diagnostics changed the launcher's shared stderr descriptor flags")
        logs = [path for path in temp_root.glob("spark_crash_*/SymbolicationCanary_*.log") if path.is_file()]
        require(len(logs) == 1, f"unavailable stderr left {logs}, expected exactly one report")
        text = logs[0].read_text(errors="replace")
        for marker in ("Signal     : 11 - SIGSEGV", "*** STACK TRACE ***", "*** SYMBOLIC FRAMES ***"):
            require(marker in text, f"unavailable stderr prevented the report's {marker!r}")
        if stalled:
            require(not list(temp_root.glob("spark_crash_*/crash_manifest_*.json")),
                    "stalled stderr report unexpectedly published its incomplete manifest")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired as exc:
                raise CanaryFailure("unavailable-stderr probe could not be reaped after termination") from exc
        if read_fd >= 0:
            os.close(read_fd)
        os.close(write_fd)


def check_file_stderr_report(probe: Path, temp_root: Path) -> None:
    """The independent diagnostic description appends without moving shared stderr."""
    stderr_path = temp_root / "stderr.log"
    sentinel = b"existing stderr contents\n"
    with stderr_path.open("w+b") as stderr:
        stderr.write(sentinel)
        stderr.flush()
        stderr.seek(0)
        original_flags = fcntl.fcntl(stderr.fileno(), fcntl.F_GETFL)
        completed = subprocess.run(
            [str(probe)], env=dict(os.environ, TMPDIR=str(temp_root)), cwd=temp_root,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=stderr,
            timeout=PROBE_TIMEOUT_SECONDS,
        )
        require(completed.returncode == -signal.SIGSEGV, "file stderr changed the original fatal signal")
        require(stderr.tell() == 0, "fatal diagnostics moved the launcher's shared stderr offset")
        require(fcntl.fcntl(stderr.fileno(), fcntl.F_GETFL) == original_flags,
                "fatal diagnostics changed the launcher's file stderr flags")
        output = stderr.read()
        require(output.startswith(sentinel) and b"[SPARK ENGINE] CRASH:" in output[len(sentinel):],
                "fatal diagnostics failed to append after the existing stderr contents")


def symbolicate(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SYMBOLICATE), *arguments],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=300,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CanaryFailure(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--probe-source", type=Path, required=True)
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--addr2line", default="addr2line")
    args = parser.parse_args()

    try:
        expected_line = fault_line(args.probe_source)
        with tempfile.TemporaryDirectory(prefix="spark-symbolication-canary-") as scratch:
            scratch_root = Path(scratch)
            temp_root = scratch_root / "tmp"
            store = scratch_root / "symbols"
            temp_root.mkdir()
            store.mkdir()

            log = run_probe(args.probe, temp_root)
            stored = symbolicate("store", "--store", str(store), "--objcopy", args.objcopy, str(args.probe))
            require(stored.returncode == 0, f"store failed: {stored.stderr.strip()}")
            entry = store / stored.stdout.strip()
            require(entry.is_file(), f"store did not create {entry}")

            resolved = symbolicate("resolve", "--store", str(store), "--log", str(log), "--addr2line", args.addr2line, "--json")
            require(resolved.returncode == 0, f"resolve failed: {resolved.stderr.strip()}")
            frames = json.loads(resolved.stdout)["frames"]
            top = frames[0]
            require(top.get("kind") == "pc", f"frame 0 is not the exact faulting pc: {top}")
            require(top.get("status") == "resolved", f"frame 0 did not resolve: {top}")
            require(top.get("function") == CRASH_SITE, f"frame 0 function {top.get('function')!r} != {CRASH_SITE}")
            require(
                Path(str(top.get("file"))).name == args.probe_source.name and top.get("line") == expected_line,
                f"frame 0 resolved to {top.get('file')}:{top.get('line')}, expected "
                f"{args.probe_source.name}:{expected_line}",
            )
            require(
                any(frame.get("kind") == "ra" and frame.get("function") == "main" for frame in frames[1:]),
                "no return-address frame resolved to main",
            )

            # Wrong build-id: same path, but the stored file carries another id.
            build_id = str(top["buildId"])
            original = entry.read_bytes()
            data = bytearray(original)
            position = data.find(bytes.fromhex(build_id))
            require(position >= 0, "stored debug file does not contain its build-id note")
            data[position + len(build_id) // 2 - 1] ^= 0xFF
            entry.unlink()
            entry.write_bytes(bytes(data))
            mismatch = symbolicate("resolve", "--store", str(store), "--log", str(log), "--addr2line", args.addr2line)
            require(
                mismatch.returncode == 2 and "build-id mismatch" in mismatch.stderr,
                f"a wrong build-id was not refused (exit {mismatch.returncode}): {mismatch.stderr.strip()}",
            )

            # Symlinked store entry: refused, never followed. The target holds
            # the original, matching debug file, so following the link would
            # resolve successfully; only the no-follow open can refuse it.
            outside = scratch_root / "outside.debug"
            outside.write_bytes(original)
            entry.unlink()
            entry.symlink_to(outside)
            linked = symbolicate("resolve", "--store", str(store), "--log", str(log), "--addr2line", args.addr2line)
            require(
                linked.returncode == 2
                and "symbol store entry" in linked.stderr
                and "refused" in linked.stderr
                and "build-id mismatch" not in linked.stderr,
                f"a symlinked store entry was not refused (exit {linked.returncode}): {linked.stderr.strip()}",
            )

            stalled_root = scratch_root / "tmp-stalled"
            stalled_root.mkdir()
            check_stalled_report(args.probe, stalled_root)
            overflow_root = scratch_root / "tmp-overflow"
            overflow_root.mkdir()
            check_stack_overflow_report(args.probe, overflow_root)
            for closed in (False, True):
                for stalled in (False, True):
                    stderr_root = scratch_root / f"tmp-stderr-{closed}-{stalled}"
                    stderr_root.mkdir()
                    check_unavailable_stderr_report(args.probe, stderr_root, closed=closed, stalled=stalled)
            file_stderr_root = scratch_root / "tmp-file-stderr"
            file_stderr_root.mkdir()
            check_file_stderr_report(args.probe, file_stderr_root)

        print(
            f"symbolication canary passed: {CRASH_SITE} at {args.probe_source.name}:{expected_line} "
            f"(build-id {build_id}); wrong build-id and symlinked entry refused; "
            "stalled report terminated by its watchdog; stack overflow reported; "
            "full and closed stderr pipes preserve reports, fatal signals and descriptor flags"
        )
        return 0
    except (CanaryFailure, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as exc:
        print(f"symbolication canary FAILED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
