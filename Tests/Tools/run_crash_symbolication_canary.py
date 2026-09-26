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
   recorded one is refused (exit 2), and a symlinked store entry is refused.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYMBOLICATE = ROOT / "tools" / "ops" / "symbolicate_crash.py"
CRASH_SITE = "SparkSymbolicationCanaryCrashSite"
FAULT_MARKER = "SPARK_SYMBOLICATION_CANARY_FAULT_LINE"
PROBE_TIMEOUT_SECONDS = 60


class CanaryFailure(Exception):
    pass


def fault_line(source: Path) -> int:
    matches = [number for number, line in enumerate(source.read_text().splitlines(), 1) if FAULT_MARKER in line]
    if len(matches) != 1:
        raise CanaryFailure(f"{source} must mark exactly one fault line with {FAULT_MARKER}")
    return matches[0]


def run_probe(probe: Path, temp_root: Path) -> Path:
    env = dict(os.environ, TMPDIR=str(temp_root))
    completed = subprocess.run(
        [str(probe)],
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

        print(
            f"symbolication canary passed: {CRASH_SITE} at {args.probe_source.name}:{expected_line} "
            f"(build-id {build_id}); wrong build-id and symlinked entry refused"
        )
        return 0
    except (CanaryFailure, subprocess.TimeoutExpired, OSError, ValueError, KeyError) as exc:
        print(f"symbolication canary FAILED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
