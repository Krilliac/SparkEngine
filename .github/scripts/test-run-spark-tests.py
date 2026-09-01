#!/usr/bin/env python3
"""Executable and mutation tests for the CTest SparkTests wrapper."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
WRAPPER = REPO_ROOT / "cmake" / "RunSparkTests.cmake"
TEST_MAIN = REPO_ROOT / "Tests" / "TestMain.cpp"

FAKE_RUNNER = textwrap.dedent(
    r'''
    import os
    from pathlib import Path
    import sys
    import time

    def option(name: str) -> Path:
        try:
            return Path(sys.argv[sys.argv.index(name) + 1])
        except (ValueError, IndexError) as error:
            raise SystemExit(f"missing {name}") from error

    output = option("--output-file")
    junit = option("--junit-xml")
    mode = os.environ.get("SPARK_FAKE_TEST_MODE", "success")
    output.parent.mkdir(parents=True, exist_ok=True)
    junit.parent.mkdir(parents=True, exist_ok=True)

    def write_framework(text: str) -> None:
        with output.open("w", encoding="utf-8") as stream:
            stream.write(text)
            stream.flush()

    def write_junit(*, failed: bool = False, empty: bool = False) -> None:
        if empty:
            junit.write_bytes(b"")
            return
        failure = '<failure message="synthetic failure">failed</failure>' if failed else ""
        junit.write_text(
            f'<testsuite tests="1" failures="{1 if failed else 0}">'
            f'<testcase name="Synthetic">{failure}</testcase></testsuite>\n',
            encoding="utf-8",
        )

    print("RAW_STDOUT_MARKER", flush=True)
    print("RAW_STDERR_MARKER", file=sys.stderr, flush=True)

    if mode == "success":
        write_framework("[ RUN    ] Synthetic\\n[   OK   ] Synthetic\\n")
        write_junit()
        raise SystemExit(0)
    if mode == "failure":
        write_framework("[ RUN    ] Synthetic\\nASSERTION_MARKER\\n[ FAILED ] Synthetic\\n")
        write_junit(failed=True)
        raise SystemExit(7)
    if mode == "abrupt":
        framework = output.open("w", encoding="utf-8")
        framework.write("[ RUN    ] AbruptSynthetic\\nFRAMEWORK_BEFORE_ABRUPT\\n")
        print("[ RUN    ] AbruptSynthetic", flush=True)
        print("RAW_BEFORE_ABRUPT", flush=True)
        os._exit(23)
    if mode == "timeout":
        framework = output.open("w", encoding="utf-8")
        framework.write("[ RUN    ] HungSynthetic\\nFRAMEWORK_BEFORE_TIMEOUT\\n")
        print("[ RUN    ] HungSynthetic", flush=True)
        print("RAW_BEFORE_TIMEOUT", flush=True)
        time.sleep(30)
        raise SystemExit(99)
    if mode == "empty-junit":
        write_framework("[ RUN    ] Synthetic\\n[   OK   ] Synthetic\\n")
        write_junit(empty=True)
        raise SystemExit(0)
    if mode == "large-failure":
        write_framework("FRAMEWORK_BEGIN\\n" + "F" * 40000 + "\\nFRAMEWORK_END\\n")
        print("RAW_BEGIN")
        print("R" * 40000)
        print("RAW_END", flush=True)
        raise SystemExit(9)
    raise SystemExit(f"unknown fake mode: {mode}")
    '''
).lstrip()


def wrapper_contract_errors(wrapper: str, test_main: str) -> list[str]:
    errors: list[str] = []
    exact_fragments = (
        'file(REMOVE "${SPARK_JUNIT_REPORT}" "${SPARK_TEST_LOG}" "${SPARK_TEST_OUTPUT}")',
        '--output-file "${SPARK_TEST_OUTPUT}"',
        'OUTPUT_FILE "${SPARK_TEST_LOG}"',
        'ERROR_FILE "${SPARK_TEST_LOG}"',
        'TIMEOUT "${SPARK_TEST_TIMEOUT_SECONDS}"',
        '_spark_read_tail("${SPARK_TEST_LOG}" _raw_excerpt)',
        '_spark_read_tail("${SPARK_TEST_OUTPUT}" _runner_excerpt)',
        'paths must be distinct',
    )
    for fragment in exact_fragments:
        if wrapper.count(fragment) != 1:
            errors.append(f"wrapper is missing or duplicating {fragment}")
    cleanup = wrapper.find("file(REMOVE")
    executable_check = wrapper.find('if(NOT EXISTS "${SPARK_TEST_EXECUTABLE}")')
    launch = wrapper.find("execute_process(")
    if cleanup < 0 or executable_check < 0 or launch < 0 or not cleanup < executable_check < launch:
        errors.append("evidence cleanup must precede executable validation and launch")
    if test_main.count('out.PrintProgress("[ RUN    ] " + g_currentTest + "\\n");') != 1:
        errors.append("TestMain active-test marker is missing or duplicated")
    marker = test_main.find('out.PrintProgress("[ RUN    ] " + g_currentTest + "\\n");')
    test_call = test_main.find("test->func();", marker)
    if marker < 0 or test_call < marker:
        errors.append("TestMain must flush the active-test marker before invoking the test")
    output_start = test_main.find("struct TestOutput")
    output_end = test_main.find("// ============================================================================\n// JUnit XML", output_start)
    output_contract = test_main[output_start:output_end]
    if output_start < 0 or output_end < 0:
        errors.append("TestMain TestOutput contract is missing")
    else:
        if output_contract.count("void PrintProgress(") != 1:
            errors.append("TestOutput must define exactly one progress-only printer")
        progress_start = output_contract.find("void PrintProgress(")
        progress_end = output_contract.find("\n    }", progress_start)
        progress_body = output_contract[progress_start:progress_end]
        stdout_write = progress_body.find("std::cout << msg;")
        stdout_flush = progress_body.find("std::cout.flush();")
        report_write = progress_body.find("file << msg;")
        if not 0 <= stdout_write < stdout_flush < report_write:
            errors.append("TestOutput progress must write and flush stdout before buffering the report")
        if "quiet" in progress_body[:stdout_flush] or "errorsOnly" in progress_body[:stdout_flush]:
            errors.append("TestOutput progress stdout must not be suppressed by output modes")
        if output_contract.count("std::cout.flush();") != 1:
            errors.append("TestOutput must flush stdout exactly once")
        if "file.flush();" in output_contract:
            errors.append("TestOutput must not flush the framework report per test")
    return errors


class RunSparkTestsHarness(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = shutil.which("cmake")
        if cls.cmake is None:
            raise unittest.SkipTest("cmake is unavailable")
        cls.wrapper_text = WRAPPER.read_text(encoding="utf-8")
        cls.test_main_text = TEST_MAIN.read_text(encoding="utf-8")

    def run_case(
        self,
        mode: str,
        *,
        timeout: int = 3,
        missing_executable: bool = False,
        alias_logs: bool = False,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, Path]]:
        temporary = tempfile.TemporaryDirectory(prefix="spark-run-tests-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        driver = root / "fake_runner.py"
        driver.write_text(FAKE_RUNNER, encoding="utf-8")
        paths = {
            "junit": root / "SparkTests-junit.xml",
            "raw": root / "SparkTests.log",
            "framework": root / "SparkTests-output.log",
        }
        if alias_logs:
            paths["framework"] = paths["raw"]
        for path in set(paths.values()):
            path.write_text("STALE_EVIDENCE", encoding="utf-8")
        executable = root / "missing-SparkTests"
        driver_argument: list[str] = []
        if not missing_executable:
            executable = Path(sys.executable)
            driver_argument = [f"-DSPARK_TEST_DRIVER={driver.as_posix()}"]
        command = [
            str(self.cmake),
            f"-DSPARK_TEST_EXECUTABLE={executable.as_posix()}",
            *driver_argument,
            f"-DSPARK_JUNIT_REPORT={paths['junit'].as_posix()}",
            f"-DSPARK_TEST_LOG={paths['raw'].as_posix()}",
            f"-DSPARK_TEST_OUTPUT={paths['framework'].as_posix()}",
            f"-DSPARK_TEST_TIMEOUT_SECONDS={timeout}",
            "-P",
            str(WRAPPER),
        ]
        environment = os.environ.copy()
        environment["SPARK_FAKE_TEST_MODE"] = mode
        completed = subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=environment,
            text=True,
            capture_output=True,
            timeout=15,
            check=False,
        )
        return completed, paths

    def test_live_contract_is_complete(self) -> None:
        self.assertEqual(wrapper_contract_errors(self.wrapper_text, self.test_main_text), [])

    def test_contract_rejects_channel_cleanup_timeout_and_flush_mutations(self) -> None:
        def mutate_progress(old: str, new: str) -> str:
            start = self.test_main_text.find("void PrintProgress(")
            self.assertGreaterEqual(start, 0)
            prefix = self.test_main_text[:start]
            progress_and_tail = self.test_main_text[start:]
            mutated_tail = progress_and_tail.replace(old, new, 1)
            self.assertNotEqual(mutated_tail, progress_and_tail)
            return prefix + mutated_tail

        mutations = {
            "cleanup": self.wrapper_text.replace(
                'file(REMOVE "${SPARK_JUNIT_REPORT}" "${SPARK_TEST_LOG}" "${SPARK_TEST_OUTPUT}")\n',
                "",
                1,
            ),
            "framework argument": self.wrapper_text.replace("--output-file", "--discard-output", 1),
            "raw stdout": self.wrapper_text.replace(
                '    OUTPUT_FILE "${SPARK_TEST_LOG}"\n', "", 1
            ),
            "raw stderr": self.wrapper_text.replace(
                '    ERROR_FILE "${SPARK_TEST_LOG}"\n', "", 1
            ),
            "timeout": self.wrapper_text.replace(
                '    TIMEOUT "${SPARK_TEST_TIMEOUT_SECONDS}"\n', "", 1
            ),
            "raw tail": self.wrapper_text.replace(
                '_spark_read_tail("${SPARK_TEST_LOG}" _raw_excerpt)', "", 1
            ),
            "framework tail": self.wrapper_text.replace(
                '_spark_read_tail("${SPARK_TEST_OUTPUT}" _runner_excerpt)', "", 1
            ),
        }
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.wrapper_text)
                self.assertTrue(wrapper_contract_errors(mutated, self.test_main_text))
        unflushed = self.test_main_text.replace(
            '        out.PrintProgress("[ RUN    ] " + g_currentTest + "\\n");\n',
            '        out.Print("[ RUN    ] " + g_currentTest + "\\n");\n',
            1,
        )
        self.assertNotEqual(unflushed, self.test_main_text)
        self.assertTrue(wrapper_contract_errors(self.wrapper_text, unflushed))
        report_flushed = self.test_main_text.replace(
            "        std::cout.flush();\n",
            "        std::cout.flush();\n        file.flush();\n",
            1,
        )
        self.assertNotEqual(report_flushed, self.test_main_text)
        self.assertTrue(wrapper_contract_errors(self.wrapper_text, report_flushed))
        no_stdout_marker = mutate_progress("        std::cout << msg;\n", "")
        self.assertTrue(wrapper_contract_errors(self.wrapper_text, no_stdout_marker))
        flush_before_marker = mutate_progress(
            "        std::cout << msg;\n        std::cout.flush();\n",
            "        std::cout.flush();\n        std::cout << msg;\n",
        )
        self.assertTrue(wrapper_contract_errors(self.wrapper_text, flush_before_marker))
        quiet_marker = mutate_progress(
            "        std::cout << msg;\n",
            "        if (!quiet)\n            std::cout << msg;\n",
        )
        self.assertTrue(wrapper_contract_errors(self.wrapper_text, quiet_marker))

    def test_success_keeps_distinct_complete_evidence(self) -> None:
        completed, paths = self.run_case("success")
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn("RAW_STDOUT_MARKER", paths["raw"].read_text(encoding="utf-8"))
        self.assertIn("RAW_STDERR_MARKER", paths["raw"].read_text(encoding="utf-8"))
        self.assertIn("[   OK   ] Synthetic", paths["framework"].read_text(encoding="utf-8"))
        self.assertGreater(paths["junit"].stat().st_size, 0)

    def test_normal_failure_reports_both_channels_and_keeps_junit(self) -> None:
        completed, paths = self.run_case("failure")
        combined = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("RAW_STDOUT_MARKER", combined)
        self.assertIn("ASSERTION_MARKER", combined)
        self.assertGreater(paths["junit"].stat().st_size, 0)

    def test_abrupt_exit_and_timeout_keep_active_context_without_junit(self) -> None:
        for mode, timeout in (("abrupt", 3), ("timeout", 1)):
            with self.subTest(mode=mode):
                completed, paths = self.run_case(mode, timeout=timeout)
                self.assertNotEqual(completed.returncode, 0)
                self.assertFalse(paths["junit"].exists())
                self.assertIn("RAW_", paths["raw"].read_text(encoding="utf-8"))
                active_test = "AbruptSynthetic" if mode == "abrupt" else "HungSynthetic"
                combined = completed.stdout + completed.stderr
                self.assertIn(active_test, paths["raw"].read_text(encoding="utf-8"))
                self.assertIn(active_test, combined)
                self.assertEqual(paths["framework"].stat().st_size, 0)

    def test_missing_executable_scrubs_all_stale_evidence(self) -> None:
        completed, paths = self.run_case("success", missing_executable=True)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("executable is missing", completed.stdout + completed.stderr)
        for path in set(paths.values()):
            self.assertFalse(path.exists(), path)

    def test_empty_junit_and_aliased_logs_fail_closed(self) -> None:
        empty, paths = self.run_case("empty-junit")
        self.assertNotEqual(empty.returncode, 0)
        self.assertIn("empty JUnit", empty.stdout + empty.stderr)
        self.assertEqual(paths["junit"].stat().st_size, 0)
        aliased, _ = self.run_case("success", alias_logs=True)
        self.assertNotEqual(aliased.returncode, 0)
        self.assertIn("paths must be distinct", aliased.stdout + aliased.stderr)

    def test_failure_output_is_bounded_to_the_tail_of_each_channel(self) -> None:
        completed, _ = self.run_case("large-failure")
        combined = completed.stdout + completed.stderr
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("RAW_END", combined)
        self.assertIn("FRAMEWORK_END", combined)
        self.assertNotIn("RAW_BEGIN", combined)
        self.assertNotIn("FRAMEWORK_BEGIN", combined)


if __name__ == "__main__":
    unittest.main(verbosity=2)
