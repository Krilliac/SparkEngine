#!/usr/bin/env python3
"""PERF-100: NullRHI soak harness (leak-rate and crash-count metrics).

LeakFitTests and ResultTests cover the slope fit and the validate_result
document. HarnessTests drive tools/perf-budget/run_nullrhi_soak.py end to end
against a stand-in engine executable that behaves like the headless host
(module-ready marker, 60 Hz sleeping loop, final tick-stats record) and can be
made to leak, crash, deadlock, spin, run over budget, quit early, or never
become ready -- proving each failure mode is detected and that a slow but live
loop is not mistaken for a hang. SoakNullRHIHeadlessSmoke runs the real SparkEngine
headless host with the SparkGame module when CTest supplies
SPARK_HEADLESS_ENGINE/SPARK_HEADLESS_MODULE (Soak_NullRHIHeadlessSmoke);
SPARK_SOAK_DURATION overrides its 120 s length.
"""

from __future__ import annotations

import io
import json
import math
import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "perf-budget"
sys.path.insert(0, str(TOOL_DIR))

import run_nullrhi_soak as soak_tool  # noqa: E402
from collect_headless_result import CollectionError, TickStats, engine_command  # noqa: E402
from validate_budget import validate_result  # noqa: E402

BUDGET_DIR = REPO_ROOT / "perf-budgets" / "v1"
SHA = "0123456789abcdef0123456789abcdef01234567"
LINUX_ONLY = unittest.skipUnless(sys.platform.startswith("linux"), "soak harness supports the Linux row only")

# Stand-in host: argv check, ready marker, a sleeping 60 Hz loop with an
# optional per-tick fault hook (which may set live_resources), then the NullRHI
# live-resource record and the tick-stats record with its own VmHWM.
STAND_IN = """\
import os, signal, sys, threading, time
frames = int(sys.argv[sys.argv.index("-test-frames") + 1])
assert sys.argv[1:] == ["-headless", "-game", sys.argv[3], "-require-game", "-test-frames", str(frames)], sys.argv
print("boot log line")
print("SPARK_MODULE_READY count=1", flush=True)
hoard = []
live_resources = 0
start = time.monotonic()
for tick in range(frames):
    elapsed = time.monotonic() - start
{fault}
    time.sleep(1 / 60)
hwm = next(l.split()[1] for l in open("/proc/self/status") if l.startswith("VmHWM:"))
print(f"SPARK_HEADLESS_NULLRHI_RESOURCES live={{live_resources}}")
print(f"SPARK_HEADLESS_TICK_STATS backend=null frames={{frames}} p50_us=90 p99_us=160 max_us=900 peak_rss_kib={{hwm}}")
"""

# Lets the few-second stand-in runs exercise the --out path, which production
# refuses below the one-hour headless-soak-1h scene length.
SHORT_RESULT_RUNS = mock.patch.object(soak_tool, "MIN_RESULT_DURATION_S", 3.0)
FAST = soak_tool.SoakConfig(duration_s=3.0, warmup_s=0.5, sample_interval_s=0.1, heartbeat_timeout_s=1.0,
                            startup_timeout_s=5.0, teardown_timeout_s=5.0)


def _outcome(**overrides: object) -> soak_tool.SoakOutcome:
    outcome = soak_tool.SoakOutcome(frames=216000, exit_status=0, slope_bytes_per_hour=1234.5,
                                    fit_sample_count=2700,
                                    stats=TickStats(frames=216000, p50_us=90, p99_us=160, max_us=900,
                                                    peak_rss_kib=29000))
    for key, value in overrides.items():
        setattr(outcome, key, value)
    return outcome


class LeakFitTests(unittest.TestCase):
    def test_linear_growth_is_recovered_in_bytes_per_hour(self) -> None:
        samples = [(t * 0.5, 10_000_000 + int(t * 0.5 * 1000)) for t in range(100)]
        slope, count = soak_tool.fit_leak_slope(samples, 0.0, 100.0)
        self.assertEqual(count, 100)
        self.assertAlmostEqual(slope, 1000 * 3600, delta=1.0)

    def test_window_excludes_warmup_and_teardown(self) -> None:
        # Startup growth before t=10 and a teardown drop after t=40 must not
        # leak into the fit: the plateau between them is flat.
        samples = [(float(t), 5_000_000 * t) for t in range(10)]
        samples += [(float(t), 50_000_000) for t in range(10, 41)]
        samples += [(float(t), 1_000_000) for t in range(41, 50)]
        slope, count = soak_tool.fit_leak_slope(samples, 10.0, 40.0)
        self.assertEqual(count, 31)
        self.assertEqual(slope, 0.0)

    def test_too_few_or_degenerate_samples_give_no_slope(self) -> None:
        few = [(float(t), 1000) for t in range(soak_tool.MIN_FIT_SAMPLES - 1)]
        self.assertEqual(soak_tool.fit_leak_slope(few, 0.0, 100.0), (None, soak_tool.MIN_FIT_SAMPLES - 1))
        same_instant = [(5.0, 1000 + t) for t in range(20)]
        self.assertEqual(soak_tool.fit_leak_slope(same_instant, 0.0, 100.0), (None, 20))


class ResultTests(unittest.TestCase):
    def test_clean_result_passes_schema_and_budget_definitions(self) -> None:
        result = soak_tool.build_result(_outcome(), SHA, "2026-09-25T00:00:00Z")
        self.assertEqual(validate_result(result, ["linux-nullrhi-ci"], expected_sha=SHA), [])
        soak_tool.check_against_budget(result, BUDGET_DIR, SHA)
        values = {m["metricId"]: m for m in result["measurements"]}
        self.assertEqual(values[soak_tool.METRIC_LEAK_RATE]["value"], 1234.5)
        self.assertEqual(values[soak_tool.METRIC_LEAK_RATE]["sampleCount"], 2700)
        self.assertEqual(values[soak_tool.METRIC_CRASH_COUNT]["value"], 0)

    def test_crash_is_counted_and_shrinking_rss_is_zero_leak(self) -> None:
        result = soak_tool.build_result(_outcome(crashed=True, exit_status=-11, slope_bytes_per_hour=-5e6),
                                        SHA, "2026-09-25T00:00:00Z")
        soak_tool.check_against_budget(result, BUDGET_DIR, SHA)
        values = {m["metricId"]: m["value"] for m in result["measurements"]}
        self.assertEqual(values, {soak_tool.METRIC_LEAK_RATE: 0.0, soak_tool.METRIC_CRASH_COUNT: 1})

    def test_early_crash_without_fit_reports_crash_count_only(self) -> None:
        result = soak_tool.build_result(_outcome(crashed=True, slope_bytes_per_hour=None, fit_sample_count=3),
                                        SHA, "2026-09-25T00:00:00Z")
        soak_tool.check_against_budget(result, BUDGET_DIR, SHA)
        self.assertEqual([m["metricId"] for m in result["measurements"]], [soak_tool.METRIC_CRASH_COUNT])

    def test_hung_run_is_not_a_measurement(self) -> None:
        with self.assertRaisesRegex(CollectionError, "hung"):
            soak_tool.build_result(_outcome(hung=True), SHA, "2026-09-25T00:00:00Z")

    def test_clean_exit_without_frame_proof_is_not_a_measurement(self) -> None:
        with self.assertRaisesRegex(CollectionError, "truncated soak"):
            soak_tool.build_result(_outcome(stats=None), SHA, "2026-09-25T00:00:00Z")

    def test_exit_status_names_realtime_signals(self) -> None:
        self.assertEqual(soak_tool.describe_exit_status(2), "status 2")
        self.assertEqual(soak_tool.describe_exit_status(-11), "signal SIGSEGV")
        # SIGRTMIN+1 on Linux: a valid signal with no Signals enum member.
        self.assertEqual(soak_tool.describe_exit_status(-35), "signal 35")

    def test_sha_mismatch_rejected(self) -> None:
        result = soak_tool.build_result(_outcome(), SHA, "2026-09-25T00:00:00Z")
        with self.assertRaisesRegex(CollectionError, "expectedSha"):
            soak_tool.check_against_budget(result, BUDGET_DIR, "f" * 40)


class HarnessTests(unittest.TestCase):
    """Drive soak() with a stand-in engine executable."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.module = self.root / "libSparkGame.so"
        self.module.write_bytes(b"\x7fELF")
        self.out = self.root / "out" / "result.json"
        self.report = self.root / "out" / "report.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def engine(self, fault: str = "", *, script: str | None = None) -> Path:
        engine = self.root / "SparkEngine"
        body = script if script is not None else STAND_IN.format(fault=textwrap.indent(fault, " " * 4))
        engine.write_text(f"#!{sys.executable}\n{body}", encoding="utf-8")
        engine.chmod(engine.stat().st_mode | stat.S_IXUSR)
        return engine

    def soak(self, engine: Path, config: soak_tool.SoakConfig = FAST, **kwargs: object) -> soak_tool.SoakOutcome:
        options: dict[str, object] = {"expected_sha": None, "out": None, "report": self.report,
                                      "budget_dir": BUDGET_DIR}
        options.update(kwargs)
        return soak_tool.soak(engine, self.module, config, **options)  # type: ignore[arg-type]

    @LINUX_ONLY
    def test_steady_run_passes_and_reports(self) -> None:
        outcome = self.soak(self.engine())
        self.assertEqual(outcome.failures, [])
        self.assertFalse(outcome.crashed or outcome.hung)
        self.assertEqual(outcome.stats.frames, FAST.frames)
        self.assertGreaterEqual(outcome.fit_sample_count, soak_tool.MIN_FIT_SAMPLES)
        self.assertLess(outcome.slope_bytes_per_hour, FAST.max_leak_bytes_per_hour)
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["schema"], soak_tool.REPORT_SCHEMA)
        self.assertEqual(report["failures"], [])
        self.assertEqual(report["nullrhiLiveResourcesAtShutdown"], 0)
        self.assertGreater(len(report["rssSamples"]), soak_tool.MIN_FIT_SAMPLES)

    @LINUX_ONLY
    def test_resources_held_past_device_shutdown_fail(self) -> None:
        outcome = self.soak(self.engine("live_resources = 3"))
        self.assertFalse(outcome.crashed or outcome.hung)
        self.assertEqual(outcome.nullrhi_live_resources, 3)
        self.assertTrue(any(f.startswith("leak: 3 NullRHI resources") for f in outcome.failures), outcome.failures)

    @LINUX_ONLY
    def test_missing_resource_record_is_invalid_evidence(self) -> None:
        script = STAND_IN.format(fault="").replace('print(f"SPARK_HEADLESS_NULLRHI_RESOURCES live={live_resources}")\n',
                                                   "")
        self.assertNotIn("SPARK_HEADLESS_NULLRHI_RESOURCES", script)
        outcome = self.soak(self.engine(script=script))
        self.assertTrue(any("SPARK_HEADLESS_NULLRHI_RESOURCES records" in f for f in outcome.failures),
                        outcome.failures)

    def test_resource_record_parser_fails_closed(self) -> None:
        self.assertEqual(soak_tool.parse_nullrhi_live_resources("x\nSPARK_HEADLESS_NULLRHI_RESOURCES live=0\r\n"), 0)
        for text, fragment in (("", "found 0"),
                               ("SPARK_HEADLESS_NULLRHI_RESOURCES live=0\nSPARK_HEADLESS_NULLRHI_RESOURCES live=0",
                                "found 2"),
                               ("SPARK_HEADLESS_NULLRHI_RESOURCES live=-1", "malformed"),
                               ("SPARK_HEADLESS_NULLRHI_RESOURCES live=01", "malformed"),
                               ("SPARK_HEADLESS_NULLRHI_RESOURCES live=0 extra=1", "malformed")):
            with self.subTest(text), self.assertRaisesRegex(CollectionError, fragment):
                soak_tool.parse_nullrhi_live_resources(text)

    @LINUX_ONLY
    def test_leaking_engine_fails_the_ceiling(self) -> None:
        # 256 KiB per tick at 60 Hz is ~54 GiB/hour.
        outcome = self.soak(self.engine("hoard.append(bytearray(b'x' * 262144))"))
        self.assertTrue(any(f.startswith("leak:") for f in outcome.failures), outcome.failures)
        self.assertGreater(outcome.slope_bytes_per_hour, 10 * FAST.max_leak_bytes_per_hour)

    @LINUX_ONLY
    def test_crash_is_counted_and_result_written(self) -> None:
        engine = self.engine("if elapsed > 2.8:\n    os.kill(os.getpid(), signal.SIGSEGV)")
        with SHORT_RESULT_RUNS:
            outcome = self.soak(engine, expected_sha=SHA, out=self.out)
        self.assertTrue(outcome.crashed)
        self.assertEqual(outcome.exit_status, -11)
        self.assertTrue(any("SIGSEGV" in f for f in outcome.failures), outcome.failures)
        result = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertEqual(validate_result(result, ["linux-nullrhi-ci"], expected_sha=SHA), [])
        crash = next(m for m in result["measurements"] if m["metricId"] == soak_tool.METRIC_CRASH_COUNT)
        self.assertEqual(crash["value"], 1)

    @LINUX_ONLY
    def test_clean_long_run_writes_valid_result(self) -> None:
        with SHORT_RESULT_RUNS:
            outcome = self.soak(self.engine(), expected_sha=SHA.upper(), out=self.out)
        self.assertEqual(outcome.failures, [])
        result = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertEqual(result["commitSha"], SHA)
        self.assertEqual(validate_result(result, ["linux-nullrhi-ci"], expected_sha=SHA), [])
        self.assertEqual({m["metricId"] for m in result["measurements"]},
                         {soak_tool.METRIC_LEAK_RATE, soak_tool.METRIC_CRASH_COUNT})

    @LINUX_ONLY
    def test_deadlocked_main_thread_is_a_hang(self) -> None:
        engine = self.engine("if elapsed > 1.0:\n    lock = threading.Lock(); lock.acquire(); lock.acquire()")
        with SHORT_RESULT_RUNS:
            outcome = self.soak(engine, expected_sha=SHA, out=self.out)
        self.assertTrue(outcome.hung)
        self.assertTrue(any("neither slept nor ran" in f for f in outcome.failures), outcome.failures)
        self.assertFalse(self.out.exists())
        self.assertTrue(json.loads(self.report.read_text(encoding="utf-8"))["hung"])

    @LINUX_ONLY
    def test_spinning_main_thread_is_a_hang(self) -> None:
        # A spin burns CPU like a slow loop, so it is caught by the exit
        # deadline (1.5 * duration + teardown), not the heartbeat.
        outcome = self.soak(self.engine("if elapsed > 1.0:\n    while True:\n        pass"))
        self.assertTrue(outcome.hung)
        self.assertTrue(any("livelock spin" in f for f in outcome.failures), outcome.failures)

    @LINUX_ONLY
    def test_over_budget_loop_is_not_a_hang(self) -> None:
        # Every tick takes 20 ms (> 16.7 ms), so like the real host the loop
        # never sleeps; its CPU time is the heartbeat.
        busy_tick = "    until = time.monotonic() + 0.02\n    while time.monotonic() < until:\n        pass\n"
        script = STAND_IN.format(fault="").replace("    time.sleep(1 / 60)\n", busy_tick)
        self.assertIn(busy_tick, script)
        outcome = self.soak(self.engine(script=script))
        self.assertFalse(outcome.hung, outcome.failures)
        self.assertEqual(outcome.failures, [])
        self.assertEqual(outcome.stats.frames, FAST.frames)

    @LINUX_ONLY
    def test_never_ready_is_a_startup_hang(self) -> None:
        config = soak_tool.SoakConfig(**{**FAST.__dict__, "startup_timeout_s": 1.0})
        outcome = self.soak(self.engine(script="import time\nprint('booting', flush=True)\ntime.sleep(60)\n"),
                            config)
        self.assertTrue(outcome.hung)
        self.assertIn("no SPARK_MODULE_READY", outcome.failures[0])

    @LINUX_ONLY
    def test_clean_exit_without_tick_record_is_invalid_evidence(self) -> None:
        script = STAND_IN.format(fault="").rsplit("print(f\"SPARK_HEADLESS_TICK_STATS", 1)[0]
        outcome = self.soak(self.engine(script=script))
        self.assertFalse(outcome.crashed)
        self.assertTrue(any(f.startswith("invalid exit evidence") for f in outcome.failures), outcome.failures)

    @LINUX_ONLY
    def test_early_clean_exit_writes_no_result(self) -> None:
        with SHORT_RESULT_RUNS:
            outcome = self.soak(self.engine("if elapsed > 1.5:\n    sys.exit(0)"), expected_sha=SHA, out=self.out)
        self.assertFalse(outcome.crashed or outcome.hung)
        self.assertEqual(outcome.exit_status, 0)
        self.assertTrue(any(f.startswith("invalid exit evidence") for f in outcome.failures), outcome.failures)
        self.assertFalse(self.out.exists())
        self.assertTrue(self.report.exists())

    @LINUX_ONLY
    def test_nonzero_exit_is_a_crash(self) -> None:
        outcome = self.soak(self.engine("if elapsed > 2.0:\n    sys.exit(2)"))
        self.assertTrue(outcome.crashed)
        self.assertTrue(any("status 2" in f for f in outcome.failures), outcome.failures)

    def test_launch_line_is_shared_with_the_tick_collector(self) -> None:
        self.assertEqual(engine_command(Path("/e"), Path("/m.so"), 7200),
                         ["/e", "-headless", "-game", "/m.so", "-require-game", "-test-frames", "7200"])
        self.assertEqual(soak_tool.SoakConfig(duration_s=120.0).frames, 7200)

    def test_inputs_rejected_before_launch(self) -> None:
        engine = self.engine(script="raise SystemExit('must not run')\n")
        cases = {
            "short --out": ({"expected_sha": SHA, "out": self.out}, FAST, "headless-soak-1h"),
            "--out without sha": ({"out": self.out}, soak_tool.SoakConfig(duration_s=3600.0), "--expected-sha"),
            "abbreviated sha": ({"expected_sha": "0123456"}, FAST, "expectedSha"),
            "warm-up swallows window": ({}, soak_tool.SoakConfig(duration_s=3.0, warmup_s=2.9), "fewer than"),
            "non-finite duration": ({}, soak_tool.SoakConfig(duration_s=math.nan), "--duration"),
            "zero ceiling": ({}, soak_tool.SoakConfig(max_leak_bytes_per_hour=0.0), "--max-leak"),
            "heartbeat below interval": ({}, soak_tool.SoakConfig(heartbeat_timeout_s=0.5), "--heartbeat"),
        }
        for name, (kwargs, config, fragment) in cases.items():
            with self.subTest(name), self.assertRaisesRegex(CollectionError, fragment):
                self.soak(engine, config, **kwargs)
        self.assertFalse(self.out.exists())
        self.assertFalse(self.report.exists())

    def test_cli_reports_failure_status(self) -> None:
        engine = self.engine(script="raise SystemExit('must not run')\n")
        with redirect_stderr(io.StringIO()) as stderr:
            status = soak_tool.main(["--engine", str(engine), "--module", str(self.module), "--duration", "60",
                                     "--out", str(self.out), "--expected-sha", SHA])
        self.assertEqual(status, 1)
        self.assertIn("headless-soak-1h", stderr.getvalue())


class SoakNullRHIHeadlessSmoke(unittest.TestCase):
    """Real host soak; registered as CTest Soak_NullRHIHeadlessSmoke (SparkGame, 120 s)
    and, opt-in, Soak_FPSHeadlessNullRHI (SparkGameFPS, SPARK_SOAK_DURATION=600)."""

    def test_real_headless_host_soak(self) -> None:
        engine_env = os.environ.get("SPARK_HEADLESS_ENGINE")
        if not engine_env:
            self.skipTest("SPARK_HEADLESS_ENGINE not set (run through CTest Soak_NullRHIHeadlessSmoke)")
        engine = Path(engine_env)
        module = Path(os.environ["SPARK_HEADLESS_MODULE"])
        self.assertTrue(engine.is_file(), engine)
        self.assertTrue(module.is_file(), module)
        duration = float(os.environ.get("SPARK_SOAK_DURATION", "120"))

        with tempfile.TemporaryDirectory() as tmp:
            report_path = Path(tmp) / "soak-report.json"
            run = subprocess.run(
                [sys.executable, "-B", str(TOOL_DIR / "run_nullrhi_soak.py"), "--engine", str(engine),
                 "--module", str(module), "--duration", f"{duration:g}", "--report", str(report_path)],
                capture_output=True, text=True, timeout=duration * 1.5 + 300)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            report = json.loads(report_path.read_text(encoding="utf-8"))

        self.assertEqual(report["failures"], [])
        self.assertFalse(report["crashed"])
        self.assertFalse(report["hung"])
        self.assertEqual(report["tickStats"]["frames"], round(duration * 60))
        self.assertGreaterEqual(report["fitSampleCount"], soak_tool.MIN_FIT_SAMPLES)
        self.assertLessEqual(report["leakSlopeBytesPerHour"], report["provisionalMaxLeakBytesPerHour"])
        self.assertEqual(report["nullrhiLiveResourcesAtShutdown"], 0)
        print(f"soak: {module.name} {duration:g}s leak slope {report['leakSlopeBytesPerHour']:.0f} B/h over "
              f"{report['fitSampleCount']} samples, max heartbeat gap {report['maxHeartbeatGapS']:.2f}s, "
              f"peak RSS {report['tickStats']['peakRssKib']} KiB, NullRHI live resources at shutdown 0",
              file=sys.stderr)


if __name__ == "__main__":
    unittest.main()
