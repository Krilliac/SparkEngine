#!/usr/bin/env python3
"""Unit tests for the strict JUnit parser in verify-sanitizer-evidence.py.

The sanitizer lanes classify a JUnit report they cannot parse as a
verification-failure and exit 70, so a runner-side change to the report shape
that this parser rejects takes both required lanes down. These tests pin the
shape SparkTests actually emits (Tests/TestMain.cpp WriteJUnitXml) alongside the
archived Known-flaky <skipped> shape, and pin the terminal-Results arithmetic
that reconciles the two.

They also pin parity between run-sanitizer-tests.sh's grep witnesses and the
verifier's patterns (a disagreement is itself a verification failure), and
trip on engine string literals that would read as a crash to both scanners.
"""

from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
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


RUNNER = Path(__file__).with_name("run-sanitizer-tests.sh")
REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_SCAN_PATTERN = re.compile(r"^(\w+)_scan_status=\"\$\(scan '([^']*)'", re.MULTILINE)
VERIFIER_PATTERN_FOR_SCAN = {
    "signature": MODULE.ANY_SANITIZER_PATTERN,
    "warning": MODULE.WARNING_PATTERN,
    "failure": MODULE.FAILURE_PATTERN,
    "crash": MODULE.CRASH_PATTERN,
    "infrastructure": MODULE.INFRASTRUCTURE_PATTERN,
}
SCANNER_PROBES = (
    "Segmentation fault (core dumped)",
    "segmentation FAULT",
    "hot-reload aborted: x",
    "ABORTED",
    "Aborted",
    "Aborted:",
    "(Aborted)",
    "_Aborted_",
    "Coroutine_AbortedSequence",
    "terminate called after throwing an instance of 'std::runtime_error'",
    "libc++abi: terminating due to Uncaught Exception",
    "AddressSanitizer:DEADLYSIGNAL",
    "Tests: 1 passed, 10 failed, 11 total",
    "Tests: 11 passed, 0 failed, 11 total",
    "Tests:3 failed",
    "Tests: x10 failed",
    "Tests: 2 failedness",
    "Assertions: 5 passed, 2 FAILED",
    "Assertions: 7 passed, 0 failed",
    "[  FAILED  ] Contract_Two",
    "[ WARN ] Known flaky",
    "::warning title=Flaky test: x",
    "Runtime Error: shift exponent",
    "runtime error: signed integer overflow",
    "WARNING: threadsanitizer: data race",
    "ERROR: AddressSanitizer: heap-buffer-overflow",
    "SUMMARY: LeakSanitizer: 8 byte(s) leaked",
    "ThreadSanitizer instrumentation enabled",
    "bash: foo: command not found",
    "Permission Denied",
    "Failed to start process",
    "all clean",
)


class ScannerParityTests(unittest.TestCase):
    """run-sanitizer-tests.sh's grep witnesses must agree with the verifier.

    verify_scan turns any disagreement into a verification failure (exit 70), so
    the shell scan and the Python pattern have to classify every line alike.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.grep = shutil.which("grep")
        cls.scans = dict(RUNNER_SCAN_PATTERN.findall(RUNNER.read_text(encoding="utf-8")))

    def test_runner_has_exactly_the_five_verified_scans(self) -> None:
        self.assertEqual(set(self.scans), set(VERIFIER_PATTERN_FOR_SCAN))
        self.assertEqual(len(RUNNER_SCAN_PATTERN.findall(RUNNER.read_text(encoding="utf-8"))), 5)

    def test_runner_scans_are_case_insensitive(self) -> None:
        self.assertIn('grep -qiE "$pattern"', RUNNER.read_text(encoding="utf-8"))

    def test_grep_and_verifier_agree_on_every_probe(self) -> None:
        if self.grep is None:
            if os.name == "posix":
                self.fail("grep is required to check scanner parity")
            self.skipTest("grep is not on PATH on this non-POSIX host")
        with tempfile.TemporaryDirectory(prefix="spark-scanner-parity-") as scratch:
            probe_file = Path(scratch) / "probe.txt"
            disagreements = []
            for probe in SCANNER_PROBES:
                probe_file.write_bytes((probe + "\n").encode("utf-8"))
                for name, pattern in sorted(self.scans.items()):
                    status = subprocess.run(
                        [self.grep, "-qiE", pattern, str(probe_file)], check=False
                    ).returncode
                    self.assertIn(status, (0, 1), f"grep failed on the {name} pattern")
                    python_match = bool(VERIFIER_PATTERN_FOR_SCAN[name].search(probe))
                    if (status == 0) != python_match:
                        disagreements.append(f"{name}: {probe!r} grep={status == 0} python={python_match}")
            self.assertEqual(disagreements, [])


# Engine string literals that match a crash or sanitizer signature make every log
# that prints them look like a crash to both scanners (the 4edbe38d5 incident).
# CrashHandler.cpp prints its signal names only on a real crash.
CRASH_TEXT_ALLOWLIST = {"SparkEngine/Source/Utils/CrashHandler.cpp"}
CRASH_TEXT_ROOTS = ("SparkEngine/Source", "SparkEditor/Source", "SparkServer/src", "GameModules")
CPP_STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


class EngineLiteralTripwireTests(unittest.TestCase):
    def test_no_engine_string_literal_matches_a_crash_or_sanitizer_signature(self) -> None:
        # Scan the committed tree, not the working copy, so in-flight edits in a
        # shared checkout cannot change the verdict.
        # A plain git pathspec's * crosses directories, so root/*.cpp is recursive.
        pathspecs = [f"{root}/*.{ext}" for root in CRASH_TEXT_ROOTS for ext in ("cpp", "h", "hpp", "inl")]
        prefilter = (
            "segmentation fault|core dumped|deadlysignal|terminate called|uncaught exception|"
            "aborted|sanitizer:|runtime error:"
        )
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "grep", "-I", "-i", "-n", "-E", prefilter, "HEAD", "--", *pathspecs],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
        self.assertIn(result.returncode, (0, 1), result.stderr)
        hits = []
        for line in result.stdout.splitlines():
            _, path, number, text = line.split(":", 3)
            for literal in CPP_STRING_LITERAL.findall(text):
                if MODULE.CRASH_PATTERN.search(literal) or MODULE.ANY_SANITIZER_PATTERN.search(literal):
                    hits.append((path, int(number), literal))
        unexpected = [hit for hit in hits if hit[0] not in CRASH_TEXT_ALLOWLIST]
        self.assertEqual(unexpected, [])
        # The allowlist stays exact: an entry that no longer matches must be removed.
        self.assertEqual({hit[0] for hit in hits}, CRASH_TEXT_ALLOWLIST)


if __name__ == "__main__":
    unittest.main()
