#!/usr/bin/env python3
"""CLI diagnostic wording must not imply a baseline review that did not occur."""

import copy
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Tools" / "buildmatrix"))
import check_parity  # noqa: E402


class BuildMatrixDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.report = {
            "schemaVersion": 3,
            "profile": "stable-v1",
            "state": "blocked",
            "errorCount": 1,
            "warningCount": 0,
            "findings": [{"category": "fixture", "severity": "error", "message": "Pending fixture"}],
        }

    def invoke(self, baseline=None):
        output = io.BytesIO()
        stderr = io.StringIO()
        stdout = io.TextIOWrapper(output, encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="spark-matrix-diagnostics-") as directory:
            args = []
            if baseline is not None:
                path = Path(directory) / "baseline.json"
                path.write_bytes(baseline)
                args = ["--baseline", str(path)]
            with mock.patch.object(check_parity, "_load_inventory", return_value={}), \
                 mock.patch.object(check_parity, "build_report", return_value=self.report), \
                 mock.patch.object(check_parity.sys, "stdout", stdout), \
                 mock.patch.object(check_parity.sys, "stderr", stderr):
                status = check_parity.main(args)
        self.assertEqual(output.getvalue(), check_parity.render_report(self.report))
        self.assertEqual(self.report["state"], "blocked")
        return status, stderr.getvalue()

    def test_no_baseline_uses_neutral_label_and_preserves_findings_exit(self):
        status, diagnostic = self.invoke()
        self.assertEqual(status, check_parity.EXIT_FINDINGS)
        self.assertEqual(diagnostic, "FINDINGS: 1 blocking, 0 advisory\n")

    def test_matching_baseline_keeps_reviewed_label_and_findings_exit(self):
        status, diagnostic = self.invoke(check_parity.render_report(self.report))
        self.assertEqual(status, check_parity.EXIT_FINDINGS)
        self.assertEqual(diagnostic, "REVIEWED FINDINGS: 1 blocking, 0 advisory\n")

    def test_changed_baseline_still_rejects_before_reviewed_label(self):
        baseline = copy.deepcopy(self.report)
        baseline["findings"][0]["message"] = "Different fixture"
        status, diagnostic = self.invoke(check_parity.render_report(baseline))
        self.assertEqual(status, check_parity.EXIT_BASELINE_DRIFT)
        self.assertIn("BASELINE DRIFT:", diagnostic)
        self.assertNotIn("REVIEWED FINDINGS", diagnostic)

    def test_noncanonical_baseline_bytes_still_reject(self):
        status, diagnostic = self.invoke(check_parity.render_report(self.report) + b"\n")
        self.assertEqual(status, check_parity.EXIT_BASELINE_DRIFT)
        self.assertIn("BASELINE DRIFT:", diagnostic)
        self.assertNotIn("REVIEWED FINDINGS", diagnostic)

    def test_malformed_baseline_still_rejects(self):
        status, diagnostic = self.invoke(b"not JSON")
        self.assertEqual(status, check_parity.EXIT_BASELINE_DRIFT)
        self.assertIn("BASELINE DRIFT: cannot parse", diagnostic)
        self.assertNotIn("REVIEWED FINDINGS", diagnostic)


if __name__ == "__main__":
    unittest.main()
