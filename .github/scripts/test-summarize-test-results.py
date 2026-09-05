#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("summarize-test-results.py")
SPEC = importlib.util.spec_from_file_location("summarize_test_results", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class SummarizeTestResultsTests(unittest.TestCase):
    def test_counts_cases_and_slowest(self) -> None:
        report = Path("report.xml")
        cases = MODULE.parse_cases(
            """<?xml version="1.0"?>
<testsuites><testsuite name="SparkEngine">
  <testcase name="fast" time="0.1"/>
  <testcase name="slow" time="1.5"><skipped message="flaky"/></testcase>
  <testcase name="broken" time="0.2"><failure message="failed"/></testcase>
</testsuite></testsuites>
""",
            source=report,
        )
        stats = MODULE.summarize_cases(cases, [report], 2)

        self.assertEqual(stats["tests"], 3)
        self.assertEqual(stats["executed"], 2)
        self.assertEqual(stats["passed"], 1)
        self.assertEqual(stats["failures"], 1)
        self.assertEqual(stats["errors"], 0)
        self.assertEqual(stats["skipped"], 1)
        self.assertAlmostEqual(stats["durationSeconds"], 1.8)
        self.assertEqual(stats["slowest"][0]["name"], "slow")

    def test_counts_a_waived_flaky_case_as_executed_but_not_passed(self) -> None:
        report = Path("flaky.xml")
        cases = MODULE.parse_cases(
            """<testsuites><testsuite name="SparkEngine">
  <testcase name="clean" time="0.1"/>
  <testcase name="waived" time="0.2">
    <properties>
      <property name="flaky" value="true"/>
      <property name="flaky-reason" value="host scheduling"/>
    </properties>
    <flakyFailure message="Known flaky: host scheduling">assert</flakyFailure>
  </testcase>
  <testcase name="gone" time="0.3"><skipped message="no GPU"/></testcase>
</testsuite></testsuites>""",
            source=report,
        )
        stats = MODULE.summarize_cases(cases, [report], 2)

        # The waived case executed and failed; it must not be filed as a skip
        # (an unavailable capability) nor blended into the pass headline.
        self.assertEqual(stats["executed"], 2)
        self.assertEqual(stats["skipped"], 1)
        self.assertEqual(stats["flaky"], 1)
        self.assertEqual(stats["passed"], 1)
        self.assertEqual(stats["failures"], 0)

    def test_counts_an_assertionless_case_as_empty(self) -> None:
        report = Path("empty-case.xml")
        cases = MODULE.parse_cases(
            """<testsuite name="SparkEngine">
  <testcase name="asserts" time="0.1"/>
  <testcase name="asserts-nothing" time="0.1">
    <properties><property name="empty" value="true"/></properties>
  </testcase>
</testsuite>""",
            source=report,
        )
        stats = MODULE.summarize_cases(cases, [report], 2)

        self.assertEqual(stats["empty"], 1)
        self.assertEqual(stats["flaky"], 0)
        # An empty case still ran, so it stays in executed/passed; the point is
        # that the count is visible instead of hidden inside the headline.
        self.assertEqual(stats["executed"], 2)
        self.assertEqual(stats["passed"], 2)

    def test_rejects_missing_report(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing"):
            MODULE.read_cases(Path("does-not-exist.xml"))

    def test_rejects_empty_test_run(self) -> None:
        cases = MODULE.parse_cases("<testsuites/>", source=Path("empty.xml"))
        with self.assertRaisesRegex(ValueError, "expected at least 1"):
            MODULE.summarize_cases(cases, [Path("empty.xml")], 1)

    def test_rejects_all_skipped_test_run(self) -> None:
        cases = MODULE.parse_cases(
            """<testsuite>
  <testcase name="skip-one"><skipped/></testcase>
  <testcase name="skip-two"><skipped/></testcase>
</testsuite>""",
            source=Path("skipped.xml"),
        )
        with self.assertRaisesRegex(ValueError, "0 executed test cases and 2 skipped"):
            MODULE.summarize_cases(cases, [Path("skipped.xml")], 1)

    def test_rejects_registration_floor_regression(self) -> None:
        cases = MODULE.parse_cases(
            '<testsuite><testcase name="only" time="0.1"/></testsuite>',
            source=Path("one.xml"),
        )
        with self.assertRaisesRegex(ValueError, "expected at least 2"):
            MODULE.summarize_cases(cases, [Path("one.xml")], 2)

    def test_rejects_malformed_xml(self) -> None:
        with self.assertRaisesRegex(ValueError, "cannot parse"):
            MODULE.parse_cases("<testsuite>", source=Path("malformed.xml"))


if __name__ == "__main__":
    unittest.main()
