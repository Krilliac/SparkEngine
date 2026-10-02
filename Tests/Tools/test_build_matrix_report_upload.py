#!/usr/bin/env python3
"""Keep Shipping report diagnostics available without downloading binaries."""

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools" / "buildmatrix"))
from workflow import parse_workflow_yaml  # noqa: E402


class BuildMatrixReportUploadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workflow = parse_workflow_yaml((ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8"))
        cls.steps = workflow["jobs"]["build-windows-shipping"]["steps"]

    def step(self, name):
        matches = [step for step in self.steps if step.get("name") == name]
        self.assertEqual(len(matches), 1)
        return matches[0]

    def test_separate_artifact_contains_only_exact_report_inputs(self):
        step = self.step("Upload build-matrix report diagnostics")
        self.assertEqual(step["with"]["path"], "build-matrix-report-diagnostics.zip")
        bundler = self.step("Bundle bounded build-matrix diagnostics")
        self.assertEqual(bundler["if"], "always()")
        self.assertNotIn("continue-on-error", bundler)
        self.assertEqual(bundler["run"],
                         "python Tools/buildmatrix/bundle_diagnostics.py --output build-matrix-report-diagnostics.zip")
        self.assertLess(self.steps.index(bundler), self.steps.index(step))
        self.assertEqual(step["with"]["name"],
                         "build-matrix-reports-${{ github.sha }}-${{ github.run_attempt }}")
        self.assertNotIn("include-hidden-files", step["with"])
        full = self.step("Upload build-matrix evidence")
        self.assertNotEqual(step["with"]["name"], full["with"]["name"])
        self.assertIn("build/windows-shipping/bin/MinSizeRel", full["with"]["path"].splitlines())

    def test_failed_producer_diagnostics_use_existing_upload_pin_and_retention(self):
        step = self.step("Upload build-matrix report diagnostics")
        full = self.step("Upload build-matrix evidence")
        self.assertEqual(step["if"], "always()")
        self.assertEqual(step["uses"],
                         "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a")
        self.assertEqual(step["uses"], full["uses"])
        self.assertEqual(step["with"]["retention-days"], "${{ env.ARTIFACT_RETENTION_DAYS }}")
        self.assertEqual(step["with"]["retention-days"], full["with"]["retention-days"])
        self.assertEqual(step["with"]["if-no-files-found"], "ignore")
        self.assertEqual(full["with"]["if-no-files-found"], "error")

    def test_upload_follows_actual_outputs_without_skipping_pending_validator(self):
        upload = self.step("Upload build-matrix report diagnostics")
        parity = self.step("Check the configured build matrix")
        self.assertLess(self.steps.index(parity), self.steps.index(upload))
        self.assertLess(self.steps.index(upload), self.steps.index(self.step("Upload build-matrix evidence")))
        self.assertIn("--output build-matrix-parity-findings.json", parity["run"])
        self.assertIn("python Tools/buildmatrix/validate_pending_authority.py", parity["run"])
        inventory_steps = [step for step in self.steps
                           if "--output build-matrix-inventory.json" in step.get("run", "")]
        self.assertEqual(len(inventory_steps), 1)
        self.assertLess(self.steps.index(inventory_steps[0]), self.steps.index(upload))
        self.assertNotIn("continue-on-error", parity)


if __name__ == "__main__":
    unittest.main()
