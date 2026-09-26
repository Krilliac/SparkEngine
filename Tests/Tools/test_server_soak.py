#!/usr/bin/env python3
"""OPS-110: SparkServer soak harness (bounded memory, tick percentiles, graceful drain).

FitTests, DrainSequenceTests, ConfigTests, and SummaryTests cover the pure
pieces of tools/ops/server_soak.py. HarnessTests drive the harness end to end
against a stand-in server that speaks SparkServer's command line and health
contract (atomic health file plus one JSON snapshot per stdout line, a
draining snapshot on SIGTERM, then stopping and a final live=false snapshot)
and can be told to leak, stall, crash, run ticks backwards, report slow ticks,
skip the drain snapshot, exit non-zero, ignore SIGTERM, leak a child process,
never become ready, or report the wrong commit -- proving each failure is
caught and that a healthy server passes.

ServerSoakProcess soaks the real SparkServer with the SparkGame module when
CTest supplies SPARK_SERVER and SPARK_SERVER_MODULE (CTest Server_Soak, label
server-soak). SPARK_SERVER_SOAK_DURATION overrides its 60 s length,
SPARK_SERVER_SOAK_SUMMARY keeps the JSON summary, and
SPARK_SERVER_SOAK_EXPECTED_SHA labels it with an externally supplied commit.
"""

from __future__ import annotations

import io
import json
import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "ops"
sys.path.insert(0, str(TOOL_DIR))

import server_soak as soak_tool  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"
OTHER_SHA = "fedcba9876543210fedcba9876543210fedcba98"
LINUX_ONLY = unittest.skipUnless(sys.platform.startswith("linux"), "the server soak harness is Linux only")

# Stand-in SparkServer. It checks the harness's argv, then publishes the same
# health JSON SparkServer does. SOAK_FAULT selects one misbehaviour.
STAND_IN = """\
import json, os, signal, subprocess, sys, time
args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1]
assert "--module" in args and os.path.isfile(opt("--module")), args
assert opt("--bind-address") == "loopback" and "--no-lan-broadcast" in args, args
health_path = opt("--health-file")
interval = int(opt("--status-interval-ms")) / 1000.0
rate = float(opt("--tick-rate"))
fault = os.environ.get("SOAK_FAULT", "")
commit = os.environ.get("SOAK_COMMIT", "{sha}")
stop = []
signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
if fault == "ignore_sigterm":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

def rss():
    for line in open("/proc/self/status"):
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024

def publish(live, ready, draining, stopping, ticks):
    p99 = 50000 if fault == "slow_p99" else 700
    record = {{"live": live, "ready": ready, "draining": draining, "stopping": stopping, "port": 1, "players": 0,
              "ticks": ticks, "loadedModules": 1 if live else 0, "gameModule": "Stand-in" if live else "",
              "map": "soak", "error": "", "version": "0.9.0", "commit": commit, "treeState": "clean",
              "tickSamples": ticks, "tickP50Us": 50, "tickP95Us": 500, "tickP99Us": p99, "tickMaxUs": p99,
              "rssBytes": rss()}}
    text = json.dumps(record, separators=(",", ":"))
    print(text, flush=True)
    with open(health_path + ".tmp", "w") as stream:
        stream.write(text + "\\n")
    os.replace(health_path + ".tmp", health_path)

if fault == "leak_child":
    subprocess.Popen(["sleep", "30"])
start = time.monotonic()
hoard = []
next_status = 0.0
ticks = 0
while not stop:
    elapsed = time.monotonic() - start
    if fault == "never_ready":
        time.sleep(0.05)
        continue
    if fault == "crash" and elapsed > 1.0:
        os._exit(7)
    if fault == "leak":
        hoard.append(b"\\x01" * (256 * 1024))
    if not (fault == "stall" and elapsed > 1.0):
        ticks = int(elapsed * rate)
    shown = max(0, 1000 - ticks) if fault == "ticks_backwards" and elapsed > 1.0 else ticks
    if elapsed >= next_status:
        publish(True, True, False, False, shown)
        next_status = elapsed + interval
    time.sleep(1.0 / rate)

if fault != "no_drain":
    publish(True, False, True, False, ticks)
publish(True, False, fault != "no_drain", True, ticks)
publish(False, False, False, False, 0)
sys.exit(3 if fault == "exit_nonzero" else 0)
"""

FAST = soak_tool.SoakConfig(duration_s=3.0, warmup_s=0.5, sample_interval_s=0.1, status_interval_ms=100,
                            startup_timeout_s=5.0, stall_timeout_s=1.0, stop_timeout_s=3.0,
                            max_rss_growth_bytes=64 * 1024 * 1024)


def _record(**overrides: object) -> str:
    record = {"live": True, "ready": True, "draining": False, "stopping": False, "ticks": 5}
    record.update(overrides)
    return json.dumps(record)


class FitTests(unittest.TestCase):
    def test_linear_growth_is_recovered_in_bytes_per_hour(self) -> None:
        samples = [(float(t), 1000 + 10 * t, t * 60) for t in range(20)]
        slope, growth, count = soak_tool.fit_rss_slope(samples, 0.0, 19.0)
        self.assertEqual(count, 20)
        self.assertAlmostEqual(slope, 36000.0)
        self.assertEqual(growth, 190)

    def test_window_excludes_warmup_samples(self) -> None:
        samples = [(float(t), 5_000_000 if t < 5 else 1000, t) for t in range(20)]
        slope, growth, count = soak_tool.fit_rss_slope(samples, 5.0, 19.0)
        self.assertEqual(count, 15)
        self.assertEqual(slope, 0.0)
        self.assertEqual(growth, 0)

    def test_too_few_samples_yield_no_fit(self) -> None:
        samples = [(float(t), 1000, t) for t in range(soak_tool.MIN_FIT_SAMPLES - 1)]
        self.assertEqual(soak_tool.fit_rss_slope(samples, 0.0, 100.0),
                         (None, None, soak_tool.MIN_FIT_SAMPLES - 1))

    def test_samples_at_one_instant_yield_no_fit(self) -> None:
        samples = [(1.0, 1000 + index, index) for index in range(20)]
        self.assertEqual(soak_tool.fit_rss_slope(samples, 0.0, 2.0)[:2], (None, None))


class DrainSequenceTests(unittest.TestCase):
    def scan(self, *lines: str) -> soak_tool.DrainEvidence:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "stdout.log"
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return soak_tool.scan_drain_sequence(path)

    def test_graceful_sequence_is_accepted(self) -> None:
        evidence = self.scan(_record(), "log noise {not json", _record(ready=False, draining=True),
                             _record(ready=False, draining=True, stopping=True),
                             _record(live=False, ready=False, ticks=0))
        self.assertTrue(evidence.draining_ready_false)
        self.assertFalse(evidence.ready_after_drain)
        self.assertFalse(evidence.stopping_before_drain)
        self.assertEqual((evidence.final_live, evidence.final_ready), (False, False))
        self.assertEqual(evidence.records, 3)

    def test_stopping_without_drain_is_flagged(self) -> None:
        evidence = self.scan(_record(), _record(ready=False, stopping=True), _record(live=False, ready=False))
        self.assertTrue(evidence.stopping_before_drain)
        self.assertFalse(evidence.draining_ready_false)
        self.assertIsNone(evidence.final_live)

    def test_ready_after_drain_is_flagged(self) -> None:
        evidence = self.scan(_record(ready=False, draining=True), _record(), _record(live=False, ready=False))
        self.assertTrue(evidence.ready_after_drain)

    def test_oversized_and_non_health_lines_are_ignored(self) -> None:
        evidence = self.scan("{" + "x" * (soak_tool.MAX_STDOUT_LINE_BYTES + 10) + "}", '{"live": "yes"}',
                             _record(ready=False, draining=True), _record(live=False, ready=False))
        self.assertEqual(evidence.records, 2)
        self.assertTrue(evidence.draining_ready_false)


class ConfigTests(unittest.TestCase):
    def test_defaults_validate(self) -> None:
        soak_tool.SoakConfig().validate()
        FAST.validate()

    def test_invalid_values_are_rejected(self) -> None:
        cases = {
            "duration_s": 0.5, "warmup_s": -1.0, "sample_interval_s": 0.0, "tick_rate_hz": 2000.0,
            "status_interval_ms": 50, "min_tick_rate_fraction": 1.5, "max_tick_p99_us": 0,
            "max_rss_slope_bytes_per_hour": float("nan"), "stall_timeout_s": 0.2,
        }
        for key, value in cases.items():
            with self.subTest(key=key), self.assertRaises(soak_tool.SoakError):
                soak_tool.SoakConfig(**{key: value}).validate()

    def test_warmup_must_leave_room_for_the_fit(self) -> None:
        with self.assertRaises(soak_tool.SoakError):
            soak_tool.SoakConfig(duration_s=20.0, warmup_s=15.0).validate()

    def test_expected_sha_must_be_full_hex_before_launch(self) -> None:
        with self.assertRaises(soak_tool.SoakError):
            soak_tool.soak(Path("/nonexistent"), Path("/nonexistent"), FAST, expected_sha="abc123", summary=None)

    def test_missing_server_is_rejected_before_launch(self) -> None:
        with self.assertRaises(soak_tool.SoakError):
            soak_tool.soak(Path("/nonexistent/SparkServer"), Path(__file__), FAST, expected_sha=None, summary=None)


class SummaryTests(unittest.TestCase):
    def test_summary_is_strict_json_labelled_with_the_supplied_sha(self) -> None:
        outcome = soak_tool.SoakOutcome(samples=[(0.0, 100, 0), (1.0, 100, 60)],
                                        last_live_health={"tickP99Us": 700, "tickSamples": 60})
        document = soak_tool.build_summary(outcome, FAST, SHA, "2026-09-25T00:00:00Z")
        json.dumps(document, allow_nan=False)
        self.assertEqual(document["schema"], soak_tool.SUMMARY_SCHEMA)
        self.assertEqual(document["verdict"], "pass")
        self.assertEqual(document["commitSha"], SHA)
        self.assertIn("provisional", document["evidenceScope"])
        self.assertEqual(document["measurements"]["tickP99Us"], 700)
        outcome.failures.append("memory: leak")
        self.assertEqual(soak_tool.build_summary(outcome, FAST, None, "t")["verdict"], "fail")


@LINUX_ONLY
class HarnessTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.server = root / "SparkServer"
        self.server.write_text(f"#!{sys.executable}\n" + STAND_IN.format(sha=SHA), encoding="utf-8")
        self.server.chmod(self.server.stat().st_mode | stat.S_IXUSR)
        self.module = root / "libSparkGame.so"
        self.module.write_bytes(b"\0")
        self.summary = root / "summary.json"
        self._saved = {key: os.environ.get(key) for key in ("SOAK_FAULT", "SOAK_COMMIT")}

    def tearDown(self) -> None:
        for key, value in self._saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        self._tmp.cleanup()

    def run_soak(self, fault: str = "", config: soak_tool.SoakConfig = FAST,
                 expected_sha: str | None = SHA) -> dict:
        os.environ["SOAK_FAULT"] = fault
        outcome, document = soak_tool.soak(self.server, self.module, config, expected_sha=expected_sha,
                                           summary=self.summary)
        self.assertEqual(json.loads(self.summary.read_text(encoding="utf-8")), document)
        self.assertEqual(document["failures"], outcome.failures)
        return document

    def assertFailsWith(self, document: dict, prefix: str) -> None:
        self.assertEqual(document["verdict"], "fail")
        self.assertTrue(any(failure.startswith(prefix) for failure in document["failures"]), document["failures"])

    def test_healthy_server_passes(self) -> None:
        document = self.run_soak()
        self.assertEqual(document["failures"], [])
        self.assertEqual(document["verdict"], "pass")
        measurements = document["measurements"]
        self.assertEqual(measurements["exitStatus"], 0)
        self.assertTrue(measurements["drain"]["drainingReadyFalse"])
        self.assertEqual(measurements["leakedProcesses"], [])
        self.assertGreaterEqual(measurements["fitSampleCount"], soak_tool.MIN_FIT_SAMPLES)
        self.assertEqual(document["server"]["commit"], SHA)

    def test_memory_leak_fails(self) -> None:
        config = soak_tool.SoakConfig(**{**FAST.__dict__, "max_rss_growth_bytes": 16 * 1024 * 1024})
        self.assertFailsWith(self.run_soak("leak", config), "memory:")

    def test_tick_stall_fails(self) -> None:
        self.assertFailsWith(self.run_soak("stall"), "stall:")

    def test_ticks_going_backwards_fail(self) -> None:
        self.assertFailsWith(self.run_soak("ticks_backwards"), "ticks: counter went backwards")

    def test_tick_p99_over_budget_fails(self) -> None:
        self.assertFailsWith(self.run_soak("slow_p99"), "latency: tickP99Us")

    def test_crash_during_soak_fails(self) -> None:
        self.assertFailsWith(self.run_soak("crash"), "crash:")

    def test_never_ready_fails(self) -> None:
        config = soak_tool.SoakConfig(**{**FAST.__dict__, "startup_timeout_s": 1.0})
        self.assertFailsWith(self.run_soak("never_ready", config), "startup:")

    def test_missing_drain_snapshot_fails(self) -> None:
        document = self.run_soak("no_drain")
        self.assertFailsWith(document, "drain: a stopping snapshot")
        self.assertIn("drain: no live+draining snapshot with ready=false was published after SIGTERM",
                      document["failures"])

    def test_nonzero_exit_fails(self) -> None:
        self.assertFailsWith(self.run_soak("exit_nonzero"), "stop: server exited with status 3")

    def test_ignored_sigterm_fails_and_is_killed(self) -> None:
        config = soak_tool.SoakConfig(**{**FAST.__dict__, "stop_timeout_s": 1.0})
        self.assertFailsWith(self.run_soak("ignore_sigterm", config), "stop: server still running")

    def test_leaked_child_process_fails_and_is_reaped(self) -> None:
        document = self.run_soak("leak_child")
        self.assertFailsWith(document, "stop: 1 process(es) outlived the server")
        leaked = document["measurements"]["leakedProcesses"][0]["pid"]
        # The harness kills whatever outlived the server; the orphan is then
        # gone or an unreaped zombie of whichever process adopted it.
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                state = Path(f"/proc/{leaked}/stat").read_text(encoding="ascii").rpartition(")")[2].split()[0]
            except OSError:
                break
            if state == "Z":
                break
            time.sleep(0.05)
        else:
            self.fail(f"leaked process {leaked} is still running after the soak")

    def test_commit_mismatch_fails(self) -> None:
        self.assertFailsWith(self.run_soak(expected_sha=OTHER_SHA), "identity:")

    def test_cli_exit_status_and_stdout_summary(self) -> None:
        os.environ["SOAK_FAULT"] = ""
        argv = ["--server", str(self.server), "--module", str(self.module), "--duration", "3", "--warmup", "0.5",
                "--sample-interval", "0.1", "--status-interval-ms", "100", "--stall-timeout", "1",
                "--expected-sha", SHA]
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(soak_tool.main(argv), 0, stderr.getvalue())
        self.assertEqual(json.loads(stdout.getvalue())["verdict"], "pass")
        os.environ["SOAK_FAULT"] = "slow_p99"
        with redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            self.assertEqual(soak_tool.main(argv), 1)
        self.assertIn("server_soak: FAIL: latency", stderr.getvalue())


class ServerSoakProcess(unittest.TestCase):
    """Real SparkServer soak; registered as CTest Server_Soak (label server-soak)."""

    def test_real_server_soak(self) -> None:
        server_env = os.environ.get("SPARK_SERVER")
        if not server_env:
            if os.environ.get("SPARK_SERVER_SOAK_REQUIRED") == "1":
                self.fail("SPARK_SERVER is not set but the real-server soak is required")
            self.skipTest("SPARK_SERVER not set (run through CTest Server_Soak)")
        server = Path(server_env)
        module = Path(os.environ["SPARK_SERVER_MODULE"])
        self.assertTrue(server.is_file(), server)
        self.assertTrue(module.is_file(), module)
        duration = float(os.environ.get("SPARK_SERVER_SOAK_DURATION", "60"))
        expected_sha = os.environ.get("SPARK_SERVER_SOAK_EXPECTED_SHA") or None

        with tempfile.TemporaryDirectory() as tmp:
            summary_path = Path(os.environ.get("SPARK_SERVER_SOAK_SUMMARY") or Path(tmp) / "server-soak.json")
            command = [sys.executable, "-B", str(TOOL_DIR / "server_soak.py"), "--server", str(server),
                       "--module", str(module), "--duration", f"{duration:g}", "--summary", str(summary_path)]
            if expected_sha:
                command += ["--expected-sha", expected_sha]
            run = subprocess.run(command, capture_output=True, text=True, timeout=duration + 180)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        measurements = summary["measurements"]
        self.assertEqual(summary["verdict"], "pass")
        self.assertEqual(summary["failures"], [])
        self.assertEqual(measurements["exitStatus"], 0)
        self.assertTrue(measurements["drain"]["drainingReadyFalse"])
        self.assertEqual(measurements["leakedProcesses"], [])
        self.assertRegex(summary["server"]["commit"], r"^([0-9a-f]{40}|unknown)$")
        print(textwrap.dedent(f"""\
            server soak: {duration:g}s, RSS slope {measurements['rssSlopeBytesPerHour']:.0f} B/h over
              {measurements['fitSampleCount']} samples, tick p95/p99 {measurements['tickP95Us']}/
              {measurements['tickP99Us']} us, {measurements['achievedTickRateHz']:.1f} Hz, drain+exit 0 in
              {measurements['stopAfterS']:.2f}s"""), file=sys.stderr)


if __name__ == "__main__":
    unittest.main()
