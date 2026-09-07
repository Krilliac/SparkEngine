#!/usr/bin/env python3
"""Adversarial tests for tools/check-supply-chain.py.

Tests verify the checker is fail-closed: every kind of drift, tampering,
or policy violation produces an error, not a silent pass.
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

# Add tools/ to path so we can import the checker
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


def _make_lockfile(
    *,
    submodule_gitlinks: dict | None = None,
    managed_vendored_dirs: list | None = None,
    project_owned_dirs: list | None = None,
    sentinel_files: dict | None = None,
) -> dict:
    return {
        "version": 1,
        "generated": "2026-08-28",
        "description": "test lockfile",
        "submodule_gitlinks": submodule_gitlinks or {},
        "managed_vendored_dirs": managed_vendored_dirs or [],
        "project_owned_dirs": project_owned_dirs or [],
        "sentinel_files": sentinel_files or {},
    }


def _real_lockfile() -> dict:
    lockpath = PROJECT_ROOT / sc.LOCKFILE_REL
    with open(lockpath) as f:
        return json.load(f)


class TestSubmoduleGitlinks(unittest.TestCase):
    def test_empty_gitlinks_fails(self):
        lockfile = _make_lockfile(submodule_gitlinks={})
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        self.assertTrue(
            any(v.category == "submodule" for v in result.violations)
        )

    def test_wrong_sha_fails(self):
        lockfile = _real_lockfile()
        first_key = next(iter(lockfile["submodule_gitlinks"]))
        lockfile["submodule_gitlinks"][first_key] = "a" * 40
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("gitlink drift" in v.message for v in errors),
            f"Expected gitlink drift error, got: {[v.message for v in errors]}",
        )

    def test_missing_submodule_fails(self):
        lockfile = _real_lockfile()
        lockfile["submodule_gitlinks"]["ThirdParty/FakeLib"] = "b" * 40
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("not found" in v.message for v in errors)
        )

    def test_correct_gitlinks_pass(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_submodule_gitlinks(PROJECT_ROOT, lockfile, result)
        gitlink_errors = [
            v for v in result.violations
            if v.category == "submodule" and v.severity == "error"
        ]
        self.assertEqual(gitlink_errors, [])


class TestSentinelFiles(unittest.TestCase):
    def test_wrong_hash_fails(self):
        lockfile = _real_lockfile()
        first_sentinel = next(iter(lockfile["sentinel_files"]))
        lockfile["sentinel_files"][first_sentinel]["sha256"] = "f" * 64
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("content hash mismatch" in v.message for v in errors)
        )

    def test_wrong_size_fails(self):
        lockfile = _real_lockfile()
        first_sentinel = next(iter(lockfile["sentinel_files"]))
        lockfile["sentinel_files"][first_sentinel]["size"] = 1
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("size mismatch" in v.message for v in errors)
        )

    def test_missing_sentinel_fails(self):
        lockfile = _real_lockfile()
        lockfile["sentinel_files"]["ThirdParty/nonexistent/file.h"] = {
            "sha256": "a" * 64,
            "git_blob": None,
            "size": 100,
            "type": "source",
        }
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("missing" in v.message for v in errors)
        )

    def test_correct_sentinels_pass(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        sentinel_errors = [
            v for v in result.violations
            if v.category in ("sentinel", "integrity") and v.severity == "error"
        ]
        self.assertEqual(sentinel_errors, [])

    def test_empty_sentinels_fails(self):
        lockfile = _make_lockfile(sentinel_files={})
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)

    def test_truncated_hash_fails(self):
        """A truncated SHA-256 (not 64 hex chars) must not match."""
        lockfile = _real_lockfile()
        first_sentinel = next(iter(lockfile["sentinel_files"]))
        real_hash = lockfile["sentinel_files"][first_sentinel]["sha256"]
        lockfile["sentinel_files"][first_sentinel]["sha256"] = real_hash[:32]
        result = sc.CheckResult()
        sc.check_sentinel_files(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)


class TestLicenseValidation(unittest.TestCase):
    def test_short_license_fails(self):
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".txt", delete=False
        ) as f:
            f.write("MIT License\nCopyright 2026\n")
            f.flush()
            result = sc.CheckResult()
            sc._check_license_content(Path(f.name), "test-license", result)
        os.unlink(f.name)
        self.assertFalse(result.passed)
        self.assertTrue(
            any("implausibly short" in v.message for v in result.violations)
        )

    def test_no_copyright_fails(self):
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".txt", delete=False
        ) as f:
            f.write(
                "Permission is hereby granted, free of charge, to any person "
                "obtaining a copy of this software and associated documentation "
                "files, to deal in the Software without restriction.\n" * 5
            )
            f.flush()
            result = sc.CheckResult()
            sc._check_license_content(Path(f.name), "test-license", result)
        os.unlink(f.name)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("copyright" in v.message for v in errors)
        )

    def test_no_terms_fails(self):
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".txt", delete=False
        ) as f:
            f.write(
                "Copyright 2026 Test Author\n"
                "This is a placeholder document with no actual license terms.\n" * 10
            )
            f.flush()
            result = sc.CheckResult()
            sc._check_license_content(Path(f.name), "test-license", result)
        os.unlink(f.name)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("operative terms" in v.message for v in errors)
        )


class TestActionPins(unittest.TestCase):
    def test_unpinned_action_fails(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            wf_dir = root / ".github" / "workflows"
            wf_dir.mkdir(parents=True)
            (root / ".git").mkdir()
            (wf_dir / "test.yml").write_text(
                "jobs:\n  build:\n    steps:\n"
                "    - uses: actions/checkout@v4\n"
            )
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertFalse(result.passed)
            errors = [v for v in result.violations if v.severity == "error"]
            self.assertTrue(
                any("unpinned action" in v.message for v in errors)
            )

    def test_pinned_action_passes(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            wf_dir = root / ".github" / "workflows"
            wf_dir.mkdir(parents=True)
            (root / ".git").mkdir()
            (wf_dir / "test.yml").write_text(
                "jobs:\n  build:\n    steps:\n"
                "    - uses: actions/checkout@"
                "3d3c42e5aac5ba805825da76410c181273ba90b1 # v7\n"
            )
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertTrue(result.passed)

    def test_local_action_skipped(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            wf_dir = root / ".github" / "workflows"
            wf_dir.mkdir(parents=True)
            (root / ".git").mkdir()
            (wf_dir / "test.yml").write_text(
                "jobs:\n  build:\n    steps:\n"
                "    - uses: ./.github/actions/local\n"
            )
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertTrue(result.passed)

    def test_tag_pin_fails(self):
        """Even a semver tag (not SHA) must fail."""
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            wf_dir = root / ".github" / "workflows"
            wf_dir.mkdir(parents=True)
            (root / ".git").mkdir()
            (wf_dir / "test.yml").write_text(
                "jobs:\n  build:\n    steps:\n"
                "    - uses: actions/upload-artifact@v4.3.1\n"
            )
            result = sc.CheckResult()
            sc.check_action_pins(root, result)
            self.assertFalse(result.passed)


class TestGitmodulesConsistency(unittest.TestCase):
    def test_extra_submodule_in_gitmodules_fails(self):
        lockfile = _real_lockfile()
        first_key = next(iter(lockfile["submodule_gitlinks"]))
        del lockfile["submodule_gitlinks"][first_key]
        result = sc.CheckResult()
        sc.check_gitmodules_consistency(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("not in supply-chain.lock" in v.message for v in errors)
        )

    def test_extra_submodule_in_lockfile_fails(self):
        lockfile = _real_lockfile()
        lockfile["submodule_gitlinks"]["ThirdParty/Phantom/lib"] = "c" * 40
        result = sc.CheckResult()
        sc.check_gitmodules_consistency(PROJECT_ROOT, lockfile, result)
        self.assertFalse(result.passed)
        errors = [v for v in result.violations if v.severity == "error"]
        self.assertTrue(
            any("not in .gitmodules" in v.message for v in errors)
        )

    def test_consistent_passes(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_gitmodules_consistency(PROJECT_ROOT, lockfile, result)
        gitmod_errors = [
            v for v in result.violations
            if v.category == "gitmodules" and v.severity == "error"
        ]
        self.assertEqual(gitmod_errors, [])


class TestUnmanagedDirs(unittest.TestCase):
    def test_all_dirs_managed(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_unmanaged_dirs(PROJECT_ROOT, lockfile, result)
        inv_errors = [
            v for v in result.violations
            if v.category == "inventory" and v.severity == "error"
        ]
        self.assertEqual(inv_errors, [], [v.message for v in inv_errors])


class TestManifestConsistency(unittest.TestCase):
    def test_manifest_notice_files_covered(self):
        lockfile = _real_lockfile()
        result = sc.CheckResult()
        sc.check_manifest_consistency(PROJECT_ROOT, lockfile, result)
        manifest_errors = [
            v for v in result.violations
            if v.category == "manifest" and v.severity == "error"
        ]
        self.assertEqual(manifest_errors, [], [v.message for v in manifest_errors])


class TestEndToEnd(unittest.TestCase):
    def test_full_check_passes(self):
        """The real repository must pass the full supply-chain check."""
        result = subprocess.run(
            [sys.executable, str(PROJECT_ROOT / "tools" / "check-supply-chain.py")],
            capture_output=True,
            text=True,
            cwd=str(PROJECT_ROOT),
        )
        self.assertEqual(
            result.returncode,
            0,
            f"Supply-chain check failed:\n{result.stdout}\n{result.stderr}",
        )

    def test_json_output_valid(self):
        result = subprocess.run(
            [
                sys.executable,
                str(PROJECT_ROOT / "tools" / "check-supply-chain.py"),
                "--json",
            ],
            capture_output=True,
            text=True,
            cwd=str(PROJECT_ROOT),
        )
        self.assertEqual(result.returncode, 0)
        data = json.loads(result.stdout)
        self.assertTrue(data["passed"])
        self.assertEqual(data["violation_count"], 0)


class TestFailClosed(unittest.TestCase):
    """Verify that the checker never silently passes on broken input."""

    def test_missing_lockfile_exits_2(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            (root / ".git").mkdir()
            (root / "tools").mkdir()
            tp = root / "ThirdParty"
            tp.mkdir()
            # No supply-chain.lock — should exit 2
            import shutil
            shutil.copy(
                PROJECT_ROOT / "tools" / "check-supply-chain.py",
                root / "tools" / "check-supply-chain.py",
            )
            result = subprocess.run(
                [sys.executable, str(root / "tools" / "check-supply-chain.py")],
                capture_output=True,
                text=True,
                cwd=str(root),
            )
            self.assertEqual(result.returncode, 2)

    def test_corrupt_lockfile_exits_2(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            (root / ".git").mkdir()
            (root / "tools").mkdir()
            tp = root / "ThirdParty"
            tp.mkdir()
            (tp / "supply-chain.lock").write_text("{invalid json!!!")
            # Copy the script
            import shutil
            shutil.copy(
                PROJECT_ROOT / "tools" / "check-supply-chain.py",
                root / "tools" / "check-supply-chain.py",
            )
            result = subprocess.run(
                [sys.executable, str(root / "tools" / "check-supply-chain.py")],
                capture_output=True,
                text=True,
                cwd=str(root),
            )
            self.assertEqual(result.returncode, 2)

    def test_wrong_version_exits_2(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            (root / ".git").mkdir()
            (root / "tools").mkdir()
            tp = root / "ThirdParty"
            tp.mkdir()
            (tp / "supply-chain.lock").write_text(
                json.dumps({"version": 999})
            )
            import shutil
            shutil.copy(
                PROJECT_ROOT / "tools" / "check-supply-chain.py",
                root / "tools" / "check-supply-chain.py",
            )
            result = subprocess.run(
                [sys.executable, str(root / "tools" / "check-supply-chain.py")],
                capture_output=True,
                text=True,
                cwd=str(root),
            )
            self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
