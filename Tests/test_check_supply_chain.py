#!/usr/bin/env python3
"""Adversarial tests for tools/check-supply-chain.py.

Tests verify the checker is fail-closed: every kind of drift, tampering,
path escape, schema violation, or policy violation produces an error or
exit 2, never a silent pass.
"""

from __future__ import annotations

import copy
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch

TESTS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TESTS_DIR.parent

import importlib.util
_spec = importlib.util.spec_from_file_location(
    "check_supply_chain",
    str(PROJECT_ROOT / "tools" / "check-supply-chain.py"),
)
sc = importlib.util.module_from_spec(_spec)
sys.modules["check_supply_chain"] = sc
_spec.loader.exec_module(sc)


def _real_lockfile() -> dict:
    lockpath = PROJECT_ROOT / sc.LOCKFILE_REL
    with open(lockpath) as f:
        return json.loads(f.read(), object_pairs_hook=sc._reject_duplicate_keys)


def _make_lockfile(**overrides) -> dict:
    base = {
        "version": 1,
        "description": "test lockfile",
        "submodule_gitlinks": {},
        "managed_vendored_dirs": [],
        "project_owned_dirs": [],
        "sentinel_files": {},
    }
    base.update(overrides)
    return base


# ═══════════════════════════════════════════════════════════════════════
# Schema validation
# ═══════════════════════════════════════════════════════════════════════

class TestDuplicateJsonKeys(unittest.TestCase):
    def test_duplicate_key_rejected(self):
        raw = '{"version": 1, "version": 2}'
        with self.assertRaises(SystemExit) as ctx:
            json.loads(raw, object_pairs_hook=sc._reject_duplicate_keys)
        self.assertEqual(ctx.exception.code, 2)

    def test_duplicate_sentinel_key_rejected(self):
        raw = json.dumps({
            "version": 1,
            "description": "x",
            "submodule_gitlinks": {},
            "managed_vendored_dirs": [],
            "project_owned_dirs": [],
            "sentinel_files": {},
        })
        raw_dup = raw[:-1] + ', "sentinel_files": {}}'
        with self.assertRaises(SystemExit) as ctx:
            json.loads(raw_dup, object_pairs_hook=sc._reject_duplicate_keys)
        self.assertEqual(ctx.exception.code, 2)


class TestSchemaValidation(unittest.TestCase):
    def test_missing_version_exits_2(self):
        with tempfile.TemporaryDirectory() as d:
            lockpath = Path(d) / "ThirdParty"
            lockpath.mkdir()
            lf = lockpath / "supply-chain.lock"
            lf.write_text(json.dumps({"description": "no version"}))
            with patch.object(sc, "LOCKFILE_REL", "ThirdParty/supply-chain.lock"):
                with self.assertRaises(SystemExit) as ctx:
                    sc.load_lockfile(Path(d))
                self.assertEqual(ctx.exception.code, 2)

    def test_float_version_rejected(self):
        """version: 1.0 (float) must not pass as version 1."""
        with tempfile.TemporaryDirectory() as d:
            lockpath = Path(d) / "ThirdParty"
            lockpath.mkdir()
            lf = lockpath / "supply-chain.lock"
            lf.write_text(json.dumps({"version": 1.0}))
            with patch.object(sc, "LOCKFILE_REL", "ThirdParty/supply-chain.lock"):
                with self.assertRaises(SystemExit) as ctx:
                    sc.load_lockfile(Path(d))
                self.assertEqual(ctx.exception.code, 2)

    def test_string_version_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            lockpath = Path(d) / "ThirdParty"
            lockpath.mkdir()
            lf = lockpath / "supply-chain.lock"
            lf.write_text(json.dumps({"version": "1"}))
            with patch.object(sc, "LOCKFILE_REL", "ThirdParty/supply-chain.lock"):
                with self.assertRaises(SystemExit) as ctx:
                    sc.load_lockfile(Path(d))
                self.assertEqual(ctx.exception.code, 2)

    def test_truncated_sha256_rejected(self):
        data = _make_lockfile(sentinel_files={
            "ThirdParty/test/file.h": {
                "sha256": "abcd1234",
                "git_blob": "a" * 40,
                "size": 100,
                "type": "source",
            }
        })
        with self.assertRaises(SystemExit) as ctx:
            sc._validate_lockfile_schema(data)
        self.assertEqual(ctx.exception.code, 2)

    def test_invalid_sentinel_type_rejected(self):
        data = _make_lockfile(sentinel_files={
            "ThirdParty/test/file.h": {
                "sha256": "a" * 64,
                "git_blob": "b" * 40,
                "size": 100,
                "type": "executable",
            }
        })
        with self.assertRaises(SystemExit) as ctx:
            sc._validate_lockfile_schema(data)
        self.assertEqual(ctx.exception.code, 2)

    def test_negative_size_rejected(self):
        data = _make_lockfile(sentinel_files={
            "ThirdParty/test/file.h": {
                "sha256": "a" * 64,
                "git_blob": "b" * 40,
                "size": -1,
                "type": "source",
            }
        })
        with self.assertRaises(SystemExit) as ctx:
            sc._validate_lockfile_schema(data)
        self.assertEqual(ctx.exception.code, 2)

    def test_missing_sha256_rejected(self):
        data = _make_lockfile(sentinel_files={
            "ThirdParty/test/file.h": {
                "git_blob": "b" * 40,
                "size": 100,
                "type": "source",
            }
        })
        with self.assertRaises(SystemExit) as ctx:
            sc._validate_lockfile_schema(data)
        self.assertEqual(ctx.exception.code, 2)


# ═══════════════════════════════════════════════════════════════════════
# Path safety
# ═══════════════════════════════════════════════════════════════════════

class TestPathValidation(unittest.TestCase):
    def test_absolute_path_rejected(self):
        err = sc.validate_repo_relative_path("/etc/passwd")
        self.assertIsNotNone(err)
        self.assertIn("absolute", err)

    def test_dot_segment_rejected(self):
        err = sc.validate_repo_relative_path("ThirdParty/../../../etc/passwd")
        self.assertIsNotNone(err)
        self.assertIn("..", err)

    def test_backslash_rejected(self):
        err = sc.validate_repo_relative_path("ThirdParty\\evil\\file.h")
        self.assertIsNotNone(err)
        self.assertIn("backslash", err)

    def test_wrong_root_rejected(self):
        err = sc.validate_repo_relative_path(
            "src/evil.cpp", allowed_roots=frozenset({"ThirdParty"})
        )
        self.assertIsNotNone(err)
        self.assertIn("allowed roots", err)

    def test_valid_thirdparty_path(self):
        err = sc.validate_repo_relative_path(
            "ThirdParty/Utils/stb/stb_image.h",
            allowed_roots=frozenset({"ThirdParty"}),
        )
        self.assertIsNone(err)

    def test_empty_path_rejected(self):
        err = sc.validate_repo_relative_path("")
        self.assertIsNotNone(err)

    def test_single_dot_rejected(self):
        err = sc.validate_repo_relative_path("ThirdParty/./evil")
        self.assertIsNotNone(err)

    def test_windows_drive_letter_rejected(self):
        err = sc.validate_repo_relative_path("C:/Windows/System32/cmd.exe")
        self.assertIsNotNone(err)

    def test_sentinel_outside_thirdparty_exits_2(self):
        data = _make_lockfile(sentinel_files={
            "../../.github/workflows/build.yml": {
                "sha256": "a" * 64,
                "git_blob": "b" * 40,
                "size": 100,
                "type": "source",
            }
        })
        with self.assertRaises(SystemExit) as ctx:
            sc._validate_lockfile_schema(data)
        self.assertEqual(ctx.exception.code, 2)

    def test_managed_dir_escape_exits_2(self):
        data = _make_lockfile(managed_vendored_dirs=["../../secrets"])
        with self.assertRaises(SystemExit) as ctx:
            sc._validate_lockfile_schema(data)
        self.assertEqual(ctx.exception.code, 2)


class TestSymlinkRejection(unittest.TestCase):
    @unittest.skipIf(sys.platform == "win32" and not os.environ.get("CI"),
                     "symlink creation may require privileges on Windows")
    def test_symlink_sentinel_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            target = root / "real_file.txt"
            target.write_text("hello")
            link = root / "ThirdParty" / "evil_link"
            link.parent.mkdir(parents=True)
            link.symlink_to(target)
            err = sc.assert_regular_file_no_escape(link, root)
            self.assertIsNotNone(err)
            self.assertIn("symlink", err)

    def test_nonexistent_file_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            err = sc.assert_regular_file_no_escape(root / "nope.txt", root)
            self.assertIsNotNone(err)
            self.assertIn("does not exist", err)


# ═══════════════════════════════════════════════════════════════════════
# Submodule gitlinks
# ═══════════════════════════════════════════════════════════════════════

class TestSubmoduleGitlinks(unittest.TestCase):
    def test_empty_gitlinks_fails(self):
        lockfile = _make_lockfile(submodule_gitlinks={})
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)

    def test_wrong_sha_fails(self):
        lockfile = _real_lockfile()
        first_key = next(iter(lockfile["submodule_gitlinks"]))
        lockfile["submodule_gitlinks"][first_key] = "a" * 40
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(
            any("gitlink drift" in v.message for v in result.violations)
        )

    def test_phantom_submodule_fails(self):
        lockfile = _real_lockfile()
        lockfile["submodule_gitlinks"]["ThirdParty/FakeLib"] = "b" * 40
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(
            any("not found" in v.message for v in result.violations)
        )

    def test_correct_gitlinks_pass(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        errs = [v for v in result.violations if v.category == "submodule"
                and v.severity == "error"]
        self.assertEqual(errs, [])


# ═══════════════════════════════════════════════════════════════════════
# Sentinel files
# ═══════════════════════════════════════════════════════════════════════

class TestSentinelFiles(unittest.TestCase):
    def test_wrong_hash_fails(self):
        lockfile = _real_lockfile()
        first = next(iter(lockfile["sentinel_files"]))
        lockfile["sentinel_files"][first]["sha256"] = "f" * 64
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(any("content hash" in v.message for v in result.violations))

    def test_wrong_size_fails(self):
        lockfile = _real_lockfile()
        first = next(iter(lockfile["sentinel_files"]))
        lockfile["sentinel_files"][first]["size"] = 1
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(any("size mismatch" in v.message for v in result.violations))

    def test_missing_sentinel_fails(self):
        lockfile = _real_lockfile()
        lockfile["sentinel_files"]["ThirdParty/nonexistent/file.h"] = {
            "sha256": "a" * 64,
            "git_blob": "b" * 40,
            "size": 100,
            "type": "source",
        }
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)

    def test_correct_sentinels_pass(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        errs = [v for v in result.violations
                if v.category in ("sentinel", "integrity") and v.severity == "error"]
        self.assertEqual(errs, [])

    def test_empty_sentinels_fails(self):
        lockfile = _make_lockfile(sentinel_files={})
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)

    def test_truncated_hash_never_matches(self):
        lockfile = _real_lockfile()
        first = next(iter(lockfile["sentinel_files"]))
        real_hash = lockfile["sentinel_files"][first]["sha256"]
        lockfile["sentinel_files"][first]["sha256"] = real_hash[:32] + "0" * 32
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)

    def test_git_blob_drift_detected(self):
        lockfile = _real_lockfile()
        first = next(iter(lockfile["sentinel_files"]))
        if lockfile["sentinel_files"][first].get("git_blob"):
            lockfile["sentinel_files"][first]["git_blob"] = "c" * 40
            result = sc.CheckResult()
            sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
            self.assertFalse(result.passed)
            self.assertTrue(
                any("git blob drift" in v.message for v in result.violations)
            )


# ═══════════════════════════════════════════════════════════════════════
# EOL parity — git blob identity must be platform-stable
# ═══════════════════════════════════════════════════════════════════════

class TestEOLParity(unittest.TestCase):
    def test_git_blob_hash_is_platform_stable(self):
        """git hash-object produces the same hash regardless of checkout CRLF."""
        lockfile = _real_lockfile()
        for rel_path, entry in lockfile["sentinel_files"].items():
            expected_blob = entry.get("git_blob")
            if not expected_blob:
                continue
            actual_blob = sc.git_blob_hash(rel_path, PROJECT_ROOT)
            self.assertEqual(
                actual_blob, expected_blob,
                f"git blob hash mismatch for {rel_path} — "
                f"possible EOL or .gitattributes issue",
            )


# ═══════════════════════════════════════════════════════════════════════
# License validation
# ═══════════════════════════════════════════════════════════════════════

class TestLicenseValidation(unittest.TestCase):
    def test_short_license_fails(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                          delete=False) as f:
            f.write("MIT License\nCopyright 2026\n")
        try:
            result = sc.CheckResult()
            sc._check_license_content(Path(f.name), "test-license", result)
            self.assertFalse(result.passed)
            self.assertTrue(any("implausibly short" in v.message
                                for v in result.violations))
        finally:
            os.unlink(f.name)

    def test_no_copyright_fails(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                          delete=False) as f:
            f.write(
                "Permission is hereby granted, free of charge, to any person "
                "obtaining a copy of this software.\n" * 10
            )
        try:
            result = sc.CheckResult()
            sc._check_license_content(Path(f.name), "test", result)
            self.assertTrue(
                any("copyright" in v.message for v in result.violations)
            )
        finally:
            os.unlink(f.name)

    def test_no_terms_fails(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
                                          delete=False) as f:
            f.write("Copyright 2026 Test Author\n" * 15)
        try:
            result = sc.CheckResult()
            sc._check_license_content(Path(f.name), "test", result)
            self.assertTrue(
                any("operative terms" in v.message for v in result.violations)
            )
        finally:
            os.unlink(f.name)


# ═══════════════════════════════════════════════════════════════════════
# Action pinning
# ═══════════════════════════════════════════════════════════════════════

class TestActionPins(unittest.TestCase):
    def _make_workflow_root(self, tmpdir, content):
        root = Path(tmpdir)
        wf_dir = root / ".github" / "workflows"
        wf_dir.mkdir(parents=True)
        (wf_dir / "test.yml").write_text(content)
        return root

    def test_unpinned_action_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                "    - uses: actions/checkout@v4\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertFalse(result.passed)
            self.assertTrue(any("unpinned" in v.message for v in result.violations))

    def test_pinned_action_passes(self):
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                "    - uses: actions/checkout@"
                "3d3c42e5aac5ba805825da76410c181273ba90b1 # v7\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertTrue(result.passed)

    def test_local_action_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                "    - uses: ./.github/actions/local\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertTrue(result.passed)

    def test_tag_only_pin_fails(self):
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                "    - uses: actions/upload-artifact@v4.3.1\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertFalse(result.passed)

    def test_yaml_extension_scanned(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            wf_dir = root / ".github" / "workflows"
            wf_dir.mkdir(parents=True)
            (wf_dir / "test.yaml").write_text(
                "jobs:\n  build:\n    steps:\n"
                "    - uses: actions/checkout@v4\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertFalse(result.passed)

    def test_comment_line_ignored(self):
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                "    # uses: actions/checkout@v4\n"
                "    - uses: actions/checkout@"
                "3d3c42e5aac5ba805825da76410c181273ba90b1\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertTrue(result.passed)


# ═══════════════════════════════════════════════════════════════════════
# .gitmodules consistency
# ═══════════════════════════════════════════════════════════════════════

class TestGitmodulesConsistency(unittest.TestCase):
    def test_extra_in_gitmodules_fails(self):
        lockfile = _real_lockfile()
        first_key = next(iter(lockfile["submodule_gitlinks"]))
        del lockfile["submodule_gitlinks"][first_key]
        result = sc.CheckResult()
        sc.check_gitmodules_consistency(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(
            any("not in supply-chain.lock" in v.message for v in result.violations)
        )

    def test_extra_in_lockfile_fails(self):
        lockfile = _real_lockfile()
        lockfile["submodule_gitlinks"]["ThirdParty/Phantom/lib"] = "c" * 40
        result = sc.CheckResult()
        sc.check_gitmodules_consistency(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(
            any("not in .gitmodules" in v.message for v in result.violations)
        )

    def test_consistent_passes(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_gitmodules_consistency(PROJECT_ROOT, lockfile, result)
        errs = [v for v in result.violations
                if v.category == "gitmodules" and v.severity == "error"]
        self.assertEqual(errs, [])


# ═══════════════════════════════════════════════════════════════════════
# Unmanaged directories
# ═══════════════════════════════════════════════════════════════════════

class TestUnmanagedDirs(unittest.TestCase):
    def test_all_dirs_managed(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_unmanaged_dirs(PROJECT_ROOT, lockfile, result)
        inv_errors = [v for v in result.violations
                      if v.category == "inventory" and v.severity == "error"]
        self.assertEqual(inv_errors, [], [v.message for v in inv_errors])


# ═══════════════════════════════════════════════════════════════════════
# Manifest reconciliation
# ═══════════════════════════════════════════════════════════════════════

class TestManifestReconciliation(unittest.TestCase):
    def test_manifest_notices_covered(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_manifest_reconciliation(PROJECT_ROOT, lockfile, result)
        manifest_errors = [v for v in result.violations
                           if v.category == "manifest" and v.severity == "error"]
        self.assertEqual(manifest_errors, [], [v.message for v in manifest_errors])


# ═══════════════════════════════════════════════════════════════════════
# Git command failure (fail-closed)
# ═══════════════════════════════════════════════════════════════════════

class TestGitFailureModes(unittest.TestCase):
    def test_git_cmd_raises_on_failure(self):
        with self.assertRaises(RuntimeError):
            sc.git_cmd(["log", "--nonexistent-flag-that-should-fail"],
                       PROJECT_ROOT)

    def test_submodule_check_fatal_on_git_failure(self):
        lockfile = _real_lockfile()
        with patch.object(sc, "git_ls_tree_thirdparty",
                          side_effect=RuntimeError("simulated")):
            with self.assertRaises(SystemExit) as ctx:
                result = sc.CheckResult()
                sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
            self.assertEqual(ctx.exception.code, 2)

    def test_gitmodules_fatal_on_git_failure(self):
        lockfile = _real_lockfile()
        with patch.object(sc, "git_cmd",
                          side_effect=RuntimeError("simulated")):
            with self.assertRaises(SystemExit) as ctx:
                result = sc.CheckResult()
                sc.check_gitmodules_consistency(PROJECT_ROOT, lockfile, result)
            self.assertEqual(ctx.exception.code, 2)


# ═══════════════════════════════════════════════════════════════════════
# Fail-closed: missing/corrupt lockfile
# ═══════════════════════════════════════════════════════════════════════

class TestFailClosed(unittest.TestCase):
    def _run_checker_in_dir(self, tmpdir):
        import shutil
        root = Path(tmpdir)
        (root / "tools").mkdir()
        shutil.copy(
            PROJECT_ROOT / "tools" / "check-supply-chain.py",
            root / "tools" / "check-supply-chain.py",
        )
        return subprocess.run(
            [sys.executable, str(root / "tools" / "check-supply-chain.py")],
            capture_output=True, text=True, cwd=str(root), timeout=30,
        )

    def test_missing_lockfile_exits_2(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            subprocess.run(["git", "init"], cwd=str(root),
                           capture_output=True, timeout=10)
            (root / "ThirdParty").mkdir()
            result = self._run_checker_in_dir(d)
            self.assertEqual(result.returncode, 2)

    def test_corrupt_lockfile_exits_2(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            subprocess.run(["git", "init"], cwd=str(root),
                           capture_output=True, timeout=10)
            tp = root / "ThirdParty"
            tp.mkdir()
            (tp / "supply-chain.lock").write_text("{invalid json!!!")
            result = self._run_checker_in_dir(d)
            self.assertEqual(result.returncode, 2)

    def test_wrong_version_exits_2(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            subprocess.run(["git", "init"], cwd=str(root),
                           capture_output=True, timeout=10)
            tp = root / "ThirdParty"
            tp.mkdir()
            (tp / "supply-chain.lock").write_text(json.dumps({"version": 999}))
            result = self._run_checker_in_dir(d)
            self.assertEqual(result.returncode, 2)

    def test_not_in_git_repo_exits_2(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "ThirdParty").mkdir()
            tp = root / "ThirdParty"
            (tp / "supply-chain.lock").write_text(
                json.dumps({"version": 1})
            )
            result = self._run_checker_in_dir(d)
            self.assertEqual(result.returncode, 2)


# ═══════════════════════════════════════════════════════════════════════
# Case/canonical alias detection
# ═══════════════════════════════════════════════════════════════════════

class TestCaseAliases(unittest.TestCase):
    def test_case_variant_paths_detected_as_distinct(self):
        """Two sentinel paths differing only in case must be treated as distinct."""
        lockfile = _real_lockfile()
        first = next(iter(lockfile["sentinel_files"]))
        upper_variant = first.upper()
        if upper_variant != first:
            lockfile["sentinel_files"][upper_variant] = {
                "sha256": "a" * 64,
                "git_blob": "b" * 40,
                "size": 1,
                "type": "source",
            }
            result = sc.CheckResult()
            sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
            self.assertFalse(result.passed)


# ═══════════════════════════════════════════════════════════════════════
# End-to-end
# ═══════════════════════════════════════════════════════════════════════

class TestEndToEnd(unittest.TestCase):
    def test_full_check_passes(self):
        result = subprocess.run(
            [sys.executable, str(PROJECT_ROOT / "tools" / "check-supply-chain.py")],
            capture_output=True, text=True, cwd=str(PROJECT_ROOT), timeout=60,
        )
        self.assertEqual(
            result.returncode, 0,
            f"Supply-chain check failed:\n{result.stdout}\n{result.stderr}",
        )

    def test_json_output_valid(self):
        result = subprocess.run(
            [sys.executable,
             str(PROJECT_ROOT / "tools" / "check-supply-chain.py"), "--json"],
            capture_output=True, text=True, cwd=str(PROJECT_ROOT), timeout=60,
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertTrue(data["passed"])
        self.assertEqual(data["violation_count"], 0)

    def test_ci_mode_identical(self):
        result = subprocess.run(
            [sys.executable,
             str(PROJECT_ROOT / "tools" / "check-supply-chain.py"), "--ci"],
            capture_output=True, text=True, cwd=str(PROJECT_ROOT), timeout=60,
        )
        self.assertEqual(result.returncode, 0)


# ═══════════════════════════════════════════════════════════════════════
# Malformed manifest entries
# ═══════════════════════════════════════════════════════════════════════

class TestMalformedManifest(unittest.TestCase):
    def test_wrong_field_count_flagged(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            tp = root / "ThirdParty"
            tp.mkdir()
            (tp / "dependencies.lock").write_text(textwrap.dedent("""\
                set(SPARK_THIRDPARTY_AUDIT_ENTRIES
                    "bad|entry|only|three|fields"
                )
            """))
            sc.check_manifest_reconciliation(root, lockfile, result)
            self.assertTrue(
                any("malformed" in v.message for v in result.violations)
            )


# ═══════════════════════════════════════════════════════════════════════
# Atomic update
# ═══════════════════════════════════════════════════════════════════════

class TestAtomicUpdate(unittest.TestCase):
    def test_update_produces_valid_lockfile(self):
        result = subprocess.run(
            [sys.executable,
             str(PROJECT_ROOT / "tools" / "check-supply-chain.py"), "--update"],
            capture_output=True, text=True, cwd=str(PROJECT_ROOT), timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        verify = subprocess.run(
            [sys.executable,
             str(PROJECT_ROOT / "tools" / "check-supply-chain.py"), "--json"],
            capture_output=True, text=True, cwd=str(PROJECT_ROOT), timeout=60,
        )
        self.assertEqual(verify.returncode, 0, verify.stderr)
        data = json.loads(verify.stdout)
        self.assertTrue(data["passed"])


# ═══════════════════════════════════════════════════════════════════════
# Adversarial repair — git_blob_hash fail-open
# ═══════════════════════════════════════════════════════════════════════

class TestGitBlobFailClosed(unittest.TestCase):
    """Verify that git hash-object failure is an error, not a silent pass."""

    def test_blob_hash_failure_produces_error(self):
        lockfile = _real_lockfile()
        with patch.object(sc, "git_blob_hash", return_value=""):
            result = sc.CheckResult()
            sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
            blob_errors = [v for v in result.violations
                           if "hash-object failed" in v.message
                           or "blob" in v.message.lower()]
            sentinels_with_blob = sum(
                1 for e in lockfile["sentinel_files"].values()
                if e.get("git_blob")
            )
            self.assertGreater(len(blob_errors), 0,
                               "git_blob_hash failure must produce an error, not pass silently")
            self.assertEqual(len(blob_errors), sentinels_with_blob,
                             "every sentinel with a git_blob must report a failure")

    def test_blob_hash_failure_on_single_file(self):
        lockfile = _real_lockfile()
        first = next(iter(lockfile["sentinel_files"]))
        original_fn = sc.git_blob_hash

        def selective_fail(path, cwd):
            if path == first:
                return ""
            return original_fn(path, cwd)

        with patch.object(sc, "git_blob_hash", side_effect=selective_fail):
            result = sc.CheckResult()
            sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
            blob_errors = [v for v in result.violations
                           if "hash-object failed" in v.message]
            self.assertEqual(len(blob_errors), 1)
            self.assertIn(first, blob_errors[0].path)


# ═══════════════════════════════════════════════════════════════════════
# Adversarial repair — action pin comment injection
# ═══════════════════════════════════════════════════════════════════════

class TestActionPinCommentInjection(unittest.TestCase):
    """Verify that a SHA in a YAML comment cannot fool the pin checker."""

    def _make_workflow_root(self, tmpdir, content):
        root = Path(tmpdir)
        wf_dir = root / ".github" / "workflows"
        wf_dir.mkdir(parents=True)
        (wf_dir / "test.yml").write_text(content)
        return root

    def test_sha_in_comment_does_not_pass(self):
        fake_sha = "a" * 40
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                f"    - uses: evil/action@v1 # uses: foo/bar@{fake_sha}\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertFalse(result.passed,
                             "unpinned action with SHA in comment must be rejected")
            self.assertTrue(any("unpinned" in v.message for v in result.violations))

    def test_sha_in_comment_variant_hash_only(self):
        fake_sha = "b" * 40
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                f"    - uses: evil/action@v1 # {fake_sha}\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertFalse(result.passed)

    def test_legitimate_pinned_action_with_comment_passes(self):
        real_sha = "3d3c42e5aac5ba805825da76410c181273ba90b1"
        with tempfile.TemporaryDirectory() as d:
            root = self._make_workflow_root(d,
                "jobs:\n  build:\n    steps:\n"
                f"    - uses: actions/checkout@{real_sha} # v7\n")
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertTrue(result.passed,
                            "legitimate SHA-pinned action with version comment must pass")

    def test_strip_yaml_comment_helper(self):
        self.assertEqual(sc._strip_yaml_comment("uses: foo@abc # v1"), "uses: foo@abc")
        self.assertEqual(sc._strip_yaml_comment("uses: foo@abc"), "uses: foo@abc")
        self.assertEqual(sc._strip_yaml_comment("# full comment"), "# full comment")


# ═══════════════════════════════════════════════════════════════════════
# Adversarial repair — sentinel coverage enforcement
# ═══════════════════════════════════════════════════════════════════════

class TestSentinelCoverage(unittest.TestCase):
    """Verify that managed dirs without sentinels are flagged."""

    def test_managed_dir_without_sentinel_flagged(self):
        lockfile = _make_lockfile(
            managed_vendored_dirs=["ThirdParty/Uncovered"],
            sentinel_files={
                "ThirdParty/Other/file.h": {
                    "sha256": "a" * 64,
                    "git_blob": "b" * 40,
                    "size": 100,
                    "type": "source",
                }
            },
        )
        result = sc.CheckResult()
        sc.check_sentinel_coverage(lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(
            any("no sentinel files" in v.message for v in result.violations))
        self.assertTrue(
            any("ThirdParty/Uncovered" in v.path for v in result.violations))

    def test_managed_dir_with_sentinel_passes(self):
        lockfile = _make_lockfile(
            managed_vendored_dirs=["ThirdParty/Covered"],
            sentinel_files={
                "ThirdParty/Covered/file.h": {
                    "sha256": "a" * 64,
                    "git_blob": "b" * 40,
                    "size": 100,
                    "type": "source",
                }
            },
        )
        result = sc.CheckResult()
        sc.check_sentinel_coverage(lockfile, result)
        self.assertTrue(result.passed)

    def test_real_lockfile_has_full_coverage(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_sentinel_coverage(lockfile, result)
        coverage_errors = [v for v in result.violations
                           if v.category == "coverage" and v.severity == "error"]
        self.assertEqual(coverage_errors, [],
                         f"real lockfile has uncovered dirs: "
                         f"{[v.path for v in coverage_errors]}")

    def test_empty_managed_dirs_passes(self):
        lockfile = _make_lockfile(managed_vendored_dirs=[], sentinel_files={})
        result = sc.CheckResult()
        sc.check_sentinel_coverage(lockfile, result)
        coverage_errors = [v for v in result.violations
                           if v.category == "coverage"]
        self.assertEqual(coverage_errors, [])


if __name__ == "__main__":
    unittest.main()
