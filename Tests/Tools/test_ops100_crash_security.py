"""Adversarial regressions for the read-only crash-package validator."""

from __future__ import annotations

import importlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
OPS = ROOT / "tools" / "ops"
sys.path.insert(0, str(OPS))
crash = importlib.import_module("validate_crash_package")

READY = "crash_manifest_0123456789abcdef.json"


class CrashSecurityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_manifest(self, value: object, name: str = READY) -> Path:
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def valid_package(self) -> Path:
        (self.root / "crash.log").write_text("ordinary log", encoding="utf-8")
        return self.write_manifest({"logFile": "crash.log"})

    @staticmethod
    def checks(validator: crash.CrashPackageValidator) -> set[str]:
        return {item.check for item in validator.errors}

    def test_valid_minimal_package(self) -> None:
        self.valid_package()
        validator = crash.CrashPackageValidator(check_names=True)
        validator.validate_artifact_directory(self.root)
        self.assertFalse(validator.errors)

    def test_runtime_limit_constants_remain_source_anchored(self) -> None:
        source = (ROOT / "SparkCrashReporter" / "src" / "CrashReporterApp.cpp").read_text(encoding="utf-8")
        for declaration in (
            "kMaxManifestBytes = 1024 * 1024",
            "kMaxJsonStringBytes = 256 * 1024",
            "kMaxJsonDepth = 16",
            "kMaxCollectionEntries = 4096",
            "kMaxReadyManifests = 32",
            "kMaxCrashLogBytes = 8 * 1024 * 1024",
        ):
            self.assertIn(declaration, source)

    def test_missing_referenced_log_is_fatal(self) -> None:
        manifest = self.write_manifest({"logFile": "missing.log"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("artifact-missing", self.checks(validator))

    def test_optional_referenced_artifact_must_exist(self) -> None:
        manifest = self.write_manifest({"logFile": "missing.log", "dumpFile": "missing.dmp"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertGreaterEqual(sum(item.check == "artifact-missing" for item in validator.errors), 2)

    def test_windows_and_unix_escape_spellings_are_rejected(self) -> None:
        for value in (
            "../outside.log",
            "..\\outside.log",
            "/etc/passwd",
            "C:\\Windows\\win.ini",
            "\\\\server\\share\\file.log",
            "file.log:stream",
            "nested/file.log",
            "nested\\file.log",
            "file.log.",
        ):
            with self.subTest(value=value):
                manifest = self.write_manifest({"logFile": value})
                validator = crash.CrashPackageValidator()
                validator.validate_manifest_file(manifest)
                self.assertIn("artifact-path", self.checks(validator))

    def test_duplicate_case_alias_artifacts_are_rejected(self) -> None:
        (self.root / "same.log").write_text("safe", encoding="utf-8")
        manifest = self.write_manifest({"logFile": "same.log", "dumpFile": "SAME.LOG"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("artifact-alias", self.checks(validator))

    def test_hardlink_is_rejected_without_reading_target_secret(self) -> None:
        outside = self.root.parent / f"outside-{self.root.name}.log"
        outside.write_text("ghu_" + "A" * 40, encoding="utf-8")
        try:
            os.link(outside, self.root / "crash.log")
            self.write_manifest({"logFile": "crash.log"})
            validator = crash.CrashPackageValidator()
            validator.validate_artifact_directory(self.root)
            self.assertIn("artifact-open", self.checks(validator))
            self.assertNotIn("secret-exposure", self.checks(validator))
        finally:
            outside.unlink(missing_ok=True)

    def test_manifest_hardlink_is_rejected_before_json_read(self) -> None:
        outside = self.root.parent / f"manifest-outside-{self.root.name}.json"
        secret = "ghp_" + "A" * 40
        outside.write_text(json.dumps({"logFile": secret}), encoding="utf-8")
        try:
            os.link(outside, self.root / READY)
            validator = crash.CrashPackageValidator()
            validator.validate_manifest_file(self.root / READY)
            self.assertIn("manifest-open", self.checks(validator))
            self.assertNotIn("secret-exposure", self.checks(validator))
        finally:
            outside.unlink(missing_ok=True)

    def test_file_symlink_is_rejected_without_secret_scan(self) -> None:
        outside = self.root.parent / f"outside-{self.root.name}.log"
        outside.write_text("AKIA" + "A" * 16, encoding="utf-8")
        try:
            try:
                os.symlink(outside, self.root / "crash.log")
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")
            self.write_manifest({"logFile": "crash.log"})
            validator = crash.CrashPackageValidator()
            validator.validate_artifact_directory(self.root)
            self.assertIn("artifact-open", self.checks(validator))
            self.assertNotIn("secret-exposure", self.checks(validator))
        finally:
            outside.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows junction regression")
    def test_junction_root_is_rejected(self) -> None:
        target = self.root / "target"
        target.mkdir()
        (target / "crash.log").write_text("safe", encoding="utf-8")
        link = self.root / "junction"
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            self.skipTest(f"junctions unavailable: {result.stderr}")
        try:
            self.assertEqual(crash.main([str(link)]), 1)
        finally:
            os.rmdir(link)

    @unittest.skipUnless(os.name == "nt", "Windows ancestor-junction regression")
    def test_junction_ancestor_is_rejected_even_when_final_root_is_normal(self) -> None:
        target = self.root / "target"
        child = target / "child"
        child.mkdir(parents=True)
        link = self.root / "junction"
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            self.skipTest(f"junctions unavailable: {result.stderr}")
        try:
            validator = crash.CrashPackageValidator()
            validator.validate_artifact_directory(link / "child")
            self.assertIn("package-root", self.checks(validator))
        finally:
            os.rmdir(link)

    @unittest.skipIf(os.name == "nt", "POSIX ancestor-symlink regression")
    def test_posix_symlink_ancestor_is_rejected(self) -> None:
        target = self.root / "target"
        child = target / "child"
        child.mkdir(parents=True)
        link = self.root / "alias"
        os.symlink(target, link)
        validator = crash.CrashPackageValidator()
        validator.validate_artifact_directory(link / "child")
        self.assertIn("package-root", self.checks(validator))

    def test_directory_check_names_checks_children(self) -> None:
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        self.write_manifest({"logFile": "crash.log"}, "crash_manifest_NOTHEX.json")
        validator = crash.CrashPackageValidator(check_names=True)
        validator.validate_artifact_directory(self.root)
        self.assertIn("manifest-name", self.checks(validator))

    def test_writer_rejects_even_empty_legacy_transport_field(self) -> None:
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        manifest = self.write_manifest({"logFile": "crash.log", "githubToken": ""})
        validator = crash.CrashPackageValidator(writer_output=True)
        validator.validate_manifest_file(manifest)
        self.assertIn("writer-field", self.checks(validator))

    def test_writer_rejects_local_root_and_timeout(self) -> None:
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        manifest = self.write_manifest({"logFile": "crash.log", "artifactRoot": "x", "timeoutSeconds": 5})
        validator = crash.CrashPackageValidator(writer_output=True)
        validator.validate_manifest_file(manifest)
        self.assertEqual(sum(item.check == "writer-field" for item in validator.errors), 2)

    def test_duplicate_json_key_is_rejected(self) -> None:
        validator = crash.CrashPackageValidator()
        self.assertIsNone(validator.validate_manifest_json(b'{"logFile":"a","logFile":"b"}'))
        self.assertIn("manifest-json", self.checks(validator))

    def test_nonfinite_numbers_are_rejected(self) -> None:
        for token in (b"NaN", b"Infinity", b"-Infinity", b"1e999"):
            with self.subTest(token=token):
                validator = crash.CrashPackageValidator()
                self.assertIsNone(validator.validate_manifest_json(b'{"logFile":"a","x":' + token + b"}"))

    def test_nested_unknown_json_is_depth_bounded(self) -> None:
        nested = "0"
        for _ in range(crash.MAX_JSON_DEPTH + 2):
            nested = "[" + nested + "]"
        validator = crash.CrashPackageValidator()
        self.assertIsNone(validator.validate_manifest_json((f'{{"logFile":"a","x":{nested}}}').encode()))

    def test_every_nested_collection_is_bounded(self) -> None:
        value = {"logFile": "a", "x": list(range(crash.MAX_COLLECTION_ENTRIES + 1))}
        validator = crash.CrashPackageValidator()
        self.assertIsNone(validator.validate_manifest_json(json.dumps(value).encode()))

    def test_known_field_types_are_exact(self) -> None:
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_fields({"logFile": 7, "requireConsent": 1, "timeoutSeconds": True})
        self.assertEqual(sum(item.check == "field-type" for item in validator.errors), 3)

    def test_manifest_secret_policy_catches_structured_credential(self) -> None:
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        manifest = self.write_manifest({"logFile": "crash.log", "password": "real-value"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("secret-exposure", self.checks(validator))

    def test_binary_dump_scans_ascii_and_utf16(self) -> None:
        for payload in (
            b"\x00\x01ghu_" + b"A" * 40,
            ("padding password=real-value").encode("utf-16-le"),
        ):
            with self.subTest(payload=payload[:4]):
                (self.root / "crash.log").write_text("safe", encoding="utf-8")
                (self.root / "crash.dmp").write_bytes(payload)
                manifest = self.write_manifest({"logFile": "crash.log", "dumpFile": "crash.dmp"})
                validator = crash.CrashPackageValidator()
                validator.validate_manifest_file(manifest)
                self.assertIn("secret-exposure", self.checks(validator))

    def test_opaque_image_requires_format_aware_policy(self) -> None:
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        (self.root / "shot.png").write_bytes(b"\x89PNG\r\n\x1a\nopaque")
        manifest = self.write_manifest({"logFile": "crash.log", "screenshotFile": "shot.png"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("binary-policy", self.checks(validator))

    def test_zip_member_is_scanned_without_extraction(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            output.writestr("inside.log", "sk-proj-" + "A" * 30)
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        (self.root / "package.zip").write_bytes(archive.getvalue())
        manifest = self.write_manifest({"logFile": "crash.log", "zipFile": "package.zip"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("secret-exposure", self.checks(validator))

    def test_zip_traversal_member_is_rejected(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("../outside.log", "safe")
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        (self.root / "package.zip").write_bytes(archive.getvalue())
        manifest = self.write_manifest({"logFile": "crash.log", "zipFile": "package.zip"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("binary-policy", self.checks(validator))

    def test_package_aggregate_is_bounded(self) -> None:
        (self.root / "crash.log").write_bytes(b"a" * 16)
        manifest = self.write_manifest({"logFile": "crash.log"})
        with mock.patch.object(crash, "MAX_PACKAGE_BYTES", 8):
            validator = crash.CrashPackageValidator()
            validator.validate_manifest_file(manifest)
        self.assertIn("package-size", self.checks(validator))

    def test_directory_entry_count_is_bounded(self) -> None:
        (self.root / "one.log").write_text("one", encoding="utf-8")
        (self.root / "two.log").write_text("two", encoding="utf-8")
        with mock.patch.object(crash, "MAX_DIRECTORY_ENTRIES", 1):
            validator = crash.CrashPackageValidator()
            validator.validate_artifact_directory(self.root)
        self.assertIn("package-root", self.checks(validator))

    def test_missing_cli_input_is_nonzero(self) -> None:
        self.assertEqual(crash.main([str(self.root / "missing")]), 1)


if __name__ == "__main__":
    unittest.main()
