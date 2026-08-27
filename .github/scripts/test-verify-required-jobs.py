#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("verify-required-jobs.py")
SPEC = importlib.util.spec_from_file_location("verify_required_jobs", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class VerifyRequiredJobsTests(unittest.TestCase):
    def test_accepts_only_successful_jobs(self) -> None:
        passed, failed = MODULE.verify(
            {"build": {"result": "success"}, "tests": {"result": "success"}}
        )
        self.assertEqual(passed, ["build", "tests"])
        self.assertEqual(failed, [])

    def test_reports_failure_cancelled_and_skipped(self) -> None:
        passed, failed = MODULE.verify(
            {
                "ok": {"result": "success"},
                "bad": {"result": "failure"},
                "cancelled": {"result": "cancelled"},
                "skipped": {"result": "skipped"},
            }
        )
        self.assertEqual(passed, ["ok"])
        self.assertEqual(
            failed,
            [("bad", "failure"), ("cancelled", "cancelled"), ("skipped", "skipped")],
        )

    def test_rejects_empty_needs(self) -> None:
        with self.assertRaisesRegex(ValueError, "non-empty"):
            MODULE.verify({})

    def test_markdown_distinguishes_failure(self) -> None:
        report = MODULE.markdown(["ok"], [("bad", "failure")])
        self.assertIn("did not succeed", report)
        self.assertIn("**failure**", report)


if __name__ == "__main__":
    unittest.main()
