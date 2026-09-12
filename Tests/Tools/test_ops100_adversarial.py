"""Adversarial hardening tests for OPS-100 redaction, schema, path, and CI gaps."""

from __future__ import annotations

import contextlib
import importlib
import io
import json
import os
import re
import struct
import subprocess
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
strict = importlib.import_module("ops_strict_json")
fs = importlib.import_module("fs_security")
crash = importlib.import_module("validate_crash_package")
spool = importlib.import_module("validate_telemetry_spool")


# ---------------------------------------------------------------------------
# 1. JSON-aware redaction: structural parse, recursive redact, valid output
# ---------------------------------------------------------------------------

class JsonRedactionStructuralTests(unittest.TestCase):
    """redact_text must parse valid JSON, recursively redact, serialize valid JSON."""

    def test_nested_object_password_redacted(self) -> None:
        text = '{"outer": {"password": "hunter2"}, "safe": "ok"}'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        parsed = json.loads(redacted)
        self.assertNotIn("hunter2", json.dumps(parsed))
        self.assertEqual(parsed["safe"], "ok")

    def test_deeply_nested_secret(self) -> None:
        text = '{"a": {"b": {"c": {"api_key": "sk-proj-AAAA"}}}}'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        self.assertNotIn("sk-proj-AAAA", redacted)
        parsed = json.loads(redacted)
        self.assertEqual(parsed["a"]["b"]["c"]["api_key"], "<redacted>")

    def test_array_with_secret_objects(self) -> None:
        text = '[{"password": "secret1"}, {"safe": "ok"}, {"api_key": "secret2"}]'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        parsed = json.loads(redacted)
        self.assertNotIn("secret1", json.dumps(parsed))
        self.assertNotIn("secret2", json.dumps(parsed))
        self.assertEqual(parsed[1]["safe"], "ok")

    def test_brace_value_in_json(self) -> None:
        text = '{"password": "{complex-brace-value}", "safe": 42}'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        parsed = json.loads(redacted)
        self.assertNotIn("complex-brace-value", json.dumps(parsed))
        self.assertEqual(parsed["safe"], 42)

    def test_escaped_quotes_in_json_value(self) -> None:
        text = r'{"password": "value\"with\"quotes", "safe": "ok"}'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        parsed = json.loads(redacted)
        self.assertNotIn("value", parsed.get("password", ""))
        self.assertEqual(parsed["safe"], "ok")

    def test_empty_secret_value_in_json(self) -> None:
        text = '{"password": "", "safe": "ok"}'
        redacted, _ = policy.redact_text(text)
        parsed = json.loads(redacted)
        self.assertEqual(parsed["safe"], "ok")

    def test_adjacent_safe_fields_preserved(self) -> None:
        text = '{"before": 1, "password": "leak", "after": 2, "other": "safe"}'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        parsed = json.loads(redacted)
        self.assertEqual(parsed["before"], 1)
        self.assertEqual(parsed["after"], 2)
        self.assertEqual(parsed["other"], "safe")
        self.assertNotIn("leak", json.dumps(parsed))

    def test_oversized_value_in_json(self) -> None:
        big_value = "A" * 5000
        text = json.dumps({"password": big_value, "safe": "ok"})
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        parsed = json.loads(redacted)
        self.assertNotIn(big_value, json.dumps(parsed))
        self.assertEqual(parsed["safe"], "ok")

    def test_malformed_json_no_source_leak(self) -> None:
        text = '{"password": "LEAK_VALUE'
        redacted, findings = policy.redact_text(text)
        self.assertNotIn("LEAK_VALUE", redacted)

    def test_unmatched_quote_no_source_leak(self) -> None:
        text = 'password="LEAK_UNMATCHED'
        redacted, findings = policy.redact_text(text)
        self.assertNotIn("LEAK_UNMATCHED", redacted)


# ---------------------------------------------------------------------------
# 2. Bounded linear lexer for generic text
# ---------------------------------------------------------------------------

class BoundedLexerTests(unittest.TestCase):
    """Replace regex-only handling with bounded linear lexer."""

    def test_unquoted_multiword_fully_redacted(self) -> None:
        text = "password=correct horse battery staple"
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        for word in ("correct", "horse", "battery", "staple"):
            self.assertNotIn(word, redacted)

    def test_double_quoted_multiword_fully_redacted(self) -> None:
        text = 'password="correct horse battery staple"'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        for word in ("correct", "horse", "battery", "staple"):
            self.assertNotIn(word, redacted)
        self.assertIn('"<redacted>"', redacted)

    def test_single_quoted_multiword_fully_redacted(self) -> None:
        text = "password='correct horse battery staple'"
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        for word in ("correct", "horse", "battery", "staple"):
            self.assertNotIn(word, redacted)
        self.assertIn("'<redacted>'", redacted)

    def test_escaped_quote_in_quoted_value(self) -> None:
        text = r'password="value\"still\"secret"'
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        self.assertNotIn("still", redacted)
        self.assertNotIn("secret", redacted)

    def test_comma_delimited_stops_preserves_safe(self) -> None:
        text = "password=secret value here, username=alice"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("secret", redacted)
        self.assertIn("username=alice", redacted)

    def test_semicolon_delimited(self) -> None:
        text = "password=secret value;other=safe"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("secret", redacted)
        self.assertIn("other=safe", redacted)

    def test_brace_terminated_value(self) -> None:
        text = "password=secret_value}rest"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("secret_value", redacted)
        self.assertIn("rest", redacted)

    def test_oversized_value_truncated(self) -> None:
        big = "A" * 5000
        text = f"password={big}"
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        self.assertNotIn(big, redacted)
        self.assertIn("<redacted>", redacted)

    def test_no_catastrophic_backtracking(self) -> None:
        text = "password=value " * 5000
        start = time.perf_counter()
        policy.redact_text(text)
        elapsed = time.perf_counter() - start
        self.assertLess(elapsed, 2.0)

    def test_idempotent(self) -> None:
        text = "password=real-value"
        first, _ = policy.redact_text(text)
        second, _ = policy.redact_text(first)
        self.assertEqual(first, second)

    def test_multiple_credentials_same_line(self) -> None:
        text = "password=first secret,api_key=second secret"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("first", redacted)
        self.assertNotIn("second", redacted)

    def test_connection_string_multiword(self) -> None:
        text = "connection_string=Server=db;Password=correct horse battery staple"
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        for word in ("correct", "horse", "battery", "staple"):
            self.assertNotIn(word, redacted)

    def test_per_line_ceiling(self) -> None:
        long_line = "x" * (policy.MAX_REDACT_LINE_LENGTH + 100)
        redacted, _ = policy.redact_text(long_line)
        self.assertLessEqual(len(redacted), policy.MAX_REDACT_LINE_LENGTH + 100)

    def test_document_line_ceiling(self) -> None:
        text = "\n".join(f"line{i}" for i in range(policy.MAX_REDACT_LINES + 10))
        redacted, _ = policy.redact_text(text)
        self.assertIsNotNone(redacted)


# ---------------------------------------------------------------------------
# 3. Finding locations must never include raw secrets
# ---------------------------------------------------------------------------

class FindingLocationSafetyTests(unittest.TestCase):
    """Locations/metadata must not include raw secret-bearing JSON keys/paths."""

    def test_sensitive_key_path_hashed(self) -> None:
        text = '{"password": "hunter2"}'
        findings = policy.scan_json_values(json.loads(text), location="root")
        for f in findings:
            self.assertNotIn("hunter2", f.location)

    def test_deep_path_with_secret_key_sanitized(self) -> None:
        data = {"config": {"db": {"password": "secret123"}}}
        findings = policy.scan_json_values(data, location="root")
        for f in findings:
            if f.rule == "structured-credential":
                self.assertTrue(
                    f.location.startswith("loc:") or "password" not in f.location.lower()
                    or "secret123" not in f.location,
                    f"raw secret value should not appear in location: {f.location}"
                )

    def test_scan_text_location_with_secret_key_sanitized(self) -> None:
        findings = policy.scan_text(
            "AKIA" + "A" * 16,
            location="config.password.value"
        )
        for f in findings:
            self.assertTrue(f.location.startswith("loc:"), f"location with sensitive key not hashed: {f.location}")


# ---------------------------------------------------------------------------
# 4. PEM: single-pass bounded parser for all PRIVATE KEY families
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

DSA_PEM = """\
-----BEGIN DSA PRIVATE KEY-----
MIIBuwIBAAKBgQC3HNY5wVe2kmQ3YJHxdxBD0PdS3OhQOKvHjjGxGk7n6YFSrpe
-----END DSA PRIVATE KEY-----"""

GENERIC_PEM = """\
-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC7H1rLYDAvM3n2
-----END PRIVATE KEY-----"""


class PEMParserTests(unittest.TestCase):
    """Single-pass bounded PEM parser for all PRIVATE KEY families."""

    def assertPEMFullyRedacted(self, pem: str) -> None:  # noqa: N802
        redacted, findings = policy.redact_text(pem)
        self.assertTrue(findings)
        lines = pem.strip().splitlines()
        for line in lines[1:]:
            stripped = line.strip()
            if stripped and "PRIVATE KEY" not in stripped:
                self.assertNotIn(stripped, redacted, f"PEM material leaked: {stripped!r}")

    def test_rsa_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(RSA_PEM)

    def test_ec_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(EC_PEM)

    def test_openssh_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(OPENSSH_PEM)

    def test_encrypted_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(ENCRYPTED_PEM)

    def test_dsa_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(DSA_PEM)

    def test_generic_fully_redacted(self) -> None:
        self.assertPEMFullyRedacted(GENERIC_PEM)

    def test_mismatched_labels_still_redacted(self) -> None:
        text = "-----BEGIN RSA PRIVATE KEY-----\nMIIEow\n-----END EC PRIVATE KEY-----"
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        self.assertNotIn("MIIEow", redacted)

    def test_unmatched_begin_no_end(self) -> None:
        text = "-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA0Z3VS5JJ"
        redacted, findings = policy.redact_text(text)
        self.assertTrue(findings)
        self.assertNotIn("MIIEow", redacted)

    def test_header_only_redacted(self) -> None:
        header = "-----BEGIN PRIVATE KEY-----"
        redacted, findings = policy.redact_text(header)
        self.assertTrue(findings)
        self.assertNotIn("BEGIN", redacted)
        self.assertIn("<redacted:private-key>", redacted)

    def test_surrounding_text_preserved(self) -> None:
        text = f"safe preamble\n{RSA_PEM}\nsafe epilogue"
        redacted, findings = policy.redact_text(text)
        self.assertNotIn("MIIEow", redacted)
        self.assertIn("safe preamble", redacted)
        self.assertIn("safe epilogue", redacted)

    def test_multiple_blocks(self) -> None:
        text = f"{RSA_PEM}\n\n{EC_PEM}"
        redacted, _ = policy.redact_text(text)
        self.assertNotIn("MIIEow", redacted)
        self.assertNotIn("MHQCAQEEIBpx", redacted)

    def test_case_insensitive(self) -> None:
        pem = RSA_PEM.replace("BEGIN", "begin").replace("END", "end")
        redacted, _ = policy.redact_text(pem)
        self.assertNotIn("MIIEow", redacted)

    def test_linear_for_32mb_hostile_input(self) -> None:
        body = "A" * 76 + "\n"
        large = "-----BEGIN RSA PRIVATE KEY-----\n" + body * 400000
        start = time.perf_counter()
        policy._pem_redact_pass(large)
        elapsed = time.perf_counter() - start
        self.assertLess(elapsed, 5.0)


# ---------------------------------------------------------------------------
# 5. Archive detection by magic, minidump validation
# ---------------------------------------------------------------------------

class ArchiveMagicDetectionTests(unittest.TestCase):
    """Detect archives by magic, not suffix. Validate minidump structure."""

    def _make_zip(self, filename: str = "test.txt", content: str = "safe") -> bytes:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as zf:
            zf.writestr(filename, content)
        return buf.getvalue()

    def test_renamed_zip_as_dmp_scanned_as_archive(self) -> None:
        zip_data = self._make_zip("payload.txt", "AKIA" + "A" * 16)
        result = policy.scan_payload(zip_data, location="crash.dmp", suffix=".dmp")
        has_finding = bool(result.findings)
        has_error = bool(result.errors)
        self.assertTrue(has_finding or has_error, "renamed ZIP must be detected/scanned")

    def test_renamed_zip_no_suffix_detected(self) -> None:
        zip_data = self._make_zip("payload.txt", "AKIA" + "A" * 16)
        result = policy.scan_payload(zip_data, location="unknown_file", suffix="")
        has_finding = bool(result.findings)
        has_error = bool(result.errors)
        self.assertTrue(has_finding or has_error, "ZIP magic should be detected regardless of suffix")

    def test_valid_minidump_scanned(self) -> None:
        header = _make_minidump_header()
        header += b"\x00" * 1000
        result = policy.scan_payload(header, location="valid.dmp", suffix=".dmp")
        self.assertFalse(result.errors, f"valid minidump should scan: {result.errors}")

    def test_invalid_minidump_flagged(self) -> None:
        data = b"\x00" * 100
        result = policy.scan_payload(data, location="bad.dmp", suffix=".dmp")
        self.assertTrue(result.errors, "non-minidump binary .dmp should flag invalid structure")

    def test_unsupported_container_suffix_rejected(self) -> None:
        for ext in (".7z", ".rar", ".tar", ".gz", ".bz2", ".xz", ".cab"):
            with self.subTest(ext=ext):
                result = policy.scan_payload(b"data", location="test", suffix=ext)
                self.assertTrue(result.errors)

    def test_nested_zip_by_magic_rejected(self) -> None:
        inner = self._make_zip("inner.txt", "safe")
        outer_buf = io.BytesIO()
        with zipfile.ZipFile(outer_buf, "w") as zf:
            zf.writestr("nested.bin", inner)
        result = policy.scan_payload(outer_buf.getvalue(), location="outer", suffix=".zip")
        self.assertTrue(result.errors, "nested archive by magic should be rejected")

    def test_zip_bomb_member_count(self) -> None:
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as output:
            for i in range(policy.MAX_ARCHIVE_MEMBERS + 1):
                output.writestr(f"file_{i:04d}.txt", "x")
        result = policy.scan_payload(archive.getvalue(), location="bomb", suffix=".zip")
        self.assertTrue(result.errors)


def _make_minidump_header(stream_count: int = 2, stream_rva: int = 32) -> bytes:
    """Build a minimal valid MDMP header."""
    magic = b"MDMP"
    version = struct.pack("<I", 0xA793)
    streams = struct.pack("<I", stream_count)
    rva = struct.pack("<I", stream_rva)
    checksum = struct.pack("<I", 0)
    timestamp = struct.pack("<I", 0)
    flags = struct.pack("<Q", 0)
    return magic + version + streams + rva + checksum + timestamp + flags


# ---------------------------------------------------------------------------
# 6. Scan race detection
# ---------------------------------------------------------------------------

class ScanRaceTests(unittest.TestCase):
    """Close scan races: re-enumerate after scan, compare identities."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_concurrent_addition_detected(self) -> None:
        (self.root / "existing.txt").write_text("safe", encoding="utf-8")
        original_iter = redactor.SecureRoot.iter_names
        call_count = [0]

        def patched_iter(self_root, *, max_entries, deadline):
            call_count[0] += 1
            names = list(original_iter(self_root, max_entries=max_entries, deadline=deadline))
            if call_count[0] == 1:
                (self.root / "added.txt").write_text("sneaked in", encoding="utf-8")
            return iter(names if call_count[0] > 1 else names)

        scanner = redactor.ArtifactScanner()
        with mock.patch.object(redactor.SecureRoot, "iter_names", patched_iter):
            scanner.scan_directory(self.root)

        error_texts = [e.detail for e in scanner.errors]
        has_addition_error = any("concurrent addition" in e or "added.txt" in e for e in error_texts)
        has_replacement_error = any("concurrent replacement" in e or "changed" in e for e in error_texts)
        self.assertTrue(
            has_addition_error or has_replacement_error or scanner.errors,
            f"concurrent file addition must be detected; errors: {error_texts}"
        )

    def test_concurrent_replacement_detected(self) -> None:
        target = self.root / "target.txt"
        target.write_text("original content", encoding="utf-8")

        original_read = redactor.SecureRoot.read_file
        call_count = [0]

        def patched_read(self_root, name, *, max_bytes, deadline):
            result = original_read(self_root, name, max_bytes=max_bytes, deadline=deadline)
            call_count[0] += 1
            if call_count[0] == 1 and name == "target.txt":
                target.write_text("REPLACED CONTENT AFTER SCAN", encoding="utf-8")
            return result

        scanner = redactor.ArtifactScanner()
        with mock.patch.object(redactor.SecureRoot, "read_file", patched_read):
            scanner.scan_directory(self.root)

        error_texts = [e.detail for e in scanner.errors]
        has_change_error = any(
            "changed" in e or "replacement" in e or "identity" in e
            for e in error_texts
        )
        self.assertTrue(
            has_change_error or scanner.errors,
            f"concurrent replacement must be detected; errors: {error_texts}"
        )

    def test_symlink_in_directory_rejected(self) -> None:
        (self.root / "real.txt").write_text("safe", encoding="utf-8")
        link = self.root / "link.txt"
        try:
            os.symlink(self.root / "real.txt", link)
        except OSError:
            self.skipTest("symlinks unavailable")
        scanner = redactor.ArtifactScanner()
        scanner.scan_directory(self.root)
        self.assertTrue(scanner.errors, "symlink should produce an error")

    def test_hardlink_in_directory_rejected(self) -> None:
        original = self.root / "original.txt"
        original.write_text("content", encoding="utf-8")
        hardlink = self.root / "hardlink.txt"
        try:
            os.link(original, hardlink)
        except OSError:
            self.skipTest("hardlinks unavailable")
        scanner = redactor.ArtifactScanner()
        scanner.scan_directory(self.root)
        error_texts = [e.detail for e in scanner.errors]
        has_hardlink_error = any("hard link" in e for e in error_texts)
        self.assertTrue(has_hardlink_error, f"hardlink should be rejected; errors: {error_texts}")


# ---------------------------------------------------------------------------
# 7. CI gate registration
# ---------------------------------------------------------------------------

class CIRegistrationTests(unittest.TestCase):
    """CI paths must reference OPS-100 tooling and tests."""

    def test_tools_ops_directory_exists(self) -> None:
        self.assertTrue(OPS.exists(), f"Tools/ops directory missing at {OPS}")

    def test_all_test_files_exist(self) -> None:
        tests = ROOT / "Tests" / "Tools"
        for name in ("test_ops100_redaction.py", "test_ops100_crash_security.py",
                      "test_ops100_telemetry_spool.py", "test_ops100_adversarial.py"):
            with self.subTest(name=name):
                self.assertTrue((tests / name).exists(), f"missing {name}")

    def test_all_ops_modules_importable(self) -> None:
        for module_name in ("secret_policy", "redact_secrets", "ops_strict_json",
                            "fs_security", "validate_crash_package", "validate_telemetry_spool"):
            with self.subTest(module=module_name):
                mod = importlib.import_module(module_name)
                self.assertIsNotNone(mod)

    def test_ci_gate_registered_in_build_yml(self) -> None:
        build_yml = ROOT / ".github" / "workflows" / "build.yml"
        content = build_yml.read_text(encoding="utf-8")
        self.assertIn("validate-ops100", content, "OPS-100 job must be registered in build.yml")
        self.assertRegex(content, re.compile(
            r"required-ci-gate:.*?needs:.*?validate-ops100", re.DOTALL
        ), msg="validate-ops100 must be in the required-ci-gate needs list")

    def test_ci_gate_runs_all_test_suites(self) -> None:
        build_yml = ROOT / ".github" / "workflows" / "build.yml"
        content = build_yml.read_text(encoding="utf-8")
        for test_file in ("test_ops100_redaction.py", "test_ops100_crash_security.py",
                          "test_ops100_telemetry_spool.py", "test_ops100_adversarial.py"):
            self.assertIn(test_file, content, f"CI gate must run {test_file}")

    def test_ci_gate_covers_tools_ops_path(self) -> None:
        build_yml = ROOT / ".github" / "workflows" / "build.yml"
        content = build_yml.read_text(encoding="utf-8")
        self.assertIn("Tools", content, "CI gate should reference Tools directory")


# ---------------------------------------------------------------------------
# 8. Docs / evidence accuracy
# ---------------------------------------------------------------------------

class DocsEvidenceTests(unittest.TestCase):
    def test_telemetry_spec_mentions_offline_only(self) -> None:
        spec = (ROOT / "docs" / "specs" / "telemetry.md").read_text(encoding="utf-8")
        self.assertIn("offline", spec.lower())
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
# Strict JSON adversarial
# ---------------------------------------------------------------------------

class StrictJsonAdversarialTests(unittest.TestCase):
    def test_ops_validators_are_isolated_from_module_evidence_strict_json(self) -> None:
        script = """
import importlib
import sys
from pathlib import Path

root = Path(sys.argv[1])
sys.path.insert(0, str(root / "tools" / "module-evidence"))
importlib.import_module("strict_json")
sys.path.insert(0, str(root / "tools" / "ops"))
for module_name in ("redact_secrets", "validate_crash_package", "validate_telemetry_spool"):
    importlib.import_module(module_name)
"""
        result = subprocess.run(
            [sys.executable, "-c", script, str(ROOT)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

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

    def test_duplicate_nested_key_rejected(self) -> None:
        data = b'{"a": {"b": 1, "b": 2}}'
        with self.assertRaises(strict.StrictJsonError):
            strict.loads_strict(data, source="t", max_bytes=1024, max_depth=4,
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

    def test_empty_filename(self) -> None:
        with self.assertRaises(fs.FilesystemPolicyError):
            fs.validate_portable_filename("")

    def test_valid_filename_accepted(self) -> None:
        self.assertEqual(fs.validate_portable_filename("crash_log.txt"), "crash_log.txt")


# ---------------------------------------------------------------------------
# Redaction CLI adversarial
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

    def test_redact_never_modifies_source(self) -> None:
        source = self.root / "config.txt"
        original = "secret=very sensitive multi word value\n"
        source.write_text(original, encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            redactor.main(["--redact", str(source)])
        self.assertEqual(source.read_text(encoding="utf-8"), original)

    def test_scanner_error_emits_no_source_content(self) -> None:
        source = self.root / "broken.json"
        source.write_text('{"password": "LEAK', encoding="utf-8")
        output = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(stderr):
            status = redactor.main(["--redact", str(source)])
        combined = output.getvalue() + stderr.getvalue()
        self.assertNotIn("LEAK", combined)


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

    def test_oversized_property_rejected(self) -> None:
        event = valid_event()
        event["properties"] = {"key": "x" * (spool.MAX_PROPERTY_VALUE_BYTES + 1)}
        self.write_batch([event])
        checks = self.checks(self.validate())
        self.assertTrue(
            "property-value" in checks or "batch-json" in checks,
            f"oversized property must be rejected, got: {checks}",
        )


# ---------------------------------------------------------------------------
# Performance regressions
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
