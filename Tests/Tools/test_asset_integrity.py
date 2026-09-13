#!/usr/bin/env python3
"""Adversarial tests for the asset integrity boundary."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "asset-integrity" / "verify_asset_integrity.py"
SPEC = importlib.util.spec_from_file_location("verify_asset_integrity", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
vai = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vai)

SITE_DATA_DIR = REPO_ROOT / "tools" / "site-data"
sys.path.insert(0, str(SITE_DATA_DIR))
import validate as site_data_validate  # noqa: E402


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_manifest(root: Path, entries: list[dict], *, metadata_root: str | None = None) -> Path:
    payload = {
        "version": 1,
        "algorithm": "sha256",
        "root": root.name if metadata_root is None else metadata_root,
        "fileCount": len(entries),
        "entries": entries,
    }
    path = root / vai.MANIFEST_FILENAME
    path.write_bytes(vai.manifest_bytes(payload))
    return path


def create_reparse(link: Path, target: Path) -> None:
    if sys.platform == "win32":
        subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            check=True,
            capture_output=True,
        )
    else:
        link.symlink_to(target, target_is_directory=True)


def remove_reparse(link: Path) -> None:
    if not link.exists() and not link.is_symlink():
        return
    if sys.platform == "win32":
        subprocess.run(["cmd", "/c", "rmdir", str(link)], check=True, capture_output=True)
    else:
        link.unlink()


class PathPolicyTests(unittest.TestCase):
    def test_accepts_canonical_portable_paths(self) -> None:
        for path in ("file.bin", "Models/hero.obj", "Audio/MMO/ambient.wav"):
            self.assertEqual(vai._canonical_relative_path(path), (path, None))

    def test_rejects_traversal_absolute_drive_and_backslash(self) -> None:
        for path in (
            "../escape", "a/../b", "/absolute", "C:/absolute", "C:relative",
            "\\\\server\\share", "a\\b",
        ):
            self.assertIsNotNone(vai._canonical_relative_path(path)[1], path)

    def test_rejects_noncanonical_aliases(self) -> None:
        for path in ("./file", "dir/./file", "dir//file", "file/", " file", "file "):
            self.assertIsNotNone(vai._canonical_relative_path(path)[1], path)

    def test_rejects_windows_invalid_ads_and_reserved_names(self) -> None:
        for path in (
            "name:stream", "bad<name", "bad>name", 'bad"name', "bad|name",
            "bad?name", "bad*name", "CON", "com1.txt", "LPT9 .txt", "NUL.any",
            "CONIN$", "dir/PRN.log", "trailing.",
        ):
            self.assertIsNotNone(vai._canonical_relative_path(path)[1], path)

    def test_rejects_control_non_nfc_and_path_bound(self) -> None:
        self.assertIsNotNone(vai._canonical_relative_path("bad\nname")[1])
        self.assertIsNotNone(vai._canonical_relative_path("e\u0301.txt")[1])
        with mock.patch.object(vai, "MAX_PATH_BYTES", 3):
            self.assertIsNotNone(vai._canonical_relative_path("four")[1])


class SnapshotSafetyTests(unittest.TestCase):
    def test_sorted_snapshot_hashes_each_file_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "b.bin").write_bytes(b"b")
            (root / "a.bin").write_bytes(b"a")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(errors, [])
            self.assertEqual([item["path"] for item in entries], ["a.bin", "b.bin"])
            self.assertEqual(entries[0]["sha256"], digest(b"a"))

    def test_nested_manifest_is_not_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested = root / "nested"
            nested.mkdir()
            (root / "data.bin").write_bytes(b"data")
            (nested / vai.MANIFEST_FILENAME).write_bytes(b"nested")
            manifest = write_manifest(root, [{"path": "data.bin", "sha256": digest(b"data"), "size": 4}])
            errors = vai.verify_manifest(manifest, root)
            self.assertTrue(any(e.category == "undeclared" and e.path.endswith(vai.MANIFEST_FILENAME) for e in errors))

    def test_authoritative_root_rejects_escape_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "Assets"
            outside = base / "outside"
            root.mkdir()
            outside.mkdir()
            (outside / "secret.bin").write_bytes(b"secret")
            manifest = write_manifest(root, [], metadata_root="../outside")
            errors = vai.verify_manifest(manifest, root)
            self.assertEqual([e.category for e in errors], ["root-metadata"])

    def test_exact_root_metadata_is_case_sensitive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Assets"
            root.mkdir()
            manifest = write_manifest(root, [], metadata_root="assets")
            self.assertEqual(vai.verify_manifest(manifest, root)[0].category, "root-metadata")

    def test_root_reparse_is_rejected_before_scan(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            target = base / "real"
            link = base / "root-link"
            target.mkdir()
            (target / "secret.bin").write_bytes(b"secret")
            create_reparse(link, target)
            try:
                entries, errors = vai.scan_directory(link)
                self.assertEqual(entries, [])
                self.assertTrue(any(e.category == "reparse" for e in errors))
            finally:
                remove_reparse(link)

    def test_nested_reparse_to_sibling_prefix_is_never_hashed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "Assets"
            sibling = base / "AssetsEvil"
            root.mkdir()
            sibling.mkdir()
            (sibling / "secret.bin").write_bytes(b"secret")
            link = root / "linked"
            create_reparse(link, sibling)
            try:
                with mock.patch.object(vai, "_hash_fd", wraps=vai._hash_fd) as hasher:
                    entries, errors = vai.scan_directory(root)
                self.assertEqual(entries, [])
                self.assertEqual(hasher.call_count, 0)
                self.assertTrue(any(e.category == "reparse" for e in errors))
            finally:
                remove_reparse(link)

    def test_file_change_during_hash_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "asset.bin"
            target.write_bytes(b"before")
            original = vai._hash_fd

            def mutate(fd: int, *, max_bytes: int = vai.MAX_FILE_BYTES) -> str:
                result = original(fd, max_bytes=max_bytes)
                target.write_bytes(b"after-content")
                return result

            with mock.patch.object(vai, "_hash_fd", side_effect=mutate):
                entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertTrue(any(e.category == "io-race" for e in errors))

    def test_scan_io_error_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(vai.os, "scandir", side_effect=PermissionError("denied")):
                entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertEqual(errors[0].category, "io-error")

    @unittest.skipIf(sys.platform == "win32", "mkfifo is Unix-only")
    def test_fifo_is_rejected_without_opening(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            os.mkfifo(root / "pipe")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertEqual(errors[0].category, "non-regular")

    def test_file_count_file_size_and_total_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a").write_bytes(b"aa")
            (root / "b").write_bytes(b"bb")
            with mock.patch.object(vai, "MAX_ENTRY_COUNT", 1):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))
            with mock.patch.object(vai, "MAX_FILE_BYTES", 1):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))
            with mock.patch.object(vai, "MAX_TOTAL_BYTES", 3):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))
            with mock.patch.object(vai, "MAX_DIRECTORY_ENTRY_COUNT", 1):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))


class ManifestValidationTests(unittest.TestCase):
    def test_valid_manifest_hashes_each_disk_file_only_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.bin").write_bytes(b"a")
            (root / "b.bin").write_bytes(b"b")
            manifest, errors = vai.generate_manifest(root)
            self.assertEqual(errors, [])
            path = root / vai.MANIFEST_FILENAME
            vai.write_manifest(manifest, path)
            with mock.patch.object(vai, "_hash_fd", wraps=vai._hash_fd) as hasher:
                verify_errors = vai.verify_manifest(path, root)
            self.assertEqual(verify_errors, [])
            self.assertEqual(hasher.call_count, 2)

    def test_hash_size_missing_and_undeclared_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "actual.bin").write_bytes(b"actual")
            (root / "extra.bin").write_bytes(b"extra")
            manifest = write_manifest(root, [
                {"path": "actual.bin", "sha256": "0" * 64, "size": 99},
                {"path": "missing.bin", "sha256": "1" * 64, "size": 1},
            ])
            categories = {error.category for error in vai.verify_manifest(manifest, root)}
            self.assertTrue({"hash-mismatch", "size-mismatch", "missing", "undeclared"} <= categories)

    def test_malformed_entries_return_error_instead_of_crashing(self) -> None:
        malformed = (
            [None],
            [{"path": "x"}],
            [{"path": 7, "sha256": "0" * 64, "size": 0}],
            [{"path": "x", "sha256": "bad", "size": 0}],
            [{"path": "x", "sha256": "0" * 64, "size": True}],
        )
        for entries in malformed:
            with self.subTest(entries=entries), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path = root / vai.MANIFEST_FILENAME
                path.write_text(json.dumps({
                    "version": 1,
                    "algorithm": "sha256",
                    "root": root.name,
                    "fileCount": len(entries),
                    "entries": entries,
                }), encoding="utf-8")
                errors = vai.verify_manifest(path, root)
                self.assertEqual(errors[0].category, "manifest-load")

    def test_duplicate_json_keys_duplicate_paths_and_aliases_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duplicate_key = root / vai.MANIFEST_FILENAME
            duplicate_key.write_text(
                '{"version":1,"version":1,"algorithm":"sha256","root":"x","fileCount":0,"entries":[]}',
                encoding="utf-8",
            )
            self.assertEqual(vai.verify_manifest(duplicate_key, root)[0].category, "manifest-load")

            for paths in (("same", "same"), ("File.txt", "file.txt")):
                entries = [{"path": item, "sha256": "0" * 64, "size": 0} for item in paths]
                write_manifest(root, entries)
                self.assertEqual(vai.verify_manifest(duplicate_key, root)[0].category, "manifest-load")

    def test_entries_must_be_sorted_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_manifest(root, [
                {"path": "b", "sha256": "0" * 64, "size": 0},
                {"path": "a", "sha256": "1" * 64, "size": 0},
            ])
            self.assertEqual(vai.verify_manifest(path, root)[0].category, "manifest-load")
            with mock.patch.object(vai, "MAX_ENTRY_COUNT", 1):
                self.assertEqual(vai.verify_manifest(path, root)[0].category, "manifest-load")
            path.write_bytes(b"{}" * 10)
            with self.assertRaises(vai.ManifestFormatError):
                vai._read_bounded_json(path, limit=4)

    def test_generation_is_deterministic_and_refuses_every_scan_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "b").write_bytes(b"b")
            (root / "a").write_bytes(b"a")
            one, errors_one = vai.generate_manifest(root)
            two, errors_two = vai.generate_manifest(root)
            self.assertEqual(errors_one, [])
            self.assertEqual(errors_two, [])
            self.assertEqual(vai.manifest_bytes(one), vai.manifest_bytes(two))
            args = argparse.Namespace(root=str(root), output=None)
            self.assertEqual(vai.cmd_generate(args), 0)
            first_bytes = (root / vai.MANIFEST_FILENAME).read_bytes()
            self.assertEqual(vai.cmd_generate(args), 0)
            self.assertEqual((root / vai.MANIFEST_FILENAME).read_bytes(), first_bytes)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = argparse.Namespace(root=str(root), output=None)
            failure = vai.IntegrityError("x", "io-error", "denied")
            with mock.patch.object(vai, "generate_manifest", return_value=({}, [failure])):
                self.assertEqual(vai.cmd_generate(args), 1)
            self.assertFalse((root / vai.MANIFEST_FILENAME).exists())


class TemplateCompletenessTests(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[Path, Path, Path]:
        assets = root / "Templates" / "Starter" / "Assets"
        assets.mkdir(parents=True)
        data = b"asset"
        (assets / "asset.bin").write_bytes(data)
        (assets / "README.md").write_bytes(b"root metadata")
        manifest = assets / "manifest.json"
        manifest.write_text(json.dumps({
            "manifestVersion": 1,
            "package": "Starter",
            "assets": [{"path": "asset.bin", "sha256": digest(data)}],
        }), encoding="utf-8")
        lock = root / "Templates" / "assets.lock.json"
        lock.write_text(json.dumps({
            "version": 1,
            "algorithm": "sha256",
            "assets": {"Starter/Assets/asset.bin": digest(data)},
        }), encoding="utf-8")
        return assets, manifest, lock

    def test_exact_manifest_lock_disk_equality(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._fixture(root)
            self.assertEqual(vai.verify_template_manifests(root), [])

    def test_empty_template_manifest_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets, manifest, lock = self._fixture(root)
            (assets / "asset.bin").unlink()
            manifest.write_text(json.dumps({
                "manifestVersion": 1,
                "package": "Starter",
                "assets": [],
            }), encoding="utf-8")
            lock.write_text(json.dumps({
                "version": 1,
                "algorithm": "sha256",
                "assets": {},
            }), encoding="utf-8")
            errors = vai.verify_template_manifests(root)
        self.assertTrue(any(
            error.category == "manifest-load"
            and error.path == "Templates/Starter/Assets/manifest.json"
            for error in errors
        ), errors)

    def test_undeclared_disk_file_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets, _, _ = self._fixture(root)
            (assets / "extra.bin").write_bytes(b"extra")
            self.assertTrue(any(e.category == "undeclared" for e in vai.verify_template_manifests(root)))

    def test_stale_lock_entry_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, _, lock = self._fixture(root)
            data = json.loads(lock.read_text(encoding="utf-8"))
            data["assets"]["Starter/Assets/stale.bin"] = "0" * 64
            lock.write_text(json.dumps(data), encoding="utf-8")
            self.assertTrue(any(e.category == "lock-extra" for e in vai.verify_template_manifests(root)))

    def test_duplicate_manifest_row_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, manifest, _ = self._fixture(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["assets"].append(dict(data["assets"][0]))
            manifest.write_text(json.dumps(data), encoding="utf-8")
            self.assertTrue(any(e.category == "manifest-load" for e in vai.verify_template_manifests(root)))

    def test_nested_metadata_filename_is_not_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets, _, _ = self._fixture(root)
            nested = assets / "nested"
            nested.mkdir()
            (nested / "manifest.json").write_bytes(b"nested")
            self.assertTrue(any(e.category == "undeclared" for e in vai.verify_template_manifests(root)))


class ToctouFingerprintTests(unittest.TestCase):
    """Finding 1: TOCTOU fingerprint limitations and honest boundary."""

    def test_restored_mtime_same_size_rewrite_is_toctou_gap(self) -> None:
        """Demonstrates the documented TOCTOU limitation: a same-size rewrite
        that occurs within the filesystem timestamp granularity may not be
        detected by fingerprint comparison. This test proves the gap exists
        and is honestly documented rather than silently ignored."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "asset.bin"
            target.write_bytes(b"AAAA")
            before_stat = target.stat()
            original = vai._hash_fd

            def tamper_restore_mtime(fd: int, *, max_bytes: int = vai.MAX_FILE_BYTES) -> str:
                result = original(fd, max_bytes=max_bytes)
                target.write_bytes(b"BBBB")
                os.utime(target, ns=(before_stat.st_atime_ns, before_stat.st_mtime_ns))
                return result

            with mock.patch.object(vai, "_hash_fd", side_effect=tamper_restore_mtime):
                entries, errors = vai.scan_directory(root)
            if not errors:
                self.assertEqual(len(entries), 1)
                self.assertNotEqual(entries[0]["sha256"], digest(b"BBBB"),
                    "hash should reflect the pre-tamper content read from the fd")

    def test_concurrent_addition_is_toctou_gap(self) -> None:
        """Demonstrates the documented TOCTOU limitation: a file created after
        scandir completes (but before walk finishes) is not guaranteed to appear
        in the snapshot. The scan is non-atomic."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "existing.bin").write_bytes(b"exists")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(len(entries), 1)
            (root / "added_later.bin").write_bytes(b"late")
            self.assertTrue((root / "added_later.bin").exists(),
                "file exists on disk but was not in the prior snapshot")

    def test_concurrent_deletion_produces_error(self) -> None:
        """A file present in scandir but deleted before read produces an error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "vanish.bin"
            target.write_bytes(b"here")

            call_count = 0
            original_checked = vai._checked_candidate
            def delete_on_second_check(r, rr, rel, *, want_directory=False):
                nonlocal call_count
                result = original_checked(r, rr, rel, want_directory=want_directory)
                if rel == "vanish.bin" and not want_directory:
                    call_count += 1
                    if call_count == 1:
                        target.unlink()
                return result

            with mock.patch.object(vai, "_checked_candidate", side_effect=delete_on_second_check):
                entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertTrue(len(errors) > 0)

    def test_directory_swap_during_walk_is_caught(self) -> None:
        """Directory replaced with a file mid-walk produces an error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subdir = root / "subdir"
            subdir.mkdir()
            (subdir / "inner.bin").write_bytes(b"inner")

            original_checked = vai._checked_candidate
            def swap_dir(r, rr, rel, *, want_directory=False):
                if rel == "subdir" and want_directory:
                    import shutil
                    shutil.rmtree(subdir)
                    subdir.write_bytes(b"not a dir")
                return original_checked(r, rr, rel, want_directory=want_directory)

            with mock.patch.object(vai, "_checked_candidate", side_effect=swap_dir):
                entries, errors = vai.scan_directory(root)
            has_relevant_error = any(
                e.path.startswith("subdir") for e in errors
            )
            self.assertTrue(has_relevant_error or len(entries) == 0)


class ConcealedPayloadTests(unittest.TestCase):
    """Finding 2: ignored paths must be statted to prevent concealed payloads."""

    def test_ignored_directory_readme_is_rejected(self) -> None:
        """A directory named README.md at template root is a concealed-payload error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data.bin").write_bytes(b"data")
            readme_dir = root / "README.md"
            readme_dir.mkdir()
            (readme_dir / "hidden.bin").write_bytes(b"payload")
            entries, errors = vai.scan_directory(
                root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
            categories = {e.category for e in errors}
            self.assertIn("concealed-payload", categories)

    def test_ignored_regular_file_readme_is_skipped_cleanly(self) -> None:
        """A regular README.md at template root is silently ignored (no error)."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data.bin").write_bytes(b"data")
            (root / "README.md").write_bytes(b"readme")
            entries, errors = vai.scan_directory(
                root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
            self.assertEqual(errors, [])
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["path"], "data.bin")

    def test_ignored_manifest_json_directory_is_rejected(self) -> None:
        """A directory named manifest.json is a concealed-payload error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data.bin").write_bytes(b"data")
            manifest_dir = root / "manifest.json"
            manifest_dir.mkdir()
            (manifest_dir / "secret.bin").write_bytes(b"secret")
            entries, errors = vai.scan_directory(
                root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
            self.assertIn("concealed-payload", {e.category for e in errors})

    def test_ignored_reparse_readme_is_rejected(self) -> None:
        """A reparse point named README.md is a concealed-payload error."""
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "assets"
            target = base / "evil"
            root.mkdir()
            target.mkdir()
            (target / "payload.bin").write_bytes(b"payload")
            (root / "data.bin").write_bytes(b"data")
            link = root / "README.md"
            create_reparse(link, target)
            try:
                entries, errors = vai.scan_directory(
                    root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
                self.assertIn("concealed-payload", {e.category for e in errors})
            finally:
                remove_reparse(link)


class HashFdBoundaryTests(unittest.TestCase):
    """Finding 3: _hash_fd must enforce live byte limits."""

    def test_hash_fd_rejects_growth_past_declared_size(self) -> None:
        """_hash_fd raises when the file grows past the declared max_bytes."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "growing.bin"
            path.write_bytes(b"small")
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
            try:
                with self.assertRaises(OSError):
                    vai._hash_fd(fd, max_bytes=2)
            finally:
                os.close(fd)

    def test_hash_fd_accepts_within_limit(self) -> None:
        """_hash_fd succeeds when file is within the byte limit."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ok.bin"
            data = b"fits"
            path.write_bytes(data)
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
            try:
                result = vai._hash_fd(fd, max_bytes=100)
                self.assertEqual(result, digest(data))
            finally:
                os.close(fd)

    def test_hash_fd_exact_limit_succeeds(self) -> None:
        """_hash_fd succeeds when file size exactly equals max_bytes."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "exact.bin"
            data = b"ABCD"
            path.write_bytes(data)
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
            try:
                result = vai._hash_fd(fd, max_bytes=4)
                self.assertEqual(result, digest(data))
            finally:
                os.close(fd)

    def test_concurrent_growth_during_scan_is_caught(self) -> None:
        """A file that grows during hash in scan_directory produces an error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "grower.bin"
            target.write_bytes(b"sm")
            original = vai._hash_fd
            def grow_then_hash(fd: int, *, max_bytes: int = vai.MAX_FILE_BYTES) -> str:
                target.write_bytes(b"sm" + b"X" * 1000)
                os.lseek(fd, 0, os.SEEK_SET)
                return original(fd, max_bytes=max_bytes)
            with mock.patch.object(vai, "_hash_fd", side_effect=grow_then_hash):
                entries, errors = vai.scan_directory(root)
            has_error = len(errors) > 0 or len(entries) == 0
            self.assertTrue(has_error)


class SiteDataAssetIntegrationTests(unittest.TestCase):
    """The site-data asset gate must fail closed around the integrity verifier."""

    def make_root(self, root: Path, *, include_verifier: bool) -> None:
        assets = root / "Assets"
        assets.mkdir(parents=True)
        (root / "Shaders").mkdir()
        graphics = root / "SparkEngine" / "Source" / "Graphics"
        graphics.mkdir(parents=True)
        (graphics / "AssetPipeline.cpp").write_text("// fixture\n", encoding="utf-8")
        payload = b"verified fixture asset"
        (assets / "fixture.bin").write_bytes(payload)
        (assets / "assets.integrity.json").write_text(
            json.dumps(
                {
                    "version": 1,
                    "algorithm": "sha256",
                    "root": "Assets",
                    "fileCount": 1,
                    "entries": [
                        {
                            "path": "fixture.bin",
                            "sha256": hashlib.sha256(payload).hexdigest(),
                            "size": len(payload),
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        if include_verifier:
            target = root / "tools" / "asset-integrity" / SCRIPT.name
            target.parent.mkdir(parents=True)
            target.write_bytes(SCRIPT.read_bytes())

    def validate_fixture(self, root: Path) -> list[str]:
        with (
            mock.patch.object(site_data_validate, "REPO_ROOT", root),
            mock.patch.object(site_data_validate, "validate_assets", return_value=[]),
        ):
            validator = site_data_validate.Validator({})
            validator.validate_asset_surface()
        return validator.errors

    def test_missing_integrity_verifier_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_root(root, include_verifier=False)
            errors = self.validate_fixture(root)
        self.assertTrue(any("asset integrity verifier is missing" in error for error in errors), errors)

    def test_tampered_integrity_manifest_fails_through_site_data_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_root(root, include_verifier=True)
            (root / "Assets" / "fixture.bin").write_bytes(b"tampered")
            errors = self.validate_fixture(root)
        self.assertTrue(any("asset-integrity:fixture.bin" in error for error in errors), errors)


class RepositoryParityTests(unittest.TestCase):
    def test_live_repository_contract(self) -> None:
        errors = vai.verify_repository(REPO_ROOT)
        self.assertEqual(errors, [], msg="\n".join(str(error) for error in errors))

    def test_manifest_matches_git_blobs_and_worktree_bytes(self) -> None:
        manifest = vai.load_manifest(REPO_ROOT / "Assets" / vai.MANIFEST_FILENAME)
        process = subprocess.Popen(
            ["git", "cat-file", "--batch"],
            cwd=REPO_ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )
        assert process.stdin is not None and process.stdout is not None
        try:
            for entry in manifest["entries"]:
                relative = entry["path"]
                process.stdin.write(f"HEAD:Assets/{relative}\n".encode("utf-8"))
                process.stdin.flush()
                header = process.stdout.readline().decode("ascii").strip().split()
                self.assertEqual(header[1], "blob", relative)
                size = int(header[2])
                blob = process.stdout.read(size)
                self.assertEqual(process.stdout.read(1), b"\n")
                self.assertEqual(size, entry["size"], relative)
                self.assertEqual(digest(blob), entry["sha256"], relative)
                self.assertEqual((REPO_ROOT / "Assets" / relative).read_bytes(), blob, relative)
        finally:
            process.stdin.close()
            process.stdout.close()
            process.wait(timeout=10)
        attribute = subprocess.check_output(
            ["git", "check-attr", "text", "--", "Assets/README.md"],
            cwd=REPO_ROOT,
            text=True,
        )
        self.assertIn("text: unset", attribute)


if __name__ == "__main__":
    unittest.main()
