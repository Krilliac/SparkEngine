#!/usr/bin/env python3
"""Adversarial unit tests for the stable release bundle consumer."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from verify_release_bundle import BundleError, verify_release_bundle


class ReleaseBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.names = ["SparkEngine-1.2.3-Windows.zip", "SparkEngine.spdx.json", "SparkEngine-Exact-CI-Evidence.json"]
        (self.root / self.names[0]).write_bytes(b"package bytes\n")
        (self.root / self.names[1]).write_text(json.dumps({
            "spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT",
            "documentNamespace": "https://example.invalid/spark/1.2.3", "files": [],
        }), encoding="utf-8")
        (self.root / self.names[2]).write_text(json.dumps({
            "schemaVersion": 2, "sourceCommit": "a" * 40,
        }), encoding="utf-8")
        self.expected = self.root / "expected.txt"
        self.expected.write_text("\n".join(self.names) + "\n", encoding="utf-8")
        self.sums = self.root / "SHA256SUMS"
        self.signature_manifest = self.root / "release-signatures.json"
        for name in self.names:
            (self.root / (name + ".sig")).write_bytes(b"external signature bytes")
        self._write_inputs()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _sha(self, name: str) -> str:
        return hashlib.sha256((self.root / name).read_bytes()).hexdigest()

    def _write_inputs(self) -> None:
        self.sums.write_text(
            "\n".join(f"{self._sha(name)}  {name}" for name in self.names[:-1]) + "\n",
            encoding="utf-8",
        )
        self.signature_manifest.write_text(json.dumps({
            "schemaVersion": 1, "algorithm": "detached-sha256",
            "artifacts": [
                {"name": name, "signature": name + ".sig", "artifactSha256": self._sha(name),
                 "signerFingerprint": "EXTERNAL-PROVISIONED-SIGNER"}
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
        )

    def test_accepts_complete_external_signature_bundle(self) -> None:
        self._verify()

    def test_missing_signature_manifest_is_fail_closed(self) -> None:
        self.signature_manifest.unlink()
        with self.assertRaisesRegex(BundleError, "detached signature manifest"):
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


if __name__ == "__main__":
    unittest.main()
