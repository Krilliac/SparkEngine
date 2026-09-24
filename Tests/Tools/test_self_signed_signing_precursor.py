"""Static and syntax-only checks for the owner-run signing precursor helper."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Tools" / "platform-cert" / "create-self-signed-signing-precursor.ps1"


class SelfSignedSigningPrecursorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_owner_only_noninteractive_and_private_output_contract(self) -> None:
        for required in (
            "[Environment]::UserInteractive",
            "$NonInteractive",
            "Read-Host -AsSecureString",
            "Cert:\\CurrentUser\\My",
            "KeyAlgorithm RSA",
            "HashAlgorithm SHA256",
            "-Type CodeSigningCert",
            "KeyExportPolicy Exportable",
            "SPARK_RELEASE_SIGNER_THUMBPRINT",
            "SPARK_RELEASE_TRUST_MODEL=self-signed",
            "SPARK_RELEASE_SIGNING_PFX_BASE64",
            "SPARK_RELEASE_SIGNING_PFX_PASSWORD",
            "Unknown Publisher",
            "detached release-signature bundle",
        ):
            self.assertIn(required, self.text)

    def test_refuses_overwrite_and_reparse_or_repository_output(self) -> None:
        for required in (
            "Refusing to overwrite existing PFX",
            "ReparsePoint",
            "must not be inside the SparkEngine repository",
            "must remain under the current user LOCALAPPDATA private directory",
            "Assert-NoReparseComponents",
            "Assert-PrivateAcl",
            "Get-Acl",
            "IdentityReference.Value",
            "WindowsIdentity]::GetCurrent",
            "matching subject already exists",
            "removable",
            "Test-PathInside",
        ):
            self.assertIn(required, self.text)

    def test_password_is_not_plaintext_or_logged(self) -> None:
        self.assertIn("$password = Read-Host -AsSecureString", self.text)
        self.assertNotIn("Read-Host -Prompt", self.text)
        self.assertNotIn("ConvertFrom-SecureString", self.text)
        self.assertNotIn("PlainText", self.text)
        self.assertNotIn("Write-Output $password", self.text)

    def test_export_failure_cleans_only_the_exact_new_certificate(self) -> None:
        for required in (
            "$createdThumbprint = $certificate.Thumbprint.ToUpperInvariant()",
            "$createdThumbprint -match '^[0-9A-F]{40}$'",
            "Where-Object { $_.Thumbprint -eq $createdThumbprint }",
            "if ($createdCertificate.Count -eq 1)",
            "Remove-Item -LiteralPath ([string]$createdCertificate[0].PSPath) -DeleteKey -Force -ErrorAction Stop",
            "$cleanupErrors +=",
            "if ($cleanupErrors.Count -ne 0)",
        ):
            self.assertIn(required, self.text)

    def test_powershell_parser_accepts_script_without_executing_it(self) -> None:
        powershell = shutil.which("pwsh") or shutil.which("powershell")
        if not powershell:
            self.skipTest("PowerShell is unavailable; static checks still cover the contract")
        command = (
            "$tokens=$null; $errors=$null; "
            "[System.Management.Automation.Language.Parser]::ParseFile(" 
            "[IO.Path]::GetFullPath($env:SPARK_SIGNING_SCRIPT_PATH), [ref]$tokens, [ref]$errors) | Out-Null; "
            "if ($errors.Count) { $errors | ForEach-Object { $_.ToString() }; exit 1 }"
        )
        result = subprocess.run(
            [powershell, "-NoProfile", "-NonInteractive", "-Command", command],
            env={**os.environ, "SPARK_SIGNING_SCRIPT_PATH": str(SCRIPT)},
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
