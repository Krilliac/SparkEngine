from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "check-test-registration.sh"


def bash_executable() -> str | None:
    found = shutil.which("bash")
    if found:
        return found
    windows_git_bash = Path("C:/Program Files/Git/bin/bash.exe")
    return str(windows_git_bash) if windows_git_bash.is_file() else None


class CheckTestRegistrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.tests = self.root / "Tests"
        self.tests.mkdir()
        self.write("TestTop.cpp", "TEST(Top)\n")
        self.write("harden/TestNested.cpp", "TEST(Nested)\n")
        self.write("apple/TestMetal.mm", "TEST(Metal)\n")
        self.write("A/TestSame.cpp", "TEST(SameA)\n")
        self.write("ignored/TestIgnored.cpp", "// test-registration: ignore\nTEST(Ignored)\n")
        self.cmake = self.tests / "CMakeLists.txt"
        self.cmake.write_text(
            "add_executable(SparkTests\n"
            "  TestTop.cpp\n"
            "  harden/TestNested.cpp\n"
            "  apple/TestMetal.mm\n"
            "  A/TestSame.cpp\n"
            ")\n",
            encoding="utf-8",
            newline="\n",
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write(self, relative: str, content: str) -> None:
        path = self.tests / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")

    def run_guard(self) -> subprocess.CompletedProcess[str]:
        bash = bash_executable()
        if not bash:
            self.skipTest("bash is required to exercise the POSIX registration guard")
        env = os.environ.copy()
        env["SPARK_TEST_REGISTRATION_ROOT"] = self.root.as_posix()
        return subprocess.run(
            [bash, str(SCRIPT)],
            cwd=SCRIPT.parents[1],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_recurses_matches_relative_paths_and_honors_explicit_ignore(self) -> None:
        passing = self.run_guard()
        self.assertEqual(passing.returncode, 0, passing.stdout + passing.stderr)

        self.write("B/TestSame.cpp", "TEST(SameB)\n")
        self.cmake.write_text(
            self.cmake.read_text(encoding="utf-8") + "# B/TestSame.cpp\n",
            encoding="utf-8",
            newline="\n",
        )
        missing = self.run_guard()
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("MISSING REGISTRATION: Tests/B/TestSame.cpp", missing.stderr)

        content = self.cmake.read_text(encoding="utf-8")
        self.cmake.write_text(
            content + "target_sources(SparkTests PRIVATE B/TestSame.cpp)\n",
            encoding="utf-8",
            newline="\n",
        )
        repaired = self.run_guard()
        self.assertEqual(repaired.returncode, 0, repaired.stdout + repaired.stderr)


if __name__ == "__main__":
    unittest.main()
