#!/usr/bin/env python3
"""Soak the real headless NullRHI host and measure its leak rate and crash count.

Launches SparkEngine exactly as collect_headless_result.py does
(``-headless -game <module> -require-game -test-frames N`` with
``N = duration * 60``) and watches the process from outside while it runs:

* Readiness -- the host flushes ``SPARK_MODULE_READY count=1`` to stdout once
  the game module is initialized, immediately before the 60 Hz loop starts.
  Its absence within ``--startup-timeout`` is a startup hang.
* Heartbeat -- the main thread either sleeps or runs. Progress is either
  its ``voluntary_ctxt_switches`` counter (``/proc/<pid>/task/<pid>/status``;
  an on-budget tick ends in a sleep, so a live loop advances it about 60
  times per second) or its CPU time (utime+stime in
  ``/proc/<pid>/task/<pid>/stat``; a tick over the 16.7 ms budget skips the
  sleep but still burns CPU). A main thread blocked on a lock advances
  neither: no progress for ``--heartbeat-timeout`` seconds is a deadlock
  hang. A thread that is running without ever sleeping looks the same from
  outside whether it is a slow, over-budget loop or a livelock spin, so it is
  never called a hang early. Because the loop is bounded by frame count, a
  spin (or a loop so slow it cannot finish) is instead caught when the
  engine is still running ``1.5 * duration + --teardown-timeout`` seconds
  after module ready.
* Memory -- ``VmRSS`` sampled every ``--sample-interval`` seconds. The leak
  slope is an ordinary least-squares fit of RSS against time over
  ``[ready + warmup, ready + duration]``. The loop runs at most 60 Hz, so it
  cannot finish before ``ready + duration``: the window never includes
  teardown, whose frees would bias the slope downward.
* Exit -- any non-zero status or signal death counts as a crash. A clean exit
  must still carry exactly one valid ``SPARK_HEADLESS_TICK_STATS`` record for
  exactly the requested frame count (parse_tick_stats), proving the loop ran
  to completion on NullRHI with the module loaded.

The run fails on a crash, a hang, invalid exit evidence, too few samples in
the fit window, or a leak slope above ``--max-leak-bytes-per-hour``. That
ceiling is PROVISIONAL (a harness guard, not a budget): nullrhi.soak.leak_rate
stays ``pending_measurement`` with ``budget: null`` in perf-budgets/v1.

``--out`` writes a ``validate_result`` document for ``nullrhi.soak.leak_rate``
and ``nullrhi.soak.crash_count`` on the uncertified ``linux-nullrhi-ci`` row.
Both metrics are defined for the ``headless-soak-1h`` scene, so ``--out`` is
refused for runs shorter than one hour, and the commit SHA must be supplied
externally with ``--expected-sha``. A result is written only for a run that
crashed (a truthful crash_count of 1) or that proved the full requested frame
count through its exit record. A run killed as hung, or one that exited
cleanly without that proof (for example a one-hour soak that quit after 30
seconds), writes no result: a truncated soak is not a measurement. ``--report`` writes the full local
diagnostic record (samples, raw slope, heartbeat gaps) for any duration.

The PERF-100 soak row of record is Windows NullRHI; results from this Linux
harness are precursor evidence and carry no certification claim.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from collect_headless_result import (
    HARDWARE_ROW_ID,
    MAX_STDOUT_BYTES,
    CollectionError,
    TickStats,
    default_module,
    engine_command,
    parse_tick_stats,
)
from validate_budget import _check_sha, load_bounded_json, validate_result

DEFAULT_BUDGET_DIR = Path(__file__).resolve().parents[2] / "perf-budgets" / "v1"
METRIC_LEAK_RATE = "nullrhi.soak.leak_rate"
METRIC_CRASH_COUNT = "nullrhi.soak.crash_count"
TICK_HZ = 60
# nullrhi.soak.* are defined for scene "headless-soak-1h"; a shorter run is
# not a measurement of that scene and may only produce a --report.
MIN_RESULT_DURATION_S = 3600.0
MAX_DURATION_S = 7 * 24 * 3600.0
MIN_FIT_SAMPLES = 10
# Provisional harness ceiling, not a governed budget. It is loose enough to
# absorb allocator noise in a two-minute fit window yet still trips on a
# steady leak of a few KiB per tick (1 KiB/tick at 60 Hz is ~211 MiB/hour).
DEFAULT_MAX_LEAK_BYTES_PER_HOUR = 64 * 1024 * 1024
_READY_RE = re.compile(r"^SPARK_MODULE_READY count=1\r?$", re.MULTILINE)
REPORT_SCHEMA = "spark-nullrhi-soak-report/1"


@dataclass(frozen=True)
class SoakConfig:
    duration_s: float = 120.0
    warmup_s: float = 30.0
    sample_interval_s: float = 1.0
    heartbeat_timeout_s: float = 10.0
    startup_timeout_s: float = 120.0
    teardown_timeout_s: float = 120.0
    max_leak_bytes_per_hour: float = float(DEFAULT_MAX_LEAK_BYTES_PER_HOUR)

    @property
    def frames(self) -> int:
        return round(self.duration_s * TICK_HZ)

    def validate(self) -> None:
        if not math.isfinite(self.duration_s) or not 1.0 <= self.duration_s <= MAX_DURATION_S:
            raise CollectionError(f"--duration must be in [1, {MAX_DURATION_S:g}] seconds")
        for name, value in (("--sample-interval", self.sample_interval_s),
                            ("--heartbeat-timeout", self.heartbeat_timeout_s),
                            ("--startup-timeout", self.startup_timeout_s),
                            ("--teardown-timeout", self.teardown_timeout_s),
                            ("--max-leak-bytes-per-hour", self.max_leak_bytes_per_hour)):
            if not math.isfinite(value) or value <= 0:
                raise CollectionError(f"{name} must be a positive finite number")
        if not math.isfinite(self.warmup_s) or self.warmup_s < 0:
            raise CollectionError("--warmup must be a non-negative finite number")
        if self.heartbeat_timeout_s <= self.sample_interval_s:
            raise CollectionError("--heartbeat-timeout must exceed --sample-interval")
        fit_span = self.duration_s - self.warmup_s
        if fit_span < MIN_FIT_SAMPLES * self.sample_interval_s:
            raise CollectionError(
                f"duration minus warm-up ({fit_span:g}s) leaves room for fewer than {MIN_FIT_SAMPLES} samples "
                f"at --sample-interval {self.sample_interval_s:g}s"
            )


@dataclass
class SoakOutcome:
    frames: int
    exit_status: int | None = None
    crashed: bool = False
    hung: bool = False
    failures: list[str] = field(default_factory=list)
    samples: list[tuple[float, int]] = field(default_factory=list)
    ready_after_s: float | None = None
    max_heartbeat_gap_s: float = 0.0
    slope_bytes_per_hour: float | None = None
    fit_sample_count: int = 0
    stats: TickStats | None = None


def read_main_thread_status(pid: int) -> tuple[int | None, tuple[int, int] | None]:
    """Return (process VmRSS in bytes, main-thread heartbeat).

    The heartbeat is (voluntary context switches, utime+stime clock ticks) of
    the main thread; it changes whenever the thread sleeps or runs. Either
    value is None when unavailable (the process has exited or is a zombie
    awaiting reaping, which has no VmRSS line).
    """
    task = Path(f"/proc/{pid}/task/{pid}")
    try:
        text = (task / "status").read_text(encoding="ascii", errors="replace")
        stat_text = (task / "stat").read_text(encoding="ascii", errors="replace")
    except OSError:
        return None, None
    # comm (field 2) may contain spaces or parentheses; fields after the last
    # ')' start at field 3 (state), so utime/stime (14, 15) are indices 11, 12.
    stat_fields = stat_text.rpartition(")")[2].split()
    cpu_ticks: int | None = None
    if len(stat_fields) > 12 and stat_fields[11].isdigit() and stat_fields[12].isdigit():
        cpu_ticks = int(stat_fields[11]) + int(stat_fields[12])
    rss_bytes: int | None = None
    switches: int | None = None
    for line in text.splitlines():
        if line.startswith("VmRSS:"):
            parts = line.split()
            if len(parts) == 3 and parts[1].isdigit() and parts[2] == "kB":
                rss_bytes = int(parts[1]) * 1024
        elif line.startswith("voluntary_ctxt_switches:"):
            parts = line.split()
            if len(parts) == 2 and parts[1].isdigit():
                switches = int(parts[1])
    heartbeat = None if switches is None or cpu_ticks is None else (switches, cpu_ticks)
    return rss_bytes, heartbeat


def fit_leak_slope(samples: list[tuple[float, int]], window_start_s: float,
                   window_end_s: float) -> tuple[float | None, int]:
    """Least-squares RSS slope in bytes/hour over samples inside the window.

    Returns (None, n) when fewer than MIN_FIT_SAMPLES samples fall inside the
    window or they do not span any time.
    """
    window = [(t, rss) for t, rss in samples if window_start_s <= t <= window_end_s]
    count = len(window)
    if count < MIN_FIT_SAMPLES:
        return None, count
    mean_t = sum(t for t, _ in window) / count
    mean_rss = sum(rss for _, rss in window) / count
    variance = sum((t - mean_t) ** 2 for t, _ in window)
    if variance <= 0.0:
        return None, count
    covariance = sum((t - mean_t) * (rss - mean_rss) for t, rss in window)
    return covariance / variance * 3600.0, count


def _read_bounded(path: Path) -> str:
    with path.open("rb") as stream:
        data = stream.read(MAX_STDOUT_BYTES + 1)
    if len(data) > MAX_STDOUT_BYTES:
        raise CollectionError(f"engine stdout exceeded {MAX_STDOUT_BYTES} bytes")
    return data.decode("utf-8", "replace")


def _kill(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGKILL)
        process.wait()


def describe_exit_status(exit_status: int) -> str:
    """Name a Popen return code; real-time signals have no Signals member."""
    if exit_status >= 0:
        return f"status {exit_status}"
    try:
        return f"signal {signal.Signals(-exit_status).name}"
    except ValueError:
        return f"signal {-exit_status}"


def run_soak(engine: Path, module: Path, config: SoakConfig) -> SoakOutcome:
    """Run one soak and return its outcome; failures are recorded, not raised."""
    if not sys.platform.startswith("linux"):
        raise CollectionError("the soak harness reads /proc and supports the Linux linux-nullrhi-ci row only")
    outcome = SoakOutcome(frames=config.frames)
    command = engine_command(engine, module, config.frames)
    poll_interval_s = min(0.05, config.sample_interval_s / 4)
    exit_deadline_s = config.duration_s * 1.5 + config.teardown_timeout_s

    with tempfile.TemporaryDirectory(prefix="spark-soak-") as tmp:
        stdout_path = Path(tmp) / "stdout.log"
        stderr_path = Path(tmp) / "stderr.log"
        with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
            process = subprocess.Popen(command, cwd=engine.parent, stdin=subprocess.DEVNULL, stdout=stdout_file,
                                       stderr=stderr_file)
        try:
            launched_at = time.monotonic()
            ready_at: float | None = None
            last_heartbeat: tuple[int, int] | None = None
            last_progress_at = launched_at
            next_sample_at = 0.0
            while process.poll() is None:
                now = time.monotonic()
                if ready_at is None:
                    if _READY_RE.search(_read_bounded(stdout_path)):
                        ready_at = now
                        last_progress_at = now
                        outcome.ready_after_s = now - launched_at
                    elif now - launched_at > config.startup_timeout_s:
                        outcome.hung = True
                        outcome.failures.append(
                            f"hang: no SPARK_MODULE_READY within {config.startup_timeout_s:g}s of launch")
                        break
                else:
                    rss_bytes, heartbeat = read_main_thread_status(process.pid)
                    if heartbeat is not None and heartbeat != last_heartbeat:
                        outcome.max_heartbeat_gap_s = max(outcome.max_heartbeat_gap_s, now - last_progress_at)
                        last_heartbeat = heartbeat
                        last_progress_at = now
                    elif now - last_progress_at > config.heartbeat_timeout_s:
                        outcome.hung = True
                        outcome.failures.append(
                            f"hang: main thread neither slept nor ran for {config.heartbeat_timeout_s:g}s "
                            f"at t={now - ready_at:.1f}s after module ready (blocked: deadlock)")
                        break
                    elapsed_s = now - ready_at
                    if rss_bytes is not None and elapsed_s >= next_sample_at:
                        outcome.samples.append((elapsed_s, rss_bytes))
                        next_sample_at = elapsed_s + config.sample_interval_s
                    if elapsed_s > exit_deadline_s:
                        outcome.hung = True
                        outcome.failures.append(
                            f"hang: engine still running {elapsed_s:.0f}s after module ready "
                            f"(limit {exit_deadline_s:g}s for {config.frames} frames): livelock spin, or ticks "
                            "so far over budget that the run cannot finish")
                        break
                time.sleep(poll_interval_s)
        finally:
            _kill(process)

        outcome.exit_status = process.returncode
        stdout_text = _read_bounded(stdout_path)
        stderr_tail = stderr_path.read_bytes()[-4000:].decode("utf-8", "replace")

    if outcome.hung:
        return outcome
    if outcome.exit_status != 0:
        outcome.crashed = True
        how = describe_exit_status(outcome.exit_status)
        outcome.failures.append(f"crash: engine exited with {how}; stderr tail:\n{stderr_tail}")
    else:
        try:
            outcome.stats = parse_tick_stats(stdout_text, config.frames)
        except CollectionError as exc:
            outcome.failures.append(f"invalid exit evidence: {exc}")

    outcome.slope_bytes_per_hour, outcome.fit_sample_count = fit_leak_slope(
        outcome.samples, config.warmup_s, config.duration_s)
    if outcome.slope_bytes_per_hour is None:
        if not outcome.crashed:
            outcome.failures.append(
                f"insufficient RSS samples: {outcome.fit_sample_count} in the fit window "
                f"[{config.warmup_s:g}s, {config.duration_s:g}s], need {MIN_FIT_SAMPLES}")
    elif outcome.slope_bytes_per_hour > config.max_leak_bytes_per_hour:
        outcome.failures.append(
            f"leak: RSS slope {outcome.slope_bytes_per_hour:.0f} B/h exceeds the provisional ceiling "
            f"{config.max_leak_bytes_per_hour:.0f} B/h")
    return outcome


def build_result(outcome: SoakOutcome, commit_sha: str, timestamp: str) -> dict[str, Any]:
    """Assemble the validate_result document for a crashed or fully proven run."""
    if outcome.hung:
        raise CollectionError("a run killed as hung is not a soak measurement")
    if not outcome.crashed and outcome.stats is None:
        raise CollectionError("a clean exit without a tick-stats record for the requested frame count "
                              "is a truncated soak, not a measurement")
    measurements: list[dict[str, Any]] = []
    if outcome.slope_bytes_per_hour is not None:
        # The metric is lower_is_better in [0, inf): a shrinking RSS is no leak.
        measurements.append({"metricId": METRIC_LEAK_RATE, "value": max(0.0, outcome.slope_bytes_per_hour),
                             "unit": "bytes_per_hour", "sampleCount": outcome.fit_sample_count})
    measurements.append({"metricId": METRIC_CRASH_COUNT, "value": 1 if outcome.crashed else 0, "unit": "count",
                         "sampleCount": 1})
    return {"commitSha": commit_sha, "timestamp": timestamp, "hardwareRowId": HARDWARE_ROW_ID,
            "measurements": measurements}


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


def build_report(outcome: SoakOutcome, config: SoakConfig, commit_sha: str | None, timestamp: str) -> dict[str, Any]:
    stats = outcome.stats
    return {
        "schema": REPORT_SCHEMA,
        "evidenceScope": "local-precursor (uncertified linux-nullrhi-ci row; soak row of record is Windows NullRHI)",
        "commitSha": commit_sha,
        "timestamp": timestamp,
        "durationS": config.duration_s,
        "frames": config.frames,
        "warmupS": config.warmup_s,
        "sampleIntervalS": config.sample_interval_s,
        "heartbeatTimeoutS": config.heartbeat_timeout_s,
        "provisionalMaxLeakBytesPerHour": config.max_leak_bytes_per_hour,
        "readyAfterS": outcome.ready_after_s,
        "exitStatus": outcome.exit_status,
        "crashed": outcome.crashed,
        "hung": outcome.hung,
        "maxHeartbeatGapS": outcome.max_heartbeat_gap_s,
        "leakSlopeBytesPerHour": outcome.slope_bytes_per_hour,
        "fitSampleCount": outcome.fit_sample_count,
        "tickStats": None if stats is None else {"frames": stats.frames, "p50Us": stats.p50_us,
                                                 "p99Us": stats.p99_us, "maxUs": stats.max_us,
                                                 "peakRssKib": stats.peak_rss_kib},
        "failures": outcome.failures,
        "rssSamples": [[round(t, 3), rss] for t, rss in outcome.samples],
    }


def _write_json_atomic(document: dict[str, Any], out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(prefix=".soak-", suffix=".json", dir=out.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(temp_name, out)
    except BaseException:
        Path(temp_name).unlink(missing_ok=True)
        raise


def soak(engine: Path, module: Path, config: SoakConfig, *, expected_sha: str | None, out: Path | None,
         report: Path | None, budget_dir: Path) -> SoakOutcome:
    """Validate inputs before launch, run the soak, and write the requested documents."""
    config.validate()
    if out is not None:
        if expected_sha is None:
            raise CollectionError("--out requires --expected-sha (the commit SHA is never inferred)")
        if config.duration_s < MIN_RESULT_DURATION_S:
            raise CollectionError(
                f"--out requires --duration >= {MIN_RESULT_DURATION_S:g}: nullrhi.soak.* measure scene "
                "headless-soak-1h; use --report for shorter runs")
    if expected_sha is not None:
        sha_errors = _check_sha(expected_sha, "expectedSha", "soak")
        if sha_errors:
            raise CollectionError(sha_errors[0])
    for label, path in (("engine", engine), ("module", module)):
        if not path.is_file():
            raise CollectionError(f"{label} {path} is not a file")

    outcome = run_soak(engine.resolve(), module.resolve(), config)
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    commit_sha = expected_sha.lower() if expected_sha is not None else None
    if report is not None:
        _write_json_atomic(build_report(outcome, config, commit_sha, timestamp), report)
    # Only a crash (crash_count=1) or an exit record proving every requested
    # frame is a measurement; hung and truncated runs leave --out unwritten.
    measured = not outcome.hung and (outcome.crashed or outcome.stats is not None)
    if out is not None and commit_sha is not None and measured:
        result = build_result(outcome, commit_sha, timestamp)
        check_against_budget(result, budget_dir, commit_sha)
        _write_json_atomic(result, out)
    return outcome


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    defaults = SoakConfig()
    parser.add_argument("--engine", type=Path, required=True, help="SparkEngine executable")
    parser.add_argument("--module", type=Path, help="game module (default: libSparkGame.so beside the engine)")
    parser.add_argument("--duration", type=float, default=defaults.duration_s,
                        help=f"soak length in seconds of 60 Hz ticks (default {defaults.duration_s:g})")
    parser.add_argument("--warmup", type=float,
                        help="seconds after module ready excluded from the leak fit "
                             "(default: 25%% of --duration, at most 300)")
    parser.add_argument("--sample-interval", type=float, default=defaults.sample_interval_s)
    parser.add_argument("--heartbeat-timeout", type=float, default=defaults.heartbeat_timeout_s)
    parser.add_argument("--startup-timeout", type=float, default=defaults.startup_timeout_s)
    parser.add_argument("--teardown-timeout", type=float, default=defaults.teardown_timeout_s)
    parser.add_argument("--max-leak-bytes-per-hour", type=float, default=defaults.max_leak_bytes_per_hour,
                        help="provisional harness ceiling on the fitted RSS slope (not a budget)")
    parser.add_argument("--expected-sha", help="full commit SHA of the measured build (required with --out)")
    parser.add_argument("--out", type=Path, help="validate_result JSON (requires --duration >= 3600)")
    parser.add_argument("--report", type=Path, help="full diagnostic report JSON (any duration)")
    parser.add_argument("--budget-dir", type=Path, default=DEFAULT_BUDGET_DIR)
    args = parser.parse_args(argv)

    warmup_s = args.warmup if args.warmup is not None else min(300.0, 0.25 * args.duration)
    config = SoakConfig(duration_s=args.duration, warmup_s=warmup_s, sample_interval_s=args.sample_interval,
                        heartbeat_timeout_s=args.heartbeat_timeout, startup_timeout_s=args.startup_timeout,
                        teardown_timeout_s=args.teardown_timeout,
                        max_leak_bytes_per_hour=args.max_leak_bytes_per_hour)
    module = args.module if args.module is not None else default_module(args.engine)
    try:
        outcome = soak(args.engine, module, config, expected_sha=args.expected_sha, out=args.out,
                       report=args.report, budget_dir=args.budget_dir)
    except CollectionError as exc:
        print(f"run_nullrhi_soak: FAIL: {exc}", file=sys.stderr)
        return 1

    slope = "n/a" if outcome.slope_bytes_per_hour is None else f"{outcome.slope_bytes_per_hour:.0f} B/h"
    summary = (f"{config.frames} frames over {config.duration_s:g}s, leak slope {slope} "
               f"({outcome.fit_sample_count} samples), crash_count={1 if outcome.crashed else 0}, "
               f"max heartbeat gap {outcome.max_heartbeat_gap_s:.2f}s")
    if outcome.failures:
        for failure in outcome.failures:
            print(f"run_nullrhi_soak: FAIL: {failure}", file=sys.stderr)
        print(f"run_nullrhi_soak: {summary}", file=sys.stderr)
        return 1
    print(f"run_nullrhi_soak: PASS: {summary}; uncertified row, budgets pending")
    return 0


if __name__ == "__main__":
    sys.exit(main())
