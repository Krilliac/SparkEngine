#!/usr/bin/env python3
"""Behavioral tests for cmake/SparkOptionGuard.cmake (CI-120).

Each case configures a throwaway fixture project that includes the real guard
module exactly the way the root CMakeLists.txt does: options are declared in the
top-level file and in a subdirectory, and the guard runs last. The static cases
pin the root wiring and audit CMakePresets.json so a preset can never pass a
string-valued ENABLE_*/SPARK_*/BUILD_* name the tree does not declare.
"""
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GUARD_MODULE = REPO_ROOT / "cmake" / "SparkOptionGuard.cmake"
ROOT_CMAKELISTS = REPO_ROOT / "CMakeLists.txt"
PRESETS = REPO_ROOT / "CMakePresets.json"
GUARDED_PREFIX = re.compile(r"^(ENABLE|SPARK|BUILD)_")
CMAKE = shutil.which("cmake")

FIXTURE_ROOT = """cmake_minimum_required(VERSION 3.25)
project(SparkOptionGuardFixture NONE)
option(ENABLE_DECLARED_FEATURE "Declared at the top level" OFF)
set(SPARK_DECLARED_PATH "" CACHE PATH "Declared cache path")
add_subdirectory(sub)
include("{module}")
spark_reject_undeclared_options()
"""

FIXTURE_SUB = """option(BUILD_SUBDIR_FEATURE "Declared only by a subdirectory" OFF)
"""


@unittest.skipUnless(CMAKE, "cmake executable required")
class OptionGuardBehaviorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="spark-option-guard-"))
        self.source = self.tmp / "src"
        self.build = self.tmp / "build"
        (self.source / "sub").mkdir(parents=True)
        (self.source / "CMakeLists.txt").write_text(
            FIXTURE_ROOT.format(module=GUARD_MODULE.as_posix()), encoding="utf-8")
        (self.source / "sub" / "CMakeLists.txt").write_text(FIXTURE_SUB, encoding="utf-8")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def configure(self, *args):
        return subprocess.run(
            [CMAKE, "-S", str(self.source), "-B", str(self.build), *args],
            capture_output=True, text=True, timeout=120, check=False)

    def assert_passes(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("SparkOptionGuard", result.stderr)

    def assert_rejects(self, result, *names):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("SparkOptionGuard: undeclared build option(s)", result.stderr)
        for name in names:
            self.assertIn(name, result.stderr)

    def test_no_options_configures(self):
        self.assert_passes(self.configure())

    def test_declared_options_configure(self):
        self.assert_passes(self.configure(
            "-DENABLE_DECLARED_FEATURE=ON",
            "-DSPARK_DECLARED_PATH=/opt/spark",
            "-DBUILD_SUBDIR_FEATURE=ON"))

    def test_typo_option_is_rejected(self):
        result = self.configure("-DENABLE_TYPO_OPTION=ON")
        self.assert_rejects(result, "ENABLE_TYPO_OPTION=ON", "-U ENABLE_TYPO_OPTION")

    def test_every_guarded_prefix_is_checked_and_reported_sorted(self):
        result = self.configure(
            "-DSPARK_TYPO_B=1", "-DBUILD_TYPO_C=ON", "-DENABLE_TYPO_A=OFF",
            "-DENABLE_DECLARED_FEATURE=ON")
        self.assert_rejects(result, "BUILD_TYPO_C", "ENABLE_TYPO_A", "SPARK_TYPO_B")
        report = result.stderr
        self.assertLess(report.index("BUILD_TYPO_C"), report.index("ENABLE_TYPO_A"))
        self.assertLess(report.index("ENABLE_TYPO_A"), report.index("SPARK_TYPO_B"))
        self.assertNotIn("ENABLE_DECLARED_FEATURE=", report)

    def test_names_outside_the_guarded_namespaces_are_ignored(self):
        self.assert_passes(self.configure("-DMY_UNRELATED_SETTING=1", "-DXENABLE_THING=ON"))

    def test_rejected_entry_persists_until_removed(self):
        self.assert_rejects(self.configure("-DSPARK_TYPO_OPTION=ON"), "SPARK_TYPO_OPTION")
        # A plain reconfigure must not quietly accept the stale cache entry.
        self.assert_rejects(self.configure(), "SPARK_TYPO_OPTION")
        self.assert_passes(self.configure("-U", "SPARK_TYPO_OPTION"))

    def test_explicitly_typed_entries_are_outside_the_guard(self):
        # Documented limit: a typed -D entry looks exactly like a declared one.
        self.assert_passes(self.configure("-DENABLE_TYPED_TYPO:BOOL=ON"))


class OptionGuardWiringTests(unittest.TestCase):
    def test_root_calls_guard_as_its_last_command(self):
        lines = [line.strip() for line in ROOT_CMAKELISTS.read_text(encoding="utf-8").splitlines()]
        commands = [line for line in lines if line and not line.startswith("#")]
        self.assertEqual(commands[-2], 'include("${CMAKE_SOURCE_DIR}/cmake/SparkOptionGuard.cmake")')
        self.assertEqual(commands[-1], "spark_reject_undeclared_options()")

    def test_presets_only_pass_declared_guarded_names(self):
        declared = set()
        declaration = re.compile(
            r"(?:\boption|\bcmake_dependent_option)\(\s*([A-Za-z0-9_]+)"
            r"|\bset\(\s*([A-Za-z0-9_]+)\b[^)]*\bCACHE\b", re.S)
        sources = [ROOT_CMAKELISTS, *sorted((REPO_ROOT / "cmake").glob("*.cmake"))]
        for tracked in subprocess.run(
                ["git", "-C", str(REPO_ROOT), "ls-files", "*CMakeLists.txt"],
                capture_output=True, text=True, check=True).stdout.splitlines():
            if not tracked.startswith("ThirdParty/"):
                sources.append(REPO_ROOT / tracked)
        for source in sources:
            for match in declaration.finditer(source.read_text(encoding="utf-8", errors="replace")):
                declared.add(match.group(1) or match.group(2))

        presets = json.loads(PRESETS.read_text(encoding="utf-8"))
        undeclared = []
        for preset in presets.get("configurePresets", []):
            for name, value in preset.get("cacheVariables", {}).items():
                # Only untyped string values reach the cache as UNINITIALIZED.
                if isinstance(value, str) and GUARDED_PREFIX.match(name) and name not in declared:
                    undeclared.append(f"{preset['name']}: {name}")
        self.assertEqual(undeclared, [])


if __name__ == "__main__":
    unittest.main()
