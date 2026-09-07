#!/usr/bin/env python3
"""Regression coverage for generated runtime artifact hygiene."""

from __future__ import annotations

from pathlib import Path
import subprocess
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]


class GeneratedRuntimeArtifactHygieneTests(unittest.TestCase):
    def test_runtime_database_lock_output_is_ignored_by_git(self) -> None:
        completed = subprocess.run(
            ["git", "check-ignore", "--quiet", "--", "Saves/test_tfdb_roundtrip.db.lock"],
            cwd=REPO_ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        self.assertEqual(
            completed.returncode,
            0,
            "runtime database lock output must be ignored so it cannot enter a release change",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
