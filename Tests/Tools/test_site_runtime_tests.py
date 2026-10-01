#!/usr/bin/env python3
"""Tests for the Node runtime-test wrapper (Tests/Tools/run_site_runtime_tests.py)."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / "Tests" / "Tools" / "run_site_runtime_tests.py"

# Fake node: records "<file> <SPARK_SITE_DATA_DIR>", then waits (bounded) until
# every file of the run has started, so a runner that runs the files one at a
# time fails with status 9 instead of passing.
FAKE_NODE = """\
import os, pathlib, sys, time
test_file = sys.argv[2]
log = pathlib.Path(os.environ['SITE_RUNTIME_TEST_LOG'])
with log.open('a', encoding='utf-8') as handle:
    handle.write(test_file + ' ' + os.environ.get('SPARK_SITE_DATA_DIR', '<unset>') + '\\n')
expected = int(os.environ['SITE_RUNTIME_TEST_COUNT'])
deadline = time.monotonic() + 30
while len(log.read_text(encoding='utf-8').splitlines()) < expected:
    if time.monotonic() > deadline:
        raise SystemExit(9)
    time.sleep(0.05)
print('ran', test_file)
raise SystemExit(7 if test_file == 'second.mjs' else (5 if test_file == 'third.mjs' else 0))
"""

# Fake generator: records its arguments and creates the --output directory.
FAKE_PYTHON = """\
import os, pathlib, sys
with pathlib.Path(os.environ['SITE_RUNTIME_GENERATE_LOG']).open('a', encoding='utf-8') as handle:
    handle.write(' '.join(sys.argv[1:]) + '\\n')
if os.environ.get('SITE_RUNTIME_GENERATE_FAIL'):
    raise SystemExit(4)
pathlib.Path(sys.argv[sys.argv.index('--output') + 1]).mkdir(parents=True)
"""


class SiteRuntimeRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory(prefix="site-runtime-runner-")
        self.root = Path(self._directory.name)
        self.log = self.root / "files.log"
        self.generate_log = self.root / "generate.log"
        self.node = self._executable("fake-node", FAKE_NODE)
        self.python = self._executable("fake-python", FAKE_PYTHON)
        self.files = ["first.mjs", "second.mjs", "third.mjs"]

    def tearDown(self) -> None:
        self._directory.cleanup()

    def _executable(self, name: str, script: str) -> Path:
        source = self.root / f"{name}.py"
        if os.name == "nt":
            source.write_text(script, encoding="utf-8")
            command = self.root / f"{name}.cmd"
            command.write_text(f'@echo off\n"{sys.executable}" "{source}" %*\n', encoding="utf-8")
            return command
        source.write_text(f"#!{sys.executable}\n{script}", encoding="utf-8")
        source.chmod(0o755)
        return source

    def _run(self, **extra: str) -> subprocess.CompletedProcess[str]:
        environment = {key: value for key, value in os.environ.items() if key != "SPARK_SITE_DATA_DIR"}
        environment.update(
            SITE_RUNTIME_TEST_LOG=str(self.log),
            SITE_RUNTIME_TEST_COUNT=str(len(self.files)),
            SITE_RUNTIME_GENERATE_LOG=str(self.generate_log),
            PYTHON=str(self.python),
            **extra,
        )
        return subprocess.run([sys.executable, str(WRAPPER), str(self.node), *self.files], cwd=ROOT,
                              env=environment, capture_output=True, text=True, check=False)

    def _logged(self) -> dict[str, str]:
        return dict(line.split(" ", 1) for line in self.log.read_text(encoding="utf-8").splitlines())

    def test_given_publication_runs_every_file_concurrently_and_keeps_first_failure(self) -> None:
        given = str(self.root / "given-bundle")
        result = self._run(SPARK_SITE_DATA_DIR=given)
        self.assertEqual(7, result.returncode, result.stdout + result.stderr)
        self.assertEqual({name: given for name in self.files}, self._logged())
        self.assertFalse(self.generate_log.exists(), "a given publication must not be regenerated")
        # Output is replayed per file, in argument order.
        positions = [result.stdout.index(f"ran {name}") for name in self.files]
        self.assertEqual(sorted(positions), positions)

    def test_generates_one_publication_shared_by_every_file(self) -> None:
        result = self._run()
        self.assertEqual(7, result.returncode, result.stdout + result.stderr)
        generations = self.generate_log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(1, len(generations))
        self.assertIn("tools/site-data/generate.py --allow-dirty --skip-doc-health --output ", generations[0])
        output = generations[0].split("--output ", 1)[1]
        self.assertEqual({name: output for name in self.files}, self._logged())
        self.assertFalse(Path(output).exists(), "the generated publication outlives the run")

    def test_generation_failure_is_returned_and_runs_no_file(self) -> None:
        result = self._run(SITE_RUNTIME_GENERATE_FAIL="1")
        self.assertEqual(4, result.returncode, result.stdout + result.stderr)
        self.assertIn("generate.py failed", result.stderr)
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()
