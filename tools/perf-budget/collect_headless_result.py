#!/usr/bin/env python3
"""Measure the real headless NullRHI tick loop and write a perf-budget result.

Runs the production SparkEngine host as ``-headless -game <module>
-require-game -test-frames N``, parses the single ``SPARK_HEADLESS_TICK_STATS``
record the host prints after teardown, and writes one result JSON in the
``validate_result`` schema for the ``linux-nullrhi-ci`` hardware row:

* ``nullrhi.headless.tick_time.p50`` / ``.p99`` -- pre-sleep tick work time (ms)
* ``nullrhi.headless.memory.peak_rss`` -- peak RSS (megabytes, 2**20 bytes)

Peak RSS is the ``peak_rss_kib`` field of that record: the host reads its own
``VmHWM`` from ``/proc/self/status`` after teardown. exec gives the engine a
fresh address space, so ``VmHWM`` covers only the engine. The ``ru_maxrss``
returned by ``wait4`` is NOT the engine's peak: Linux keeps it per signal
group and folds the pre-exec image's high-water mark into it at exec, so it is
``max(collector RSS, engine peak)``. It is used only as an upper-bound cross
check on the reported value.

The commit SHA is never inferred: ``--expected-sha`` must be supplied by the
caller (the workflow or the person running it). The result carries no
certification claim; ``compare_results.py`` keeps reporting it as
NON-AUTHORITATIVE until the hardware row is certified and budgets are approved.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from validate_budget import _check_sha, load_bounded_json, validate_result

HARDWARE_ROW_ID = "linux-nullrhi-ci"
DEFAULT_BUDGET_DIR = Path(__file__).resolve().parents[2] / "perf-budgets" / "v1"
METRIC_P50 = "nullrhi.headless.tick_time.p50"
METRIC_P99 = "nullrhi.headless.tick_time.p99"
METRIC_PEAK_RSS = "nullrhi.headless.memory.peak_rss"
MAX_FRAMES = 1_000_000
# Mirrors HeadlessTickStats::MAX_TRACKED_US: the host can never print more.
MAX_TRACKED_US = (1 << 33) - 1
MAX_STDOUT_BYTES = 64 * 1024 * 1024

RECORD_PREFIX = "SPARK_HEADLESS_TICK_STATS"
_UINT = r"(0|[1-9][0-9]{0,19})"
_TICK_STATS_RE = re.compile(
    rf"{RECORD_PREFIX} backend=(null|none) frames={_UINT} p50_us={_UINT} "
    rf"p99_us={_UINT} max_us={_UINT} peak_rss_kib={_UINT}"
)
_MODULE_READY_RE = re.compile(r"SPARK_MODULE_READY count=(0|[1-9][0-9]{0,9})")


class CollectionError(Exception):
    """The run or its output cannot be turned into trustworthy evidence."""


@dataclass(frozen=True)
class TickStats:
    frames: int
    p50_us: int
    p99_us: int
    max_us: int
    peak_rss_kib: int


def parse_tick_stats(stdout_text: str, expected_frames: int) -> TickStats:
    """Parse and cross-check the host's stdout records.

    Fails closed on a missing, duplicated, malformed, or out-of-order record,
    a non-NullRHI backend, a frame count other than the one requested, or
    percentiles that are not ordered ``p50 <= p99 <= max``.
    """
    lines = stdout_text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    ready_indices: list[int] = []
    stats_matches: list[tuple[int, re.Match[str]]] = []
    for index, line in enumerate(lines):
        ready = _MODULE_READY_RE.fullmatch(line)
        if ready:
            if ready.group(1) != "1":
                raise CollectionError(f"module-ready record reports {line!r}, expected exactly one module")
            ready_indices.append(index)
        if not line.startswith(RECORD_PREFIX):
            continue
        match = _TICK_STATS_RE.fullmatch(line)
        if match is None:
            raise CollectionError(f"malformed {RECORD_PREFIX} record: {line[:200]!r}")
        stats_matches.append((index, match))

    if len(ready_indices) != 1:
        raise CollectionError(
            f"found {len(ready_indices)} SPARK_MODULE_READY records, expected exactly 1 (game module not proven loaded)"
        )
    if len(stats_matches) != 1:
        raise CollectionError(f"found {len(stats_matches)} {RECORD_PREFIX} records, expected exactly 1")
    stats_index, match = stats_matches[0]
    if stats_index < ready_indices[0]:
        raise CollectionError(f"{RECORD_PREFIX} was printed before the module became ready")

    backend = match.group(1)
    frames, p50_us, p99_us, max_us, peak_rss_kib = (int(match.group(group)) for group in range(2, 7))
    if backend != "null":
        raise CollectionError(f"tick loop ran with backend={backend}, expected backend=null")
    if frames != expected_frames:
        raise CollectionError(f"record reports frames={frames}, expected exactly {expected_frames}")
    if max(p50_us, p99_us, max_us) > MAX_TRACKED_US:
        raise CollectionError("record reports a tick time above the host histogram range")
    if not p50_us <= p99_us <= max_us:
        raise CollectionError(f"percentiles are not ordered: p50={p50_us} p99={p99_us} max={max_us}")
    if peak_rss_kib == 0:
        raise CollectionError("host reported peak_rss_kib=0 (its own peak RSS could not be measured)")
    return TickStats(frames=frames, p50_us=p50_us, p99_us=p99_us, max_us=max_us, peak_rss_kib=peak_rss_kib)


def check_peak_rss_bound(stats: TickStats, wait4_maxrss_kib: int) -> None:
    """Reject a host-reported peak above the kernel's ``ru_maxrss`` upper bound.

    ``ru_maxrss`` is at least the engine's own ``VmHWM`` (it may also include
    the pre-exec collector image), so a larger self-reported value is not a
    real measurement of the process that ran.
    """
    if stats.peak_rss_kib > wait4_maxrss_kib:
        raise CollectionError(
            f"host reported peak_rss_kib={stats.peak_rss_kib}, above the kernel ru_maxrss bound {wait4_maxrss_kib}"
        )


def build_result(stats: TickStats, commit_sha: str, timestamp: str) -> dict[str, Any]:
    """Assemble the result document (exactly the validate_result schema keys)."""
    peak_rss_kib = stats.peak_rss_kib
    if peak_rss_kib <= 0:
        raise CollectionError(f"peak RSS {peak_rss_kib} KiB is not a positive measurement")
    return {
        "commitSha": commit_sha,
        "timestamp": timestamp,
        "hardwareRowId": HARDWARE_ROW_ID,
        "measurements": [
            {"metricId": METRIC_P50, "value": stats.p50_us / 1000.0, "unit": "ms", "sampleCount": stats.frames},
            {"metricId": METRIC_P99, "value": stats.p99_us / 1000.0, "unit": "ms", "sampleCount": stats.frames},
            {"metricId": METRIC_PEAK_RSS, "value": peak_rss_kib / 1024.0, "unit": "megabytes", "sampleCount": 1},
        ],
    }


def check_against_budget(result: dict[str, Any], budget_dir: Path, expected_sha: str) -> None:
    """Validate the result schema and that each metric matches its governed definition."""
    hardware, hardware_errors = load_bounded_json(budget_dir / "hardware.json", "hardware.json",
                                                  trusted_root=budget_dir)
    budget, budget_errors = load_bounded_json(budget_dir / "budget.json", "budget.json", trusted_root=budget_dir)
    if hardware_errors or budget_errors:
        raise CollectionError("; ".join(hardware_errors + budget_errors))
    hardware_ids = [row.get("id") for row in hardware.get("rows", []) if isinstance(row, dict)]
    errors = validate_result(result, hardware_ids, expected_sha=expected_sha)
    metrics = {m.get("id"): m for m in budget.get("metrics", []) if isinstance(m, dict)}
    for measurement in result["measurements"]:
        metric = metrics.get(measurement["metricId"])
        if metric is None:
            errors.append(f"{measurement['metricId']}: not defined in budget.json")
        elif metric.get("unit") != measurement["unit"] or metric.get("hardwareRowId") != result["hardwareRowId"]:
            errors.append(f"{measurement['metricId']}: unit/hardware row disagree with budget.json")
    if errors:
        raise CollectionError("result failed validation: " + "; ".join(errors))


def run_engine(engine: Path, module: Path, frames: int, timeout_s: float) -> tuple[str, int]:
    """Run the headless host to completion; return (stdout text, wait4 ru_maxrss in KiB).

    The ru_maxrss value is only an upper bound on the engine's peak (see the
    module docstring); the measurement itself comes from the host record.
    """
    if not sys.platform.startswith("linux"):
        raise CollectionError("peak-RSS collection is implemented for the Linux linux-nullrhi-ci row only")
    command = [str(engine), "-headless", "-game", str(module), "-require-game", "-test-frames", str(frames)]
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        process = subprocess.Popen(command, cwd=engine.parent, stdin=subprocess.DEVNULL, stdout=stdout_file,
                                   stderr=stderr_file)
        deadline = time.monotonic() + timeout_s
        while True:
            # wait4 reaps the child and returns its rusage; Popen.wait would
            # discard ru_maxrss (needed for the upper-bound cross-check).
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid == process.pid:
                break
            if time.monotonic() > deadline:
                process.send_signal(signal.SIGKILL)
                os.wait4(process.pid, 0)
                process.returncode = -signal.SIGKILL
                raise CollectionError(f"engine did not exit within {timeout_s:g}s")
            time.sleep(0.05)
        process.returncode = os.waitstatus_to_exitcode(status)
        stdout_file.seek(0)
        stdout_bytes = stdout_file.read(MAX_STDOUT_BYTES + 1)
        stderr_file.seek(0)
        stderr_tail = stderr_file.read()[-4000:].decode("utf-8", "replace")
    if process.returncode != 0:
        raise CollectionError(f"engine exited with status {process.returncode}; stderr tail:\n{stderr_tail}")
    if len(stdout_bytes) > MAX_STDOUT_BYTES:
        raise CollectionError(f"engine stdout exceeded {MAX_STDOUT_BYTES} bytes")
    return stdout_bytes.decode("utf-8", "replace"), int(usage.ru_maxrss)


def default_module(engine: Path) -> Path:
    return engine.parent / "libSparkGame.so"


def collect(engine: Path, module: Path, frames: int, expected_sha: str, out: Path, budget_dir: Path,
            timeout_s: float) -> dict[str, Any]:
    """Run, parse, validate, and atomically write the result; return it."""
    sha_errors = _check_sha(expected_sha, "expectedSha", "collector")
    if sha_errors:
        raise CollectionError(sha_errors[0])
    if not 1 <= frames <= MAX_FRAMES:
        raise CollectionError(f"--frames must be in [1, {MAX_FRAMES}]")
    for label, path in (("engine", engine), ("module", module)):
        if not path.is_file():
            raise CollectionError(f"{label} {path} is not a file")

    stdout_text, wait4_maxrss_kib = run_engine(engine.resolve(), module.resolve(), frames, timeout_s)
    stats = parse_tick_stats(stdout_text, frames)
    check_peak_rss_bound(stats, wait4_maxrss_kib)
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    result = build_result(stats, expected_sha.lower(), timestamp)
    check_against_budget(result, budget_dir, expected_sha)

    out.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(prefix=".headless-result-", suffix=".json", dir=out.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(temp_name, out)
    except BaseException:
        Path(temp_name).unlink(missing_ok=True)
        raise
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", type=Path, required=True, help="SparkEngine executable")
    parser.add_argument("--module", type=Path, help="game module (default: libSparkGame.so beside the engine)")
    parser.add_argument("--frames", type=int, default=600, help="ticks to measure (default 600, ~10 s)")
    parser.add_argument("--expected-sha", required=True, help="full commit SHA of the measured build")
    parser.add_argument("--out", type=Path, required=True, help="result JSON path")
    parser.add_argument("--budget-dir", type=Path, default=DEFAULT_BUDGET_DIR)
    parser.add_argument("--timeout", type=float, default=300.0, help="engine run timeout in seconds")
    args = parser.parse_args(argv)
    module = args.module if args.module is not None else default_module(args.engine)
    try:
        result = collect(args.engine, module, args.frames, args.expected_sha, args.out, args.budget_dir,
                         args.timeout)
    except CollectionError as exc:
        print(f"collect_headless_result: FAIL: {exc}", file=sys.stderr)
        return 1
    summary = ", ".join(f"{m['metricId']}={m['value']:g}{m['unit']}" for m in result["measurements"])
    print(f"collect_headless_result: wrote {args.out} ({summary}); uncertified row, budgets pending")
    return 0


if __name__ == "__main__":
    sys.exit(main())
