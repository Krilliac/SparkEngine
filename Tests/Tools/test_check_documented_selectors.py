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
                    planned: list[str] | None = None) -> tuple[int, str]:
        spec = self.root / "commands.json"
        spec.write_text(json.dumps([{"source": f"fixture[{index}]", "command": command, "planned": planned or []}
                                    for index, command in enumerate(commands)]), encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = checker.main(["--build-dir", str(build_dir or self.build_dir), "--commands-json", str(spec)])
        return code, out.getvalue() + err.getvalue()

    def test_enabled_built_selection_passes(self) -> None:
        code, output = self.run_checker([f"ctest --test-dir {TREE} -L alpha --output-on-failure --no-tests=error"])
        self.assertEqual(code, 0, output)
        self.assertIn("PASS: fixture[0]", output)
        self.assertIn("1 applicable command(s): 1 pass, 0 fail", output)

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

    def test_selection_arguments_keep_only_selection_flags(self) -> None:
        arguments = ["--test-dir", "build/x", "-C", "Release", "-L", "^unit$", "--tests-regex=Foo", "-E", "Soak",
                     "-U", "--output-on-failure", "--no-tests=error", "-j8"]
        self.assertEqual(checker.selection_arguments(arguments),
                         ["-L", "^unit$", "--tests-regex", "Foo", "-E", "Soak", "-U"])
        self.assertIsNone(checker.selection_arguments(["-R", "${NAME}"]))


if __name__ == "__main__":
    unittest.main()
