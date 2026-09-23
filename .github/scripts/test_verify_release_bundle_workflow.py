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

    def test_msi_qualification_supplies_explicit_external_predecessor_publisher(self) -> None:
        start = self.text.index("- name: Qualify Windows stable MSI install upgrade rollback repair and uninstall")
        block = self.text[start:self.text.index("\n    - name:", start + 10)]
        invocation = block[block.index("python .github/scripts/qualify-windows-msi.py"):]
        self.assertEqual(invocation.count("--previous-signer-thumbprint"), 1)
        self.assertIn('--previous-signer-thumbprint "${{ vars.SPARK_PREVIOUS_RELEASE_SIGNER_THUMBPRINT }}" `',
                      invocation)
        self.assertIn("if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }", invocation)

    def test_windows_authenticode_gate_requires_explicit_self_signed_policy(self) -> None:
        start = self.text.index("- name: Verify Windows stable outer installer signatures")
        block = self.text[start:self.text.index("\n    - name:", start + 10)]
        self.assertIn("SPARK_RELEASE_SIGNER_THUMBPRINT", block)
        self.assertIn("SPARK_RELEASE_TRUST_MODEL", block)
        self.assertIn("SPARK_RELEASE_TRUST_MODEL: ${{ vars.SPARK_RELEASE_TRUST_MODEL }}", block)
        self.assertIn("self-signed", self.text[start - 500:start])

    def test_stable_signing_is_protected_and_precedes_native_verification(self) -> None:
        signing = self.text.index("- name: Sign Windows stable outer installers")
        verification = self.text.index("- name: Verify Windows stable outer installer signatures")
        self.assertLess(signing, verification)
        block = self.text[signing:self.text.index("\n    - name:", signing + 10)]
        self.assertIn("if: needs.prepare.outputs.is_versioned == 'true'", block)
        self.assertIn("SPARK_RELEASE_SIGNING_PFX_BASE64: ${{ secrets.SPARK_RELEASE_SIGNING_PFX_BASE64 }}", block)
        self.assertIn("SPARK_RELEASE_SIGNING_PFX_PASSWORD: ${{ secrets.SPARK_RELEASE_SIGNING_PFX_PASSWORD }}", block)
        self.assertIn("Import-PfxCertificate", block)
        self.assertIn("Get-PfxData", block)
        self.assertIn("$myThumbprintsBefore", block)
        self.assertIn("Pinned signer already exists in CurrentUser personal store", block)
        self.assertIn("PFX must contain exactly the pinned signing certificate", block)
        self.assertIn("Cert:\\CurrentUser\\Root", block)
        self.assertIn("spark-signing-root-added.txt", block)
        self.assertIn("Export-Certificate", block)
        self.assertIn("$signtoolPath sign", block)
        self.assertIn("$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\\10\\bin'", block)
        self.assertIn("Windows SDK x64 SignTool root was not found", block)
        self.assertIn("Windows Kits\\\\10\\\\bin\\\\[^\\\\]+\\\\x64\\\\signtool\\.exe$", block)
        self.assertNotIn("Get-Command signtool.exe", block)
        self.assertIn("/fd SHA256", block)
        self.assertIn("/tr 'http://timestamp.digicert.com'", block)
        self.assertIn("/td SHA256", block)
        self.assertIn("thumbprint does not match", block)
        self.assertIn("Remove-Item -LiteralPath $pfxPath", block)
        self.assertIn("Temporary PFX remains after cleanup", block)
        self.assertIn("Temporary certificate remains after cleanup", block)
        self.assertIn("-Confirm:$false", block)

    def test_partial_pfx_import_cleanup_uses_prevalidated_pinned_identity(self) -> None:
        signing = self.text.index("- name: Sign Windows stable outer installers")
        block = self.text[signing:self.text.index("\n    - name:", signing + 10)]
        self.assertLess(block.index("$pfxThumbprints.Count -ne 1"),
                        block.index("Import-PfxCertificate"))
        self.assertLess(block.index("$importAttempted = $true"),
                        block.index("Import-PfxCertificate"))
        cleanup = block[block.index("} finally {"):]
        self.assertIn("if ($importAttempted)", cleanup)
        self.assertIn("'Cert:\\CurrentUser\\My\\' + $env:SPARK_RELEASE_SIGNER_THUMBPRINT", cleanup)


if __name__ == "__main__":
    unittest.main()
