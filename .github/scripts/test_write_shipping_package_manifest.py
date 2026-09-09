#!/usr/bin/env python3
"""Behavioral contract tests for the shipping MSI manifest producer."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("write-shipping-package-manifest.py")
SPEC = importlib.util.spec_from_file_location("write_shipping_package_manifest", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
VERSION = "1.2.3"
COMMIT_SHA = "0123456789abcdef0123456789abcdef01234567"
MSI_NAME = f"SparkEngine-{VERSION}-Windows-AMD64-MinSizeRel-Runtime.msi"


@unittest.skipUnless(os.name == "nt", "Windows Shipping package publication")
class ShippingPackageManifestTests(unittest.TestCase):
    def _run(self, packages: Path, out: Path, *, version: str = VERSION,
             commit_sha: str = COMMIT_SHA, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable, str(SCRIPT),
                "--packages", str(packages),
                "--version", version,
                "--commit-sha", commit_sha,
                "--out", str(out),
            ],
            capture_output=True,
            text=True,
            cwd=cwd,
            timeout=30,
            check=False,
        )

    def test_writes_exact_hash_bound_shipping_manifest(self) -> None:
        """One exact MSI yields the closed manifest consumers use for identity."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            payload = b"shipping fixture MSI bytes"
            (packages / MSI_NAME).write_bytes(payload)
            out = root / "shipping-package-manifest.json"

            result = self._run(packages, out)

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(out.read_text(encoding="utf-8")), {
                "schemaVersion": "spark-shipping-package-v1",
                "commitSHA": COMMIT_SHA,
                "profile": "stable-v1",
                "configuration": "MinSizeRel",
                "version": VERSION,
                "msi": MSI_NAME,
                "sha256": hashlib.sha256(payload).hexdigest(),
            })
            raw_output = out.read_bytes()
            self.assertFalse(raw_output.startswith(b"\xef\xbb\xbf"))
            self.assertTrue(raw_output.endswith(b"\n"))

    def test_accepts_relative_package_and_output_paths(self) -> None:
        """The CLI accepts normal relative paths without weakening its leaf check."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            (packages / MSI_NAME).write_bytes(b"relative fixture")

            result = self._run(Path("packages"), Path("manifest.json"), cwd=root)

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root / "manifest.json").is_file())

    def test_rejects_ambiguous_or_unbound_inputs_without_writing_output(self) -> None:
        """Any invalid package identity fails before a consumer-visible manifest exists."""
        cases = (
            ("missing", (), VERSION, COMMIT_SHA, "exactly one"),
            ("wrong-leaf", ("wrong.msi",), VERSION, COMMIT_SHA, "exactly one"),
            ("multiple", (MSI_NAME, "second.msi"), VERSION, COMMIT_SHA, "exactly one"),
            ("bad-version", (MSI_NAME,), "1.2", COMMIT_SHA, "semantic"),
            ("bad-sha", (MSI_NAME,), VERSION, COMMIT_SHA.upper(), "commit SHA"),
        )
        for case, names, version, commit_sha, expected in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                packages = root / "packages"
                packages.mkdir()
                for name in names:
                    (packages / name).write_bytes(b"fixture")
                out = root / "shipping-package-manifest.json"

                result = self._run(packages, out, version=version, commit_sha=commit_sha)

                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse(out.exists(), "invalid input published a manifest")

    def test_refuses_to_overwrite_an_existing_manifest(self) -> None:
        """A stale or attacker-provided manifest cannot be silently replaced."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            (packages / MSI_NAME).write_bytes(b"fixture")
            out = root / "shipping-package-manifest.json"
            out.write_text("preserve", encoding="utf-8")

            result = self._run(packages, out)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("already exists", result.stderr)
            self.assertEqual(out.read_text(encoding="utf-8"), "preserve")

    def test_rejects_a_symlinked_msi_leaf(self) -> None:
        """The MSI leaf must be a direct regular file, not a redirection."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            target = root / "payload.msi"
            target.write_bytes(b"fixture")
            link = packages / MSI_NAME
            try:
                os.symlink(target, link)
            except OSError as exc:
                self.skipTest(f"symlink fixture is unavailable: {exc}")
            out = root / "shipping-package-manifest.json"

            result = self._run(packages, out)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("regular file", result.stderr)
            self.assertFalse(out.exists())

    def test_racing_destination_is_never_overwritten(self) -> None:
        """Atomic publication must lose cleanly when a destination appears mid-publish."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            (packages / MSI_NAME).write_bytes(b"fixture")
            out = root / "shipping-package-manifest.json"

            def race_link(source, destination):
                Path(destination).write_text("attacker", encoding="utf-8")
                raise FileExistsError("destination appeared during publish")

            with mock.patch.object(MODULE.package_evidence_io.os, "link", side_effect=race_link):
                with self.assertRaises(MODULE.package_evidence_io.PackageEvidenceIOError):
                    MODULE.write_manifest(packages, VERSION, COMMIT_SHA, out)

            self.assertEqual(out.read_text(encoding="utf-8"), "attacker")

    @unittest.skipUnless(os.name == "nt", "Windows held staging identity")
    def test_windows_held_staging_leaf_cannot_be_replaced_before_final_link(self) -> None:
        """The final hard link must reference the handle-protected staged bytes."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            payload = b"trusted fixture"
            (packages / MSI_NAME).write_bytes(payload)
            out = root / "shipping-package-manifest.json"
            original_link = MODULE.package_evidence_io.os.link
            staged_replacement_succeeded = False

            def race_link(source, destination):
                nonlocal staged_replacement_succeeded
                replacement = Path(str(source) + ".attacker")
                replacement.write_bytes(b"attacker bytes")
                try:
                    os.replace(replacement, source)
                    staged_replacement_succeeded = True
                except OSError:
                    pass
                return original_link(source, destination)

            with mock.patch.object(MODULE.package_evidence_io.os, "link", side_effect=race_link):
                MODULE.write_manifest(packages, VERSION, COMMIT_SHA, out)

            self.assertFalse(staged_replacement_succeeded)
            document = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(document["sha256"], hashlib.sha256(payload).hexdigest())

    @unittest.skipUnless(os.name == "nt", "Windows exact-handle output cleanup")
    def test_windows_write_failure_removes_partial_final_manifest(self) -> None:
        """A failed native write cannot leave a success-named manifest leaf behind."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            (packages / MSI_NAME).write_bytes(b"fixture")
            out = root / "shipping-package-manifest.json"
            try:
                with mock.patch.object(
                    MODULE.package_evidence_io, "_write_windows_staged_payload",
                    side_effect=OSError("injected staged write failure"),
                ):
                    with self.assertRaises(MODULE.package_evidence_io.PackageEvidenceIOError):
                        MODULE.write_manifest(packages, VERSION, COMMIT_SHA, out)
            except AttributeError as exc:
                self.fail(f"publisher has no exact-handle write boundary: {exc}")

            self.assertFalse(out.exists(), "failed publication left a partial final manifest")

    @unittest.skipUnless(os.name == "nt", "Windows exact-handle output cleanup")
    def test_windows_staged_close_failure_removes_final_manifest_leaf(self) -> None:
        """A staging close failure occurs before the success name is linked."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            (packages / MSI_NAME).write_bytes(b"fixture")
            out = root / "shipping-package-manifest.json"
            with mock.patch.object(
                MODULE.package_evidence_io, "_write_windows_staged_payload",
                side_effect=OSError("injected staged close failure"),
            ):
                with self.assertRaises(MODULE.package_evidence_io.PackageEvidenceIOError):
                    MODULE.write_manifest(packages, VERSION, COMMIT_SHA, out)

            self.assertFalse(out.exists(), "close failure left a final manifest leaf")


if __name__ == "__main__":
    unittest.main()
