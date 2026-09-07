#!/usr/bin/env python3
"""Injected-process orchestration checks; native unsigned fixtures run on Windows."""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("verify-windows-package-signatures.py")
SPEC = importlib.util.spec_from_file_location("signatures", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC) if SCRIPT.exists() else None
if MODULE:
    SPEC.loader.exec_module(MODULE)
THUMBPRINT = "A" * 40
SOURCE = "b" * 40
PREFIX = "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime"


class SignatureTests(unittest.TestCase):
    def test_orchestration_rejects_invalid_packages_and_signature_evidence(self):
        self.assertIsNotNone(MODULE, "Production signature verifier is required")
        for case in ("valid", "unsigned", "unsigned_msi", "untrusted", "hash_mismatch", "expired", "catalog", "wrong_signer",
                     "no_timestamp", "malformed", "bad_schema", "process_failure", "timeout", "changed",
                     "missing", "extra", "directory", "hardlink", "missing_config", "bad_config", "bad_source"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                packages = root / "packages"
                packages.mkdir()
                for suffix in (".exe", ".msi"):
                    (packages / (PREFIX + suffix)).write_bytes(b"fixture " + suffix.encode())
                exe = packages / (PREFIX + ".exe")
                if case == "missing":
                    exe.unlink()
                if case == "extra":
                    (packages / "other.EXE").write_bytes(b"unexpected")
                if case == "directory":
                    exe.unlink()
                    exe.mkdir()

                if case == "hardlink":
                    os.link(exe, root / "linked.exe")

                def runner(argv, **kwargs):
                    self.assertEqual(argv[0], "trusted-powershell")
                    decoded = base64.b64decode(argv[-1]).decode("utf-16-le")
                    self.assertIn("Get-AuthenticodeSignature", decoded)
                    self.assertIn("-LiteralPath $env:SPARK_SIGNATURE_PATH", decoded)
                    self.assertNotIn(str(packages), decoded)
                    artifact = Path(kwargs["env"]["SPARK_SIGNATURE_PATH"])
                    evidence = {"Status": "Valid", "SignatureType": "Authenticode",
                                "SignerThumbprint": THUMBPRINT, "SignerSubject": "CN=Fixture",
                                "TimestampThumbprint": "C" * 40, "TimestampSubject": "CN=Timestamp"}
                    changes = {"unsigned": {"Status": "NotSigned"}, "untrusted": {"Status": "NotTrusted"},
                               "hash_mismatch": {"Status": "HashMismatch"}, "expired": {"Status": "UnknownError"},
                               "catalog": {"SignatureType": "Catalog"}, "wrong_signer": {"SignerThumbprint": "D" * 40},
                               "no_timestamp": {"TimestampThumbprint": None}, "bad_schema": {"Status": 0}}
                    evidence.update(changes.get(case, {}))
                    if case == "unsigned_msi" and artifact.suffix == ".msi":
                        evidence["Status"] = "NotSigned"
                    if case == "timeout":
                        raise subprocess.TimeoutExpired(argv, 120)
                    if case == "changed":
                        artifact.write_bytes(b"substituted")
                    return subprocess.CompletedProcess(argv, 1 if case == "process_failure" else 0,
                                                       "invalid json" if case == "malformed" else json.dumps(evidence), "")

                report = root / "result.json"
                result = MODULE.verify(packages, "1.2.3", SOURCE if case != "bad_source" else "bad",
                                       "" if case == "missing_config" else "bad" if case == "bad_config" else THUMBPRINT,
                                       report, powershell="trusted-powershell", runner=runner)
                data = json.loads(report.read_text())
                self.assertEqual(result == 0, case == "valid", data)
                self.assertEqual(data["passed"], case == "valid")
                if case == "valid":
                    self.assertEqual(len(data["artifacts"]), 2)
                    self.assertEqual(data["source_sha"], SOURCE)
                    self.assertEqual(data["artifacts"][0]["sha256"], hashlib.sha256(exe.read_bytes()).hexdigest())
                    self.assertEqual(MODULE.check_hashes(packages, "1.2.3", SOURCE, report), 0)
                    exe.write_bytes(b"changed after native installer probe")
                    self.assertNotEqual(MODULE.check_hashes(packages, "1.2.3", SOURCE, report), 0)

    def test_symlink_rejected(self):
        self.assertIsNotNone(MODULE)
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            real = root / "real"
            real.write_bytes(b"fixture")
            try:
                (root / (PREFIX + ".exe")).symlink_to(real)
            except OSError:
                self.skipTest("Symlink creation unavailable")
            (root / (PREFIX + ".msi")).write_bytes(b"fixture")
            def no_process(*args, **kwargs):
                self.fail("Linked packages must be rejected before native verification")
            self.assertEqual(MODULE.verify(root, "1.2.3", SOURCE, THUMBPRINT, root / "result.json",
                                           powershell="trusted-powershell", runner=no_process), 1)

    @unittest.skipUnless(os.name == "nt", "Requires native Windows Authenticode")
    def test_native_unsigned_fixture_rejected(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            # Probe each format independently: orchestration correctly stops at
            # the first rejection, so it cannot establish MSI query behavior.
            encoded = base64.b64encode(MODULE.SIGNATURE_SCRIPT.encode("utf-16-le")).decode("ascii")
            for suffix in (".exe", ".msi"):
                with self.subTest(suffix=suffix):
                    artifact = root / (PREFIX + suffix)
                    artifact.write_bytes(b"unsigned fixture")
                    result = subprocess.run(
                        [MODULE.trusted_powershell(), "-NoLogo", "-NoProfile", "-NonInteractive",
                         "-EncodedCommand", encoded],
                        env={**os.environ, "SPARK_SIGNATURE_PATH": str(artifact)},
                        capture_output=True, text=True, encoding="utf-8-sig", timeout=120, check=False)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    evidence = json.loads(result.stdout)
                    self.assertIsInstance(evidence, dict)
                    self.assertEqual(set(evidence), {"Status", "SignatureType", "SignerThumbprint",
                                                    "SignerSubject", "TimestampThumbprint", "TimestampSubject"})
                    self.assertIsInstance(evidence["Status"], str)
                    self.assertTrue(evidence["Status"])
                    self.assertNotEqual(evidence["Status"], "Valid")
                    self.assertIsInstance(evidence["SignatureType"], str)
                    self.assertIsNone(evidence["SignerThumbprint"])
                    self.assertIsNone(evidence["TimestampThumbprint"])
            report = root / "result.json"
            self.assertEqual(MODULE.verify(root, "1.2.3", SOURCE, THUMBPRINT, report,
                                           powershell=MODULE.trusted_powershell()), 1)
            data = json.loads(report.read_text())
            self.assertIn("signature status", data["errors"][0])


if __name__ == "__main__":
    unittest.main()
