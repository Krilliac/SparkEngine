#!/usr/bin/env python3
"""OPS-110: soak the real SparkServer process and assert bounded memory and tick latency.

The harness launches SparkServer exactly as an operator would host one game
module (``--module <lib> --health-file <path> --bind-address loopback``) and
watches it from outside for ``--duration`` seconds after it first reports
``ready``:

* Readiness -- the atomically replaced health file must report ``live`` and
  ``ready`` with a non-empty ``gameModule`` within ``--startup-timeout``.
* Tick progress -- the health ``ticks`` counter is sampled every
  ``--sample-interval`` seconds. It must never decrease while the server is
  live, must not stall for ``--stall-timeout`` seconds, and must average at
  least ``--min-tick-rate-fraction`` of the requested tick rate.
* Memory -- ``VmRSS`` from ``/proc/<pid>/status`` is sampled on the same
  cadence. The least-squares RSS slope over ``[warmup, duration]`` must stay
  under ``--max-rss-slope-bytes-per-hour`` and the peak-minus-first RSS in
  that window under ``--max-rss-growth-bytes``. The health surface must also
  carry a non-null ``rssBytes`` so the in-process metric is proven live.
* Tick latency -- the server's own bounded histogram (``tickP95Us``,
  ``tickP99Us`` in the last live health snapshot before the stop request)
  must be at or under ``--max-tick-p95-us`` / ``--max-tick-p99-us``.
* Drain and exit -- the harness sends SIGTERM. The server's stdout health
  stream must then show a ``live, draining, ready=false`` snapshot before any
  ``stopping`` snapshot and end on ``live=false, ready=false``; the process
  must exit with status 0 within ``--stop-timeout`` seconds, and no process
  may remain in the server's session (it is launched as a session leader, so
  every descendant that did not detach itself is found by session id).

Every budget above is PROVISIONAL: a harness guard with headroom for
shared-runner noise, not a governed SLO. OPS-110's versioned p95/p99 budgets
remain open. A pass here is local precursor evidence, not certification.

The summary (``--summary``) is a machine-readable JSON document labelled with
the externally supplied ``--expected-sha``. The SHA is never inferred; when it
is supplied the server's reported build commit must equal it, so a summary
cannot be attributed to a different build than the one that ran.

Linux only: memory and process accounting read ``/proc``.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SUMMARY_SCHEMA = "spark-server-soak-summary/1"
EVIDENCE_SCOPE = "local-precursor: provisional budgets on an uncertified host; not release certification"
MAX_DURATION_S = 7 * 24 * 3600.0
MIN_FIT_SAMPLES = 10
MAX_HEALTH_BYTES = 64 * 1024
MAX_STDOUT_LINE_BYTES = 64 * 1024
_SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")

# Provisional harness ceilings (not SLOs). The slope ceiling still trips on a
# steady leak of a few KiB per tick (1 KiB/tick at 60 Hz is ~211 MiB/hour).
DEFAULT_MAX_RSS_SLOPE_BYTES_PER_HOUR = 64 * 1024 * 1024
DEFAULT_MAX_RSS_GROWTH_BYTES = 32 * 1024 * 1024
# A 60 Hz tick has a 16.7 ms budget; p99 must fit inside it and p95 inside half.
DEFAULT_MAX_TICK_P95_US = 8000
DEFAULT_MAX_TICK_P99_US = 16000


class SoakError(Exception):
    """Invalid harness input or an environment the harness cannot run in."""


@dataclass(frozen=True)
class SoakConfig:
    duration_s: float = 60.0
    warmup_s: float = 15.0
    sample_interval_s: float = 1.0
    status_interval_ms: int = 250
    tick_rate_hz: float = 60.0
    startup_timeout_s: float = 60.0
    stall_timeout_s: float = 10.0
    stop_timeout_s: float = 30.0
    min_tick_rate_fraction: float = 0.5
    max_rss_slope_bytes_per_hour: float = float(DEFAULT_MAX_RSS_SLOPE_BYTES_PER_HOUR)
    max_rss_growth_bytes: int = DEFAULT_MAX_RSS_GROWTH_BYTES
    max_tick_p95_us: int = DEFAULT_MAX_TICK_P95_US
    max_tick_p99_us: int = DEFAULT_MAX_TICK_P99_US

    def validate(self) -> None:
        if not math.isfinite(self.duration_s) or not 1.0 <= self.duration_s <= MAX_DURATION_S:
            raise SoakError(f"--duration must be in [1, {MAX_DURATION_S:g}] seconds")
        for name, value in (("--sample-interval", self.sample_interval_s),
                            ("--startup-timeout", self.startup_timeout_s),
                            ("--stall-timeout", self.stall_timeout_s),
                            ("--stop-timeout", self.stop_timeout_s),
                            ("--max-rss-slope-bytes-per-hour", self.max_rss_slope_bytes_per_hour)):
            if not math.isfinite(value) or value <= 0:
                raise SoakError(f"{name} must be a positive finite number")
        if not math.isfinite(self.warmup_s) or self.warmup_s < 0:
            raise SoakError("--warmup must be a non-negative finite number")
        if not math.isfinite(self.tick_rate_hz) or not 1.0 <= self.tick_rate_hz <= 1000.0:
            raise SoakError("--tick-rate must be between 1 and 1000 (SparkServer's accepted range)")
        if not 100 <= self.status_interval_ms <= 3_600_000:
            raise SoakError("--status-interval-ms must be between 100 and 3600000 (SparkServer's accepted range)")
        if not math.isfinite(self.min_tick_rate_fraction) or not 0.0 < self.min_tick_rate_fraction <= 1.0:
            raise SoakError("--min-tick-rate-fraction must be in (0, 1]")
        for name, value in (("--max-rss-growth-bytes", self.max_rss_growth_bytes),
                            ("--max-tick-p95-us", self.max_tick_p95_us),
                            ("--max-tick-p99-us", self.max_tick_p99_us)):
            if value <= 0:
                raise SoakError(f"{name} must be positive")
        # The health file only changes once per status interval, so a stall
        # window shorter than a few intervals would flag a healthy server.
        if self.stall_timeout_s < max(2 * self.sample_interval_s, 3 * self.status_interval_ms / 1000.0):
            raise SoakError("--stall-timeout must cover two sample intervals and three status intervals")
        fit_span = self.duration_s - self.warmup_s
        if fit_span < MIN_FIT_SAMPLES * self.sample_interval_s:
            raise SoakError(
                f"duration minus warm-up ({fit_span:g}s) leaves room for fewer than {MIN_FIT_SAMPLES} samples "
                f"at --sample-interval {self.sample_interval_s:g}s")


@dataclass
class DrainEvidence:
    """What the server's stdout health stream showed after the stop request."""

    records: int = 0
    draining_ready_false: bool = False
    ready_after_drain: bool = False
    stopping_before_drain: bool = False
    final_live: bool | None = None
    final_ready: bool | None = None


@dataclass
class SoakOutcome:
    failures: list[str] = field(default_factory=list)
    ready_after_s: float | None = None
    identity: dict[str, str] = field(default_factory=dict)
    samples: list[tuple[float, int, int]] = field(default_factory=list)  # (t, rssBytes, ticks)
    health_rss_bytes: int | None = None
    last_live_health: dict[str, Any] | None = None
    rss_slope_bytes_per_hour: float | None = None
    rss_growth_bytes: int | None = None
    fit_sample_count: int = 0
    achieved_tick_rate_hz: float | None = None
    max_tick_stall_s: float = 0.0
    stop_after_s: float | None = None
    exit_status: int | None = None
    drain: DrainEvidence = field(default_factory=DrainEvidence)
    leaked_processes: list[dict[str, Any]] = field(default_factory=list)
    stderr_tail: str = ""


def fit_rss_slope(samples: list[tuple[float, int, int]], window_start_s: float,
                  window_end_s: float) -> tuple[float | None, int | None, int]:
    """Return (least-squares RSS slope in bytes/hour, peak-minus-first RSS, samples in window).

    Slope and growth are None when fewer than MIN_FIT_SAMPLES samples fall in
    the window or the samples do not span any time.
    """
    window = [(t, rss) for t, rss, _ in samples if window_start_s <= t <= window_end_s]
    count = len(window)
    if count < MIN_FIT_SAMPLES:
        return None, None, count
    mean_t = sum(t for t, _ in window) / count
    mean_rss = sum(rss for _, rss in window) / count
    variance = sum((t - mean_t) ** 2 for t, _ in window)
    if variance <= 0.0:
        return None, None, count
    covariance = sum((t - mean_t) * (rss - mean_rss) for t, rss in window)
    growth = max(rss for _, rss in window) - window[0][1]
    return covariance / variance * 3600.0, growth, count


def read_process_rss(pid: int) -> int | None:
    """VmRSS of a live process in bytes, or None once it has exited or is a zombie."""
    try:
        text = Path(f"/proc/{pid}/status").read_text(encoding="ascii", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith("VmRSS:"):
            parts = line.split()
            if len(parts) == 3 and parts[1].isdigit() and parts[2] == "kB":
                return int(parts[1]) * 1024
    return None


def session_members(session_id: int) -> list[dict[str, Any]]:
    """Every live process whose session id is ``session_id`` (zombies excluded)."""
    members: list[dict[str, Any]] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat_text = (entry / "stat").read_text(encoding="ascii", errors="replace")
        except OSError:
            continue
        # comm (field 2) may contain spaces or parentheses; fields after the
        # last ')' start at field 3 (state), so session (field 6) is index 3.
        head, _, tail = stat_text.rpartition(")")
        fields = tail.split()
        if len(fields) < 4 or fields[0] == "Z" or not fields[3].isdigit():
            continue
        if int(fields[3]) == session_id:
            members.append({"pid": int(entry.name), "comm": head.partition("(")[2], "state": fields[0]})
    return members


def read_health(path: Path) -> dict[str, Any] | None:
    """The current health snapshot, or None when absent, oversized, or mid-replace."""
    try:
        with path.open("rb") as stream:
            data = stream.read(MAX_HEALTH_BYTES + 1)
    except OSError:
        return None
    if len(data) > MAX_HEALTH_BYTES:
        return None
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def is_health_record(value: Any) -> bool:
    return (isinstance(value, dict) and isinstance(value.get("live"), bool) and isinstance(value.get("ready"), bool)
            and isinstance(value.get("draining"), bool) and isinstance(value.get("stopping"), bool))


def scan_drain_sequence(stdout_path: Path) -> DrainEvidence:
    """Check the post-stop health records in the server's stdout stream.

    SparkServer prints every published snapshot as one JSON line. Records from
    the first ``draining`` snapshot on are the stop sequence. The stream is
    read line by line with a per-line cap, so a long soak's log never has to
    fit in memory.
    """
    evidence = DrainEvidence()
    in_stop_sequence = False
    with stdout_path.open("rb") as stream:
        for raw in stream:
            if len(raw) > MAX_STDOUT_LINE_BYTES or not raw.startswith(b"{"):
                continue
            try:
                record = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if not is_health_record(record):
                continue
            if not in_stop_sequence:
                if record["stopping"] and record["live"]:
                    evidence.stopping_before_drain = True
                if not record["draining"]:
                    continue
                in_stop_sequence = True
            evidence.records += 1
            if record["live"] and record["draining"] and not record["ready"]:
                evidence.draining_ready_false = True
            if record["ready"]:
                evidence.ready_after_drain = True
            evidence.final_live = record["live"]
            evidence.final_ready = record["ready"]
    return evidence


def reserve_udp_port() -> int:
    """An ephemeral loopback UDP port that was free a moment ago."""
    reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        reservation.bind(("127.0.0.1", 0))
        return int(reservation.getsockname()[1])
    finally:
        reservation.close()


def server_command(server: Path, module: Path, port: int, health_file: Path, config: SoakConfig) -> list[str]:
    return [str(server), "--module", str(module), "--port", str(port), "--bind-address", "loopback",
            "--no-lan-broadcast", "--map", "soak", "--name", "OPS-110 soak", "--max-clients", "8",
            "--tick-rate", f"{config.tick_rate_hz:g}", "--health-file", str(health_file),
            "--status-interval-ms", str(config.status_interval_ms)]


def describe_exit_status(exit_status: int) -> str:
    if exit_status >= 0:
        return f"status {exit_status}"
    try:
        return f"signal {signal.Signals(-exit_status).name}"
    except ValueError:
        return f"signal {-exit_status}"


def _tail(path: Path, limit: int = 4000) -> str:
    try:
        with path.open("rb") as stream:
            stream.seek(0, os.SEEK_END)
            size = stream.tell()
            stream.seek(max(0, size - limit))
            return stream.read().decode("utf-8", "replace")
    except OSError:
        return ""


def _kill_session(process: subprocess.Popen[bytes]) -> None:
    """Kill the server and everything left in its session so a failed soak leaks nothing."""
    for member in session_members(process.pid):
        try:
            os.kill(member["pid"], signal.SIGKILL)
        except OSError:
            pass
    if process.poll() is None:
        process.kill()
    process.wait()


def _wait_ready(process: subprocess.Popen[bytes], health_file: Path, config: SoakConfig,
                outcome: SoakOutcome, launched_at: float) -> bool:
    while process.poll() is None:
        health = read_health(health_file)
        if health is not None and health.get("live") is True and health.get("ready") is True and \
                isinstance(health.get("gameModule"), str) and health["gameModule"]:
            outcome.ready_after_s = time.monotonic() - launched_at
            outcome.identity = {key: str(health.get(key, "")) for key in ("version", "commit", "treeState",
                                                                        "gameModule")}
            return True
        if time.monotonic() - launched_at > config.startup_timeout_s:
            outcome.failures.append(f"startup: no live+ready health snapshot within {config.startup_timeout_s:g}s")
            return False
        time.sleep(0.05)
    outcome.failures.append(f"startup: server exited with {describe_exit_status(process.returncode)} before ready")
    return False


def _sample_until_duration(process: subprocess.Popen[bytes], health_file: Path, config: SoakConfig,
                           outcome: SoakOutcome) -> bool:
    """Sample RSS and ticks for the soak duration; return False on an early failure."""
    ready_at = time.monotonic()
    last_ticks: int | None = None
    last_progress_at = ready_at
    next_sample_at = 0.0
    while True:
        now = time.monotonic()
        elapsed_s = now - ready_at
        if process.poll() is not None:
            outcome.failures.append(
                f"crash: server exited with {describe_exit_status(process.returncode)} at t={elapsed_s:.1f}s")
            return False
        if elapsed_s >= config.duration_s:
            return True
        health = read_health(health_file)
        if health is not None and health.get("live") is True:
            ticks = health.get("ticks")
            if not isinstance(ticks, int) or isinstance(ticks, bool) or ticks < 0:
                outcome.failures.append(f"health: ticks is not a non-negative integer: {ticks!r}")
                return False
            if not health.get("ready"):
                outcome.failures.append(f"health: server reported ready=false at t={elapsed_s:.1f}s before any stop")
                return False
            if last_ticks is not None and ticks < last_ticks:
                outcome.failures.append(f"ticks: counter went backwards ({last_ticks} -> {ticks}) at t={elapsed_s:.1f}s")
                return False
            if last_ticks is None or ticks > last_ticks:
                outcome.max_tick_stall_s = max(outcome.max_tick_stall_s, now - last_progress_at)
                last_ticks = ticks
                last_progress_at = now
            outcome.last_live_health = health
            rss_value = health.get("rssBytes")
            if isinstance(rss_value, int) and not isinstance(rss_value, bool) and rss_value > 0:
                outcome.health_rss_bytes = rss_value
        if now - last_progress_at > config.stall_timeout_s:
            outcome.max_tick_stall_s = max(outcome.max_tick_stall_s, now - last_progress_at)
            outcome.failures.append(
                f"stall: ticks did not advance for {config.stall_timeout_s:g}s at t={elapsed_s:.1f}s (hang)")
            return False
        if elapsed_s >= next_sample_at and last_ticks is not None:
            rss_bytes = read_process_rss(process.pid)
            if rss_bytes is not None:
                outcome.samples.append((elapsed_s, rss_bytes, last_ticks))
            next_sample_at = elapsed_s + config.sample_interval_s
        time.sleep(min(0.05, config.sample_interval_s / 4))


def _evaluate_run(config: SoakConfig, outcome: SoakOutcome) -> None:
    """Apply the memory, tick-rate, and latency budgets to a run that lasted the full duration."""
    slope, growth, count = fit_rss_slope(outcome.samples, config.warmup_s, config.duration_s)
    outcome.rss_slope_bytes_per_hour, outcome.rss_growth_bytes, outcome.fit_sample_count = slope, growth, count
    if slope is None or growth is None:
        outcome.failures.append(f"memory: {count} RSS samples in the fit window [{config.warmup_s:g}s, "
                                f"{config.duration_s:g}s], need {MIN_FIT_SAMPLES}")
    else:
        if slope > config.max_rss_slope_bytes_per_hour:
            outcome.failures.append(f"memory: RSS slope {slope:.0f} B/h exceeds the provisional ceiling "
                                    f"{config.max_rss_slope_bytes_per_hour:.0f} B/h")
        if growth > config.max_rss_growth_bytes:
            outcome.failures.append(f"memory: RSS grew {growth} B in the fit window, over the provisional ceiling "
                                    f"{config.max_rss_growth_bytes} B")
    if outcome.health_rss_bytes is None:
        outcome.failures.append("health: rssBytes was never a positive integer while live")

    if len(outcome.samples) >= 2:
        (first_t, _, first_ticks), (last_t, _, last_ticks) = outcome.samples[0], outcome.samples[-1]
        if last_t > first_t:
            outcome.achieved_tick_rate_hz = (last_ticks - first_ticks) / (last_t - first_t)
    required_rate = config.tick_rate_hz * config.min_tick_rate_fraction
    if outcome.achieved_tick_rate_hz is None or outcome.achieved_tick_rate_hz < required_rate:
        outcome.failures.append(f"ticks: achieved {outcome.achieved_tick_rate_hz!r} Hz, need at least "
                                f"{required_rate:g} Hz ({config.min_tick_rate_fraction:g} x {config.tick_rate_hz:g})")

    health = outcome.last_live_health or {}
    for key, ceiling in (("tickP95Us", config.max_tick_p95_us), ("tickP99Us", config.max_tick_p99_us)):
        value = health.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            outcome.failures.append(f"latency: health {key} is missing or invalid: {value!r}")
        elif value > ceiling:
            outcome.failures.append(f"latency: {key} {value} us exceeds the provisional budget {ceiling} us")
    samples = health.get("tickSamples")
    if not isinstance(samples, int) or isinstance(samples, bool) or samples <= 0:
        outcome.failures.append(f"latency: health tickSamples is {samples!r}; the percentiles cover no ticks")


def _stop_and_verify(process: subprocess.Popen[bytes], stdout_path: Path, health_file: Path,
                     config: SoakConfig, outcome: SoakOutcome) -> None:
    stop_sent_at = time.monotonic()
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=config.stop_timeout_s)
    except subprocess.TimeoutExpired:
        outcome.failures.append(f"stop: server still running {config.stop_timeout_s:g}s after SIGTERM")
        return
    outcome.stop_after_s = time.monotonic() - stop_sent_at
    outcome.exit_status = process.returncode
    if process.returncode != 0:
        outcome.failures.append(f"stop: server exited with {describe_exit_status(process.returncode)} after SIGTERM, "
                                "expected a graceful status 0")
    outcome.leaked_processes = session_members(process.pid)
    if outcome.leaked_processes:
        outcome.failures.append(f"stop: {len(outcome.leaked_processes)} process(es) outlived the server in its "
                                f"session: {outcome.leaked_processes}")

    drain = scan_drain_sequence(stdout_path)
    outcome.drain = drain
    if drain.stopping_before_drain:
        outcome.failures.append("drain: a stopping snapshot was published before any draining snapshot")
    if not drain.draining_ready_false:
        outcome.failures.append("drain: no live+draining snapshot with ready=false was published after SIGTERM")
    if drain.ready_after_drain:
        outcome.failures.append("drain: the server reported ready=true after it began draining")
    if drain.final_live is not False or drain.final_ready is not False:
        outcome.failures.append(f"drain: the last published snapshot was live={drain.final_live}, "
                                f"ready={drain.final_ready}; expected live=false, ready=false")
    final_file = read_health(health_file)
    if final_file is None or final_file.get("live") is not False or final_file.get("ready") is not False:
        outcome.failures.append(f"drain: the health file does not end on live=false, ready=false: {final_file!r}")


def run_soak(server: Path, module: Path, config: SoakConfig, *, expected_sha: str | None = None,
             work_dir: Path | None = None) -> SoakOutcome:
    """Run one soak and return its outcome; soak failures are recorded, not raised."""
    if not sys.platform.startswith("linux"):
        raise SoakError("the server soak harness reads /proc and supports Linux only")
    outcome = SoakOutcome()
    with tempfile.TemporaryDirectory(prefix="spark-server-soak-") as tmp:
        root = work_dir if work_dir is not None else Path(tmp)
        root.mkdir(parents=True, exist_ok=True)
        health_file = root / "server-health.json"
        health_file.unlink(missing_ok=True)
        stdout_path, stderr_path = root / "server-stdout.log", root / "server-stderr.log"
        command = server_command(server, module, reserve_udp_port(), health_file, config)
        with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
            # A session leader of its own, so its descendants can be found by
            # session id after it exits and killed as a unit on failure.
            process = subprocess.Popen(command, cwd=server.parent, stdin=subprocess.DEVNULL, stdout=stdout_file,
                                       stderr=stderr_file, start_new_session=True)
        try:
            launched_at = time.monotonic()
            if _wait_ready(process, health_file, config, outcome, launched_at):
                reported = outcome.identity.get("commit", "")
                if expected_sha is not None and reported.lower() != expected_sha.lower():
                    outcome.failures.append(f"identity: server reports commit {reported!r}, "
                                            f"expected {expected_sha.lower()!r}")
                if _sample_until_duration(process, health_file, config, outcome):
                    _evaluate_run(config, outcome)
                    _stop_and_verify(process, stdout_path, health_file, config, outcome)
        finally:
            _kill_session(process)
            outcome.stderr_tail = _tail(stderr_path)
    return outcome


def build_summary(outcome: SoakOutcome, config: SoakConfig, commit_sha: str | None, timestamp: str) -> dict[str, Any]:
    health = outcome.last_live_health or {}
    rss_values = [rss for _, rss, _ in outcome.samples]
    return {
        "schema": SUMMARY_SCHEMA,
        "verdict": "fail" if outcome.failures else "pass",
        "evidenceScope": EVIDENCE_SCOPE,
        "commitSha": commit_sha,
        "timestamp": timestamp,
        "server": outcome.identity,
        "config": {"durationS": config.duration_s, "warmupS": config.warmup_s,
                   "sampleIntervalS": config.sample_interval_s, "statusIntervalMs": config.status_interval_ms,
                   "tickRateHz": config.tick_rate_hz},
        "provisionalBudgets": {"maxRssSlopeBytesPerHour": config.max_rss_slope_bytes_per_hour,
                               "maxRssGrowthBytes": config.max_rss_growth_bytes,
                               "maxTickP95Us": config.max_tick_p95_us, "maxTickP99Us": config.max_tick_p99_us,
                               "minTickRateFraction": config.min_tick_rate_fraction,
                               "stallTimeoutS": config.stall_timeout_s, "stopTimeoutS": config.stop_timeout_s},
        "measurements": {
            "readyAfterS": outcome.ready_after_s,
            "sampleCount": len(outcome.samples),
            "fitSampleCount": outcome.fit_sample_count,
            "rssFirstBytes": rss_values[0] if rss_values else None,
            "rssPeakBytes": max(rss_values) if rss_values else None,
            "rssSlopeBytesPerHour": outcome.rss_slope_bytes_per_hour,
            "rssGrowthBytes": outcome.rss_growth_bytes,
            "healthRssBytes": outcome.health_rss_bytes,
            "achievedTickRateHz": outcome.achieved_tick_rate_hz,
            "maxTickStallS": outcome.max_tick_stall_s,
            "ticks": health.get("ticks"),
            "tickSamples": health.get("tickSamples"),
            "tickP50Us": health.get("tickP50Us"),
            "tickP95Us": health.get("tickP95Us"),
            "tickP99Us": health.get("tickP99Us"),
            "tickMaxUs": health.get("tickMaxUs"),
            "stopAfterS": outcome.stop_after_s,
            "exitStatus": outcome.exit_status,
            "drain": {"records": outcome.drain.records,
                      "drainingReadyFalse": outcome.drain.draining_ready_false,
                      "finalLive": outcome.drain.final_live, "finalReady": outcome.drain.final_ready},
            "leakedProcesses": outcome.leaked_processes,
        },
        "failures": outcome.failures,
    }


def write_json_atomic(document: dict[str, Any], out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(prefix=".server-soak-", suffix=".json", dir=out.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(temp_name, out)
    except BaseException:
        Path(temp_name).unlink(missing_ok=True)
        raise


def soak(server: Path, module: Path, config: SoakConfig, *, expected_sha: str | None, summary: Path | None,
         work_dir: Path | None = None) -> tuple[SoakOutcome, dict[str, Any]]:
    """Validate inputs before launch, run the soak, and write the summary when requested."""
    config.validate()
    if expected_sha is not None and not _SHA_RE.fullmatch(expected_sha):
        raise SoakError("--expected-sha must be a full 40-character hexadecimal commit SHA")
    for label, path in (("server", server), ("module", module)):
        if not path.is_file():
            raise SoakError(f"{label} {path} is not a file")
    outcome = run_soak(server.resolve(), module.resolve(), config, expected_sha=expected_sha, work_dir=work_dir)
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    document = build_summary(outcome, config, expected_sha.lower() if expected_sha else None, timestamp)
    if summary is not None:
        write_json_atomic(document, summary)
    return outcome, document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    defaults = SoakConfig()
    parser.add_argument("--server", type=Path, required=True, help="SparkServer executable")
    parser.add_argument("--module", type=Path, required=True, help="game module library (e.g. libSparkGame.so)")
    parser.add_argument("--duration", type=float, default=defaults.duration_s,
                        help=f"seconds to soak after ready (default {defaults.duration_s:g}; 1800 for the release "
                             "smoke)")
    parser.add_argument("--warmup", type=float,
                        help="seconds after ready excluded from the memory fit (default: 25%% of --duration, "
                             "at most 300)")
    parser.add_argument("--sample-interval", type=float, default=defaults.sample_interval_s)
    parser.add_argument("--status-interval-ms", type=int, default=defaults.status_interval_ms)
    parser.add_argument("--tick-rate", type=float, default=defaults.tick_rate_hz)
    parser.add_argument("--startup-timeout", type=float, default=defaults.startup_timeout_s)
    parser.add_argument("--stall-timeout", type=float, default=defaults.stall_timeout_s)
    parser.add_argument("--stop-timeout", type=float, default=defaults.stop_timeout_s)
    parser.add_argument("--min-tick-rate-fraction", type=float, default=defaults.min_tick_rate_fraction)
    parser.add_argument("--max-rss-slope-bytes-per-hour", type=float,
                        default=defaults.max_rss_slope_bytes_per_hour, help="provisional ceiling (not an SLO)")
    parser.add_argument("--max-rss-growth-bytes", type=int, default=defaults.max_rss_growth_bytes,
                        help="provisional ceiling (not an SLO)")
    parser.add_argument("--max-tick-p95-us", type=int, default=defaults.max_tick_p95_us,
                        help="provisional budget (not an SLO)")
    parser.add_argument("--max-tick-p99-us", type=int, default=defaults.max_tick_p99_us,
                        help="provisional budget (not an SLO)")
    parser.add_argument("--expected-sha", help="full commit SHA of the build under test; labels the summary and "
                                               "must equal the server's reported commit")
    parser.add_argument("--summary", type=Path, help="write the machine-readable JSON summary here")
    parser.add_argument("--work-dir", type=Path, help="keep the server health file and logs here")
    args = parser.parse_args(argv)

    warmup_s = args.warmup if args.warmup is not None else min(300.0, 0.25 * args.duration)
    config = SoakConfig(duration_s=args.duration, warmup_s=warmup_s, sample_interval_s=args.sample_interval,
                        status_interval_ms=args.status_interval_ms, tick_rate_hz=args.tick_rate,
                        startup_timeout_s=args.startup_timeout, stall_timeout_s=args.stall_timeout,
                        stop_timeout_s=args.stop_timeout, min_tick_rate_fraction=args.min_tick_rate_fraction,
                        max_rss_slope_bytes_per_hour=args.max_rss_slope_bytes_per_hour,
                        max_rss_growth_bytes=args.max_rss_growth_bytes, max_tick_p95_us=args.max_tick_p95_us,
                        max_tick_p99_us=args.max_tick_p99_us)
    try:
        outcome, document = soak(args.server, args.module, config, expected_sha=args.expected_sha,
                                 summary=args.summary, work_dir=args.work_dir)
    except SoakError as exc:
        print(f"server_soak: FAIL: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(document, allow_nan=False))
    if outcome.failures:
        for failure in outcome.failures:
            print(f"server_soak: FAIL: {failure}", file=sys.stderr)
        if outcome.stderr_tail:
            print(f"server_soak: server stderr tail:\n{outcome.stderr_tail}", file=sys.stderr)
        return 1
    print(f"server_soak: PASS: {config.duration_s:g}s soak within provisional budgets (not certification)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
