#!/usr/bin/env python3
"""Adversarial unit tests for the stable release bundle consumer."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from verify_release_bundle import BundleError, verify_release_bundle


class ReleaseBundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("openssl") is None:
            raise unittest.SkipTest("openssl is required for cryptographic bundle tests")

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.names = ["SparkEngine-1.2.3-Windows.zip", "SparkEngine.spdx.json", "SparkEngine-Exact-CI-Evidence.json"]
        (self.root / self.names[0]).write_bytes(b"package bytes\n")
        (self.root / self.names[1]).write_text(json.dumps({
            "spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT",
            "documentNamespace": "https://example.invalid/spark/1.2.3",
            "files": [{"fileName": "SparkEngine-1.2.3-Windows.zip", "checksums": [
                {"algorithm": "SHA256", "checksumValue": hashlib.sha256(
                    (self.root / self.names[0]).read_bytes()).hexdigest()}]}],
            "packages": [{"SPDXID": "SPDXRef-Package", "name": "SparkEngine"}],
        }), encoding="utf-8")
        (self.root / self.names[2]).write_text(json.dumps({
            "schemaVersion": 2, "sourceCommit": "a" * 40,
        }), encoding="utf-8")
        self.expected = self.root / "expected.txt"
        # This mirrors the release workflow's generated inventory: SHA256SUMS
        # is listed as a control asset but is never included in its own sums.
        self.expected.write_text("\n".join([*self.names, "SHA256SUMS"]) + "\n", encoding="utf-8")
        self.sums = self.root / "SHA256SUMS"
        self.signature_manifest = self.root / "release-signatures.json"
        self.private_key = self.root / "signing-key.pem"
        self.public_key = self.root / "trusted-public-key.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                        "-out", str(self.private_key)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(self.private_key), "-pubout",
                        "-out", str(self.public_key)], check=True, capture_output=True)
        der = subprocess.run(["openssl", "pkey", "-pubin", "-in", str(self.public_key),
                              "-outform", "DER"], check=True, capture_output=True).stdout
        self.fingerprint = hashlib.sha256(der).hexdigest()
        for name in self.names:
            subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(self.private_key),
                            "-out", str(self.root / (name + ".sig")), str(self.root / name)], check=True,
                           capture_output=True)
        self._write_inputs()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _sha(self, name: str) -> str:
        return hashlib.sha256((self.root / name).read_bytes()).hexdigest()

    def _write_inputs(self) -> None:
        self.sums.write_text(
            "\n".join(f"{self._sha(name)}  {name}" for name in self.names if name != self.names[2]) + "\n",
            encoding="utf-8",
        )
        self.signature_manifest.write_text(json.dumps({
            "schemaVersion": 1, "algorithm": "detached-sha256",
            "artifacts": [
                {"name": name, "signature": name + ".sig", "artifactSha256": self._sha(name),
                 "signerFingerprint": self.fingerprint}
                for name in self.names
            ],
        }), encoding="utf-8")

    def _verify(self) -> None:
        verify_release_bundle(
            bundle_directory=self.root,
            expected_assets_file=self.expected,
            sha256sums=self.sums,
            sbom=self.root / self.names[1],
            provenance_manifest=self.root / self.names[2],
            signature_manifest=self.signature_manifest,
            source_commit="a" * 40,
            trusted_public_key=self.public_key,
            trusted_key_fingerprint=self.fingerprint,
        )

    def test_accepts_complete_external_signature_bundle(self) -> None:
        self._verify()

    def test_missing_signature_manifest_is_fail_closed(self) -> None:
        self.signature_manifest.unlink()
        with self.assertRaisesRegex(BundleError, "detached signature manifest"):
            self._verify()

    def test_missing_trusted_public_key_is_fail_closed(self) -> None:
        self.public_key.unlink()
        with self.assertRaisesRegex(BundleError, "public key is not provisioned"):
            self._verify()

    def test_rejects_unsigned_or_uncovered_artifact(self) -> None:
        data = json.loads(self.signature_manifest.read_text(encoding="utf-8"))
        data["artifacts"].pop()
        self.signature_manifest.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "cover every expected"):
            self._verify()

    def test_rejects_signature_digest_binding_drift(self) -> None:
        data = json.loads(self.signature_manifest.read_text(encoding="utf-8"))
        data["artifacts"][0]["artifactSha256"] = "0" * 64
        self.signature_manifest.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "binding drifted"):
            self._verify()

    def test_rejects_extra_promotable_signature(self) -> None:
        (self.root / "unreferenced.sig").write_bytes(b"stray")
        with self.assertRaisesRegex(BundleError, "unreferenced"):
            self._verify()

    def test_rejects_case_insensitive_stray_signature(self) -> None:
        (self.root / "unreferenced.SIG").write_bytes(b"stray")
        with self.assertRaisesRegex(BundleError, "unreferenced"):
            self._verify()

    def test_rejects_extra_promotable_package(self) -> None:
        (self.root / "SparkEngine-rogue.zip").write_bytes(b"rogue")
        with self.assertRaisesRegex(BundleError, "extra promotable"):
            self._verify()

    def test_rejects_checksum_drift_and_missing_sbom(self) -> None:
        original = (self.root / self.names[0]).read_bytes()
        (self.root / self.names[0]).write_bytes(original + b"tampered")
        with self.assertRaisesRegex(BundleError, "SHA256SUMS digest mismatch"):
            self._verify()
        (self.root / self.names[0]).write_bytes(original)
        (self.root / self.names[1]).unlink()
        with self.assertRaisesRegex(BundleError, "expected stable asset"):
            self._verify()

    def test_rejects_empty_spdx_coverage(self) -> None:
        sbom = self.root / self.names[1]
        data = json.loads(sbom.read_text(encoding="utf-8"))
        data["files"] = []
        sbom.write_text(json.dumps(data), encoding="utf-8")
        self._write_inputs()
        with self.assertRaisesRegex(BundleError, "files inventory is empty"):
            self._verify()

    def test_rejects_forged_spdx_checksum(self) -> None:
        sbom = self.root / self.names[1]
        data = json.loads(sbom.read_text(encoding="utf-8"))
        data["files"][0]["checksums"][0]["checksumValue"] = "0" * 64
        sbom.write_text(json.dumps(data), encoding="utf-8")
        self._write_inputs()
        with self.assertRaisesRegex(BundleError, "SPDX checksum does not match"):
            self._verify()

    def test_rejects_missing_package_coverage(self) -> None:
        second = self.root / "SparkEngine-1.2.3-Windows.msi"
        second.write_bytes(b"second package")
        self.names.append(second.name)
        names = self.expected.read_text(encoding="utf-8").splitlines()
        self.expected.write_text("\n".join([*names[:-1], second.name, "SHA256SUMS"]) + "\n", encoding="utf-8")
        self._write_inputs()
        with self.assertRaisesRegex(BundleError, "every stable package"):
            self._verify()

    def test_rejects_provenance_commit_drift(self) -> None:
        data = json.loads((self.root / self.names[2]).read_text(encoding="utf-8"))
        data["sourceCommit"] = "b" * 40
        (self.root / self.names[2]).write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "source commit"):
            self._verify()

    def test_rejects_duplicate_json_keys(self) -> None:
        self.signature_manifest.write_text(
            '{"schemaVersion":1,"schemaVersion":1,"algorithm":"detached-sha256","artifacts":[]}',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(BundleError, "repeats key"):
            self._verify()

    def test_rejects_case_folded_expected_collision(self) -> None:
        self.expected.write_text("sparkengine-1.2.3-windows.zip\nSparkEngine-1.2.3-Windows.zip\nSHA256SUMS\n",
                                encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "case-fold collision"):
            self._verify()


if __name__ == "__main__":
    unittest.main()
