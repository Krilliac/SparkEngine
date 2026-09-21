"""Consumer orchestration tests; cryptographic checks have their own real-OpenSSL suite."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from verify_published_stable_release import asset_identity, expected_assets, release_identity, unique_object, verify


class PublishedConsumerTests(unittest.TestCase):
    def setUp(self):
        self.tag = "v1.2.3"
        self.sha = "a" * 40
        self.release = {"id": 123, "tag_name": self.tag, "draft": False, "prerelease": False,
                        "immutable": True,
                        "published_at": "2026-09-21T00:00:00Z"}
        self.payloads = {name: name.encode() for name in expected_assets(self.tag)}
        self.assets = [{"id": index, "name": name, "size": len(data), "state": "uploaded",
                        "digest": "sha256:" + hashlib.sha256(data).hexdigest()}
                       for index, (name, data) in enumerate(sorted(self.payloads.items()), 1)]

    def test_rejects_nightly_draft_prerelease_and_wrong_version(self):
        for field, value in (("draft", True), ("prerelease", True), ("tag_name", "v1.2.2"), ("immutable", False)):
            bad = {**self.release, field: value}
            with self.assertRaises(ValueError):
                release_identity(bad, self.tag)
        with self.assertRaises(ValueError):
            expected_assets("nightly")

    def test_rejects_extra_missing_duplicate_unuploaded_and_unhashed_assets(self):
        for kind in ("extra", "missing", "duplicate", "state", "digest"):
            assets = copy.deepcopy(self.assets)
            if kind == "extra":
                assets.append(assets[0])
            elif kind == "missing":
                assets.pop()
            elif kind == "duplicate":
                assets[-1] = assets[0]
            else:
                assets[0][kind] = None
            with self.assertRaises(ValueError, msg=kind):
                asset_identity(assets, self.tag)

    def run_consumer(self, root, *, tamper=False, drift=False, signature_failure=False):
        tag_record = {"ref": "refs/tags/" + self.tag, "object": {"type": "commit", "sha": self.sha}}
        api = Mock()
        api.json.side_effect = [self.release, tag_record, self.assets,
                                {**self.release, "draft": drift}, self.assets, tag_record]
        by_id = {entry["id"]: entry["name"] for entry in self.assets}
        def download(asset_id, path):
            path.write_bytes(self.payloads[by_id[asset_id]] + (b"tamper" if tamper else b""))
        api.download.side_effect = download
        bundle = Mock(side_effect=ValueError("signature failed") if signature_failure else None)
        provenance = Mock()
        with patch("verify_published_stable_release.values_from_gate_output", return_value={"source": self.sha}):
            result = verify(repository="owner/repo", tag=self.tag, source_commit=self.sha,
                            directory=root / "download", signature_directory=root / "signatures",
                            fingerprint="b" * 64, gate_output=root / "gate.txt", receipt=root / "receipt.json",
                            run_id=456, run_attempt=2, api=api, bundle_verifier=bundle,
                            provenance_verifier=provenance)
        self.assertEqual(api.download.call_count, 7)
        bundle.assert_called_once()
        provenance.assert_called_once()
        api.verify_attestation.assert_called_once_with(self.tag)
        return result

    def test_fresh_downloads_emit_exact_sha_receipt_without_ready_claim(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = self.run_consumer(root)
            self.assertEqual(result["state"], "publication-verified")
            self.assertEqual(result["sourceCommit"], self.sha)
            self.assertEqual(result["verifier"]["runId"], 456)
            self.assertEqual(json.loads((root / "receipt.json").read_text()), result)

    def test_tamper_signature_failure_or_publication_drift_never_emits_receipt(self):
        for fault in ("tamper", "signature_failure", "drift"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                with self.assertRaises(ValueError):
                    self.run_consumer(root, **{fault: True})
                self.assertFalse((root / "receipt.json").exists())

    def test_duplicate_metadata_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            json.loads('{"draft":true,"draft":false}', object_pairs_hook=unique_object)

    def test_existing_receipt_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "receipt.json").write_bytes(b"existing evidence")
            with self.assertRaises(ValueError):
                self.run_consumer(root)
            self.assertEqual((root / "receipt.json").read_bytes(), b"existing evidence")


if __name__ == "__main__":
    unittest.main()
