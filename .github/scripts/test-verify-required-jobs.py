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

    def test_accepts_only_the_exact_deferred_failure(self) -> None:
        passed, deferred, failed = MODULE.verify_with_policy(
            {
                "ordinary": {"result": "success"},
                "build-windows-shipping": {"result": "failure"},
            },
            deferred_failures={"build-windows-shipping": "failure"},
            expected_jobs=["ordinary", "build-windows-shipping"],
        )
        self.assertEqual(passed, ["ordinary"])
        self.assertEqual(deferred, [("build-windows-shipping", "failure")])
        self.assertEqual(failed, [])

    def test_deferred_job_must_not_turn_green_skip_or_cancel(self) -> None:
        for result in ("success", "skipped", "cancelled", None):
            with self.subTest(result=result):
                passed, deferred, failed = MODULE.verify_with_policy(
                    {"build-windows-shipping": {"result": result}},
                    deferred_failures={"build-windows-shipping": "failure"},
                    expected_jobs=["build-windows-shipping"],
                )
                self.assertEqual(passed, [])
                self.assertEqual(deferred, [])
                self.assertEqual(len(failed), 1)
                self.assertIn("expected failure", failed[0][1])

    def test_required_job_inventory_is_exact(self) -> None:
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            MODULE.verify_with_policy(
                {"one": {"result": "success"}, "extra": {"result": "success"}},
                expected_jobs=["one", "missing"],
            )

    def test_deferred_policy_is_narrow_and_present(self) -> None:
        with self.assertRaisesRegex(ValueError, "map exact job names"):
            MODULE.verify_with_policy(
                {"one": {"result": "failure"}},
                deferred_failures={"one": "success"},
            )
        with self.assertRaisesRegex(ValueError, "absent from needs"):
            MODULE.verify_with_policy(
                {"one": {"result": "success"}},
                deferred_failures={"missing": "failure"},
            )

    def test_rejects_empty_needs(self) -> None:
        with self.assertRaisesRegex(ValueError, "non-empty"):
            MODULE.verify({})

    def test_markdown_distinguishes_failure(self) -> None:
        report = MODULE.markdown(["ok"], [("bad", "failure")])
        self.assertIn("did not succeed", report)
        self.assertIn("**failure**", report)

    def test_markdown_names_the_external_deferred_gate(self) -> None:
        report = MODULE.markdown(
            ["ordinary"], [], [("build-windows-shipping", "failure")]
        )
        self.assertIn("protected external status", report)
        self.assertIn("deferred to exact external gate", report)


if __name__ == "__main__":
    unittest.main()
