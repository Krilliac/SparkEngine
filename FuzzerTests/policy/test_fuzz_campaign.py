#!/usr/bin/env python3
"""Tests for tools/fuzz-policy/run_campaign.py (SEC-120 scheduled campaign).

Lives beside the policy tests so FuzzPolicyAdversarial (the required
fuzz-policy job) discovers and runs it on every change.

The end-to-end cases configure a real CTest tree (a compiler-free CMake
project) whose fuzz-labelled tests point at a small stand-in for a libFuzzer
binary, so target discovery goes through the same ``ctest --show-only`` path
the scheduled job uses.  The stand-in honours the libFuzzer flags the runner
relies on (-artifact_prefix, -minimize_crash, -exact_artifact_path,
-print_final_stats, UBSAN_OPTIONS halt_on_error) and records every argv it
receives.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "fuzz-policy"))

import run_campaign  # noqa: E402

FAKE_FUZZER = textwrap.dedent(
    """\
    import json, os, pathlib, sys
    args = sys.argv[1:]
    options = dict(a[1:].split("=", 1) for a in args if a.startswith("-"))
    positionals = [a for a in args if not a.startswith("-")]
    with open(os.environ["FAKE_ARGV_LOG"], "a") as log:
        log.write(json.dumps(args) + "\\n")
    mode = os.environ.get("FAKE_MODE", "clean")
    if options.get("minimize_crash") == "1":
        if mode == "minimize-nothing":
            print("INFO: the input did not crash")
            sys.exit(1)
        data = pathlib.Path(positionals[0]).read_bytes()
        pathlib.Path(options["exact_artifact_path"]).write_bytes(data[:1])
        print("CRASH_MIN: minimized")
        sys.exit(0 if mode != "minimize-fails" else 1)
    corpus = pathlib.Path(positionals[0])
    (corpus / "new-unit").write_bytes(b"mutated")
    print("#2\\tINITED")
    print("#77\\tpulse")
    if mode == "ubsan":
        # Recoverable UBSan: the report is printed; only halt_on_error=1 makes
        # it fatal, and then libFuzzer's death callback writes a crash- unit.
        print("Json.cpp:12:5: runtime error: signed integer overflow")
        if "halt_on_error=1" in os.environ.get("UBSAN_OPTIONS", ""):
            pathlib.Path(options["artifact_prefix"] + "crash-ub").write_bytes(b"UB")
            sys.exit(1)
    if mode in ("crash", "minimize-fails", "minimize-nothing"):
        pathlib.Path(options["artifact_prefix"] + "crash-deadbeef").write_bytes(b"XYZ-crash")
        print("==1==ERROR: AddressSanitizer: heap-buffer-overflow")
        sys.exit(1)
    if mode == "mutate-committed":
        pathlib.Path(os.environ["FAKE_COMMITTED"], "leaked").write_bytes(b"x")
    if mode == "mutate-generated":
        pathlib.Path(positionals[1], "leaked").write_bytes(b"x")
    if mode == "exit-nonzero":
        sys.exit(3)
    print("Done 123 runs in 1 second(s)")
    print("stat::number_executed_units: 123")
    print("stat::peak_rss_mb: 42")
    sys.exit(0)
    """
)


def _have_cmake() -> bool:
    return shutil.which("cmake") is not None and shutil.which("ctest") is not None


class ParseCtestTests(unittest.TestCase):
    def _doc(self, command: list[str], name: str = "FuzzThingSmoke", workdir: str | None = None) -> dict:
        entry: dict = {"name": name, "command": command}
        if workdir:
            entry["properties"] = [{"name": "WORKING_DIRECTORY", "value": workdir}]
        return {"tests": [entry]}

    def test_replay_flags_are_dropped_and_limits_kept(self) -> None:
        targets = run_campaign.parse_ctest_tests(
            self._doc(["/b/fuzz", "-max_len=64", "-timeout=1", "-rss_limit_mb=256", "-runs=8",
                       "-max_total_time=4", "/src/corpus"])
        )
        self.assertEqual(len(targets), 1)
        self.assertEqual(targets[0].options, ("-max_len=64", "-timeout=1", "-rss_limit_mb=256"))
        self.assertEqual(targets[0].corpus, Path("/src/corpus"))

    def test_relative_corpus_resolves_against_working_directory(self) -> None:
        targets = run_campaign.parse_ctest_tests(self._doc(["/b/fuzz", "corpus"], workdir="/repo"))
        self.assertEqual(targets[0].corpus, Path("/repo/corpus"))

    def test_rejects_shapes_the_runner_cannot_honour(self) -> None:
        hostile = {
            "runner-owned flag": self._doc(["/b/fuzz", "-artifact_prefix=/x/", "/c"]),
            "fork mode": self._doc(["/b/fuzz", "-fork=4", "/c"]),
            "two corpora": self._doc(["/b/fuzz", "/c1", "/c2"]),
            "no corpus": self._doc(["/b/fuzz", "-runs=1"]),
            "flag without value": self._doc(["/b/fuzz", "-help", "/c"]),
            "path-like name": self._doc(["/b/fuzz", "/c"], name="../escape"),
            "no tests array": {"tests": None},
        }
        for label, document in hostile.items():
            with self.subTest(label), self.assertRaises(run_campaign.CampaignError):
                run_campaign.parse_ctest_tests(document)

    def test_asan_quarantine_is_capped_unless_caller_sets_it(self) -> None:
        with mock.patch.dict(os.environ, {"ASAN_OPTIONS": "detect_leaks=1"}):
            self.assertEqual(
                run_campaign.campaign_environment()["ASAN_OPTIONS"], "detect_leaks=1:quarantine_size_mb=32"
            )
        with mock.patch.dict(os.environ, {"ASAN_OPTIONS": "quarantine_size_mb=64"}):
            self.assertEqual(run_campaign.campaign_environment()["ASAN_OPTIONS"], "quarantine_size_mb=64")

    def test_ubsan_reports_are_fatal_unless_caller_sets_halt_on_error(self) -> None:
        with mock.patch.dict(os.environ, {"UBSAN_OPTIONS": "suppressions=x.supp"}):
            self.assertEqual(
                run_campaign.campaign_environment()["UBSAN_OPTIONS"],
                "suppressions=x.supp:halt_on_error=1:print_stacktrace=1",
            )
        with mock.patch.dict(os.environ, {"UBSAN_OPTIONS": "halt_on_error=0"}):
            self.assertEqual(run_campaign.campaign_environment()["UBSAN_OPTIONS"], "halt_on_error=0")

    def test_exit_status_prefers_findings_over_setup_errors(self) -> None:
        def result(status: str) -> run_campaign.TargetResult:
            outcome = run_campaign.TargetResult(name="t", binary="b", corpus="c")
            outcome.status = status
            return outcome

        self.assertEqual(run_campaign.exit_status([result("clean"), result("clean")]), 0)
        self.assertEqual(run_campaign.exit_status([result("clean"), result("setup-error")]), 2)
        for finding in run_campaign.FINDING_STATUSES:
            with self.subTest(finding):
                self.assertEqual(run_campaign.exit_status([result("setup-error"), result(finding)]), 1)

    def test_stats_fall_back_to_progress_lines(self) -> None:
        self.assertEqual(run_campaign.parse_run_stats("#2\tINITED\n#900\tpulse\n"), {"number_executed_units": 900})
        stats = run_campaign.parse_run_stats("stat::number_executed_units: 5\nstat::peak_rss_mb: 31\n")
        self.assertEqual(stats, {"number_executed_units": 5, "peak_rss_mb": 31})


@unittest.skipUnless(_have_cmake(), "cmake/ctest not available")
class CampaignEndToEnd(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="fuzz-campaign-test-")
        self.root = Path(self._tmp.name)
        # Same layout as the repository: FuzzerTests/corpora/<name> pairs with
        # FuzzerTests/generated/<name>, which individual tests create when needed.
        self.corpus = self.root / "corpora" / "committed-corpus"
        self.corpus.mkdir(parents=True)
        self.generated = self.root / "generated" / "committed-corpus"
        (self.corpus / "seed-a").write_bytes(b"seed a")
        (self.corpus / "seed-b").write_bytes(b"seed b")
        self.argv_log = self.root / "argv.log"

        fuzzer = self.root / "fake_fuzzer"
        fuzzer.write_text(f"#!{sys.executable}\n" + FAKE_FUZZER, encoding="utf-8")
        fuzzer.chmod(0o755)

        source = self.root / "src"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            textwrap.dedent(
                f"""\
                cmake_minimum_required(VERSION 3.25)
                project(FakeFuzz LANGUAGES NONE)
                enable_testing()
                add_test(NAME FakeFuzzSmoke COMMAND "{fuzzer}" -max_len=64 -timeout=1 -runs=2 "{self.corpus}")
                set_tests_properties(FakeFuzzSmoke PROPERTIES LABELS "fuzz;fuzz-smoke;security")
                add_test(NAME NotACampaignTarget COMMAND "{fuzzer}" "{self.corpus}")
                set_tests_properties(NotACampaignTarget PROPERTIES LABELS "fuzz-smoke")
                """
            ),
            encoding="utf-8",
        )
        self.build = self.root / "build"
        configure = subprocess.run(
            ["cmake", "-S", str(source), "-B", str(self.build)], capture_output=True, text=True, check=False
        )
        self.assertEqual(configure.returncode, 0, configure.stderr)
        self.output = self.root / "out"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, mode: str, *extra: str, env: dict[str, str] | None = None) -> tuple[int, dict | None]:
        child_env = {"FAKE_MODE": mode, "FAKE_ARGV_LOG": str(self.argv_log), "FAKE_COMMITTED": str(self.corpus)}
        child_env.update(env or {})
        with mock.patch.dict(os.environ, child_env):
            if "UBSAN_OPTIONS" not in child_env:
                # The runner's default must be what the stand-in sees.
                os.environ.pop("UBSAN_OPTIONS", None)
            code = run_campaign.main(
                ["--build-dir", str(self.build), "--output", str(self.output), "--seconds", "3",
                 "--minimize-seconds", "2", *extra]
            )
        summary_path = self.output / "campaign-summary.json"
        summary = json.loads(summary_path.read_text()) if summary_path.is_file() else None
        return code, summary

    def _argv(self) -> list[list[str]]:
        return [json.loads(line) for line in self.argv_log.read_text().splitlines()]

    def _committed(self) -> dict[str, bytes]:
        return {p.name: p.read_bytes() for p in self.corpus.iterdir()}

    def test_clean_campaign_mutates_only_a_disposable_copy(self) -> None:
        before = self._committed()
        code, summary = self._run("clean")
        self.assertEqual(code, 0)
        self.assertEqual(self._committed(), before, "committed corpus must never gain units")
        self.assertTrue(summary["passed"])
        self.assertEqual([t["name"] for t in summary["targets"]], ["FakeFuzzSmoke"])
        target = summary["targets"][0]
        self.assertEqual(target["status"], "clean")
        self.assertEqual(target["executed_units"], 123)
        self.assertEqual(target["peak_rss_mb"], 42)
        self.assertEqual(target["seed_count"], 2)
        self.assertEqual(target["new_units"], 1)
        self.assertTrue(target["committed_corpus_unchanged"])
        self.assertGreater(target["crash_free_seconds"], 0)

        (argv,) = self._argv()
        self.assertIn("-max_total_time=3", argv)
        self.assertIn("-max_len=64", argv)
        self.assertNotIn("-runs=2", argv)
        self.assertNotEqual(Path(argv[-1]), self.corpus)
        self.assertFalse(Path(argv[-1]).exists(), "the working corpus copy must be removed")

    def test_crash_fails_and_retains_raw_and_minimized_reproducer(self) -> None:
        code, summary = self._run("crash")
        self.assertEqual(code, 1)
        self.assertFalse(summary["passed"])
        target = summary["targets"][0]
        self.assertEqual(target["status"], "crash")
        (artifact,) = target["artifacts"]
        self.assertEqual((self.output / artifact["raw"]).read_bytes(), b"XYZ-crash")
        self.assertEqual((self.output / artifact["minimized"]).read_bytes(), b"X")
        self.assertEqual(artifact["minimized_bytes"], 1)
        self.assertTrue((self.output / artifact["minimize_log"]).is_file())
        self.assertTrue((self.output / target["log"]).is_file())
        minimize_argv = self._argv()[1]
        self.assertIn("-minimize_crash=1", minimize_argv)
        self.assertIn("-timeout=1", minimize_argv)

    def test_failed_minimization_still_keeps_the_raw_crash(self) -> None:
        # The stand-in writes an output yet reports failure; the raw file is the
        # evidence that must survive whatever minimization does.
        code, summary = self._run("minimize-fails")
        self.assertEqual(code, 1)
        (artifact,) = summary["targets"][0]["artifacts"]
        self.assertEqual(artifact["minimize_exit_code"], 1)
        self.assertTrue((self.output / artifact["raw"]).is_file())

    def test_minimization_that_writes_nothing_still_keeps_the_raw_crash(self) -> None:
        # Real libFuzzer often leaves no output ("the input did not crash").
        code, summary = self._run("minimize-nothing")
        self.assertEqual(code, 1)
        target = summary["targets"][0]
        self.assertEqual(target["status"], "crash")
        (artifact,) = target["artifacts"]
        self.assertIsNone(artifact["minimized"])
        self.assertEqual(artifact["minimize_exit_code"], 1)
        self.assertEqual((self.output / artifact["raw"]).read_bytes(), b"XYZ-crash")
        self.assertEqual(artifact["raw_sha256"], hashlib.sha256(b"XYZ-crash").hexdigest())

    def test_recoverable_ubsan_report_becomes_a_crash_reproducer(self) -> None:
        code, summary = self._run("ubsan")
        self.assertEqual(code, 1)
        target = summary["targets"][0]
        self.assertEqual(target["status"], "crash")
        (artifact,) = target["artifacts"]
        self.assertEqual((self.output / artifact["raw"]).read_bytes(), b"UB")

    def test_ubsan_report_with_clean_exit_is_still_a_finding(self) -> None:
        code, summary = self._run("ubsan", env={"UBSAN_OPTIONS": "halt_on_error=0"})
        self.assertEqual(code, 1)
        target = summary["targets"][0]
        self.assertEqual((target["status"], target["exit_code"]), ("sanitizer-report", 0))
        self.assertFalse(summary["passed"])

    def test_writing_into_the_committed_corpus_is_a_finding(self) -> None:
        code, summary = self._run("mutate-committed")
        self.assertEqual(code, 1)
        target = summary["targets"][0]
        self.assertEqual(target["status"], "corpus-mutated")
        self.assertFalse(target["committed_corpus_unchanged"])

    def _add_generated(self) -> None:
        self.generated.mkdir(parents=True)
        for index in range(3):
            (self.generated / f"unit-{index}").write_bytes(bytes([index, 0x0D, 0xFF]))

    def test_generated_corpus_is_a_read_only_second_input(self) -> None:
        self._add_generated()
        generated_before = {p.name: p.read_bytes() for p in self.generated.iterdir()}
        code, summary = self._run("clean")
        self.assertEqual(code, 0)
        target = summary["targets"][0]
        self.assertEqual((target["status"], target["seed_count"], target["generated_count"]), ("clean", 2, 3))
        self.assertTrue(target["committed_corpus_unchanged"])
        self.assertEqual({p.name: p.read_bytes() for p in self.generated.iterdir()}, generated_before)

        (argv,) = self._argv()
        self.assertEqual(Path(argv[-1]), self.generated)
        self.assertNotIn(Path(argv[-2]), (self.corpus, self.generated), "the writable corpus must be the copy")

    def test_writing_into_the_generated_corpus_is_a_finding(self) -> None:
        self._add_generated()
        code, summary = self._run("mutate-generated")
        self.assertEqual(code, 1)
        target = summary["targets"][0]
        self.assertEqual(target["status"], "corpus-mutated")
        self.assertFalse(target["committed_corpus_unchanged"])

    def test_abnormal_exit_without_reproducer_fails(self) -> None:
        code, summary = self._run("exit-nonzero")
        self.assertEqual(code, 1)
        self.assertEqual(summary["targets"][0]["status"], "abnormal-exit")

    def test_setup_errors_exit_two_without_running(self) -> None:
        code, summary = self._run("clean", "--target", "NoSuchSmoke")
        self.assertEqual((code, summary), (2, None))
        self.output.mkdir()
        code, _ = self._run("clean")
        self.assertEqual(code, 2, "an existing output directory must not be reused")
        self.assertFalse(self.argv_log.exists())

    def test_per_target_setup_errors_exit_two_with_a_summary(self) -> None:
        shutil.rmtree(self.corpus)
        code, summary = self._run("clean")
        self.assertEqual(code, 2)
        target = summary["targets"][0]
        self.assertEqual(target["status"], "setup-error")
        self.assertIn("corpus", target["detail"])
        self.assertTrue(summary["complete"])
        self.assertFalse(summary["passed"])

    def test_non_executable_fuzz_binary_is_a_setup_error(self) -> None:
        # (A missing binary is already refused by ctest discovery itself.)
        (self.root / "fake_fuzzer").chmod(0o644)
        code, summary = self._run("clean")
        self.assertEqual(code, 2)
        self.assertEqual(summary["targets"][0]["status"], "setup-error")
        self.assertFalse(self.argv_log.exists())

    def test_budget_over_the_campaign_cap_is_refused_before_running(self) -> None:
        code, summary = self._run("clean", "--max-campaign-seconds", "2")
        self.assertEqual((code, summary), (2, None))
        self.assertFalse(self.argv_log.exists())
        code, summary = self._run("clean", "--max-campaign-seconds", "3")
        self.assertEqual(code, 0)
        self.assertTrue(summary["complete"])

    def test_summary_is_written_before_and_after_each_target(self) -> None:
        snapshots: list[dict] = []
        real_run_target = run_campaign.run_target

        def observing_run_target(*args, **kwargs):
            snapshots.append(json.loads((self.output / "campaign-summary.json").read_text()))
            return real_run_target(*args, **kwargs)

        with mock.patch.object(run_campaign, "run_target", observing_run_target):
            code, summary = self._run("clean")
        self.assertEqual(code, 0)
        (before,) = snapshots
        self.assertEqual((before["complete"], before["passed"], before["targets"]), (False, False, []))
        self.assertEqual(before["planned_targets"], 1)
        self.assertTrue(summary["complete"])
        self.assertTrue(summary["passed"])
        self.assertFalse((self.output / "campaign-summary.json.tmp").exists())

    def test_output_inside_committed_corpus_is_refused(self) -> None:
        self._add_generated()
        for committed in (self.corpus, self.generated):
            with self.subTest(committed.parent.name):
                self.output = committed / "campaign"
                code, _ = self._run("clean")
                self.assertEqual(code, 2)
                self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
