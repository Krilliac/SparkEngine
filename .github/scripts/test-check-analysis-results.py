#!/usr/bin/env python3
"""CI-110: real SARIF gate rejects alerts, missing data and suppressed findings."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("check-analysis-results.py")
SPEC = importlib.util.spec_from_file_location("analysis_gate", SCRIPT)
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


def clean():
    return {"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "CodeQL"}},
            "invocations": [{"executionSuccessful": True}], "results": []}]}


class AnalysisGateTests(unittest.TestCase):
    def test_successful_scan_without_findings(self):
        self.assertEqual(GATE.check_report(clean()), 0)

    def test_every_finding_blocks_even_if_locally_marked_accepted(self):
        for overrides in ({}, {"level": "note"}, {"baselineState": "unchanged"},
                          {"suppressions": [{"kind": "inSource", "status": "accepted"}]}):
            with self.subTest(overrides=overrides):
                report = clean()
                report["runs"][0]["results"] = [{"ruleId": "cpp/unsafe", **overrides}]
                self.assertEqual(GATE.check_report(report), 1)

    def test_missing_or_unsuccessful_analysis_fails_closed(self):
        cases = [None, {}, {"version": "2.1.0", "runs": []}]
        for field, value in (("results", None), ("invocations", []),
                             ("invocations", [{"executionSuccessful": False}]),
                             ("results", [{}]), ("tool", {"driver": {"name": "other"}}),
                             ("invocations", [{"executionSuccessful": True,
                               "toolExecutionNotifications": [{"level": "warning"}]}])):
            report = clean()
            report["runs"][0][field] = value
            cases.append(report)
        for report in cases:
            with self.subTest(report=report), self.assertRaises(ValueError):
                GATE.check_report(report)

    def test_cli_exit_codes_are_gate_consumable(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.sarif"
            report = clean()
            for results, expected in (([], 0), ([{"ruleId": "cpp/regression"}], 1)):
                report["runs"][0]["results"] = results
                path.write_text(json.dumps(report), encoding="utf-8")
                result = subprocess.run([sys.executable, str(SCRIPT), str(path)], capture_output=True, timeout=20)
                self.assertEqual(result.returncode, expected, result.stderr)
            path.write_text('{"version":"2.1.0","runs":[],"runs":[]}', encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), str(path)], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
