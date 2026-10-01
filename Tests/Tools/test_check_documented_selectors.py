#!/usr/bin/env python3
"""CI-110: tools/site-data/check_documented_selectors.py resolves documented ctest commands in a real tree.

A tiny fixture CMake project is configured into a temporary ``linux-gcc-release``
tree (the name of a real preset's build tree) with an enabled ``alpha`` test, a
``beta`` test that is only DISABLED, and a ``gamma`` test whose command does not
exist. Documented commands are fed through ``--commands-json`` so no ledger file
is read or touched. A selection must resolve to an enabled test with a built
executable; a disabled-only, missing-executable or empty selection fails; a
command for another preset's tree is not applicable and never counts; and a
tree with nothing to check exits 2 instead of passing.
"""
from __future__ import annotations

import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import check_documented_selectors as checker  # noqa: E402

FIXTURE_CMAKE = """cmake_minimum_required(VERSION 3.20)
project(DocumentedSelectorFixture NONE)
enable_testing()
add_test(NAME alpha_runs COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(alpha_runs PROPERTIES LABELS alpha)
add_test(NAME beta_disabled COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(beta_disabled PROPERTIES LABELS beta DISABLED TRUE)
add_test(NAME gamma_unbuilt COMMAND "${CMAKE_BINARY_DIR}/never-built-tool")
set_tests_properties(gamma_unbuilt PROPERTIES LABELS gamma)
add_test(NAME delta_fails COMMAND "${CMAKE_COMMAND}" -E false)
set_tests_properties(delta_fails PROPERTIES LABELS delta)
add_test(NAME epsilon_advisory_fails COMMAND "${CMAKE_COMMAND}" -E false)
set_tests_properties(epsilon_advisory_fails PROPERTIES LABELS "epsilon;advisory-only")
add_test(NAME epsilon_runs COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(epsilon_runs PROPERTIES LABELS epsilon)
"""
TREE = "build/linux-gcc-release"


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "cmake and ctest must be on PATH")
class DocumentedSelectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._temp = tempfile.TemporaryDirectory(prefix="documented-selectors-")
        root = Path(cls._temp.name)
        source = root / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(FIXTURE_CMAKE, encoding="utf-8")
        cls.build_dir = root / "linux-gcc-release"
        subprocess.run(["cmake", "-S", str(source), "-B", str(cls.build_dir), "-DFIXTURE_ENABLED=ON"], check=True,
                       capture_output=True, text=True)
        cls.root = root

    @classmethod
    def tearDownClass(cls) -> None:
        cls._temp.cleanup()

    def run_checker(self, commands: list[str], *, build_dir: Path | None = None,
                    planned: list[str] | None = None, execute: bool = False,
                    label_exclude: list[str] | None = None) -> tuple[int, str]:
        spec = self.root / "commands.json"
        spec.write_text(json.dumps([{"source": f"fixture[{index}]", "command": command, "planned": planned or []}
                                    for index, command in enumerate(commands)]), encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            # The default generator on a Windows host is multi-config Visual Studio, whose
            # tests are unavailable without ctest -C; a single-config tree ignores it.
            arguments = ["--build-dir", str(build_dir or self.build_dir), "--commands-json", str(spec),
                         "--config", "Debug"]
            if execute:
                arguments.append("--execute")
            for label in label_exclude or []:
                arguments += ["--label-exclude", label]
            code = checker.main(arguments)
        return code, out.getvalue() + err.getvalue()

    def test_enabled_built_selection_passes(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L alpha --output-on-failure --no-tests=error"])
        self.assertEqual(code, 0, output)
        self.assertIn("PASS: fixture[0]", output)
        self.assertIn("1 applicable command(s): 1 pass, 0 fail", output)

    def test_execute_runs_enabled_selection(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L alpha --no-tests=error"], execute=True)
        self.assertEqual(code, 0, output)
        self.assertIn("1 applicable command(s): 1 pass, 0 fail", output)

    def test_execute_propagates_test_failure(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L delta --no-tests=error"], execute=True)
        self.assertEqual(code, 1, output)
        self.assertIn("selection execution failed: ctest exited", output)

    def test_execute_runs_each_distinct_selection_once(self) -> None:
        command = f"ctest --test-dir {TREE} -L delta --no-tests=error"
        with patch.object(checker, "execute_selected", wraps=checker.execute_selected) as execute:
            code, output = self.run_checker([command, command], execute=True)
        self.assertEqual(code, 1, output)
        self.assertEqual(execute.call_count, 1)
        self.assertIn("2 applicable command(s): 0 pass, 2 fail", output)
        self.assertIn("same selection as fixture[0]", output)

    def test_lane_excluded_label_is_neither_required_nor_run(self) -> None:
        # Without the exclusion the advisory failure is executed and fails.
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L epsilon --no-tests=error"], execute=True)
        self.assertEqual(code, 1, output)
        # With it, the rest of the selection still runs and passes.
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L epsilon --no-tests=error"], execute=True,
                                        label_exclude=["advisory-only"])
        self.assertEqual(code, 0, output)
        self.assertIn("executed 1 enabled test(s)", output)
        # A selection made only of excluded tests is not applicable, never a pass.
        code, output = self.run_checker([f"ctest --test-dir {TREE} -R epsilon_advisory --no-tests=error",
                                         f"ctest --test-dir {TREE} -L alpha --no-tests=error"],
                                        execute=True, label_exclude=["advisory-only"])
        self.assertEqual(code, 0, output)
        self.assertIn("1 applicable command(s): 1 pass, 0 fail", output)
        self.assertIn("1 not applicable", output)

    def test_documented_tree_can_map_to_ci_build_directory(self) -> None:
        spec = checker.DocumentedCommand(
            "fixture[ci-map]", f"ctest --test-dir {TREE} -L alpha", ["--test-dir", TREE, "-L", "alpha"]
        )
        outcome = checker.evaluate(spec, "ctest", self.build_dir, "Debug", documented_tree=TREE)
        self.assertEqual(outcome.status, "pass", outcome.detail)
        mismatched = checker.evaluate(spec, "ctest", self.build_dir, None, documented_tree="build/other")
        self.assertEqual(mismatched.status, "not-applicable")

    def test_disabled_only_selection_fails(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L beta --no-tests=error"])
        self.assertEqual(code, 1, output)
        self.assertIn("selects 1 test(s), none enabled", output)

    def test_missing_executable_fails(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L gamma --no-tests=error"])
        self.assertEqual(code, 1, output)
        self.assertIn("no built executable: gamma_unbuilt", output)

    def test_empty_selection_fails(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L nothing --no-tests=error"])
        self.assertEqual(code, 1, output)
        self.assertIn("selects 0 test(s), none enabled", output)

    def test_one_bad_command_fails_the_run(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L alpha --no-tests=error",
                                         f"ctest --test-dir {TREE} -R '^gamma' --no-tests=error"])
        self.assertEqual(code, 1, output)
        self.assertIn("2 applicable command(s): 1 pass, 1 fail", output)

    def test_other_preset_tree_is_not_applicable_and_never_counts(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L alpha --no-tests=error",
                                         "ctest --test-dir build/windows-release -C Release -L gamma --no-tests=error",
                                         "cd build && ctest -L gamma --no-tests=error"])
        self.assertEqual(code, 0, output)
        self.assertIn("1 applicable command(s): 1 pass, 0 fail; 0 declared debt; 2 not applicable", output)

    def test_command_needing_an_unset_configure_option_is_not_applicable(self) -> None:
        configure = "cmake --preset linux-gcc-release"
        code, output = self.run_checker([
            f"{configure} -DFIXTURE_ENABLED=TRUE && ctest --test-dir {TREE} -L alpha --no-tests=error",
            f"{configure} -DFIXTURE_OPT_IN=ON && ctest --test-dir {TREE} -L nothing --no-tests=error",
        ])
        self.assertEqual(code, 0, output)
        self.assertIn("1 applicable command(s): 1 pass, 0 fail; 0 declared debt; 1 not applicable", output)
        outcome = checker.evaluate(checker.load_override(self.root / "commands.json")[1], "ctest", self.build_dir, None)
        self.assertEqual((outcome.status, outcome.detail),
                         ("not-applicable", "tree is not configured with -DFIXTURE_OPT_IN=ON"))

    def test_only_inapplicable_commands_exit_2(self) -> None:
        code, output = self.run_checker(["ctest --test-dir build/windows-release -C Release -L alpha --no-tests=error"])
        self.assertEqual(code, 2, output)
        self.assertIn("no documented command applies to linux-gcc-release", output)

    def test_declared_debt_is_listed_not_passed(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L future-family --no-tests=error"],
                                        planned=["future-family*"])
        self.assertEqual(code, 2, output)
        self.assertIn("DEBT: fixture[0]", output)
        self.assertIn("0 applicable command(s)", output)

    def test_unconfigured_tree_exits_2(self) -> None:
        empty = self.root / "linux-gcc-release-empty"
        empty.mkdir(exist_ok=True)
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L alpha --no-tests=error"], build_dir=empty)
        self.assertEqual(code, 2, output)
        self.assertIn("has no CTestTestfile.cmake", output)


class DocumentedCommandInventoryTests(unittest.TestCase):
    """The real ledger and testing page keep commands for the trees CTest runs this check in."""

    def test_real_documents_target_the_linux_and_windows_release_trees(self) -> None:
        commands = checker.work_item_commands() + checker.testing_page_commands(REPO_ROOT)
        trees = [checker.command_tree(command.arguments) for command in commands]
        self.assertGreater(trees.count("build/linux-gcc-release"), 0)
        self.assertGreater(trees.count("build/windows-release"), 0)

    def test_execute_cannot_pass_from_discovery_alone(self) -> None:
        command = checker.DocumentedCommand("fixture", "ctest -N", ["--test-dir", TREE, "-N"])
        selected = [{"name": "alpha", "command": [sys.executable]}]
        with patch.object(checker, "work_item_commands", return_value=[command]), \
                patch.object(checker, "testing_page_commands", return_value=[]), \
                patch.object(checker, "show_only", return_value=selected), \
                patch.object(Path, "is_file", lambda path: path.name == "CTestTestfile.cmake"), \
                patch.object(checker, "execute_selected") as execute, \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = checker.main(["--build-dir", TREE, "--ctest", "ctest", "--execute"])
        self.assertEqual(code, 2)
        execute.assert_not_called()

    def test_selection_arguments_keep_only_selection_flags(self) -> None:
        arguments = ["--test-dir", "build/x", "-C", "Release", "-L", "^unit$", "--tests-regex=Foo", "-E", "Soak",
                     "-U", "--output-on-failure", "--no-tests=error", "-j8"]
        self.assertEqual(checker.selection_arguments(arguments),
                         ["-L", "^unit$", "--tests-regex", "Foo", "-E", "Soak", "-U"])
        self.assertIsNone(checker.selection_arguments(["-R", "${NAME}"]))

    def test_execute_passes_bounded_limit_to_ctest_child(self) -> None:
        completed = type("Completed", (), {"returncode": 0, "stdout": "", "stderr": ""})()
        with patch.object(checker.subprocess, "run", return_value=completed) as run:
            with patch.dict(checker.os.environ, {"SPARK_TEST_NAME_PREFIX": "ambient", "SPARK_TEST_LIMIT": "999"}):
                checker.execute_selected("ctest", Path("build/linux-gcc-release"), None, ["-L", "unit"], 7)
        arguments, options = run.call_args
        self.assertEqual(
            arguments[0][-8:],
            ["-L", "unit", "--output-on-failure", "--no-tests=error", "--parallel", "2", "--timeout", "120"],
        )
        self.assertEqual(options["env"]["SPARK_TEST_LIMIT"], "7")
        self.assertNotIn("SPARK_TEST_NAME_PREFIX", options["env"])
        self.assertEqual(options["timeout"], checker.CTEST_EXECUTION_TIMEOUT_SECONDS)

    def test_execute_timeout_reports_the_tests_ctest_completed(self) -> None:
        progress = "1/3 Test #7: SlowOne ....   Passed  600.01 sec\n"
        timeout = subprocess.TimeoutExpired(["ctest"], checker.CTEST_EXECUTION_TIMEOUT_SECONDS, output=progress)
        with patch.object(checker.subprocess, "run", side_effect=timeout):
            with self.assertRaises(RuntimeError) as raised:
                checker.execute_selected("ctest", Path("build/linux-gcc-release"), None, ["-L", "unit"], 7)
        self.assertIn(f"timed out after {checker.CTEST_EXECUTION_TIMEOUT_SECONDS} seconds", str(raised.exception))
        self.assertIn("SlowOne ....   Passed  600.01 sec", str(raised.exception))

    def test_expected_count_pins_raise_limit_and_reject_malformed_values(self) -> None:
        tests = [{"properties": [{"name": "ENVIRONMENT", "value": "SPARK_TEST_EXPECT_COUNT=73"}]}]
        self.assertEqual(checker.effective_test_limit(tests, 50), 73)
        with self.assertRaisesRegex(ValueError, "malformed"):
            checker.effective_test_limit(
                [{"properties": [{"name": "ENVIRONMENT", "value": "SPARK_TEST_EXPECT_COUNT=oops"}]}], 50
            )

    def test_execute_planned_empty_selection_stays_declared_debt(self) -> None:
        # Declared debt is tracked by its work item; execution must neither run it
        # nor count it as a pass or a regression.
        command = checker.DocumentedCommand(
            "fixture[planned]", "ctest --test-dir build/linux-gcc-release -L future",
            ["--test-dir", "build/linux-gcc-release", "-L", "future"], ["future-family*"]
        )
        with patch.object(checker, "ctest_filter_errors", side_effect=[True, False]), \
                patch.object(checker, "command_tree", return_value="build/linux-gcc-release"), \
                patch.object(checker, "execute_selected") as execute:
            outcome = checker.evaluate(command, "ctest", Path("build/linux-gcc-release"), None, execute=True)
        self.assertEqual(outcome.status, "debt")
        execute.assert_not_called()

    def test_discovery_only_flag_is_detected(self) -> None:
        self.assertTrue(checker._discovery_only(["--show-only=json-v1"]))
        self.assertTrue(checker._discovery_only(["-N"]))
        self.assertFalse(checker._discovery_only(["-L", "unit"]))

    def test_documented_tree_maps_to_ci_build_directory(self) -> None:
        command = checker.DocumentedCommand(
            "fixture[ci-map]", "ctest --test-dir build/linux-gcc-release -L unit",
            ["--test-dir", "build/linux-gcc-release", "-L", "unit"]
        )
        selected = [{"name": "unit", "command": ["SparkTests"], "properties": []}]
        with patch.object(checker, "show_only", return_value=selected), \
                patch.object(checker, "_executable_exists", return_value=True):
            outcome = checker.evaluate(command, "ctest", Path("build"), None,
                                       documented_tree="build/linux-gcc-release")
        self.assertEqual(outcome.status, "pass", outcome.detail)


if __name__ == "__main__":
    unittest.main()
