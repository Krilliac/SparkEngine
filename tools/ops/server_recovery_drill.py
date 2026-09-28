#!/usr/bin/env python3
"""OPS-110: drill detecting, diagnosing, draining, restarting and recovering a failing SparkServer.

The drill hosts SparkServer the way an operator does (``--module <lib>
--health-file <path> --bind-address loopback --stop-file <path>``), waits for
``ready``, records the reported build, then injects one failure per scenario
and plays the operator's side using only the operator surface:

* ``crash`` -- the process is killed outright (SIGKILL / TerminateProcess). Its
  health file keeps the last ``live=true`` snapshot, so detection must come
  from staleness: the snapshot (file identity and ``ticks``) has not changed
  for ``--stale-after`` seconds. Diagnosis: the process has exited.
* ``wedge`` (POSIX only) -- the process is stopped with SIGSTOP. Detection is
  the same staleness rule; diagnosis: the process is alive but not ticking.
  The operator action is SIGKILL, then restart.
* ``drain`` -- the operator requests a graceful stop through the stop file.
  The server's stdout health stream must show a ``live, draining,
  ready=false`` snapshot before ``stopping``, end on ``live=false,
  ready=false``, and the process must exit 0 within ``--stop-timeout``.

After every scenario the server is restarted with identical arguments. The
restart must publish a fresh ``live+ready`` snapshot within
``--startup-timeout``, report the same build commit as the first launch, and
advance ``ticks``. Detection and recovery latencies are recorded per scenario.

The summary (``--summary``) is JSON schema ``spark-server-recovery-drill/1``,
labelled with the externally supplied ``--expected-sha``; the SHA is never
inferred, and when supplied the server must report exactly that commit from a
clean tree. Every health snapshot is parsed by
tools/ops/validate_server_health.py, and one that violates the
``spark-server-health/1`` contract fails the drill. A local pass is precursor
evidence, not a recorded release drill.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

from server_soak import (  # noqa: E402 -- sibling tool, shared health contract
    _SHA_RE,
    describe_exit_status,
    read_health,
    reserve_udp_port,
    scan_drain_sequence,
    write_json_atomic,
)
from validate_server_health import HealthContractError, build_identity_problem  # noqa: E402

SUMMARY_SCHEMA = "spark-server-recovery-drill/1"
EVIDENCE_SCOPE = "local-precursor: operator drill on an uncertified host; not a recorded release drill"
POSIX_ONLY_SCENARIOS = {"wedge"}
ALL_SCENARIOS = ("crash", "wedge", "drain")
POLL_S = 0.05


class DrillError(Exception):
    """Invalid drill input or an environment the drill cannot run in."""


@dataclass(frozen=True)
class DrillConfig:
    status_interval_ms: int = 250
    stale_after_s: float = 3.0
    startup_timeout_s: float = 60.0
    stop_timeout_s: float = 30.0
    tick_rate_hz: float = 60.0

    def validate(self) -> None:
        for name, value in (("--stale-after", self.stale_after_s), ("--startup-timeout", self.startup_timeout_s),
                            ("--stop-timeout", self.stop_timeout_s)):
            if not math.isfinite(value) or value <= 0:
                raise DrillError(f"{name} must be a positive finite number")
        if not 100 <= self.status_interval_ms <= 3_600_000:
            raise DrillError("--status-interval-ms must be between 100 and 3600000 (SparkServer's accepted range)")
        if not math.isfinite(self.tick_rate_hz) or not 1.0 <= self.tick_rate_hz <= 1000.0:
            raise DrillError("--tick-rate must be between 1 and 1000 (SparkServer's accepted range)")
        # A healthy server rewrites its health file once per status interval;
        # a shorter staleness window would declare a live server dead.
        if self.stale_after_s < 3 * self.status_interval_ms / 1000.0:
            raise DrillError("--stale-after must cover at least three status intervals")


@dataclass
class ScenarioResult:
    name: str
    status: str = "pass"
    diagnosis: str | None = None
    last_snapshot_live: bool | None = None
    detect_s: float | None = None
    stop_s: float | None = None
    recover_s: float | None = None
    exit_status: int | None = None
    failures: list[str] = field(default_factory=list)

    def fail(self, message: str) -> None:
        self.status = "fail"
        self.failures.append(f"{self.name}: {message}")


@dataclass
class DrillOutcome:
    identity: dict[str, str] = field(default_factory=dict)
    launches: int = 0
    scenarios: list[ScenarioResult] = field(default_factory=list)
    failures: list[str] = field(default_factory=list)
    stderr_tail: str = ""


def _snapshot(health_file: Path) -> tuple[int, dict[str, Any]] | None:
    """(mtime in ns, parsed snapshot) of the health file, or None when absent or mid-replace."""
    try:
        modified = health_file.stat().st_mtime_ns
    except OSError:
        return None
    health = read_health(health_file)
    return None if health is None else (modified, health)


def _ticks(health: dict[str, Any]) -> int | None:
    value = health.get("ticks")
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None


class ServerHost:
    """Launches SparkServer with fixed operator arguments and tracks the current process."""

    def __init__(self, launcher: Sequence[str], module: Path, root: Path, config: DrillConfig) -> None:
        self.launcher = list(launcher)
        self.module = module
        self.root = root
        self.config = config
        self.health_file = root / "server-health.json"
        self.port = reserve_udp_port()
        self.process: subprocess.Popen[bytes] | None = None
        self.launches = 0
        self.previous_mtime_ns: int | None = None
        self.stdout_path = root / "server-stdout-0.log"
        self.stderr_path = root / "server-stderr-0.log"
        self.stop_file = root / "stop-0"

    def launch(self) -> None:
        self.launches += 1
        self.stdout_path = self.root / f"server-stdout-{self.launches}.log"
        self.stderr_path = self.root / f"server-stderr-{self.launches}.log"
        self.stop_file = self.root / f"stop-{self.launches}"
        command = self.launcher + [
            "--module", str(self.module), "--port", str(self.port), "--bind-address", "loopback",
            "--no-lan-broadcast", "--map", "drill", "--name", "OPS-110 recovery drill", "--max-clients", "8",
            "--tick-rate", f"{self.config.tick_rate_hz:g}", "--health-file", str(self.health_file),
            "--stop-file", str(self.stop_file), "--status-interval-ms", str(self.config.status_interval_ms)]
        previous = _snapshot(self.health_file)
        self.previous_mtime_ns = None if previous is None else previous[0]
        with self.stdout_path.open("wb") as stdout_file, self.stderr_path.open("wb") as stderr_file:
            self.process = subprocess.Popen(command, cwd=self.root, stdin=subprocess.DEVNULL, stdout=stdout_file,
                                            stderr=stderr_file, start_new_session=os.name == "posix")

    def wait_ready(self) -> dict[str, Any] | str:
        """The first fresh live+ready snapshot of this launch, or a failure message."""
        assert self.process is not None
        deadline = time.monotonic() + self.config.startup_timeout_s
        while self.process.poll() is None:
            current = _snapshot(self.health_file)
            # The file as it stood before this launch is the previous process's
            # last word; a stale live=true snapshot must never read as recovered.
            if current is not None and current[0] != self.previous_mtime_ns:
                health = current[1]
                if health.get("live") is True and health.get("ready") is True and \
                        isinstance(health.get("gameModule"), str) and health["gameModule"]:
                    return health
            if time.monotonic() > deadline:
                return f"no fresh live+ready health snapshot within {self.config.startup_timeout_s:g}s"
            time.sleep(POLL_S)
        return f"server exited with {describe_exit_status(self.process.returncode)} before ready"

    def wait_ticks_advance(self, ready: dict[str, Any]) -> str | None:
        """None once ``ticks`` moves past the ready snapshot, else a failure message."""
        assert self.process is not None
        start_ticks = _ticks(ready)
        if start_ticks is None:
            return f"ready snapshot carries no tick counter: {ready.get('ticks')!r}"
        deadline = time.monotonic() + self.config.stale_after_s
        while time.monotonic() < deadline and self.process.poll() is None:
            current = _snapshot(self.health_file)
            if current is not None:
                ticks = _ticks(current[1])
                if ticks is not None and ticks > start_ticks and current[1].get("live") is True:
                    return None
            time.sleep(POLL_S)
        return f"ticks did not advance past {start_ticks} within {self.config.stale_after_s:g}s of ready"

    def detect_stale(self) -> float | str:
        """Seconds until the health snapshot stopped changing for the staleness window, or a failure."""
        started = time.monotonic()
        deadline = started + 3 * self.config.stale_after_s
        last_signature: tuple[int, int | None] | None = None
        unchanged_since = started
        while time.monotonic() < deadline:
            current = _snapshot(self.health_file)
            signature = None if current is None else (current[0], _ticks(current[1]))
            now = time.monotonic()
            if signature != last_signature:
                last_signature = signature
                unchanged_since = now
            elif now - unchanged_since >= self.config.stale_after_s:
                return now - started
            time.sleep(POLL_S)
        return f"health file kept changing for {3 * self.config.stale_after_s:g}s after the fault"

    def kill(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
        if self.process is not None:
            self.process.wait()

    def cleanup(self) -> None:
        """Kill the current server and, on POSIX, anything left in its session."""
        if self.process is None:
            return
        if os.name == "posix":
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except OSError:
                pass
        self.kill()


def _recover(host: ServerHost, result: ScenarioResult, fault_at: float, expected_commit: str) -> bool:
    host.launch()
    ready = host.wait_ready()
    if isinstance(ready, str):
        result.fail(f"restart: {ready}")
        return False
    commit = str(ready.get("commit", ""))
    if commit != expected_commit:
        result.fail(f"identity: restarted server reports commit {commit!r}, first launch reported "
                    f"{expected_commit!r}")
    stalled = host.wait_ticks_advance(ready)
    if stalled is not None:
        result.fail(f"restart: {stalled}")
        return False
    result.recover_s = time.monotonic() - fault_at
    return True


def _run_crash(host: ServerHost, result: ScenarioResult) -> float:
    fault_at = time.monotonic()
    host.kill()
    detected = host.detect_stale()
    if isinstance(detected, str):
        result.fail(f"detect: {detected}")
    else:
        result.detect_s = detected
    current = _snapshot(host.health_file)
    result.last_snapshot_live = None if current is None else current[1].get("live")
    result.diagnosis = "exited" if host.process is not None and host.process.poll() is not None else "running"
    if result.diagnosis != "exited":
        result.fail("diagnose: the killed server is still running")
    return fault_at


def _run_wedge(host: ServerHost, result: ScenarioResult) -> float:
    assert host.process is not None
    fault_at = time.monotonic()
    os.kill(host.process.pid, signal.SIGSTOP)
    detected = host.detect_stale()
    if isinstance(detected, str):
        result.fail(f"detect: {detected}")
    else:
        result.detect_s = detected
    result.diagnosis = "wedged" if host.process.poll() is None else "exited"
    if result.diagnosis != "wedged":
        result.fail("diagnose: a stopped server should still be alive while its health goes stale")
    host.kill()
    return fault_at


def _run_drain(host: ServerHost, result: ScenarioResult) -> float:
    assert host.process is not None
    fault_at = time.monotonic()
    host.stop_file.write_bytes(b"")
    try:
        host.process.wait(timeout=host.config.stop_timeout_s)
    except subprocess.TimeoutExpired:
        result.fail(f"stop: server still running {host.config.stop_timeout_s:g}s after the stop request")
        host.kill()
        return fault_at
    result.stop_s = time.monotonic() - fault_at
    result.exit_status = host.process.returncode
    result.diagnosis = "drained"
    if host.process.returncode != 0:
        result.fail(f"stop: server exited with {describe_exit_status(host.process.returncode)}, expected a "
                    "graceful status 0")
    drain = scan_drain_sequence(host.stdout_path)
    if drain.stopping_before_drain or not drain.draining_ready_false:
        result.fail("drain: no live+draining snapshot with ready=false preceded stopping")
    if drain.ready_after_drain:
        result.fail("drain: the server reported ready=true after it began draining")
    final = read_health(host.health_file)
    if final is None or final.get("live") is not False or final.get("ready") is not False:
        result.fail(f"drain: the health file does not end on live=false, ready=false: {final!r}")
    return fault_at


SCENARIO_RUNNERS = {"crash": _run_crash, "wedge": _run_wedge, "drain": _run_drain}


def supported_scenarios() -> tuple[str, ...]:
    return tuple(name for name in ALL_SCENARIOS if os.name == "posix" or name not in POSIX_ONLY_SCENARIOS)


def run_drill(launcher: Sequence[str], module: Path, config: DrillConfig, scenarios: Sequence[str], *,
              expected_sha: str | None = None, work_dir: Path | None = None) -> DrillOutcome:
    """Run the scenarios in order against one hosted server; drill failures are recorded, not raised."""
    outcome = DrillOutcome()
    with tempfile.TemporaryDirectory(prefix="spark-server-drill-") as tmp:
        root = work_dir if work_dir is not None else Path(tmp)
        root.mkdir(parents=True, exist_ok=True)
        host = ServerHost(launcher, module, root, config)
        host.health_file.unlink(missing_ok=True)
        try:
            host.launch()
            ready = host.wait_ready()
            if isinstance(ready, str):
                outcome.failures.append(f"startup: {ready}")
                return outcome
            outcome.identity = {key: str(ready.get(key, "")) for key in ("version", "commit", "treeState",
                                                                          "gameModule")}
            identity_problem = build_identity_problem(outcome.identity, expected_sha)
            if identity_problem is not None:
                outcome.failures.append(f"identity: {identity_problem}")
            for name in scenarios:
                result = ScenarioResult(name)
                outcome.scenarios.append(result)
                fault_at = SCENARIO_RUNNERS[name](host, result)
                # Every runner leaves the server dead; only a correctly handled
                # failure is followed by a restart.
                if result.status == "pass":
                    _recover(host, result, fault_at, outcome.identity["commit"])
                outcome.failures.extend(result.failures)
                if result.status != "pass":
                    break
            if not outcome.failures:
                # Leave the host as found: the last restart drains cleanly too.
                closing = ScenarioResult("final-drain")
                outcome.scenarios.append(closing)
                _run_drain(host, closing)
                outcome.failures.extend(closing.failures)
        except HealthContractError as exc:
            outcome.failures.append(f"health: contract violation: {exc}")
        finally:
            host.cleanup()
            outcome.launches = host.launches
            outcome.stderr_tail = _stderr_tail(host.stderr_path)
    return outcome


def _stderr_tail(path: Path, limit: int = 4000) -> str:
    try:
        data = path.read_bytes()
    except OSError:
        return ""
    return data[-limit:].decode("utf-8", "replace")


def build_summary(outcome: DrillOutcome, config: DrillConfig, commit_sha: str | None,
                  timestamp: str) -> dict[str, Any]:
    return {
        "schema": SUMMARY_SCHEMA,
        "verdict": "fail" if outcome.failures else "pass",
        "evidenceScope": EVIDENCE_SCOPE,
        "commitSha": commit_sha,
        "timestamp": timestamp,
        "host": {"os": platform.system(), "release": platform.release(), "machine": platform.machine()},
        "server": outcome.identity,
        "config": {"statusIntervalMs": config.status_interval_ms, "staleAfterS": config.stale_after_s,
                   "startupTimeoutS": config.startup_timeout_s, "stopTimeoutS": config.stop_timeout_s,
                   "tickRateHz": config.tick_rate_hz},
        "launches": outcome.launches,
        "scenarios": [{"name": result.name, "status": result.status, "diagnosis": result.diagnosis,
                       "lastSnapshotLive": result.last_snapshot_live, "detectS": result.detect_s,
                       "stopS": result.stop_s, "recoverS": result.recover_s, "exitStatus": result.exit_status,
                       "failures": result.failures} for result in outcome.scenarios],
        "failures": outcome.failures,
    }


def drill(launcher: Sequence[str], module: Path, config: DrillConfig, scenarios: Sequence[str], *,
          expected_sha: str | None, summary: Path | None,
          work_dir: Path | None = None) -> tuple[DrillOutcome, dict[str, Any]]:
    """Validate inputs before launch, run the drill, and write the summary when requested."""
    config.validate()
    if expected_sha is not None and not _SHA_RE.fullmatch(expected_sha):
        raise DrillError("--expected-sha must be a full 40-character hexadecimal commit SHA")
    if not scenarios:
        raise DrillError("at least one scenario must run")
    for name in scenarios:
        if name not in SCENARIO_RUNNERS:
            raise DrillError(f"unknown scenario {name!r}; expected one of {', '.join(ALL_SCENARIOS)}")
        if name not in supported_scenarios():
            raise DrillError(f"scenario {name!r} needs POSIX job control and cannot run on {platform.system()}")
    if not module.is_file():
        raise DrillError(f"module {module} is not a file")
    outcome = run_drill(launcher, module.resolve(), config, scenarios, expected_sha=expected_sha,
                        work_dir=work_dir)
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    document = build_summary(outcome, config, expected_sha.lower() if expected_sha else None, timestamp)
    if summary is not None:
        write_json_atomic(document, summary)
    return outcome, document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    defaults = DrillConfig()
    parser.add_argument("--server", type=Path, required=True, help="SparkServer executable")
    parser.add_argument("--module", type=Path, required=True, help="game module library")
    parser.add_argument("--scenario", action="append", choices=ALL_SCENARIOS,
                        help="scenario to run, in order; repeatable (default: every scenario this OS supports)")
    parser.add_argument("--status-interval-ms", type=int, default=defaults.status_interval_ms)
    parser.add_argument("--stale-after", type=float, default=defaults.stale_after_s,
                        help="seconds an unchanged health snapshot takes to count as a failed server")
    parser.add_argument("--startup-timeout", type=float, default=defaults.startup_timeout_s)
    parser.add_argument("--stop-timeout", type=float, default=defaults.stop_timeout_s)
    parser.add_argument("--tick-rate", type=float, default=defaults.tick_rate_hz)
    parser.add_argument("--expected-sha", help="full commit SHA of the build under test; labels the summary and "
                                               "must equal the server's reported commit")
    parser.add_argument("--summary", type=Path, help="write the machine-readable JSON summary here")
    parser.add_argument("--work-dir", type=Path, help="keep the health file, stop files and logs here")
    args = parser.parse_args(argv)

    config = DrillConfig(status_interval_ms=args.status_interval_ms, stale_after_s=args.stale_after,
                         startup_timeout_s=args.startup_timeout, stop_timeout_s=args.stop_timeout,
                         tick_rate_hz=args.tick_rate)
    try:
        if not args.server.is_file():
            raise DrillError(f"server {args.server} is not a file")
        outcome, document = drill([str(args.server.resolve())], args.module, config,
                                  args.scenario or supported_scenarios(), expected_sha=args.expected_sha,
                                  summary=args.summary, work_dir=args.work_dir)
    except DrillError as exc:
        print(f"server_recovery_drill: FAIL: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(document, allow_nan=False))
    if outcome.failures:
        for failure in outcome.failures:
            print(f"server_recovery_drill: FAIL: {failure}", file=sys.stderr)
        if outcome.stderr_tail:
            print(f"server_recovery_drill: server stderr tail:\n{outcome.stderr_tail}", file=sys.stderr)
        return 1
    print(f"server_recovery_drill: PASS: {len(outcome.scenarios)} scenario(s) over {outcome.launches} launches "
          "(local precursor, not a recorded release drill)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
