#!/usr/bin/env python3
"""Regression tests for the screenshot evidence capture script."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "capture-screenshots.sh"


class CaptureScreenshotsTests(unittest.TestCase):
    @staticmethod
    def _bash_path(bash: str, path: Path) -> str:
        if os.name != "nt":
            return str(path)
        try:
            converted = subprocess.run(
                [bash, "-lc", f"wslpath -u -- {shlex.quote(str(path))}"],
                capture_output=True,
                text=True,
                check=True,
            )
        except subprocess.CalledProcessError:
            windows_path = path.as_posix()
            drive, tail = windows_path[0], windows_path[2:]
            return f"/{drive.lower()}{tail}"
        return converted.stdout.strip()

    def _run_script(self, *, produce_capture: bool) -> subprocess.CompletedProcess[str]:
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash is required to exercise the capture script")

        with tempfile.TemporaryDirectory(prefix="spark-capture-test-") as raw:
            root = Path(raw)
            tools = root / "Tools"
            tools.mkdir()
            script = tools / "capture-screenshots.sh"
            shutil.copyfile(SCRIPT, script)
            script.chmod(script.stat().st_mode | stat.S_IXUSR)

            fake_bin = root / "fake-bin"
            fake_bin.mkdir()
            for name, body in {
                "Xvfb": "#!/bin/sh\nexit 0\n",
                "sleep": "#!/bin/sh\nexit 0\n",
                "xterm": "#!/bin/sh\nexit 0\n",
                "import": (
                    "#!/bin/sh\n"
                    + (
                        "out=\"\"\n"
                        "for argument in \"$@\"; do out=\"$argument\"; done\n"
                        "printf '%2000s' x > \"$out\"\n"
                        if produce_capture else ""
                    )
                    + "exit 0\n"
                ),
            }.items():
                command = fake_bin / name
                command.write_bytes(body.encode("utf-8"))
                command.chmod(command.stat().st_mode | stat.S_IXUSR)

            environment = os.environ.copy()
            fake_bin_bash = self._bash_path(bash, fake_bin)
            script_bash = self._bash_path(bash, script)
            runtime_bash = self._bash_path(bash, root / "runtime")
            command = (
                f"export PATH={shlex.quote(fake_bin_bash + ':/usr/bin:/bin')}; "
                f"export XDG_RUNTIME_DIR={shlex.quote(runtime_bash)}; "
                f"exec {shlex.quote(script_bash)} console"
            )
            return subprocess.run(
                [bash, "-c", command],
                # WSL's interop launcher can retain its inherited Windows
                # working-directory handle briefly after the child exits.  A
                # disposable cwd therefore makes cleanup flaky on Windows;
                # the script resolves all paths from its own location, so use
                # the stable checkout as the process cwd instead.
                cwd=REPO_ROOT,
                env=environment,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )

    def test_missing_capture_fails_the_evidence_run(self) -> None:
        result = self._run_script(produce_capture=False)

        self.assertNotEqual(
            result.returncode,
            0,
            "a missing screenshot must not produce a green evidence run",
        )
        self.assertIn("tiny or missing", result.stdout)

    def test_nonempty_capture_keeps_the_evidence_run_green(self) -> None:
        result = self._run_script(produce_capture=True)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_repeated_runs_clean_up_on_both_evidence_paths(self) -> None:
        for _ in range(5):
            missing = self._run_script(produce_capture=False)
            self.assertNotEqual(missing.returncode, 0, missing.stdout + missing.stderr)
            success = self._run_script(produce_capture=True)
            self.assertEqual(success.returncode, 0, success.stdout + success.stderr)

    def test_unrelated_spark_process_survives_capture_cleanup(self) -> None:
        sentinel = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import time; time.sleep(10)  # SparkConsole unrelated sentinel",
            ],
            cwd=REPO_ROOT,
        )
        try:
            result = self._run_script(produce_capture=False)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIsNone(
                sentinel.poll(),
                "capture cleanup must not terminate unrelated Spark-named processes",
            )
        finally:
            sentinel.terminate()
            sentinel.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
