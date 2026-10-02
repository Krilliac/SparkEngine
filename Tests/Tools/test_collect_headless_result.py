#!/usr/bin/env python3
"""PERF-100: headless NullRHI tick/RSS result producer.

ParserTests and CollectorGuardTests exercise tools/perf-budget/
collect_headless_result.py against well-formed, forged, and malformed host
output (the collector's process path is driven by a stand-in executable).
HeadlessTickLoopBenchmark runs the real SparkEngine headless host with the
SparkGame module when CTest supplies SPARK_HEADLESS_ENGINE/SPARK_HEADLESS_MODULE
(Benchmark_HeadlessTickLoop), and proves the written result passes
validate_result while compare_results stays NON-AUTHORITATIVE on the
uncertified linux-nullrhi-ci row.
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
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "perf-budget"
sys.path.insert(0, str(TOOL_DIR))

import collect_headless_result as collector  # noqa: E402
from compare_results import main as compare_main  # noqa: E402
from validate_budget import validate_result  # noqa: E402

BUDGET_DIR = REPO_ROOT / "perf-budgets" / "v1"
SHA = "0123456789abcdef0123456789abcdef01234567"
READY = "SPARK_MODULE_READY count=1"
GOOD = "SPARK_HEADLESS_TICK_STATS backend=null frames=120 p50_us=180 p99_us=911 max_us=1500 peak_rss_kib=204800"
# Stand-in engine code that prints GOOD with its own VmHWM, as the real host does.
STAND_IN_RECORD = (
    "hwm = next(l.split()[1] for l in open('/proc/self/status') if l.startswith('VmHWM:'))\n"
    f"print({GOOD.rsplit('=', 1)[0] + '='!r} + hwm)\n"
)


def _output(*lines: str) -> str:
    return "\n".join(lines) + "\n"


class ParserTests(unittest.TestCase):
    def test_valid_record_parses(self) -> None:
        stats = collector.parse_tick_stats(_output("boot", READY, "[INFO] tick", GOOD), 120)
        self.assertEqual((stats.frames, stats.p50_us, stats.p99_us, stats.max_us, stats.peak_rss_kib),
                         (120, 180, 911, 1500, 204800))

    def test_crlf_output_parses(self) -> None:
        stats = collector.parse_tick_stats(f"{READY}\r\n{GOOD}\r\n", 120)
        self.assertEqual(stats.p99_us, 911)

    def test_logger_echo_of_marker_is_not_a_record(self) -> None:
        echoed = f"[18:48:53] [INFO ] [Core] {GOOD} (file.cpp:12)"
        stats = collector.parse_tick_stats(_output(READY, echoed, GOOD), 120)
        self.assertEqual(stats.frames, 120)

    def assert_rejected(self, text: str, fragment: str, frames: int = 120) -> None:
        with self.assertRaises(collector.CollectionError) as caught:
            collector.parse_tick_stats(text, frames)
        self.assertIn(fragment, str(caught.exception))

    def test_missing_record_rejected(self) -> None:
        self.assert_rejected(_output(READY), "found 0 SPARK_HEADLESS_TICK_STATS")

    def test_forged_duplicate_record_rejected(self) -> None:
        forged = "SPARK_HEADLESS_TICK_STATS backend=null frames=120 p50_us=1 p99_us=1 max_us=1 peak_rss_kib=1"
        self.assert_rejected(_output(READY, forged, GOOD), "found 2 SPARK_HEADLESS_TICK_STATS")

    def test_malformed_records_rejected(self) -> None:
        malformed = [
            GOOD + " ",
            GOOD + " extra=1",
            GOOD.replace("p50_us=180", "p50_us=0180"),
            GOOD.replace("p50_us=180", "p50_us=-180"),
            GOOD.replace("p50_us=180", "p50_us=0x1f"),
            GOOD.replace("p50_us=180", "p50_us=18.0"),
            GOOD.replace("p50_us=180", "p50_us=١٨٠"),
            GOOD.replace("backend=null", "backend=d3d11"),
            GOOD.replace(" p99_us=911", ""),
            GOOD.replace(" peak_rss_kib=204800", ""),
            GOOD.replace("peak_rss_kib=204800", "peak_rss_kib=200MB"),
            GOOD.replace("frames=120 p50_us=180", "p50_us=180 frames=120"),
            "SPARK_HEADLESS_TICK_STATS",
        ]
        for line in malformed:
            with self.subTest(line=line):
                self.assert_rejected(_output(READY, line), "malformed")

    def test_non_null_backend_rejected(self) -> None:
        self.assert_rejected(_output(READY, GOOD.replace("backend=null", "backend=none")), "backend=none")

    def test_frame_count_must_match_request(self) -> None:
        self.assert_rejected(_output(READY, GOOD), "expected exactly 600", frames=600)

    def test_unordered_percentiles_rejected(self) -> None:
        self.assert_rejected(_output(READY, GOOD.replace("p99_us=911", "p99_us=100")), "not ordered")
        self.assert_rejected(_output(READY, GOOD.replace("max_us=1500", "max_us=900")), "not ordered")

    def test_out_of_histogram_range_rejected(self) -> None:
        huge = str(collector.MAX_TRACKED_US + 1)
        self.assert_rejected(_output(READY, GOOD.replace("max_us=1500", f"max_us={huge}")), "histogram range")

    def test_unmeasured_peak_rss_rejected(self) -> None:
        self.assert_rejected(_output(READY, GOOD.replace("peak_rss_kib=204800", "peak_rss_kib=0")), "could not be")

    def test_peak_rss_above_kernel_bound_rejected(self) -> None:
        stats = collector.parse_tick_stats(_output(READY, GOOD), 120)
        collector.check_peak_rss_bound(stats, 204800)
        with self.assertRaisesRegex(collector.CollectionError, "ru_maxrss bound"):
            collector.check_peak_rss_bound(stats, 204799)

    def test_module_ready_evidence_required(self) -> None:
        self.assert_rejected(_output(GOOD), "found 0 SPARK_MODULE_READY")
        self.assert_rejected(_output(READY, READY, GOOD), "found 2 SPARK_MODULE_READY")
        self.assert_rejected(_output("SPARK_MODULE_READY count=2", GOOD), "expected exactly one module")
        self.assert_rejected(_output(GOOD, READY), "before the module became ready")

    def test_result_passes_schema_and_budget_definitions(self) -> None:
        stats = collector.parse_tick_stats(_output(READY, GOOD), 120)
        result = collector.build_result(stats, SHA, "2026-09-24T00:00:00Z")
        self.assertEqual(sorted(result), ["commitSha", "hardwareRowId", "measurements", "timestamp"])
        values = {m["metricId"]: (m["value"], m["unit"], m["sampleCount"]) for m in result["measurements"]}
        self.assertEqual(values[collector.METRIC_P50], (0.18, "ms", 120))
        self.assertEqual(values[collector.METRIC_P99], (0.911, "ms", 120))
        self.assertEqual(values[collector.METRIC_PEAK_RSS], (200.0, "megabytes", 1))
        self.assertEqual(validate_result(result, ["linux-nullrhi-ci"], expected_sha=SHA), [])
        collector.check_against_budget(result, BUDGET_DIR, SHA)

    def test_result_rejects_sha_mismatch_and_empty_rss(self) -> None:
        stats = collector.parse_tick_stats(_output(READY, GOOD), 120)
        result = collector.build_result(stats, SHA, "2026-09-24T00:00:00Z")
        with self.assertRaises(collector.CollectionError):
            collector.check_against_budget(result, BUDGET_DIR, "f" * 40)
        with self.assertRaises(collector.CollectionError):
            collector.build_result(collector.TickStats(120, 180, 911, 1500, 0), SHA, "2026-09-24T00:00:00Z")


class CollectorGuardTests(unittest.TestCase):
    """Drive collect() end to end with a stand-in engine executable."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.module = self.root / "libSparkGame.so"
        self.module.write_bytes(b"\x7fELF")
        self.out = self.root / "out" / "result.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def fake_engine(self, body: str) -> Path:
        engine = self.root / "SparkEngine"
        engine.write_text(f"#!{sys.executable}\nimport sys, time\n" + textwrap.dedent(body), encoding="utf-8")
        engine.chmod(engine.stat().st_mode | stat.S_IXUSR)
        return engine

    def collect(self, engine: Path, sha: str = SHA, frames: int = 120, timeout: float = 30.0) -> dict:
        return collector.collect(engine, self.module, frames, sha, self.out, BUDGET_DIR, timeout)

    @unittest.skipUnless(sys.platform.startswith("linux"), "collector supports the Linux row only")
    def test_stand_in_run_writes_valid_result(self) -> None:
        engine = self.fake_engine(textwrap.dedent(f"""
            assert sys.argv[1:] == ["-headless", "-game", {str(self.module)!r}, "-require-game",
                                    "-test-frames", "120"], sys.argv
            print({READY!r})
            """) + STAND_IN_RECORD)
        result = self.collect(engine)
        written = json.loads(self.out.read_text(encoding="utf-8"))
        self.assertEqual(written, result)
        self.assertEqual(validate_result(written, ["linux-nullrhi-ci"], expected_sha=SHA), [])
        rss = next(m for m in written["measurements"] if m["metricId"] == collector.METRIC_PEAK_RSS)
        self.assertGreater(rss["value"], 1.0)

    @unittest.skipUnless(sys.platform.startswith("linux"), "collector supports the Linux row only")
    def test_collector_memory_is_not_attributed_to_engine(self) -> None:
        # Linux folds the pre-exec (collector) image's high-water mark into the
        # child's ru_maxrss. Hold a large touched buffer in this process and
        # prove the reported peak is the stand-in's own VmHWM, not ours.
        ballast_mib = 384
        ballast = bytearray(ballast_mib * 1024 * 1024)
        for offset in range(0, len(ballast), 4096):
            ballast[offset] = 1
        engine = self.fake_engine(f"print({READY!r})\n" + STAND_IN_RECORD)

        wait4_bounds: list[int] = []
        real_run_engine = collector.run_engine

        def recording_run_engine(*args: object) -> tuple[str, int]:
            stdout_text, maxrss_kib = real_run_engine(*args)
            wait4_bounds.append(maxrss_kib)
            return stdout_text, maxrss_kib

        collector.run_engine = recording_run_engine
        try:
            result = self.collect(engine)
        finally:
            collector.run_engine = real_run_engine
        self.assertEqual(ballast[0], 1)

        rss = next(m for m in result["measurements"] if m["metricId"] == collector.METRIC_PEAK_RSS)
        # The discriminating precondition: the kernel's rusage IS contaminated.
        self.assertGreaterEqual(wait4_bounds[0] / 1024.0, ballast_mib)
        self.assertGreater(rss["value"], 1.0)
        self.assertLess(rss["value"], ballast_mib / 4)

    @unittest.skipUnless(sys.platform.startswith("linux"), "collector supports the Linux row only")
    def test_self_reported_peak_above_kernel_bound_rejected(self) -> None:
        engine = self.fake_engine(f"print({READY!r}); print({GOOD.replace('204800', '999999999')!r})\n")
        with self.assertRaisesRegex(collector.CollectionError, "ru_maxrss bound"):
            self.collect(engine)
        self.assertFalse(self.out.exists())

    @unittest.skipUnless(sys.platform.startswith("linux"), "collector supports the Linux row only")
    def test_nonzero_exit_rejected_and_nothing_written(self) -> None:
        engine = self.fake_engine(f"print({READY!r}); print({GOOD!r}); sys.exit(2)\n")
        with self.assertRaisesRegex(collector.CollectionError, "status 2"):
            self.collect(engine)
        self.assertFalse(self.out.exists())

    @unittest.skipUnless(sys.platform.startswith("linux"), "collector supports the Linux row only")
    def test_hung_engine_is_killed(self) -> None:
        engine = self.fake_engine("time.sleep(60)\n")
        with self.assertRaisesRegex(collector.CollectionError, "did not exit"):
            self.collect(engine, timeout=1.0)
        self.assertFalse(self.out.exists())

    def test_abbreviated_or_missing_sha_rejected_before_launch(self) -> None:
        engine = self.fake_engine("raise SystemExit('must not run')\n")
        for sha in ("", "0123456", "g" * 40):
            with self.subTest(sha=sha), self.assertRaises(collector.CollectionError):
                self.collect(engine, sha=sha)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            collector.main(["--engine", str(engine), "--out", str(self.out)])

    def test_frame_bounds_rejected(self) -> None:
        engine = self.fake_engine("raise SystemExit('must not run')\n")
        for frames in (0, -1, collector.MAX_FRAMES + 1):
            with self.subTest(frames=frames), self.assertRaises(collector.CollectionError):
                self.collect(engine, frames=frames)


class HeadlessTickLoopBenchmark(unittest.TestCase):
    """Real host run; registered as CTest Benchmark_HeadlessTickLoop."""

    def test_real_headless_host_produces_non_authoritative_result(self) -> None:
        engine_env = os.environ.get("SPARK_HEADLESS_ENGINE")
        if not engine_env:
            self.skipTest("SPARK_HEADLESS_ENGINE not set (run through CTest Benchmark_HeadlessTickLoop)")
        engine = Path(engine_env)
        module = Path(os.environ["SPARK_HEADLESS_MODULE"])
        self.assertTrue(engine.is_file(), engine)
        self.assertTrue(module.is_file(), module)
        # The SHA is supplied to the collector by this harness, never inferred
        # by the collector from its own output.
        sha = os.environ.get("SPARK_PERF_EXPECTED_SHA") or subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"], check=True, capture_output=True,
            text=True).stdout.strip()

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "headless-result.json"
            run = subprocess.run(
                [sys.executable, "-B", str(TOOL_DIR / "collect_headless_result.py"), "--engine", str(engine),
                 "--module", str(module), "--frames", "180", "--expected-sha", sha, "--out", str(out)],
                capture_output=True, text=True, timeout=150)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            result = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(validate_result(result, ["linux-nullrhi-ci"], expected_sha=sha), [])
            values = {m["metricId"]: m for m in result["measurements"]}
            self.assertEqual(values[collector.METRIC_P50]["sampleCount"], 180)
            self.assertLessEqual(values[collector.METRIC_P50]["value"], values[collector.METRIC_P99]["value"])
            self.assertGreater(values[collector.METRIC_PEAK_RSS]["value"], 1.0)

            report = io.StringIO()
            with redirect_stdout(report):
                status = compare_main([str(BUDGET_DIR), str(out), "--expected-sha", sha])
            self.assertEqual(status, 1, report.getvalue())
            self.assertIn("NON-AUTHORITATIVE", report.getvalue())


if __name__ == "__main__":
    unittest.main()
