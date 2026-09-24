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
import tarfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from verify_published_stable_release import asset_identity, expected_assets, release_identity, unique_object, verify
from record_release_approval import canonical_bytes
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
        self.approval_record = {"schemaVersion": 1, "kind": "spark-stable-release-approval",
                                "sourceCommit": self.sha, "run": {"id": 456, "attempt": 1}}
        self.approval_calls = []

    def approval_arguments(self, *, digest=None, attempt=1, record=None):
        def collector(**identity):
            self.approval_calls.append(identity)
            return record if record is not None else self.approval_record
        expected = digest or hashlib.sha256(canonical_bytes(self.approval_record)).hexdigest()
        return {"approval_record_sha256": expected, "approval_run_attempt": attempt,
                "approval_collector": collector}

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
                            provenance_verifier=provenance, **self.approval_arguments())
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

    def test_receipt_binds_the_independently_rebuilt_release_approval(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = self.run_consumer(Path(temporary))
        self.assertEqual(self.approval_calls, [{"repository": "owner/repo", "run_id": 456, "run_attempt": 1,
                                                "source_commit": self.sha}])
        self.assertEqual(result["releaseApproval"]["record"], self.approval_record)
        self.assertEqual(result["releaseApproval"]["sha256"],
                         hashlib.sha256(canonical_bytes(self.approval_record)).hexdigest())
        self.assertIn("protected-release-approval", result["checks"])

    def test_missing_mismatched_or_future_approval_never_emits_receipt(self):
        other = {**self.approval_record, "sourceCommit": "c" * 40}
        faults = {"missing-digest": {"digest": ""}, "malformed-digest": {"digest": "Z" * 64},
                  "different-record": {"record": other}, "wrong-digest": {"digest": "d" * 64},
                  "future-attempt": {"attempt": 3}, "zero-attempt": {"attempt": 0}}
        for fault, arguments in faults.items():
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                approval = self.approval_arguments(**arguments)
                if fault == "missing-digest":
                    approval["approval_record_sha256"] = ""
                api = Mock()
                api.json.side_effect = AssertionError("no release metadata may be read before approval")
                with self.assertRaises(ValueError):
                    verify(repository="owner/repo", tag=self.tag, source_commit=self.sha,
                           directory=root / "download", signature_directory=root / "signatures",
                           fingerprint="b" * 64, gate_output=root / "gate.txt", receipt=root / "receipt.json",
                           run_id=456, run_attempt=2, api=api, bundle_verifier=Mock(),
                           provenance_verifier=Mock(), **approval)
                api.download.assert_not_called()
                self.assertFalse((root / "receipt.json").exists())

    def test_collector_failure_never_emits_receipt(self):
        def refuse(**_identity):
            raise ValueError("the stable-release deployment review was rejected")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            approval = {**self.approval_arguments(), "approval_collector": refuse}
            with self.assertRaisesRegex(ValueError, "rejected"):
                verify(repository="owner/repo", tag=self.tag, source_commit=self.sha,
                       directory=root / "download", signature_directory=root / "signatures",
                       fingerprint="b" * 64, gate_output=root / "gate.txt", receipt=root / "receipt.json",
                       run_id=456, run_attempt=2, api=Mock(), bundle_verifier=Mock(),
                       provenance_verifier=Mock(), **approval)
            self.assertFalse((root / "receipt.json").exists())

    def test_tamper_signature_failure_or_publication_drift_never_emits_receipt(self):
        for fault in ("tamper", "signature_failure", "drift"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                with self.assertRaises(ValueError):
                    self.run_consumer(root, **{fault: True})
                self.assertFalse((root / "receipt.json").exists())

    def test_fresh_consumer_downloads_and_checks_signature_control_asset(self):
        control_name = "SparkEngine-release-signature-bundle.tar.gz"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            control = root / "control.tar.gz"
            with tarfile.open(control, "w:gz") as archive:
                manifest = root / "release-signatures.json"
                public_key = root / "spark-release-public-key.pem"
                manifest.write_text("{}\n", encoding="utf-8")
                public_key.write_text("test\n", encoding="utf-8")
                archive.add(manifest, arcname="release-signatures.json")
                archive.add(public_key, arcname="spark-release-public-key.pem")
            control_bytes = control.read_bytes()
            control_asset = {"id": 99, "name": control_name, "size": len(control_bytes),
                             "state": "uploaded", "digest": "sha256:" + hashlib.sha256(control_bytes).hexdigest(),
                             "uploader": {"id": 41898282, "login": "github-actions[bot]"}}
            assets = self.assets + [control_asset]
            tag_record = {"ref": "refs/tags/" + self.tag, "object": {"type": "commit", "sha": self.sha}}
            api = Mock()
            api.json.side_effect = [self.release, tag_record, assets,
                                    {**self.release}, assets, tag_record]
            by_id = {entry["id"]: entry["name"] for entry in assets}
            def download(asset_id, path):
                if asset_id == 99:
                    path.write_bytes(control_bytes)
                else:
                    path.write_bytes(self.payloads[by_id[asset_id]])
            api.download.side_effect = download
            bundle = Mock()
            provenance = Mock()
            with patch("verify_published_stable_release.values_from_gate_output", return_value={"source": self.sha}):
                result = verify(repository="owner/repo", tag=self.tag, source_commit=self.sha,
                                directory=root / "download", signature_directory=root / "signatures",
                                fingerprint="b" * 64, gate_output=root / "gate.txt", receipt=root / "receipt.json",
                                run_id=456, run_attempt=2, api=api, bundle_verifier=bundle,
                                provenance_verifier=provenance, signature_control_asset=control_name,
                                **self.approval_arguments())
            self.assertEqual(result["state"], "publication-verified")
            self.assertEqual(api.download.call_count, 8)
            bundle.assert_called_once()
            self.assertTrue((root / "signatures" / "release-signatures.json").is_file())
            bad = copy.deepcopy(assets)
            bad[-1]["digest"] = "not-a-digest"
            with self.assertRaises(ValueError):
                asset_identity(bad, self.tag, signature_control_asset=control_name)
            with self.assertRaises(ValueError):
                asset_identity(self.assets, self.tag, signature_control_asset=control_name)

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

    def test_directory_sync_fails_closed_on_unsupported_filesystems_or_io_errors(self):
        for code in (errno.EINVAL, errno.ENOTSUP, errno.EIO):
            with self.subTest(code=code), patch.object(os, "fsync", side_effect=OSError(code, "sync failure")):
                with self.assertRaises(OSError):
                    receipt_publication._sync_directory(0)

    @unittest.skipUnless(os.name == "posix", "directory fsync is exercised by the POSIX publisher")
    def test_directory_sync_preflight_failure_never_publishes(self):
        sync = os.fsync
        def fail(descriptor):
            if stat.S_ISDIR(os.fstat(descriptor).st_mode):
                raise OSError(errno.EIO, "injected directory sync failure")
            return sync(descriptor)
        with patch.object(os, "fsync", side_effect=fail), patch.object(os, "link") as link:
            with self.assertRaises(ReceiptPublicationError):
                publish_receipt_no_replace(self.receipt, self.payload)
            link.assert_not_called()
        self.assert_clean()

    @unittest.skipUnless(os.name == "posix", "Linux directory durability boundary")
    def test_post_link_sync_failure_preserves_visible_file_and_reports_uncertainty(self):
        real_link, real_sync = os.link, os.fsync
        linked = False
        def link(*args, **kwargs):
            nonlocal linked
            real_link(*args, **kwargs)
            linked = True
        def sync(descriptor):
            if linked and stat.S_ISDIR(os.fstat(descriptor).st_mode):
                raise OSError(errno.EIO, "injected post-link sync failure")
            return real_sync(descriptor)
        with patch.object(os, "link", side_effect=link), patch.object(os, "fsync", side_effect=sync):
            with patch.object(os, "unlink", side_effect=AssertionError("must not unlink visible names")):
                with self.assertRaisesRegex(ReceiptPublicationError, "may already be visible"):
                    publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(self.receipt.read_bytes(), self.payload)
        self.assert_clean(["receipt.json"])

    @unittest.skipUnless(os.name == "posix", "Linux post-sync identity validation")
    def test_competitor_swapped_during_directory_sync_is_not_deleted(self):
        real_link, real_sync = os.link, os.fsync
        linked = False
        def link(*args, **kwargs):
            nonlocal linked
            real_link(*args, **kwargs)
            linked = True
        def sync(descriptor):
            result = real_sync(descriptor)
            if linked and stat.S_ISDIR(os.fstat(descriptor).st_mode):
                self.receipt.unlink()
                self.receipt.write_bytes(b"competing receipt")
            return result
        with patch.object(os, "link", side_effect=link), patch.object(os, "fsync", side_effect=sync):
            with self.assertRaisesRegex(ReceiptPublicationError, "may already be visible"):
                publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(self.receipt.read_bytes(), b"competing receipt")
        self.assert_clean(["receipt.json"])

    @unittest.skipUnless(os.name == "posix", "Linux anonymous inode publication")
    def test_anonymous_inode_is_synced_before_link_then_directory_is_synced(self):
        real_link, real_sync = os.link, os.fsync
        events, descriptors = [], []
        def sync(descriptor):
            events.append("directory-sync" if stat.S_ISDIR(os.fstat(descriptor).st_mode) else "file-sync")
            return real_sync(descriptor)
        def link(source, target, **kwargs):
            self.assertTrue(str(source).startswith("/proc/self/fd/"))
            descriptor = int(str(source).rsplit("/", 1)[1])
            descriptors.append(descriptor)
            self.assertEqual(os.fstat(descriptor).st_nlink, 0)
            self.assertEqual(stat.S_IMODE(os.fstat(descriptor).st_mode), 0o600)
            self.assertEqual(os.read(descriptor, 1), b"")
            self.assertEqual(list(self.root.iterdir()), [])
            self.assertTrue(kwargs["follow_symlinks"])
            events.append("link")
            result = real_link(source, target, **kwargs)
            self.assertEqual(os.fstat(descriptor).st_nlink, 1)
            return result
        with patch.object(os, "fsync", side_effect=sync), patch.object(os, "link", side_effect=link):
            with patch.object(os, "unlink", side_effect=AssertionError("anonymous staging has no cleanup name")):
                publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(events, ["directory-sync", "file-sync", "link", "directory-sync"])
        with self.assertRaises(OSError):
            os.fstat(descriptors[0])

    @unittest.skipUnless(os.name == "posix", "Linux anonymous staging cleanup")
    def test_failed_link_closes_the_unnamed_inode_without_unlink(self):
        descriptors = []
        def fail(source, target, **kwargs):
            self.assertEqual(list(self.root.iterdir()), [])
            descriptor = int(str(source).rsplit("/", 1)[1])
            descriptors.append(descriptor)
            self.assertEqual(os.fstat(descriptor).st_nlink, 0)
            raise OSError(errno.EIO, "injected link failure")
        with patch.object(os, "link", side_effect=fail):
            with patch.object(os, "unlink", side_effect=AssertionError("anonymous staging has no cleanup name")):
                with self.assertRaises(ReceiptPublicationError):
                    publish_receipt_no_replace(self.receipt, self.payload)
        with self.assertRaises(OSError):
            os.fstat(descriptors[0])
        self.assert_clean()

    @unittest.skipUnless(os.name == "posix", "Linux ownership race regression")
    def test_success_cleanup_cannot_delete_a_replaced_legacy_temp_name(self):
        # The old writer unlinked its staging pathname after linking. Swap that
        # name after the link: its unconditional success cleanup deleted the rival.
        real_link = os.link
        competitors = []
        def swap(source, target, **kwargs):
            result = real_link(source, target, **kwargs)
            source_path = Path(source)
            legacy_named_stage = not source_path.is_absolute()
            rival = self.root / (source_path.name if legacy_named_stage else ".receipt.json.rival.tmp")
            if legacy_named_stage:
                rival.unlink()
            rival.write_bytes(b"competing temporary")
            competitors.append(rival)
            return result
        with patch.object(os, "link", side_effect=swap):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(competitors[0].read_bytes(), b"competing temporary")
        self.assertEqual(self.receipt.read_bytes(), self.payload)
        self.assert_clean(["receipt.json", competitors[0].name])

    @unittest.skipUnless(os.name == "posix", "Linux ownership race regression")
    def test_failure_cleanup_cannot_delete_a_temp_swapped_after_its_stat(self):
        # Reproduce the former _unlink_owned stat-then-unlink race precisely.
        # Anonymous staging has no such stat/cleanup window, even when a rival
        # independently creates an old-style temp name during the failed link.
        real_stat = os.stat
        failed, legacy_name = False, None
        rival = self.root / ".receipt.json.rival.tmp"
        def fail(source, target, **kwargs):
            nonlocal failed, legacy_name, rival
            failed = True
            if not Path(source).is_absolute():
                legacy_name = str(source)
                rival = self.root / legacy_name
            else:
                rival.write_bytes(b"competing temporary")
            raise OSError(errno.EIO, "injected link failure")
        def swap_after_stat(path, *args, **kwargs):
            information = real_stat(path, *args, **kwargs)
            if failed and legacy_name is not None and str(path) == legacy_name:
                rival.unlink()
                rival.write_bytes(b"competing temporary")
            return information
        with patch.object(os, "link", side_effect=fail), patch.object(os, "stat", side_effect=swap_after_stat):
            with self.assertRaises(ReceiptPublicationError):
                publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(rival.read_bytes(), b"competing temporary")
        self.assert_clean([rival.name])

    @unittest.skipUnless(os.name == "posix", "Linux post-link ownership boundary")
    def test_replaced_published_name_is_preserved_on_verification_failure(self):
        real_link = os.link
        def swap(source, target, **kwargs):
            result = real_link(source, target, **kwargs)
            self.receipt.unlink()
            self.receipt.write_bytes(b"competing receipt")
            return result
        with patch.object(os, "link", side_effect=swap), self.assertRaises(ReceiptPublicationError):
            publish_receipt_no_replace(self.receipt, self.payload)
        self.assertEqual(self.receipt.read_bytes(), b"competing receipt")
        self.assert_clean(["receipt.json"])

    @unittest.skipUnless(os.name == "posix", "Linux O_TMPFILE capability boundary")
    def test_unsupported_anonymous_staging_fails_without_named_fallback(self):
        real_open = os.open
        def unsupported(path, flags, *args, **kwargs):
            if flags & os.O_TMPFILE == os.O_TMPFILE:
                raise OSError(errno.EOPNOTSUPP, "anonymous staging unavailable")
            return real_open(path, flags, *args, **kwargs)
        with patch.object(os, "open", side_effect=unsupported), patch.object(os, "link") as link:
            with self.assertRaises(ReceiptPublicationError):
                publish_receipt_no_replace(self.receipt, self.payload)
            link.assert_not_called()
        self.assert_clean()

    @unittest.skipUnless(os.name == "posix", "Linux proc fd source validation")
    def test_missing_or_mismatched_proc_fd_source_fails_before_link(self):
        real_stat = os.stat
        for kind in ("missing", "identity", "linked", "nonregular"):
            def invalid(path, *args, **kwargs):
                information = real_stat(path, *args, **kwargs)
                if str(path).startswith("/proc/self/fd/"):
                    if kind == "missing":
                        raise FileNotFoundError(errno.ENOENT, "proc fd source unavailable")
                    return SimpleNamespace(st_dev=information.st_dev,
                                           st_ino=information.st_ino + (kind == "identity"),
                                           st_nlink=1 if kind == "linked" else 0,
                                           st_mode=stat.S_IFDIR if kind == "nonregular" else information.st_mode)
                return information
            with self.subTest(kind=kind), patch.object(os, "stat", side_effect=invalid):
                with patch.object(os, "link") as link, self.assertRaises(ReceiptPublicationError):
                    publish_receipt_no_replace(self.receipt, self.payload)
                link.assert_not_called()
            self.assert_clean()

    @unittest.skipUnless(os.name == "posix", "Only the Linux POSIX backend is supported")
    def test_non_linux_posix_fails_closed(self):
        with patch("sys.platform", "darwin"), patch.object(os, "link") as link:
            with self.assertRaisesRegex(ReceiptPublicationError, "Linux"):
                publish_receipt_no_replace(self.receipt, self.payload)
            link.assert_not_called()
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
        self.assertEqual((moved / "receipt.json").read_bytes(), self.payload)
        self.assertEqual({path.name for path in moved.iterdir()}, {"receipt.json"})


if __name__ == "__main__":
    unittest.main()
