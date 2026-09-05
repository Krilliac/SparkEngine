#!/usr/bin/env python3
"""Unit tests for the strict JUnit parser in verify-sanitizer-evidence.py.

The sanitizer lanes classify a JUnit report they cannot parse as a
verification-failure and exit 70, so a runner-side change to the report shape
that this parser rejects takes both required lanes down. These tests pin the
shape SparkTests actually emits (Tests/TestMain.cpp WriteJUnitXml) alongside the
archived Known-flaky <skipped> shape, and pin the terminal-Results arithmetic
that reconciles the two.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("verify-sanitizer-evidence.py")
SPEC = importlib.util.spec_from_file_location("verify_sanitizer_evidence", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


CURRENT_SHAPE = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<testsuites tests="4" failures="1" skipped="1" flaky="1" empty="1" time="0.004">\n'
    '  <testsuite name="SparkEngine" tests="4" failures="1" skipped="1" flaky="1" empty="1" time="0.004">\n'
    '    <testcase name="Contract_Passing" time="0.001"/>\n'
    '    <testcase name="Contract_Skipped" time="0.001">\n'
    '      <skipped message="no GPU"/>\n'
    "    </testcase>\n"
    '    <testcase name="Contract_Flaky" time="0.001">\n'
    "      <properties>\n"
    '        <property name="flaky" value="true"/>\n'
    '        <property name="flaky-reason" value="synthetic"/>\n'
    '        <property name="waived-assertions" value="1"/>\n'
    "      </properties>\n"
    '      <flakyFailure message="Known flaky: synthetic">detail</flakyFailure>\n'
    "    </testcase>\n"
    '    <testcase name="Contract_EmptyPromoted" time="0.001">\n'
    "      <properties>\n"
    '        <property name="empty" value="true"/>\n'
    "      </properties>\n"
    '      <failure message="Executed no assertions (--empty-is-error)"></failure>\n'
    "    </testcase>\n"
    "  </testsuite>\n"
    "</testsuites>\n"
)

ARCHIVED_SHAPE = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<testsuites tests="2" failures="0" errors="0" skipped="1">\n'
    '<testsuite name="SparkEngine" tests="2" failures="0" errors="0" skipped="1">'
    '<testcase name="Contract_One"/>'
    '<testcase name="Contract_Flaky"><skipped message="Known flaky: synthetic"/></testcase>'
    "</testsuite></testsuites>\n"
)


def parse(text: str) -> tuple[dict[str, object], list[str]]:
    errors: list[str] = []
    parsed = MODULE.parse_junit(text.encode("utf-8"), minimum_tests=1, errors=errors)
    return parsed, errors


class CurrentJUnitShapeTests(unittest.TestCase):
    def test_current_runner_shape_parses_without_errors(self) -> None:
        parsed, errors = parse(CURRENT_SHAPE)
        self.assertEqual(errors, [])
        self.assertEqual(parsed["tests"], 4)
        self.assertEqual(parsed["failures"], 1)
        self.assertEqual(parsed["errors"], 0)
        self.assertEqual(parsed["skipped"], 1)
        self.assertEqual(parsed["flakyOutcomes"], 1)
        self.assertEqual(parsed["flakySkips"], 0)
        self.assertEqual(parsed["knownFlakyWarnings"], 1)
        self.assertEqual(parsed["empty"], 1)

    def test_terminal_results_reconcile_for_the_current_shape(self) -> None:
        parsed, _ = parse(CURRENT_SHAPE)
        passed = (
            parsed["tests"]
            - parsed["failures"]
            - parsed["errors"]
            - parsed["skipped"]
            - parsed["flakyOutcomes"]
        )
        self.assertEqual(passed, 1)
        self.assertEqual(parsed["skipped"] - parsed["flakySkips"], 1)

    def test_archived_known_flaky_skip_still_counts_as_a_waiver(self) -> None:
        parsed, errors = parse(ARCHIVED_SHAPE)
        self.assertEqual(errors, [])
        self.assertEqual(parsed["knownFlakyWarnings"], 1)
        self.assertEqual(parsed["flakySkips"], 1)
        self.assertEqual(parsed["flakyOutcomes"], 0)
        self.assertEqual(parsed["skipped"] - parsed["flakySkips"], 0)


class JUnitShapeRejectionTests(unittest.TestCase):
    """The parser must still reject shapes SparkTests never emits."""

    def test_declared_flaky_total_must_match_the_elements_present(self) -> None:
        _, errors = parse(CURRENT_SHAPE.replace('flaky="1"', 'flaky="0"'))
        self.assertTrue(
            any("flaky declares 0 but contains 1" in error for error in errors),
            errors,
        )

    def test_declared_empty_total_must_match_the_properties_present(self) -> None:
        _, errors = parse(CURRENT_SHAPE.replace('empty="1"', 'empty="4"'))
        self.assertTrue(
            any("empty declares 4 but contains 1" in error for error in errors),
            errors,
        )

    def test_unknown_property_name_is_rejected(self) -> None:
        _, errors = parse(CURRENT_SHAPE.replace('name="flaky-reason"', 'name="smuggled"'))
        self.assertTrue(
            any("property name is outside the SparkTests schema" in error for error in errors),
            errors,
        )

    def test_unknown_testcase_child_is_still_rejected(self) -> None:
        broken = CURRENT_SHAPE.replace(
            '<flakyFailure message="Known flaky: synthetic">detail</flakyFailure>',
            "<system-out>detail</system-out>",
        )
        _, errors = parse(broken)
        self.assertTrue(
            any("testcase may contain only one failure" in error for error in errors),
            errors,
        )

    def test_property_outside_a_properties_block_is_rejected(self) -> None:
        broken = CURRENT_SHAPE.replace(
            '<flakyFailure message="Known flaky: synthetic">detail</flakyFailure>',
            '<failure message="x"><property name="empty" value="true"/></failure>',
        )
        _, errors = parse(broken)
        self.assertTrue(
            any("properties may contain only property elements" in error for error in errors),
            errors,
        )

    def test_two_outcome_elements_are_still_rejected(self) -> None:
        broken = CURRENT_SHAPE.replace(
            '<flakyFailure message="Known flaky: synthetic">detail</flakyFailure>',
            '<flakyFailure message="Known flaky: synthetic"/><failure message="x"/>',
        )
        _, errors = parse(broken)
        self.assertTrue(
            any("multiple outcome elements" in error for error in errors),
            errors,
        )


if __name__ == "__main__":
    unittest.main()
