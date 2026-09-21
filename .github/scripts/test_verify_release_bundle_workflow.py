#!/usr/bin/env python3
"""Structural integration checks for the signed-bundle release boundary."""

from pathlib import Path
import unittest


WORKFLOW = Path(__file__).parents[1] / "workflows" / "release.yml"


class ReleaseBundleWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")

    def test_checksum_inventory_does_not_self_hash(self) -> None:
        collect = self.text[self.text.index("    - name: Collect release assets"):]
        collect = collect[:collect.index("    - name: Inspect existing release before mutation")]
        self.assertIn('sha256sum "$artifact" >> SHA256SUMS', collect)
        self.assertIn('FILES="${FILES}SHA256SUMS"', collect)
        self.assertNotIn('sha256sum SHA256SUMS >> SHA256SUMS', collect)

    def test_external_producer_precedes_both_consumers(self) -> None:
        producer = self.text.index("- name: Provision external stable signature bundle")
        first = self.text.index("- name: Verify complete stable release bundle before draft publication")
        second = self.text.index("- name: Reverify signed stable release bundle immediately before promotion")
        self.assertLess(producer, first)
        self.assertLess(producer, second)
        for start in (first, second):
            block = self.text[start:self.text.find("\n    - name:", start + 10)]
            self.assertIn("--trusted-public-key", block)
            self.assertIn("--trusted-key-fingerprint", block)
            self.assertIn("spark-release-signature-bundle/release-signatures.json", block)

    def test_provisioning_requires_pinned_external_bundle_digest_and_url(self) -> None:
        start = self.text.index("- name: Provision external stable signature bundle")
        block = self.text[start:self.text.index("\n    - name:", start + 10)]
        self.assertIn("SPARKENGINE_STABLE_SIGNATURE_BUNDLE_URL", block)
        self.assertIn("SPARKENGINE_STABLE_SIGNATURE_BUNDLE_SHA256", block)
        self.assertIn("sha256sum --check --status", block)
        self.assertIn("extract_release_signature_bundle.py", block)
        self.assertIn("RUNNER_TEMP/spark-release-signature-bundle", block)


if __name__ == "__main__":
    unittest.main()
