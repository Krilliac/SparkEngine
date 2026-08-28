#!/usr/bin/env python3
"""Tests for the asset integrity manifest tool."""

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "asset-integrity" / "verify_asset_integrity.py"
SPEC = importlib.util.spec_from_file_location("verify_asset_integrity", SCRIPT)
assert SPEC and SPEC.loader
vai = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vai)


class PathSafetyTests(unittest.TestCase):
    def test_rejects_empty_path(self) -> None:
        self.assertIsNotNone(vai._is_safe_path(""))

    def test_rejects_absolute_path(self) -> None:
        self.assertIsNotNone(vai._is_safe_path("/etc/passwd"))
        if sys.platform == "win32":
            self.assertIsNotNone(vai._is_safe_path("C:\\Windows\\System32"))

    def test_rejects_traversal(self) -> None:
        self.assertIsNotNone(vai._is_safe_path("foo/../bar"))
        self.assertIsNotNone(vai._is_safe_path("../escape"))

    def test_rejects_control_characters(self) -> None:
        self.assertIsNotNone(vai._is_safe_path("foo\x00bar"))
        self.assertIsNotNone(vai._is_safe_path("foo\nbar"))
        self.assertIsNotNone(vai._is_safe_path("foo\rbar"))

    def test_rejects_backslash(self) -> None:
        self.assertIsNotNone(vai._is_safe_path("foo\\bar"))

    def test_rejects_consecutive_separators(self) -> None:
        self.assertIsNotNone(vai._is_safe_path("foo//bar"))

    def test_rejects_leading_trailing_whitespace(self) -> None:
        self.assertIsNotNone(vai._is_safe_path(" foo"))
        self.assertIsNotNone(vai._is_safe_path("foo "))

    def test_rejects_windows_reserved_names(self) -> None:
        self.assertIsNotNone(vai._is_safe_path("CON"))
        self.assertIsNotNone(vai._is_safe_path("NUL"))
        self.assertIsNotNone(vai._is_safe_path("COM1"))
        self.assertIsNotNone(vai._is_safe_path("LPT1.txt"))
        self.assertIsNotNone(vai._is_safe_path("dir/AUX/file"))

    def test_accepts_valid_paths(self) -> None:
        self.assertIsNone(vai._is_safe_path("foo.txt"))
        self.assertIsNone(vai._is_safe_path("dir/subdir/file.png"))
        self.assertIsNone(vai._is_safe_path("Audio/MMO/ambient.wav"))
        self.assertIsNone(vai._is_safe_path("Models/hero.obj"))


class ScanDirectoryTests(unittest.TestCase):
    def test_empty_directory(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            entries, errors = vai.scan_directory(Path(d))
            self.assertEqual(entries, [])
            self.assertEqual(errors, [])

    def test_single_file(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "test.txt").write_bytes(b"hello")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["path"], "test.txt")
            self.assertEqual(entries[0]["size"], 5)
            self.assertEqual(errors, [])

    def test_nested_files_sorted(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "b.txt").write_bytes(b"b")
            (root / "a.txt").write_bytes(b"a")
            sub = root / "dir"
            sub.mkdir()
            (sub / "c.txt").write_bytes(b"c")
            entries, errors = vai.scan_directory(root)
            paths = [e["path"] for e in entries]
            self.assertEqual(paths, ["a.txt", "b.txt", "dir/c.txt"])
            self.assertEqual(errors, [])

    def test_ignores_manifest_and_gitkeep(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "assets.integrity.json").write_bytes(b"{}")
            (root / ".gitkeep").write_bytes(b"")
            (root / "real.txt").write_bytes(b"data")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["path"], "real.txt")

    def test_hash_correctness(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            content = b"deterministic content for hashing"
            (root / "file.bin").write_bytes(content)
            expected = hashlib.sha256(content).hexdigest()
            entries, errors = vai.scan_directory(root)
            self.assertEqual(entries[0]["sha256"], expected)

    @unittest.skipUnless(
        hasattr(os, "symlink") and sys.platform != "win32",
        "symlinks require Unix or elevated Windows")
    def test_rejects_file_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            target = root / "real.txt"
            target.write_bytes(b"data")
            link = root / "link.txt"
            link.symlink_to(target)
            entries, errors = vai.scan_directory(root)
            self.assertEqual(len(entries), 1)
            symlink_errors = [e for e in errors if e.category == "symlink"]
            self.assertEqual(len(symlink_errors), 1)
            self.assertIn("link.txt", symlink_errors[0].path)

    @unittest.skipUnless(
        hasattr(os, "symlink") and sys.platform != "win32",
        "symlinks require Unix or elevated Windows")
    def test_rejects_directory_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            real_dir = root / "real"
            real_dir.mkdir()
            (real_dir / "file.txt").write_bytes(b"data")
            link_dir = root / "linked"
            link_dir.symlink_to(real_dir)
            entries, errors = vai.scan_directory(root)
            symlink_errors = [e for e in errors if e.category == "symlink"]
            self.assertGreaterEqual(len(symlink_errors), 1)

    @unittest.skipUnless(sys.platform == "win32", "Windows junction test")
    def test_rejects_directory_junction_windows(self) -> None:
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / "scan_root"
            root.mkdir()
            real = root / "real"
            real.mkdir()
            (real / "file.txt").write_bytes(b"data")
            outside = Path(d) / "outside_target"
            outside.mkdir()
            (outside / "secret.txt").write_bytes(b"secret")
            junction = root / "linked"
            subprocess.run(
                ["cmd", "/c", "mklink", "/J", str(junction), str(outside)],
                check=True, capture_output=True)
            try:
                entries, errors = vai.scan_directory(root)
                symlink_errors = [e for e in errors if e.category == "symlink"]
                self.assertGreaterEqual(len(symlink_errors), 1)
                entry_paths = [e["path"] for e in entries]
                self.assertNotIn("linked/secret.txt", entry_paths)
            finally:
                subprocess.run(["cmd", "/c", "rmdir", str(junction)],
                               capture_output=True)

    def test_nonexistent_root(self) -> None:
        entries, errors = vai.scan_directory(Path("/nonexistent/path/abc123"))
        self.assertEqual(entries, [])
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0].category, "missing")


class CaseCollisionTests(unittest.TestCase):
    def test_detects_case_collision_in_scan(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            if sys.platform == "win32":
                (root / "File.txt").write_bytes(b"a")
                entries, _ = vai.scan_directory(root)
                self.assertEqual(len(entries), 1)
                return

            (root / "File.txt").write_bytes(b"a")
            (root / "file.txt").write_bytes(b"b")
            entries, errors = vai.scan_directory(root)
            case_errors = [e for e in errors if e.category == "case-collision"]
            self.assertEqual(len(case_errors), 1)

    def test_detects_case_collision_in_verify(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "file.txt").write_bytes(b"data")
            import hashlib
            sha = hashlib.sha256(b"data").hexdigest()
            manifest = {
                "version": 1,
                "algorithm": "sha256",
                "root": root.name,
                "fileCount": 2,
                "entries": [
                    {"path": "file.txt", "sha256": sha, "size": 4},
                    {"path": "File.txt", "sha256": sha, "size": 4},
                ],
            }
            mpath = root / "assets.integrity.json"
            mpath.write_text(json.dumps(manifest), encoding="utf-8")
            errors = vai.verify_manifest(mpath)
            case_errors = [e for e in errors if e.category == "case-collision"]
            self.assertGreaterEqual(len(case_errors), 1)


class DuplicateDetectionTests(unittest.TestCase):
    def test_detects_duplicate_paths_in_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "file.txt").write_bytes(b"data")
            import hashlib
            sha = hashlib.sha256(b"data").hexdigest()
            manifest = {
                "version": 1,
                "algorithm": "sha256",
                "root": root.name,
                "fileCount": 2,
                "entries": [
                    {"path": "file.txt", "sha256": sha, "size": 4},
                    {"path": "file.txt", "sha256": sha, "size": 4},
                ],
            }
            mpath = root / "assets.integrity.json"
            mpath.write_text(json.dumps(manifest), encoding="utf-8")
            errors = vai.verify_manifest(mpath)
            dup_errors = [e for e in errors if e.category == "duplicate"]
            self.assertEqual(len(dup_errors), 1)


class ManifestVerificationTests(unittest.TestCase):
    def _make_tree(self, root: Path, files: dict[str, bytes]) -> None:
        for rel, content in files.items():
            path = root / rel.replace("/", os.sep)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    def _make_manifest(self, root: Path, entries: list[dict]) -> Path:
        manifest = {
            "version": 1,
            "algorithm": "sha256",
            "root": root.name,
            "fileCount": len(entries),
            "entries": entries,
        }
        mpath = root / "assets.integrity.json"
        mpath.write_text(json.dumps(manifest), encoding="utf-8")
        return mpath

    def test_valid_manifest_passes(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            content = b"test data"
            self._make_tree(root, {"file.txt": content})
            sha = hashlib.sha256(content).hexdigest()
            mpath = self._make_manifest(root, [
                {"path": "file.txt", "sha256": sha, "size": len(content)}
            ])
            errors = vai.verify_manifest(mpath)
            self.assertEqual(errors, [])

    def test_hash_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._make_tree(root, {"file.txt": b"actual"})
            mpath = self._make_manifest(root, [
                {"path": "file.txt", "sha256": "0" * 64, "size": 6}
            ])
            errors = vai.verify_manifest(mpath)
            hash_errors = [e for e in errors if e.category == "hash-mismatch"]
            self.assertEqual(len(hash_errors), 1)

    def test_size_mismatch(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            content = b"data"
            self._make_tree(root, {"file.txt": content})
            sha = hashlib.sha256(content).hexdigest()
            mpath = self._make_manifest(root, [
                {"path": "file.txt", "sha256": sha, "size": 999}
            ])
            errors = vai.verify_manifest(mpath)
            size_errors = [e for e in errors if e.category == "size-mismatch"]
            self.assertEqual(len(size_errors), 1)

    def test_missing_file(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            mpath = self._make_manifest(root, [
                {"path": "ghost.txt", "sha256": "a" * 64, "size": 1}
            ])
            errors = vai.verify_manifest(mpath)
            missing = [e for e in errors if e.category == "missing"]
            self.assertEqual(len(missing), 1)

    def test_undeclared_file(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            content_a = b"a"
            content_b = b"b"
            self._make_tree(root, {"a.txt": content_a, "b.txt": content_b})
            sha_a = hashlib.sha256(content_a).hexdigest()
            mpath = self._make_manifest(root, [
                {"path": "a.txt", "sha256": sha_a, "size": 1}
            ])
            errors = vai.verify_manifest(mpath)
            undeclared = [e for e in errors if e.category == "undeclared"]
            self.assertEqual(len(undeclared), 1)
            self.assertEqual(undeclared[0].path, "b.txt")

    def test_count_mismatch(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            content = b"test"
            self._make_tree(root, {"file.txt": content})
            sha = hashlib.sha256(content).hexdigest()
            manifest = {
                "version": 1,
                "algorithm": "sha256",
                "root": root.name,
                "fileCount": 99,
                "entries": [
                    {"path": "file.txt", "sha256": sha, "size": 4}
                ],
            }
            mpath = root / "assets.integrity.json"
            mpath.write_text(json.dumps(manifest), encoding="utf-8")
            errors = vai.verify_manifest(mpath)
            count_errors = [e for e in errors if e.category == "count-mismatch"]
            self.assertEqual(len(count_errors), 1)

    def test_bad_manifest_json(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            mpath = root / "assets.integrity.json"
            mpath.write_text("not json{{{", encoding="utf-8")
            errors = vai.verify_manifest(mpath)
            self.assertEqual(len(errors), 1)
            self.assertEqual(errors[0].category, "manifest-load")

    def test_wrong_schema_version(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            mpath = root / "assets.integrity.json"
            mpath.write_text(json.dumps({
                "version": 999, "algorithm": "sha256", "entries": []
            }), encoding="utf-8")
            errors = vai.verify_manifest(mpath)
            self.assertEqual(len(errors), 1)
            self.assertIn("version", errors[0].message)

    def test_unsafe_path_in_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            mpath = self._make_manifest(root, [
                {"path": "../escape.txt", "sha256": "a" * 64, "size": 1}
            ])
            errors = vai.verify_manifest(mpath)
            safety_errors = [e for e in errors if e.category == "path-safety"]
            self.assertGreaterEqual(len(safety_errors), 1)


class DeterministicGenerationTests(unittest.TestCase):
    def test_two_generations_are_identical(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "b.txt").write_bytes(b"b content")
            (root / "a.txt").write_bytes(b"a content")
            sub = root / "sub"
            sub.mkdir()
            (sub / "c.txt").write_bytes(b"c content")

            m1, e1 = vai.generate_manifest(root)
            m2, e2 = vai.generate_manifest(root)

            t1 = json.dumps(m1, sort_keys=False)
            t2 = json.dumps(m2, sort_keys=False)
            self.assertEqual(t1, t2)
            self.assertEqual(e1, [])
            self.assertEqual(e2, [])


class TemplateManifestTests(unittest.TestCase):
    def test_cross_validation_with_real_repo(self) -> None:
        repo_root = SCRIPT.parent.parent.parent
        lock_path = repo_root / "Templates" / "assets.lock.json"
        if not lock_path.is_file():
            self.skipTest("not running from repo root")
        errors = vai.verify_template_manifests(repo_root)
        self.assertEqual(errors, [], msg="\n".join(str(e) for e in errors))


class LiveManifestTests(unittest.TestCase):
    def test_assets_manifest_passes_verification(self) -> None:
        repo_root = SCRIPT.parent.parent.parent
        manifest_path = repo_root / "Assets" / "assets.integrity.json"
        if not manifest_path.is_file():
            self.skipTest("Assets/assets.integrity.json not found")
        errors = vai.verify_manifest(manifest_path)
        self.assertEqual(errors, [], msg="\n".join(str(e) for e in errors))


if __name__ == "__main__":
    unittest.main()
