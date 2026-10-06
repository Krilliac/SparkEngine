#!/usr/bin/env python3
"""Controlled tests for the SPARK_STRICT_DEPS dependency closure (CI-120).

The strict closure is ThirdParty/dependencies.lock itself: with
SPARK_STRICT_DEPS=ON, cmake/SparkThirdPartyAudit.cmake fails configure when any
locked dependency is missing, whatever its manifest severity. There is no second
hand-maintained list in CMakeLists.txt.

The behavioral cases configure a throwaway fixture project that includes the
real audit module and calls it exactly the way the root CMakeLists.txt does,
against a copy of the real manifest. The fixture tree holds every locked
dependency's required files (as placeholders) and the real license notices, so
the closure under test is the one the repository ships. Each case then removes
a controlled part of that tree and checks the configure outcome.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
AUDIT_MODULE = REPO_ROOT / "cmake" / "SparkThirdPartyAudit.cmake"
MANIFEST = REPO_ROOT / "ThirdParty" / "dependencies.lock"
ROOT_CMAKELISTS = REPO_ROOT / "CMakeLists.txt"
CMAKE = shutil.which("cmake")

FIXTURE_CMAKELISTS = """cmake_minimum_required(VERSION 3.25)
project(SparkStrictDependenciesFixture NONE)
option(SPARK_STRICT_DEPS "Fixture copy of the root option" OFF)
include("{module}")
spark_thirdparty_audit("${{CMAKE_SOURCE_DIR}}/ThirdParty/dependencies.lock")
"""


class LockEntry:
    """One pipe-delimited manifest record as CMake evaluates it."""

    def __init__(self, line):
        fields = line.split("|")
        if len(fields) != 10:
            raise ValueError(f"malformed manifest entry: {line}")
        self.name = fields[0]
        self.path = fields[4]
        self.required = [item for item in fields[5].split(",") if item]
        self.severity = fields[8]
        self.notices = [item for item in fields[9].split(",") if item]


def export_lock_entries():
    """Expand the real manifest through CMake so the tests see what configure sees."""
    with tempfile.TemporaryDirectory(prefix="spark-strict-deps-export-") as tmp:
        output = Path(tmp) / "entries.txt"
        result = subprocess.run(
            [CMAKE, "-DSPARK_THIRDPARTY_AUDIT_VALIDATE_ONLY=ON",
             f"-DSPARK_THIRDPARTY_MANIFEST={MANIFEST.as_posix()}",
             f"-DSPARK_THIRDPARTY_ENTRIES_OUTPUT={output.as_posix()}",
             "-P", str(AUDIT_MODULE)],
            capture_output=True, text=True, timeout=120, check=False, cwd=REPO_ROOT)
        if result.returncode != 0:
            raise RuntimeError(f"manifest export failed:\n{result.stdout}{result.stderr}")
        lines = output.read_text(encoding="utf-8").splitlines()
    return [LockEntry(line) for line in lines if line]


@unittest.skipUnless(CMAKE, "cmake executable required")
class StrictDependencyClosureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = export_lock_entries()

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="spark-strict-deps-"))
        self.source = self.tmp / "src"
        self.build = self.tmp / "build"
        (self.source / "ThirdParty").mkdir(parents=True)
        shutil.copyfile(MANIFEST, self.source / "ThirdParty" / "dependencies.lock")
        (self.source / "CMakeLists.txt").write_text(
            FIXTURE_CMAKELISTS.format(module=AUDIT_MODULE.as_posix()), encoding="utf-8")
        for entry in self.entries:
            dependency_root = self.source / entry.path
            dependency_root.mkdir(parents=True, exist_ok=True)
            for required in entry.required:
                # Mirror the shape the repository ships: a required directory
                # stays a directory, everything else is a placeholder file.
                target = dependency_root / required
                if (REPO_ROOT / entry.path / required).is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text("placeholder\n", encoding="utf-8")
            for notice in entry.notices:
                destination = self.source / notice
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO_ROOT / notice, destination)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def configure(self, strict):
        shutil.rmtree(self.build, ignore_errors=True)
        return subprocess.run(
            [CMAKE, "-S", str(self.source), "-B", str(self.build),
             f"-DSPARK_STRICT_DEPS={'ON' if strict else 'OFF'}"],
            capture_output=True, text=True, timeout=120, check=False)

    def remove_required_file(self, entry):
        required = entry.required[0]
        target = self.source / entry.path / required
        if target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink()
        return f"{entry.path}/{required}"

    def assert_strict_failure(self, result, *fragments):
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("SPARK_STRICT_DEPS:", result.stderr)
        for fragment in fragments:
            self.assertIn(fragment, result.stderr)

    def test_manifest_declares_a_nonempty_closure(self):
        self.assertGreater(len(self.entries), 0)
        names = [entry.name for entry in self.entries]
        self.assertEqual(len(names), len(set(names)), "duplicate dependency names in the lock")
        for entry in self.entries:
            self.assertTrue(entry.required, f"{entry.name} declares no required files")

    def test_complete_closure_configures_under_strict(self):
        result = self.configure(strict=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"all {len(self.entries)} locked dependencies present", result.stdout)
        self.assertNotIn("[ThirdParty Audit]", result.stderr)

    def test_every_locked_dependency_is_fatal_when_missing(self):
        # Every manifest entry, ERROR and WARN severity alike, belongs to the
        # strict closure. Restore each removal so each subtest isolates one entry.
        for entry in self.entries:
            with self.subTest(dependency=entry.name, severity=entry.severity):
                target = self.source / entry.path / entry.required[0]
                backup = self.tmp / "backup"
                if target.is_dir():
                    shutil.copytree(target, backup)
                else:
                    shutil.copyfile(target, backup)
                removed = self.remove_required_file(entry)
                try:
                    result = self.configure(strict=True)
                    self.assert_strict_failure(
                        result, "1 issue(s)", f"{entry.name}: missing required file '{removed}'")
                finally:
                    if backup.is_dir():
                        shutil.copytree(backup, target)
                        shutil.rmtree(backup)
                    else:
                        shutil.move(backup, target)

    def test_missing_dependency_root_is_fatal_under_strict(self):
        # An uninitialized submodule leaves no directory at all. Pick an entry
        # whose license notice lives outside its own tree so the manifest schema
        # check still passes and the audit, not the schema, rejects it.
        entry = next(
            candidate for candidate in self.entries
            if not any(notice.startswith(candidate.path + "/") for notice in candidate.notices))
        shutil.rmtree(self.source / entry.path)
        result = self.configure(strict=True)
        self.assert_strict_failure(
            result, f"{entry.name}: declared path '{entry.path}' does not exist")

    def test_all_missing_dependencies_are_reported_together(self):
        first, second = self.entries[0], self.entries[-1]
        removed_first = self.remove_required_file(first)
        removed_second = self.remove_required_file(second)
        result = self.configure(strict=True)
        self.assert_strict_failure(
            result, "2 issue(s)",
            f"{first.name}: missing required file '{removed_first}'",
            f"{second.name}: missing required file '{removed_second}'")

    def test_missing_dependencies_only_warn_without_strict(self):
        for severity in ("ERROR", "WARN"):
            entry = next(candidate for candidate in self.entries if candidate.severity == severity)
            with self.subTest(dependency=entry.name, severity=severity):
                self.remove_required_file(entry)
                result = self.configure(strict=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"{entry.name}: missing required file", result.stderr)
                self.assertNotIn("SPARK_STRICT_DEPS:", result.stderr)


class StrictDependencyWiringTests(unittest.TestCase):
    def setUp(self):
        self.root = ROOT_CMAKELISTS.read_text(encoding="utf-8")

    def test_root_runs_the_audit_on_the_lock_after_declaring_strict(self):
        option_at = self.root.find("option(SPARK_STRICT_DEPS ")
        include_at = self.root.find('include("${CMAKE_SOURCE_DIR}/cmake/SparkThirdPartyAudit.cmake")')
        audit_at = self.root.find('spark_thirdparty_audit("${CMAKE_SOURCE_DIR}/ThirdParty/dependencies.lock")')
        self.assertGreaterEqual(option_at, 0, "SPARK_STRICT_DEPS option is not declared")
        self.assertGreaterEqual(include_at, 0, "root CMakeLists.txt does not include the audit module")
        self.assertGreaterEqual(audit_at, 0, "root CMakeLists.txt does not audit ThirdParty/dependencies.lock")
        self.assertLess(option_at, audit_at, "the audit runs before SPARK_STRICT_DEPS is declared")
        self.assertLess(include_at, audit_at)

    def test_root_keeps_no_second_strict_dependency_list(self):
        # A hard-coded strict list would drift from the lock. The only strict
        # decision belongs to the audit module.
        strict_blocks = re.findall(r"if\s*\(\s*SPARK_STRICT_DEPS\s*\)", self.root)
        self.assertEqual(strict_blocks, [], "root CMakeLists.txt branches on SPARK_STRICT_DEPS itself")
        self.assertNotRegex(self.root, r"FATAL_ERROR\s+\"SPARK_STRICT_DEPS")


if __name__ == "__main__":
    unittest.main(verbosity=2)
