"""Consumer orchestration tests; cryptographic checks have their own real-OpenSSL suite."""
import copy
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import errno
import os
from pathlib import Path
import stat
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from verify_published_stable_release import asset_identity, expected_assets, release_identity, unique_object, verify
import receipt_publication
from receipt_publication import MAX_RECEIPT_BYTES, ReceiptPublicationError, publish_receipt_no_replace


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


class ReceiptPublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.receipt = self.root / "receipt.json"
        self.payload = b'{"state":"publication-verified"}\n'

    def tearDown(self):
        self.temporary.cleanup()

    def assert_clean(self, names=()):
        self.assertEqual({path.name for path in self.root.iterdir()}, set(names))

    def test_exact_complete_receipt_has_no_staging_debris(self):
        publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(self.receipt.read_bytes(), self.payload)
        self.assertEqual(self.receipt.stat().st_nlink, 1)
        self.assert_clean(["receipt.json"])

    def test_existing_regular_hardlinked_or_directory_target_is_preserved(self):
        for kind in ("regular", "hardlink", "directory"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                source = root / "source"
                source.write_bytes(b"existing data")
                target = root / "receipt.json"
                if kind == "directory":
                    target.mkdir()
                elif kind == "hardlink":
                    os.link(source, target)
                else:
                    target.write_bytes(b"existing data")
                identity = target.stat().st_ino
                with self.assertRaises(ValueError):
                    publish_receipt_no_replace(target, self.payload)
                self.assertEqual(source.read_bytes(), b"existing data")
                self.assertEqual(target.stat().st_ino, identity)
                self.assertEqual({path.name for path in root.iterdir()}, {"source", "receipt.json"})

    def test_parent_symlink_or_windows_junction_is_rejected(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "nested").mkdir()
        link = self.root / "linked"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError:
            if os.name != "nt":
                raise
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                                    capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 0)
        try:
            with self.assertRaises(ReceiptPublicationError):
                publish_receipt_no_replace(link, self.payload)
            with self.assertRaises(ReceiptPublicationError):
                publish_receipt_no_replace(link / "nested" / "receipt.json", self.payload)
            self.assertEqual(list((outside / "nested").iterdir()), [])
        finally:
            if link.is_symlink():
                link.unlink()
            else:
                link.rmdir()

    def test_dangling_destination_symlink_is_not_followed_or_replaced(self):
        try:
            self.receipt.symlink_to(self.root / "missing-target")
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows account cannot create file symlinks; native junction coverage remains active")
            raise
        with self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assertTrue(self.receipt.is_symlink())
        self.assertFalse((self.root / "missing-target").exists())

    def test_concurrent_publishers_have_exactly_one_winner(self):
        payloads = [f'{{"writer":{index}}}\n'.encode() for index in range(4)]
        def publish(payload):
            try:
                publish_receipt_no_replace(self.receipt, payload)
                return payload
            except ReceiptPublicationError:
                return None
        with ThreadPoolExecutor(max_workers=4) as executor:
            winners = [result for result in executor.map(publish, payloads) if result is not None]
        self.assertEqual(len(winners), 1)
        self.assertEqual(self.receipt.read_bytes(), winners[0])
        self.assert_clean(["receipt.json"])

    def test_reparse_attribute_is_rejected_on_every_platform(self):
        information = SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
        with patch.object(Path, "lstat", return_value=information), self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assert_clean()

    def test_payload_and_names_are_bounded_before_writing(self):
        for payload in (b"", bytearray(b"data"), b"x" * (MAX_RECEIPT_BYTES + 1)):
            with self.assertRaises(ReceiptPublicationError):
                publish_receipt_no_replace(self.receipt, payload)
        for name in ("NUL.json", "receipt:stream", "receipt.", "x" * 201):
            with self.assertRaises(ReceiptPublicationError):
                publish_receipt_no_replace(self.root / name, self.payload)
        with self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(self.root / "missing" / "receipt.json", self.payload)
        self.assert_clean()

    def test_flush_failure_leaves_no_success_name_or_staging_file(self):
        if os.name == "nt":
            import package_evidence_io
            write = package_evidence_io._write_windows_staged_payload
            def fail(handle, payload):
                write(handle, payload)
                raise OSError(errno.EIO, "injected flush failure")
            boundary = patch.object(package_evidence_io, "_write_windows_staged_payload", side_effect=fail)
        else:
            sync = os.fsync
            def fail(descriptor):
                if stat.S_ISREG(os.fstat(descriptor).st_mode):
                    raise OSError(errno.EIO, "injected flush failure")
                return sync(descriptor)
            boundary = patch.object(os, "fsync", side_effect=fail)
        with boundary, self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assert_clean()

    def test_no_replace_race_preserves_the_competing_file(self):
        real_link = os.link
        def collide(source, target, *args, **kwargs):
            if "dst_dir_fd" in kwargs:
                descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600,
                                     dir_fd=kwargs["dst_dir_fd"])
                os.write(descriptor, b"competing data")
                os.close(descriptor)
            else:
                Path(target).write_bytes(b"competing data")
            return real_link(source, target, *args, **kwargs)
        with patch.object(os, "link", side_effect=collide), self.assertRaises(ValueError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(self.receipt.read_bytes(), b"competing data")
        self.assert_clean(["receipt.json"])

    def test_link_failure_cleans_staging_without_publishing(self):
        with patch.object(os, "link", side_effect=OSError(errno.EIO, "injected link failure")), self.assertRaises(ValueError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assert_clean()

    def test_directory_sync_only_tolerates_explicitly_unsupported_filesystems(self):
        for code in (errno.EINVAL, errno.ENOTSUP):
            with patch.object(os, "fsync", side_effect=OSError(code, "unsupported")):
                receipt_publication._sync_directory(0)
        with patch.object(os, "fsync", side_effect=OSError(errno.EIO, "I/O failure")), self.assertRaises(OSError):
            receipt_publication._sync_directory(0)

    @unittest.skipUnless(os.name == "posix", "directory fsync is exercised by the POSIX publisher")
    def test_directory_sync_failure_rolls_back_its_own_receipt(self):
        sync = os.fsync
        def fail(descriptor):
            if stat.S_ISDIR(os.fstat(descriptor).st_mode):
                raise OSError(errno.EIO, "injected directory sync failure")
            return sync(descriptor)
        with patch.object(os, "fsync", side_effect=fail), self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assert_clean()

    @unittest.skipUnless(os.name == "posix", "POSIX partial-write handling")
    def test_partial_writes_complete_and_zero_progress_fails_cleanly(self):
        write = os.write
        with patch.object(os, "write", side_effect=lambda fd, data: write(fd, data[:3])):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(self.receipt.read_bytes(), self.payload)
        self.receipt.unlink()
        with patch.object(os, "write", return_value=0), self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assert_clean()

    @unittest.skipUnless(os.name == "posix", "POSIX descriptor anchoring under directory rename")
    def test_parent_swap_never_writes_through_the_replacement_link(self):
        parent, moved, outside = (self.root / name for name in ("parent", "moved", "outside"))
        parent.mkdir()
        outside.mkdir()
        real_link = os.link
        def swap(source, target, *args, **kwargs):
            parent.rename(moved)
            parent.symlink_to(outside, target_is_directory=True)
            return real_link(source, target, *args, **kwargs)
        with patch.object(os, "link", side_effect=swap), self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(parent / "receipt.json", self.payload)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertEqual(list(moved.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
