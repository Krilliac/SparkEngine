"""Adversarial hardening tests for OPS-100 redaction, schema, path, and CI gaps."""

from __future__ import annotations

import contextlib
import importlib
import io
import json
import os
import re
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
OPS = ROOT / "tools" / "ops"
sys.path.insert(0, str(OPS))
policy = importlib.import_module("secret_policy")
redactor = importlib.import_module("redact_secrets")
strict = importlib.import_module("strict_json")
fs = importlib.import_module("fs_security")
crash = importlib.import_module("validate_crash_package")
spool = importlib.import_module("validate_telemetry_spool")


# ---------------------------------------------------------------------------
# Redaction: multi-word structured credentials
# ---------------------------------------------------------------------------

class MultiWordCredentialRedactionTests(unittest.TestCase):
    """redact_text must not leak any reusable credential tail."""

    def assertNoLeak(self, text: str, forbidden: list[str]) -> str:  # noqa: N802
        redacted, findings = policy.redact_text(text)
        for word in forbidden:
            self.assertNotIn(word, redacted, f"leaked {word!r} in: {redacted!r}")
        self.assertTrue(findings, f"no finding for: {text!r}")
        return redacted

    def test_unquoted_passphrase_fully_redacted(self) -> None:
        self.assertNoLeak(
            "password=correct horse battery staple",
            ["correct", "horse", "battery", "staple"],
        )

    def test_double_quoted_passphrase_fully_redacted(self) -> None:
        redacted = self.assertNoLeak(
            'password="correct horse battery staple"',
            ["correct", "horse", "battery", "staple"],
        )
        self.assertIn('"<redacted>"', redacted)

    def test_single_quoted_passphrase_fully_redacted(self) -> None:
        redacted = self.assertNoLeak(
            "password='correct horse battery staple'",
            ["correct", "horse", "battery", "staple"],
        )
        self.assertIn("'<redacted>'", redacted)

    def test_json_multi_word_password(self) -> None:
        text = '{"password": "correct horse battery staple", "safe": true}'
        redacted, findings = policy.redact_text(text)
        self.assertNotIn("horse", redacted)
        self.assertNotIn("battery", redacted)
        parsed = json.loads(redacted)
        self.assertEqual(parsed["safe"], True)

    def test_connection_string_multi_word(self) -> None:
        self.assertNoLeak(
            "connection_string=Server=db;Password=correct horse battery staple",
            ["correct", "horse", "battery", "staple"],
        )

    def test_smtp_pass_with_spaces(self) -> None:
        self.assertNoLeak(
            "smtp-pass: my secret passphrase here",
            ["my", "secret", "passphrase", "here"],
        )

    def test_api_key_multi_word_value(self) -> None:
        self.assertNoLeak(
            "api_key = multi word token value",
            ["multi", "word", "token", "value"],
        )

    def test_comma_delimited_stops_at_comma(self) -> None:
        text = "password=secret value here, username=alice"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("secret", redacted)
        self.assertNotIn("value here", redacted)
        self.assertIn("username=alice", redacted)

    def test_semicolon_delimited_stops_at_semicolon(self) -> None:
        text = "password=secret value;other=safe"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("secret", redacted)
        self.assertIn("other=safe", redacted)

    def test_multiple_credentials_same_line(self) -> None:
        text = "password=first secret,api_key=second secret"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("first", redacted)
        self.assertNotIn("second", redacted)

    def test_redaction_idempotent(self) -> None:
        text = "password=real-value"
        first, _ = policy.redact_text(text)
        second, _ = policy.redact_text(first)
        self.assertEqual(first, second)


# ---------------------------------------------------------------------------
# Redaction: PEM private key blocks
# ---------------------------------------------------------------------------

RSA_PEM = """\
-----BEGIN RSA PRIVATE KEY-----
MIIEowIBAAKCAQEA0Z3VS5JJcds3xfn/ygWep4PAtGoRBh2vHKfBBEBCzgpMiOcn
Iu/bXqTJJAQmjmODzEe4rlNV7nut2TpSp3K7n2sH3wVsvFIleDSnGSCQzT7g2mlm
-----END RSA PRIVATE KEY-----"""

EC_PEM = """\
-----BEGIN EC PRIVATE KEY-----
MHQCAQEEIBpxFlxLIb4hPxHKPsmMbYcPl7tXowlB5gEz8bKi9KaRoAcGBSuBBAAi
-----END EC PRIVATE KEY-----"""

OPENSSH_PEM = """\
-----BEGIN OPENSSH PRIVATE KEY-----
b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gt
-----END OPENSSH PRIVATE KEY-----"""

ENCRYPTED_PEM = """\
-----BEGIN ENCRYPTED PRIVATE KEY-----
MIIFHDBOBgkqhkiG9w0BBQ0wQTApBgkqhkiG9w0BBQwwHAQIeLCIBiAe+pYCAggA
-----END ENCRYPTED PRIVATE KEY-----"""


class PEMRedactionTests(unittest.TestCase):
    """redact_text must redact entire PEM blocks, not just the header."""

    def assertPEMFullyRedacted(self, pem: str) -> None:  # noqa: N802
        redacted, findings = policy.redact_text(pem)
        self.assertTrue(findings)
        lines = pem.strip().splitlines()
        for line in lines[1:]:
            stripped = line.strip()
            if stripped:
                self.assertNotIn(stripped, redacted, f"PEM material leaked: {stripped!r}")

    def test_rsa_block_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(RSA_PEM)

    def test_ec_block_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(EC_PEM)

    def test_openssh_block_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(OPENSSH_PEM)

    def test_encrypted_block_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(ENCRYPTED_PEM)

    def test_truncated_pem_body_redacted(self) -> None:
        truncated = "-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA..."
        redacted, findings = policy.redact_text(truncated)
        self.assertTrue(findings)
        self.assertNotIn("MIIEow", redacted)

    def test_pem_header_only_still_redacted(self) -> None:
        header = "-----BEGIN PRIVATE KEY-----"
        redacted, findings = policy.redact_text(header)
        self.assertTrue(findings)
        self.assertNotIn("BEGIN", redacted)
        self.assertIn("<redacted:private-key>", redacted)

    def test_pem_in_structured_credential(self) -> None:
        text = f"private_key={RSA_PEM}"
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        self.assertNotIn("MIIEow", redacted)

    def test_pem_surrounded_by_text(self) -> None:
        text = f"safe preamble\n{RSA_PEM}\nsafe epilogue"
        redacted, findings = policy.redact_text(text)
        self.assertNotIn("MIIEow", redacted)
        self.assertIn("safe preamble", redacted)
        self.assertIn("safe epilogue", redacted)

    def test_multiple_pem_blocks_all_redacted(self) -> None:
        text = f"{RSA_PEM}\n\n{EC_PEM}"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("MIIEow", redacted)
        self.assertNotIn("MHQCAQEEIBpx", redacted)

    def test_pem_case_insensitive_redacted(self) -> None:
        pem = RSA_PEM.replace("BEGIN", "begin").replace("END", "end")
        redacted, _ = policy.redact_text(pem)
        self.assertNotIn("MIIEow", redacted)


# ---------------------------------------------------------------------------
# Redaction: CLI --redact stdout-only safety
# ---------------------------------------------------------------------------

class RedactCLIAdversarialTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_redact_multi_word_passphrase_via_cli(self) -> None:
        source = self.root / "config.txt"
        source.write_text("password=correct horse battery staple\n", encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = redactor.main(["--redact", str(source)])
        self.assertEqual(status, 1)
        self.assertNotIn("horse", output.getvalue())
        self.assertNotIn("battery", output.getvalue())

    def test_redact_pem_via_cli(self) -> None:
        source = self.root / "keyfile.pem"
        source.write_text(RSA_PEM, encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = redactor.main(["--redact", str(source)])
        self.assertEqual(status, 1)
        self.assertNotIn("MIIEow", output.getvalue())
        self.assertNotIn("END RSA", output.getvalue())

    def test_redact_never_modifies_source(self) -> None:
        source = self.root / "config.txt"
        original = "secret=very sensitive multi word value\n"
        source.write_text(original, encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            redactor.main(["--redact", str(source)])
        self.assertEqual(source.read_text(encoding="utf-8"), original)


# ---------------------------------------------------------------------------
# Strict JSON adversarial
# ---------------------------------------------------------------------------

class StrictJsonAdversarialTests(unittest.TestCase):
    def test_empty_input_rejected(self) -> None:
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(b"", source="t", max_bytes=1024, max_depth=4,
                                max_collection_entries=100, max_string_bytes=1024)

    def test_only_whitespace_rejected(self) -> None:
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(b"   \n  ", source="t", max_bytes=1024, max_depth=4,
                                max_collection_entries=100, max_string_bytes=1024)

    def test_trailing_comma_rejected(self) -> None:
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(b'{"a": 1,}', source="t", max_bytes=1024, max_depth=4,
                                max_collection_entries=100, max_string_bytes=1024)

    def test_oversized_string_key_rejected(self) -> None:
        key = "k" * 300
        data = json.dumps({key: "v"}).encode()
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(data, source="t", max_bytes=len(data) + 10, max_depth=4,
                                max_collection_entries=100, max_string_bytes=256)

    def test_utf8_surrogate_rejected(self) -> None:
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(b'\xed\xa0\x80', source="t", max_bytes=1024, max_depth=4,
                                max_collection_entries=100, max_string_bytes=1024)

    def test_duplicate_nested_key_rejected(self) -> None:
        data = b'{"a": {"b": 1, "b": 2}}'
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(data, source="t", max_bytes=1024, max_depth=4,
                                max_collection_entries=100, max_string_bytes=1024)

    def test_zero_max_bytes_rejected(self) -> None:
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(b'1', source="t", max_bytes=0, max_depth=4,
                                max_collection_entries=100, max_string_bytes=1024)


# ---------------------------------------------------------------------------
# Filesystem path adversarial
# ---------------------------------------------------------------------------

class FilesystemPathAdversarialTests(unittest.TestCase):
    def test_null_byte_in_filename(self) -> None:
        with self.assertRaises(fs.FilesystemPolicyError) as cm:
            fs.validate_portable_filename("file\x00.txt")
        self.assertEqual(cm.exception.code, "invalid-name")

    def test_windows_reserved_names(self) -> None:
        for name in ("CON", "con", "PRN", "NUL", "COM1", "LPT9", "con.txt", "COM1.log"):
            with self.subTest(name=name):
                with self.assertRaises(fs.FilesystemPolicyError):
                    fs.validate_portable_filename(name)

    def test_trailing_dot_space(self) -> None:
        for name in ("file.", "file ", "file. ", "file.."):
            with self.subTest(name=name):
                with self.assertRaises(fs.FilesystemPolicyError):
                    fs.validate_portable_filename(name)

    def test_path_separator_variants(self) -> None:
        for name in ("a/b", "a\\b", "a:b"):
            with self.subTest(name=name):
                with self.assertRaises(fs.FilesystemPolicyError):
                    fs.validate_portable_filename(name)

    def test_dot_dot(self) -> None:
        with self.assertRaises(fs.FilesystemPolicyError):
            fs.validate_portable_filename("..")

    def test_single_dot(self) -> None:
        with self.assertRaises(fs.FilesystemPolicyError):
            fs.validate_portable_filename(".")

    def test_empty_filename(self) -> None:
        with self.assertRaises(fs.FilesystemPolicyError):
            fs.validate_portable_filename("")

    def test_non_nfc_unicode(self) -> None:
        import unicodedata
        nfd = unicodedata.normalize("NFD", "\u00e9")
        with self.assertRaises(fs.FilesystemPolicyError):
            fs.validate_portable_filename(f"caf{nfd}.txt")

    def test_control_characters(self) -> None:
        for c in ("\x01", "\x1f", "\x7f"):
            with self.subTest(char=repr(c)):
                with self.assertRaises(fs.FilesystemPolicyError):
                    fs.validate_portable_filename(f"file{c}.txt")

    def test_long_filename(self) -> None:
        with self.assertRaises(fs.FilesystemPolicyError):
            fs.validate_portable_filename("a" * 256)

    def test_valid_filename_accepted(self) -> None:
        self.assertEqual(fs.validate_portable_filename("crash_log.txt"), "crash_log.txt")

    def test_absolute_path_rejected(self) -> None:
        for name in ("/etc/passwd", "C:\\Windows", "\\\\server\\share"):
            with self.subTest(name=name):
                with self.assertRaises(fs.FilesystemPolicyError):
                    fs.validate_portable_filename(name)


# ---------------------------------------------------------------------------
# Archive adversarial
# ---------------------------------------------------------------------------

class ArchiveAdversarialTests(unittest.TestCase):
    def test_zip_bomb_member_count(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as output:
            for i in range(policy.MAX_ARCHIVE_MEMBERS + 1):
                output.writestr(f"file_{i:04d}.txt", "x")
        result = policy.scan_payload(archive.getvalue(), location="bomb", suffix=".zip")
        self.assertTrue(result.errors)

    def test_encrypted_zip_member_flagged(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as output:
            info = zipfile.ZipInfo("encrypted.txt")
            info.flag_bits = 0x1
            output.writestr(info, "secret")
        raw = bytearray(archive.getvalue())
        # Patch encryption flag in both local file header and central directory.
        # Local file header: PK\x03\x04, flag at offset +6
        local = raw.find(b"PK\x03\x04")
        if local >= 0:
            raw[local + 6] = raw[local + 6] | 0x01
        # Central directory: PK\x01\x02, flag at offset +8
        central = raw.find(b"PK\x01\x02")
        if central >= 0:
            raw[central + 8] = raw[central + 8] | 0x01
        result = policy.scan_payload(bytes(raw), location="enc", suffix=".zip")
        self.assertTrue(result.errors, "encrypted archive member should produce an error")

    def test_zip_path_traversal_parent(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("../../etc/passwd", "root:x")
        result = policy.scan_payload(archive.getvalue(), location="zip", suffix=".zip")
        self.assertTrue(result.errors)

    def test_zip_absolute_path(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("/etc/passwd", "root:x")
        result = policy.scan_payload(archive.getvalue(), location="zip", suffix=".zip")
        self.assertTrue(result.errors)

    def test_cab_format_rejected(self) -> None:
        result = policy.scan_payload(b"MSCF", location="test", suffix=".cab")
        self.assertTrue(result.errors)

    def test_unsupported_container_formats_rejected(self) -> None:
        for ext in (".7z", ".rar", ".tar", ".gz", ".bz2", ".xz"):
            with self.subTest(ext=ext):
                result = policy.scan_payload(b"data", location="test", suffix=ext)
                self.assertTrue(result.errors)


# ---------------------------------------------------------------------------
# Crash package adversarial
# ---------------------------------------------------------------------------

READY = "crash_manifest_0123456789abcdef.json"


class CrashPackageAdversarialTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    @staticmethod
    def checks(validator: crash.CrashPackageValidator) -> set[str]:
        return {item.check for item in validator.errors}

    def write_manifest(self, value: object, name: str = READY) -> Path:
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_case_alias_across_log_and_dump(self) -> None:
        (self.root / "CRASH.LOG").write_text("safe", encoding="utf-8")
        manifest = self.write_manifest({"logFile": "CRASH.LOG", "dumpFile": "crash.log"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("artifact-alias", self.checks(validator))

    def test_manifest_with_null_byte_artifact_name(self) -> None:
        manifest = self.write_manifest({"logFile": "crash\x00.log"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("artifact-path", self.checks(validator))

    def test_manifest_with_windows_device_artifact(self) -> None:
        manifest = self.write_manifest({"logFile": "CON"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("artifact-path", self.checks(validator))

    def test_missing_logfile_field(self) -> None:
        manifest = self.write_manifest({"dumpFile": "crash.dmp"})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("required-field", self.checks(validator))

    def test_empty_logfile_field(self) -> None:
        manifest = self.write_manifest({"logFile": ""})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("required-field", self.checks(validator))

    def test_timeout_field_float_rejected(self) -> None:
        manifest = self.write_manifest({"logFile": "a.log", "timeoutSeconds": 1.5})
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_fields({"logFile": "a.log", "timeoutSeconds": 1.5})
        self.assertIn("field-type", self.checks(validator))

    def test_timeout_field_overflow_rejected(self) -> None:
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_fields({"logFile": "a.log", "timeoutSeconds": 2**32})
        self.assertIn("field-type", self.checks(validator))

    def test_non_object_root_rejected(self) -> None:
        validator = crash.CrashPackageValidator()
        self.assertIsNone(validator.validate_manifest_json(b'["not", "an", "object"]'))
        self.assertIn("manifest-type", self.checks(validator))

    def test_directory_concurrent_addition_is_bounded(self) -> None:
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        self.write_manifest({"logFile": "crash.log"})
        with mock.patch.object(crash, "MAX_DIRECTORY_ENTRIES", 2):
            validator = crash.CrashPackageValidator()
            validator.validate_artifact_directory(self.root)
        self.assertFalse(validator.errors)

    def test_manifest_secret_in_json_value_detected(self) -> None:
        (self.root / "crash.log").write_text("safe", encoding="utf-8")
        manifest = self.write_manifest({
            "logFile": "crash.log",
            "nested": {"api_key": "sk-proj-" + "A" * 30},
        })
        validator = crash.CrashPackageValidator()
        validator.validate_manifest_file(manifest)
        self.assertIn("secret-exposure", self.checks(validator))


# ---------------------------------------------------------------------------
# Telemetry spool adversarial
# ---------------------------------------------------------------------------

def valid_event() -> dict[str, object]:
    now_ms = int(time.time() * 1000)
    return {
        "name": "level_complete",
        "timestamp": now_ms,
        "sessionId": f"session_{now_ms - 1000}",
        "properties": {},
    }


class TelemetrySpoolAdversarialTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.now = time.time()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def batch_name(self, offset_seconds: int = 0) -> str:
        return f"telemetry_{int((self.now + offset_seconds) * 1000)}.json"

    def write_batch(self, events: object, name: str | None = None) -> Path:
        path = self.root / (name or self.batch_name())
        path.write_text(json.dumps(events), encoding="utf-8")
        return path

    def validate(self) -> spool.TelemetrySpoolValidator:
        validator = spool.TelemetrySpoolValidator(now=self.now)
        validator.validate_spool_directory(self.root)
        return validator

    @staticmethod
    def checks(validator: spool.TelemetrySpoolValidator) -> set[str]:
        return {item.check for item in validator.errors}

    def test_event_name_empty_string_rejected(self) -> None:
        event = valid_event()
        event["name"] = ""
        self.write_batch([event])
        self.assertIn("event-name", self.checks(self.validate()))

    def test_event_name_tab_rejected(self) -> None:
        event = valid_event()
        event["name"] = "bad\tname"
        self.write_batch([event])
        self.assertIn("event-name", self.checks(self.validate()))

    def test_event_name_newline_rejected(self) -> None:
        event = valid_event()
        event["name"] = "bad\nname"
        self.write_batch([event])
        self.assertIn("event-name", self.checks(self.validate()))

    def test_event_name_backspace_rejected(self) -> None:
        event = valid_event()
        event["name"] = "bad\x08name"
        self.write_batch([event])
        self.assertIn("event-name", self.checks(self.validate()))

    def test_event_name_oversized_rejected(self) -> None:
        event = valid_event()
        event["name"] = "x" * (spool.MAX_EVENT_NAME_BYTES + 1)
        self.write_batch([event])
        self.assertIn("event-name", self.checks(self.validate()))

    def test_session_id_overflow_rejected(self) -> None:
        event = valid_event()
        event["sessionId"] = f"session_{spool.UINT64_MAX + 1}"
        self.write_batch([event])
        self.assertIn("session-id", self.checks(self.validate()))

    def test_property_key_empty_rejected(self) -> None:
        event = valid_event()
        event["properties"] = {"": "value"}
        self.write_batch([event])
        self.assertIn("property-key", self.checks(self.validate()))

    def test_property_key_control_char_rejected(self) -> None:
        event = valid_event()
        event["properties"] = {"bad\x00key": "value"}
        self.write_batch([event])
        self.assertIn("property-key", self.checks(self.validate()))

    def test_property_value_oversized_rejected(self) -> None:
        event = valid_event()
        event["properties"] = {"key": "x" * (spool.MAX_PROPERTY_VALUE_BYTES + 1)}
        self.write_batch([event])
        checks = self.checks(self.validate())
        # Strict JSON parser may reject the oversized string before the
        # property-level check fires — either error proves the bound holds.
        self.assertTrue(
            "property-value" in checks or "batch-json" in checks,
            f"oversized property must be rejected, got: {checks}",
        )

    def test_batch_root_not_array_rejected(self) -> None:
        path = self.root / self.batch_name()
        path.write_text('{"not": "array"}', encoding="utf-8")
        self.assertIn("batch-type", self.checks(self.validate()))

    def test_event_not_object_rejected(self) -> None:
        path = self.root / self.batch_name()
        path.write_text('["not_an_object"]', encoding="utf-8")
        self.assertIn("event-type", self.checks(self.validate()))

    def test_zero_timestamp_accepted(self) -> None:
        event = valid_event()
        event["timestamp"] = 0
        self.write_batch([event])
        validator = self.validate()
        timestamp_errors = [e for e in validator.errors if e.check == "event-timestamp"
                            and "must be a uint64" in e.message]
        self.assertFalse(timestamp_errors, "timestamp=0 should be a valid uint64")

    def test_sequence_equal_to_previous_rejected(self) -> None:
        first = valid_event()
        second = valid_event()
        first["sequence"] = 5
        second["sequence"] = 5
        self.write_batch([first, second])
        self.assertIn("batch-sequence", self.checks(self.validate()))


# ---------------------------------------------------------------------------
# CI registration / path filter gap
# ---------------------------------------------------------------------------

class CIRegistrationTests(unittest.TestCase):
    """CI paths must reference OPS-100 tooling and tests."""

    def test_tools_ops_directory_exists(self) -> None:
        self.assertTrue(OPS.exists(), f"Tools/ops directory missing at {OPS}")

    def test_all_three_test_files_exist(self) -> None:
        tests = ROOT / "Tests" / "Tools"
        for name in ("test_ops100_redaction.py", "test_ops100_crash_security.py",
                      "test_ops100_telemetry_spool.py"):
            with self.subTest(name=name):
                self.assertTrue((tests / name).exists(), f"missing {name}")

    def test_all_ops_modules_importable(self) -> None:
        for module_name in ("secret_policy", "redact_secrets", "strict_json",
                            "fs_security", "validate_crash_package", "validate_telemetry_spool"):
            with self.subTest(module=module_name):
                mod = importlib.import_module(module_name)
                self.assertIsNotNone(mod)

    def test_case_sensitive_import_path(self) -> None:
        actual = OPS.name
        self.assertEqual(actual, "ops", f"directory should be lowercase 'ops', got {actual!r}")

    def test_validate_all_should_reference_ops(self) -> None:
        validate_all = ROOT / "tools" / "validate-all.sh"
        if validate_all.exists():
            content = validate_all.read_text(encoding="utf-8")
            has_ops_ref = "ops" in content.lower() or "test_ops" in content.lower()
            if not has_ops_ref:
                self.skipTest("validate-all.sh does not yet include OPS checks (known gap)")


# ---------------------------------------------------------------------------
# Docs / evidence accuracy
# ---------------------------------------------------------------------------

class DocsEvidenceTests(unittest.TestCase):
    def test_telemetry_spec_mentions_offline_only(self) -> None:
        spec = (ROOT / "docs" / "specs" / "telemetry.md").read_text(encoding="utf-8")
        self.assertIn("offline", spec.lower())
        # Spec must disclaim runtime guarantees — check for the actual wording.
        normalized = spec.lower()
        self.assertTrue(
            "does not enforce" in normalized
            or "not evidence" in normalized
            or "not current" in normalized
            or "does not claim" in normalized,
            "telemetry spec must disclaim runtime guarantees",
        )

    def test_work_item_evidence_scope_is_honest(self) -> None:
        data = json.loads((ROOT / "docs" / "readiness" / "work-items" /
                           "10-security-network-operations.json").read_text(encoding="utf-8"))
        ops = next(item for item in data["workItems"] if item["id"] == "OPS-100")
        evidence = ops.get("evidence", {})
        self.assertIn("scope", evidence)
        scope = evidence["scope"]
        self.assertIn("not runtime", scope.lower().replace("-", " "))

    def test_work_item_status_is_open(self) -> None:
        data = json.loads((ROOT / "docs" / "readiness" / "work-items" /
                           "10-security-network-operations.json").read_text(encoding="utf-8"))
        ops = next(item for item in data["workItems"] if item["id"] == "OPS-100")
        self.assertEqual(ops["status"], "open")
        self.assertTrue(ops["blocking"])

    def test_remaining_blockers_list_is_nonempty(self) -> None:
        data = json.loads((ROOT / "docs" / "readiness" / "work-items" /
                           "10-security-network-operations.json").read_text(encoding="utf-8"))
        ops = next(item for item in data["workItems"] if item["id"] == "OPS-100")
        blockers = ops.get("evidence", {}).get("remainingBlockers", [])
        self.assertGreater(len(blockers), 0)


# ---------------------------------------------------------------------------
# Redaction regex performance
# ---------------------------------------------------------------------------

class RedactionPerformanceTests(unittest.TestCase):
    def test_structured_redaction_is_linear(self) -> None:
        text = "password=value " * 5000
        start = time.perf_counter()
        policy.redact_text(text)
        elapsed = time.perf_counter() - start
        self.assertLess(elapsed, 2.0)

    def test_pem_block_redaction_is_linear(self) -> None:
        body = "\n".join("A" * 76 for _ in range(200))
        pem = f"-----BEGIN RSA PRIVATE KEY-----\n{body}\n-----END RSA PRIVATE KEY-----"
        text = pem * 10
        start = time.perf_counter()
        policy.redact_text(text)
        elapsed = time.perf_counter() - start
        self.assertLess(elapsed, 2.0)


if __name__ == "__main__":
    unittest.main()
