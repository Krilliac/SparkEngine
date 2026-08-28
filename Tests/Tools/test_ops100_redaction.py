"""Unified secret-policy, binary-policy, and CLI failure regressions."""

from __future__ import annotations

import contextlib
import importlib
import io
import json
import os
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ops"))
policy = importlib.import_module("secret_policy")
redactor = importlib.import_module("redact_secrets")


class SecretPolicyTests(unittest.TestCase):
    def assertDetected(self, value: str) -> None:  # noqa: N802 - unittest convention
        self.assertTrue(policy.scan_text(value, location="fixture"), value)

    def test_github_token_families(self) -> None:
        for prefix in ("ghp_", "gho_", "ghu_", "ghs_", "ghr_"):
            with self.subTest(prefix=prefix):
                self.assertDetected(prefix + "A" * 40)
        self.assertDetected("github_pat_" + "A" * 30)

    def test_openai_key_families(self) -> None:
        self.assertDetected("sk-" + "A" * 30)
        self.assertDetected("sk-proj-" + "A" * 30)
        self.assertDetected("sk-svcacct-" + "A" * 30)

    def test_anthropic_aws_and_bearer(self) -> None:
        self.assertDetected("sk-ant-" + "A" * 30)
        self.assertDetected("AKIA" + "A" * 16)
        self.assertDetected("ASIA" + "A" * 16)
        self.assertDetected("Authorization: Bearer " + "A" * 30)

    def test_private_key_and_basic_auth_url(self) -> None:
        self.assertDetected("-----BEGIN OPENSSH PRIVATE KEY-----")
        self.assertDetected("https://alice:correct-horse@example.invalid/path")

    def test_structured_credential_spellings(self) -> None:
        for value in (
            "password=real-value",
            '"client_secret": "real-value"',
            "aws_secret_access_key=real-value",
            "smtp-pass: real-value",
            "connection_string=real-value",
        ):
            with self.subTest(value=value):
                self.assertDetected(value)

    def test_safe_placeholders_do_not_trigger_structured_rule(self) -> None:
        for value in ("password=<redacted>", "password=REDACTED", "password=not-set"):
            with self.subTest(value=value):
                self.assertFalse(policy.scan_text(value, location="fixture"))

    def test_structured_json_detects_sensitive_key_without_token_shape(self) -> None:
        findings = policy.scan_json_values({"nested": {"password": "tiny"}}, location="fixture")
        self.assertTrue(findings)

    def test_redaction_preserves_json_delimiters(self) -> None:
        text = '{"password": "real-value", "safe": true}'
        output, findings = policy.redact_text(text)
        self.assertTrue(findings)
        self.assertEqual(json.loads(output)["password"], "<redacted>")

    def test_connection_string_probe_is_linear(self) -> None:
        value = "Server=x;" * 20000
        start = time.perf_counter()
        findings = policy.scan_text(value, location="fixture")
        elapsed = time.perf_counter() - start
        self.assertFalse(findings)
        self.assertLess(elapsed, 1.0)

    def test_binary_dump_ascii_and_utf16_views(self) -> None:
        ascii_result = policy.scan_payload(b"\x00ghr_" + b"A" * 40, location="dump", suffix=".dmp")
        utf16_result = policy.scan_payload(
            "padding password=real-value".encode("utf-16-le"), location="dump", suffix=".dmp"
        )
        self.assertTrue(ascii_result.findings)
        self.assertTrue(utf16_result.findings)

    def test_opaque_binary_is_explicitly_rejected(self) -> None:
        result = policy.scan_payload(b"\x89PNG\x00opaque", location="shot", suffix=".png")
        self.assertTrue(result.errors)

    def test_unsupported_compressed_format_is_rejected(self) -> None:
        result = policy.scan_payload(b"opaque", location="archive", suffix=".7z")
        self.assertTrue(result.errors)

    def test_zip_member_secret_is_detected(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            output.writestr("payload.txt", "AKIA" + "A" * 16)
        result = policy.scan_payload(archive.getvalue(), location="archive", suffix=".zip")
        self.assertTrue(result.findings)

    def test_zip_bomb_ratio_is_rejected_before_expansion(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            output.writestr("payload.txt", "A" * 200000)
        result = policy.scan_payload(archive.getvalue(), location="archive", suffix=".zip")
        self.assertTrue(result.errors)

    def test_nested_archive_is_rejected(self) -> None:
        inner = io.BytesIO()
        with zipfile.ZipFile(inner, "w") as output:
            output.writestr("safe.txt", "safe")
        outer = io.BytesIO()
        with zipfile.ZipFile(outer, "w") as output:
            output.writestr("inner.zip", inner.getvalue())
        result = policy.scan_payload(outer.getvalue(), location="outer", suffix=".zip")
        self.assertTrue(result.errors)


class ScannerCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_missing_input_returns_nonzero_error(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(redactor.main([str(self.root / "missing")]), 2)

    def test_hardlink_returns_error_without_secret_preview(self) -> None:
        outside = self.root.parent / f"secret-outside-{self.root.name}.txt"
        secret = "ghu_" + "A" * 40
        outside.write_text(secret, encoding="utf-8")
        try:
            os.link(outside, self.root / "artifact.txt")
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = redactor.main(["--json", str(self.root)])
            self.assertEqual(status, 2)
            self.assertNotIn(secret, stdout.getvalue())
        finally:
            outside.unlink(missing_ok=True)

    def test_directory_scans_binary_dump(self) -> None:
        (self.root / "crash.dmp").write_bytes(b"\x00ASIA" + b"A" * 16)
        with contextlib.redirect_stderr(io.StringIO()):
            status = redactor.main([str(self.root)])
            self.assertIn(status, (1, 2), "binary .dmp with secrets should return nonzero")

    def test_directory_rejects_nested_directory(self) -> None:
        (self.root / "nested").mkdir()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(redactor.main([str(self.root)]), 2)

    def test_redact_is_stdout_only_and_preserves_source(self) -> None:
        source = self.root / "artifact.json"
        original = '{"password": "real-value"}'
        source.write_text(original, encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = redactor.main(["--redact", str(source)])
        self.assertEqual(status, 1)
        self.assertEqual(source.read_text(encoding="utf-8"), original)
        self.assertEqual(json.loads(output.getvalue())["password"], "<redacted>")

    def test_redact_rejects_non_utf8_binary(self) -> None:
        source = self.root / "artifact.dmp"
        source.write_bytes(b"\xff\x00")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(redactor.main(["--redact", str(source)]), 2)

    def test_json_output_never_contains_secret_value(self) -> None:
        source = self.root / "artifact.json"
        secret = "sk-proj-" + "A" * 30
        source.write_text(json.dumps({"api_key": secret}), encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(redactor.main(["--json", str(source)]), 1)
        self.assertNotIn(secret, output.getvalue())

    def test_malformed_json_is_a_structured_inspection_error(self) -> None:
        source = self.root / "artifact.json"
        source.write_text('{"password":', encoding="utf-8")
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(redactor.main([str(source)]), 2)

    def test_aggregate_limit_is_enforced(self) -> None:
        (self.root / "a.txt").write_text("12345", encoding="utf-8")
        (self.root / "b.txt").write_text("67890", encoding="utf-8")
        with mock.patch.object(redactor, "MAX_SCAN_TOTAL_BYTES", 6):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(redactor.main([str(self.root)]), 2)

    def test_file_count_limit_is_enforced(self) -> None:
        (self.root / "a.txt").write_text("a", encoding="utf-8")
        (self.root / "b.txt").write_text("b", encoding="utf-8")
        with mock.patch.object(redactor, "MAX_SCAN_FILES", 1):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(redactor.main([str(self.root)]), 2)


if __name__ == "__main__":
    unittest.main()
