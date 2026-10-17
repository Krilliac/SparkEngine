#!/usr/bin/env python3
"""RDY-010: the mirror-file ratchet of ``Tools/test_source_census.py --check``.

``--check`` fails on a mirror test file (one that includes no production
header) that MIRROR_BASELINE does not list, on a baseline entry whose file no
longer exists, and on a ``*_Skipped`` TEST that reports a pass instead of
calling SKIP_TEST. Before this test the ratchet only ran in the hosted Windows
census step, so a slice that retired a mirror file without pruning the baseline
failed only in hosted CI. These cases pin it on the real tree and prove each
regression is reported by name.
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
CENSUS_SCRIPT = REPO_ROOT / "Tools" / "test_source_census.py"


def _load_census():
    spec = importlib.util.spec_from_file_location("spark_test_source_census_baseline", CENSUS_SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot import {CENSUS_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _row(path: str, kind: str, fabricated_skips: list[str] | None = None) -> dict[str, object]:
    return {"path": path, "tests": 1, "kind": kind, "tautologies": 0, "fabricatedSkips": fabricated_skips or []}


class MirrorBaselineRatchet(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.census = _load_census()

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "Tests").mkdir()
        (self.root / "GameModules").mkdir()

    def _write_test(self, relative_path: str, text: str) -> None:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    # -- the real tree -------------------------------------------------------

    def test_real_tree_check_passes(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(CENSUS_SCRIPT), "--check"],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
            timeout=100,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Census check passed", result.stdout)

    def test_real_baseline_lists_only_mirror_files(self) -> None:
        mirrors = {str(row["path"]) for row in self.census.scan(REPO_ROOT) if row["kind"] == "mirror"}
        stale = sorted(set(self.census.MIRROR_BASELINE) - mirrors)
        self.assertEqual(stale, [], "MIRROR_BASELINE lists files that are no longer mirrors; prune them")

    # -- regressions that must be reported -----------------------------------

    def test_new_mirror_file_fails(self) -> None:
        failures, advisories = self.census.check([_row("Tests/TestNewCopy.cpp", "mirror")], set())
        self.assertEqual(advisories, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("Tests/TestNewCopy.cpp: new mirror test file", failures[0])

    def test_listed_mirror_file_passes(self) -> None:
        failures, advisories = self.census.check([_row("Tests/TestOld.cpp", "mirror")], {"Tests/TestOld.cpp"})
        self.assertEqual((failures, advisories), ([], []))

    def test_baseline_entry_that_became_production_source_is_reported(self) -> None:
        failures, advisories = self.census.check(
            [_row("Tests/TestFixed.cpp", "production-source")], {"Tests/TestFixed.cpp"}
        )
        self.assertEqual(failures, [])
        self.assertEqual(len(advisories), 1)
        self.assertIn("Tests/TestFixed.cpp: no longer a mirror file", advisories[0])

    def test_baseline_entry_for_missing_file_is_fatal(self) -> None:
        self._write_test("Tests/TestPresent.cpp", "TEST(A) {}\n")
        present_only = frozenset({"Tests/TestPresent.cpp"})
        with mock.patch.object(self.census, "MIRROR_BASELINE", present_only):
            self.assertEqual(self.census.load_baseline(self.root), set(present_only))
        with mock.patch.object(self.census, "MIRROR_BASELINE", present_only | {"Tests/TestGone.cpp"}):
            with self.assertRaises(SystemExit) as raised:
                self.census.load_baseline(self.root)
        self.assertIn("no longer exist: Tests/TestGone.cpp", str(raised.exception))

    def test_empty_baseline_is_fatal(self) -> None:
        with mock.patch.object(self.census, "MIRROR_BASELINE", frozenset()):
            with self.assertRaises(SystemExit) as raised:
                self.census.load_baseline(self.root)
        self.assertIn("MIRROR_BASELINE is empty", str(raised.exception))

    def test_fabricated_skip_is_reported(self) -> None:
        self._write_test(
            "Tests/TestSkips.cpp",
            '#include "TestFramework.h"\n'
            "TEST(Feature_Skipped)\n{\n    EXPECT_TRUE(true);\n}\n"
            'TEST(Honest_Skipped)\n{\n    SKIP_TEST("compiled out");\n}\n',
        )
        rows = self.census.scan(self.root)
        self.assertEqual([row["fabricatedSkips"] for row in rows], [["Feature_Skipped"]])
        failures, _ = self.census.check(rows, {"Tests/TestSkips.cpp"})
        self.assertEqual(len(failures), 1)
        self.assertIn("TEST(Feature_Skipped) reports a pass for a compiled-out feature", failures[0])

    def test_module_header_include_is_production_source(self) -> None:
        self._write_test("GameModules/SparkGameDemo/Source/Game/Rules.h", "#pragma once\n")
        self._write_test("Tests/TestRules.cpp", '#include "Game/Rules.h"\nTEST(Rules_A) {}\n')
        self._write_test("Tests/TestCopy.cpp", '#include "TestFramework.h"\nTEST(Copy_A) {}\n')
        kinds = {row["path"]: row["kind"] for row in self.census.scan(self.root)}
        self.assertEqual(kinds, {"Tests/TestCopy.cpp": "mirror", "Tests/TestRules.cpp": "production-source"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
