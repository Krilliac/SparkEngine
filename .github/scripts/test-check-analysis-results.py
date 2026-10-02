#!/usr/bin/env python3
"""CI-110: the CodeQL gate blocks new findings against a reviewed baseline and fails closed."""

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("check-analysis-results.py")
BASELINE = Path(__file__).resolve().parents[1] / "codeql-baseline.json"
SPEC = importlib.util.spec_from_file_location("analysis_gate", SCRIPT)
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)

REVIEW = "reviewed: vendored code"


def finding(rule="cpp/unsafe", path="Src/a.cpp", fingerprint="abc123:1", **extra):
    return {"ruleId": rule, "locations": [{"physicalLocation": {"artifactLocation": {"uri": path}}}],
            "partialFingerprints": {"primaryLocationLineHash": fingerprint}, **extra}


def clean():
    # Real CodeQL SARIF always carries informational notifications (every
    # extracted file, extractor summaries); these must not fail the gate.
    notifications = [
        {"level": "none", "descriptor": {"id": "cpp/diagnostics/successfully-extracted-files"}},
        {"level": "note", "descriptor": {"id": "cpp/autobuilder/summary"}},
        {"level": "warning", "descriptor": {"id": "cpp/extractor/parse-warning"}},
    ]
    return {"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "CodeQL"}},
            "invocations": [{"executionSuccessful": True, "toolExecutionNotifications": notifications,
                             "toolConfigurationNotifications": []}],
            "results": []}]}


def baseline(*entries):
    return {"schema": 1, "findings": [
        {"language": language, "ruleId": rule, "path": path, "fingerprint": fingerprint, "review": REVIEW}
        for language, rule, path, fingerprint in entries]}


def gate(report, document, language="c-cpp"):
    return GATE.compare(GATE.report_findings(report), GATE.load_baseline(document, language))


class AnalysisGateTests(unittest.TestCase):
    def test_successful_scan_with_informational_notifications_passes(self):
        self.assertEqual(gate(clean(), baseline()), ([], []))

    def test_baselined_finding_passes(self):
        report = clean()
        report["runs"][0]["results"] = [finding()]
        self.assertEqual(gate(report, baseline(("c-cpp", "cpp/unsafe", "Src/a.cpp", "abc123:1"))), ([], []))

    def test_new_finding_blocks_even_if_sarif_marks_it_accepted(self):
        accepted = baseline(("c-cpp", "cpp/unsafe", "Src/a.cpp", "abc123:1"))
        for overrides in ({}, {"level": "note"}, {"baselineState": "unchanged"},
                          {"suppressions": [{"kind": "inSource", "status": "accepted"}]}):
            with self.subTest(overrides=overrides):
                report = clean()
                report["runs"][0]["results"] = [finding(), finding(fingerprint="new999:1", **overrides)]
                new, stale = gate(report, accepted)
                self.assertEqual(new, [("cpp/unsafe", "Src/a.cpp", "new999:1")])
                self.assertEqual(stale, [])

    def test_baseline_matches_rule_path_and_fingerprint_exactly(self):
        accepted = baseline(("c-cpp", "cpp/unsafe", "Src/a.cpp", "abc123:1"))
        for variant in (finding(rule="cpp/other"), finding(path="Src/b.cpp"), finding(fingerprint="abc123:2")):
            with self.subTest(variant=variant):
                report = clean()
                report["runs"][0]["results"] = [variant]
                new, stale = gate(report, accepted)
                self.assertEqual(len(new), 1)
                self.assertEqual(len(stale), 1)

    def test_one_entry_accepts_one_occurrence_only(self):
        report = clean()
        report["runs"][0]["results"] = [finding(), finding()]
        new, _ = gate(report, baseline(("c-cpp", "cpp/unsafe", "Src/a.cpp", "abc123:1")))
        self.assertEqual(new, [("cpp/unsafe", "Src/a.cpp", "abc123:1")])

    def test_fixed_finding_leaves_a_stale_entry_that_blocks(self):
        new, stale = gate(clean(), baseline(("c-cpp", "cpp/unsafe", "Src/a.cpp", "abc123:1")))
        self.assertEqual((new, stale), ([], [("cpp/unsafe", "Src/a.cpp", "abc123:1")]))

    def test_other_languages_entries_do_not_accept_or_go_stale(self):
        report = clean()
        report["runs"][0]["results"] = [finding()]
        document = baseline(("python", "cpp/unsafe", "Src/a.cpp", "abc123:1"))
        self.assertEqual(gate(report, document, "c-cpp"), ([("cpp/unsafe", "Src/a.cpp", "abc123:1")], []))
        self.assertEqual(gate(clean(), document, "c-cpp"), ([], []))

    def test_incomplete_or_malformed_analysis_fails_closed(self):
        cases = [None, {}, {"version": "2.1.0", "runs": []}]
        for field, value in (("results", None), ("invocations", []),
                             ("invocations", [{"executionSuccessful": False}]),
                             ("results", [{}]), ("results", [{"ruleId": "cpp/x"}]),
                             ("results", [finding(fingerprint="")]),
                             ("results", [{"ruleId": "cpp/x", "locations": [{}],
                                           "partialFingerprints": {"primaryLocationLineHash": "a:1"}}]),
                             ("tool", {"driver": {"name": "other"}}),
                             ("invocations", [{"executionSuccessful": True,
                               "toolExecutionNotifications": [{"level": "error"}]}]),
                             ("invocations", [{"executionSuccessful": True,
                               "toolConfigurationNotifications": [{"level": "error"}]}])):
            report = clean()
            report["runs"][0][field] = value
            cases.append(report)
        for report in cases:
            with self.subTest(report=report), self.assertRaises(ValueError):
                GATE.report_findings(report)

    def test_malformed_baseline_fails_closed(self):
        good = baseline(("c-cpp", "cpp/unsafe", "Src/a.cpp", "abc123:1"))
        mutants = [None, {"findings": []}, {"schema": 2, "findings": []}, {"schema": 1}]
        for mutate in (lambda e: e.update(review=""), lambda e: e.pop("review"), lambda e: e.update(extra=1),
                       lambda e: e.update(language="java"), lambda e: e.update(fingerprint=" ")):
            document = copy.deepcopy(good)
            mutate(document["findings"][0])
            mutants.append(document)
        duplicate = copy.deepcopy(good)
        duplicate["findings"].append(copy.deepcopy(duplicate["findings"][0]))
        mutants.append(duplicate)
        for document in mutants:
            with self.subTest(document=document), self.assertRaises(ValueError):
                GATE.load_baseline(document, "c-cpp")

    def test_committed_baseline_is_valid_and_reviewed(self):
        document = json.loads(BASELINE.read_text(encoding="utf-8"))
        total = sum(len(GATE.load_baseline(document, language)) for language in GATE.LANGUAGES)
        self.assertEqual(total, len(document["findings"]))
        for entry in document["findings"]:
            self.assertGreaterEqual(len(entry["review"]), 40, entry)

    def test_cli_exit_codes_are_gate_consumable(self):
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "report.sarif"
            baseline_path = Path(directory) / "baseline.json"
            baseline_path.write_text(json.dumps(baseline(("c-cpp", "cpp/old", "Src/a.cpp", "abc123:1"))),
                                     encoding="utf-8")

            def run(results):
                report = clean()
                report["runs"][0]["results"] = results
                report_path.write_text(json.dumps(report), encoding="utf-8")
                return subprocess.run([sys.executable, str(SCRIPT), str(report_path), "--language", "c-cpp",
                                       "--baseline", str(baseline_path)], capture_output=True, text=True,
                                      timeout=20)

            for results, expected in (([finding(rule="cpp/old")], 0),
                                      ([finding(rule="cpp/old"), finding(rule="cpp/regression", path="Src/b.cpp")], 1),
                                      ([], 1)):
                result = run(results)
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            self.assertIn("NEW finding cpp/regression", run([finding(rule="cpp/old"),
                                                             finding(rule="cpp/regression")]).stdout)
            self.assertIn("STALE baseline entry cpp/old", run([]).stdout)
            report_path.write_text('{"version":"2.1.0","runs":[],"runs":[]}', encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), str(report_path), "--language", "c-cpp",
                                     "--baseline", str(baseline_path)], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
