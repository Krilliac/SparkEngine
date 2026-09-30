#!/usr/bin/env python3
"""Tests for the sequential Node runtime-test wrapper."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / "Tests" / "Tools" / "run_site_runtime_tests.py"


class SiteRuntimeRunnerTests(unittest.TestCase):
    def test_runs_every_file_and_preserves_first_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="site-runtime-runner-") as directory:
            root = Path(directory)
            log = root / "files.log"
            fake_node = root / "fake-node.py"
            fake_node.write_text(
                "#!/usr/bin/env python3\n"
                "import os, pathlib, sys\n"
                "pathlib.Path(os.environ['SITE_RUNTIME_TEST_LOG']).open('a', encoding='utf-8').write(sys.argv[2] + '\\n')\n"
                "raise SystemExit(7 if sys.argv[2] == 'second.mjs' else 0)\n",
                encoding="utf-8",
            )
            if os.name == "nt":
                fake_command = root / "fake-node.cmd"
                fake_command.write_text(f'@echo off\n"{sys.executable}" "{fake_node}" %*\n', encoding="utf-8")
            else:
                fake_command = fake_node
                fake_node.chmod(0o755)
            files = ["first.mjs", "second.mjs", "third.mjs"]
            result = subprocess.run(
                [sys.executable, str(WRAPPER), str(fake_command), *files],
                cwd=ROOT,
                env={**os.environ, "SITE_RUNTIME_TEST_LOG": str(log)},
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(7, result.returncode)
            self.assertEqual(files, log.read_text(encoding="utf-8").splitlines())


if __name__ == "__main__":
    unittest.main()
