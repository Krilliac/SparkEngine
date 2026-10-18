#!/usr/bin/env python3
"""SEC-110: tools/check-secret-scan.py repository secret gate.

Every case builds a throwaway git repository, so the inventory, the
symlink/gitlink handling and the exception reconciliation run exactly as they
do on the real tree. Planted values are assembled at runtime so that this file
does not itself carry the literal secrets it plants.

Run:  python3 -m unittest Tests.Tools.test_check_secret_scan -v
      python3 Tests/Tools/test_check_secret_scan.py
"""

from __future__ import annotations

import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

PROJECT_ROOT = Path(__file__).resolve().parents[2]


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, str(PROJECT_ROOT / rel))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


scan = _load("spark_check_secret_scan_under_test", "tools/check-secret-scan.py")

TODAY = datetime.now(timezone.utc).date()
GOOD_EXPIRY = (TODAY + timedelta(days=90)).isoformat()

# Assembled so the source of this test is not itself a finding.
GITHUB_TOKEN = "gh" + "p_" + "A1b2C3d4E5f6G7h8I9j0K1l2M3n4O5p6Q7r8"
AWS_KEY = "AK" + "IA" + "Q3EXAMPLE7KEY2ZZ"
PEM_HEADER = "-----BEGIN " + "RSA PRIVATE KEY-----"
OPENAI_KEY = "sk-" + "proj-" + "abcdefghijklmnopqrstuvwxyz012345"


def _git(root: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(root), "-c", "core.autocrlf=false", *args],
        check=True,
        capture_output=True,
    )


class SecretScanRepoCase(unittest.TestCase):
    def setUp(self) -> None:
        if shutil.which("git") is None:
            self.fail("git is required: the secret scan inventory is the git index")
        self._tmp = tempfile.TemporaryDirectory(prefix="spark-secret-scan-")
        self.root = Path(self._tmp.name) / "repo"
        self.root.mkdir()
        _git(self.root, "init", "-q")
        self.exceptions: list[dict[str, str]] = []
        self.write("README.md", "# fixture\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, rel: str, content: str | bytes, *, track: bool = True) -> Path:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, bytes):
            path.write_bytes(content)
        else:
            path.write_text(content, encoding="utf-8", newline="\n")
        if track:
            _git(self.root, "add", "--", rel)
        return path

    def except_(self, scope: str, **overrides: object) -> None:
        record = {
            "id": f"SECRET-TEST-{len(self.exceptions) + 1:03d}",
            "scope": scope,
            "owner": "Fixture Owner",
            "justification": "synthetic fixture planted by the secret-scan tests",
            "expires": GOOD_EXPIRY,
            "count": 1,
        }
        record.update(overrides)
        self.exceptions.append(record)

    def run_scan(self, document: object | None = None):
        exceptions_path = Path(self._tmp.name) / "exceptions.json"
        if document is None:
            document = {"schemaVersion": 1, "exceptions": self.exceptions}
        exceptions_path.write_text(json.dumps(document), encoding="utf-8")
        return scan.run_scan(self.root, exceptions_path, TODAY)

    def assertFinding(self, report, path: str, rule: str) -> None:
        self.assertIn((path, rule), {(f.path, f.rule) for f in report.findings}, report.findings)
        self.assertFalse(report.passed)

    def assertErrorContains(self, report, text: str) -> None:
        self.assertTrue(any(text in error for error in report.errors), report.errors)
        self.assertFalse(report.passed)


class DetectionTests(SecretScanRepoCase):
    def test_clean_repository_passes(self):
        self.write("src/main.cpp", 'int main() { return 0; }\n')
        report = self.run_scan()
        self.assertTrue(report.passed, (report.findings, report.errors))
        self.assertEqual(report.scanned_files, 2)

    def test_planted_token_formats_fail_with_line_numbers(self):
        self.write("src/github.cpp", f'// one\nconst char* token = nullptr; // {GITHUB_TOKEN}\n')
        self.write("deploy/aws.sh", f"export KEY_ID={AWS_KEY}\n")
        self.write("keys/server.txt", f"{PEM_HEADER}\nMIIBOgIBAAJBAKj\n")
        self.write("tools/client.py", f"client = make('{OPENAI_KEY}')\n")
        report = self.run_scan()
        self.assertFinding(report, "src/github.cpp", "github-token")
        self.assertFinding(report, "deploy/aws.sh", "aws-access-key-id")
        self.assertFinding(report, "keys/server.txt", "private-key")
        self.assertFinding(report, "tools/client.py", "openai-api-key")
        github = next(f for f in report.findings if f.rule == "github-token")
        self.assertEqual(github.line, 2)

    def test_output_never_contains_the_secret_value(self):
        self.write("config/server.ini", "[db]\npassword = Hunter2-Real-Value\n")
        self.write("src/github.cpp", f"// {GITHUB_TOKEN}\n")
        exceptions_path = Path(self._tmp.name) / "exceptions.json"
        exceptions_path.write_text(json.dumps({"schemaVersion": 1, "exceptions": []}), encoding="utf-8")
        for extra in ([], ["--json"]):
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = scan.main(["--root", str(self.root), "--exceptions", str(exceptions_path), *extra])
            self.assertEqual(code, 1)
            self.assertNotIn("Hunter2-Real-Value", stdout.getvalue())
            self.assertNotIn(GITHUB_TOKEN, stdout.getvalue())
            self.assertIn("config/server.ini", stdout.getvalue())

    def test_config_literal_and_credential_url_fail(self):
        self.write("config/server.ini", "[db]\npassword = Hunter2-Real-Value\n")
        self.write("config/app.yaml", "smtp:\n  smtp_pass: plain-text-pass\n")
        self.write("docs/setup.md", "clone https://deploy:" + "Sup3rSecret@git.example.invalid/repo\n")
        report = self.run_scan()
        self.assertFinding(report, "config/server.ini", "structured-credential")
        self.assertFinding(report, "config/app.yaml", "structured-credential")
        self.assertFinding(report, "docs/setup.md", "credential-in-url")

    def test_quoted_literal_in_source_fails(self):
        self.write("src/db.cpp", 'const char* dbPassword = "' + 'Hunter2-Real-Value";\n')
        self.write("src/cfg.py", 'settings.api_key = "' + 'Real-Api-Key-Value"\n')
        report = self.run_scan()
        self.assertFinding(report, "src/db.cpp", "structured-credential")
        self.assertFinding(report, "src/cfg.py", "structured-credential")

    def test_unquoted_literals_in_scripts_and_credential_stores_fail(self):
        # Each of these is caught by OPS-100 scan_text; the source-code
        # narrowing must not drop them.
        planted = {
            "deploy/deploy.sh": "export DB_PASSWORD=" + "Sup3rS3cretValue9\n",
            "docker/Dockerfile": "FROM alpine\nENV API_KEY=" + "abcd1234efgh5678\n",
            "web/.npmrc": "//registry.npmjs.org/:_authToken=" + "npm0a1b2c3d4e5f6g7h8\n",
            "ops/aws/credentials": "[default]\naws_secret_access_key = " + "wJalrXUtnFEMIK7MDENGbPxRfiCY\n",
            "tools/setup.bat": "@echo off\nset PASSWORD=" + "Hunter2Batch\n",
            "tools/login.ps1": "$env:API_KEY=" + "psLiteralKey99\n",
            "docs/notes.md": "password: " + "hunter2-in-docs\n",
        }
        for rel, content in planted.items():
            self.write(rel, content)
        report = self.run_scan()
        for rel in planted:
            with self.subTest(rel=rel):
                self.assertFinding(report, rel, "structured-credential")

    def test_unquoted_right_hand_side_in_source_is_an_expression(self):
        self.write("src/net.cpp", "auto password = ReadPassword(stdin);\nm_apiKey = config.apiKey;\n")
        self.write("src/net.py", "api_key = os.environ[KEY_NAME]\n")
        report = self.run_scan()
        self.assertTrue(report.passed, (report.findings, report.errors))

    def test_expressions_references_and_placeholders_are_not_findings(self):
        self.write(
            "src/account.cpp",
            "auto passwordHash = PasswordHash::Create(password);\n"
            "bool hasSecret = false;\n"
            'secretPath.text = "You found the hidden room";\n'
            'std::string password = "<redacted>";\n',
        )
        self.write(
            ".github/workflows/release.yml",
            "env:\n"
            "  SIGNING_PASSWORD: ${{ secrets.SIGNING_PASSWORD }}\n"
            "run: |\n"
            "  $securePassword = ConvertTo-SecureString $env:SIGNING_PASSWORD\n"
            "  git push https://x-access-token:${GH_TOKEN}@github.com/o/r.git\n"
            "  printf 'https://x-access-token:%s@github.com' \"$TOKEN\"\n",
        )
        self.write("config/server.ini", "password = $DB_PASSWORD\napi_key = none\n")
        self.write(
            "deploy/run.sh",
            'export DB_PASSWORD="$1"\nexport API_KEY=${API_KEY:?}\n[ -n "$DB_PASSWORD" ] && echo ok\n',
        )
        self.write("docs/security.md", "Hash with `Spark::PasswordHash::Create` before storage.\n")
        report = self.run_scan()
        self.assertTrue(report.passed, (report.findings, report.errors))

    def test_binary_content_only_runs_self_identifying_detectors(self):
        noise = b"\x89PNG\r\n\x1a\n\x00\x00" + b"Pwd=\xe33`1>X3" + b"\x00" * 8
        self.write("art/noise.png", noise)
        report = self.run_scan()
        self.assertTrue(report.passed, (report.findings, report.errors))

        self.write("art/key.bin", b"\x00\x01" + PEM_HEADER.encode("ascii") + b"\x00")
        report = self.run_scan()
        self.assertFinding(report, "art/key.bin", "private-key")

    def test_untracked_files_are_outside_the_inventory(self):
        self.write("scratch/notes.txt", f"{GITHUB_TOKEN}\n", track=False)
        report = self.run_scan()
        self.assertTrue(report.passed, (report.findings, report.errors))

    def test_working_tree_content_of_tracked_files_is_scanned(self):
        path = self.write("src/main.cpp", "int main() { return 0; }\n")
        path.write_text(f"// {GITHUB_TOKEN}\n", encoding="utf-8")
        report = self.run_scan()
        self.assertFinding(report, "src/main.cpp", "github-token")

    def test_oversize_file_is_an_error_not_a_skip(self):
        self.write("assets/big.txt", "x" * 64)
        with mock.patch.object(scan, "MAX_FILE_BYTES", 32):
            report = self.run_scan()
        self.assertErrorContains(report, "assets/big.txt: file exceeds")


@unittest.skipIf(os.name == "nt", "creating symlinks needs developer mode on Windows")
class LinkTests(SecretScanRepoCase):
    def test_tracked_symlink_is_not_followed(self):
        outside = Path(self._tmp.name) / "outside.txt"
        outside.write_text(f"{GITHUB_TOKEN}\n", encoding="utf-8")
        os.symlink(outside, self.root / "link.txt")
        _git(self.root, "add", "link.txt")
        report = self.run_scan()
        self.assertTrue(report.passed, (report.findings, report.errors))
        self.assertEqual(report.skipped_symlinks, 1)

    def test_regular_file_replaced_by_symlink_on_disk_is_an_error(self):
        path = self.write("src/main.cpp", "int main() { return 0; }\n")
        outside = Path(self._tmp.name) / "outside.txt"
        outside.write_text(f"{GITHUB_TOKEN}\n", encoding="utf-8")
        path.unlink()
        os.symlink(outside, path)
        report = self.run_scan()
        self.assertErrorContains(report, "src/main.cpp: tracked as a regular file but is a symbolic link")
        self.assertEqual(report.findings, [])


class ExceptionTests(SecretScanRepoCase):
    def setUp(self) -> None:
        super().setUp()
        self.write("Tests/fixture.py", f"PEM = '''{PEM_HEADER}\\nfake'''\n")

    def test_exact_owned_exception_covers_the_finding(self):
        self.except_("private-key:Tests/fixture.py")
        report = self.run_scan()
        self.assertTrue(report.passed, (report.findings, report.errors))
        self.assertEqual(len(report.excepted), 1)

    def test_second_secret_in_an_excepted_file_fails(self):
        self.except_("private-key:Tests/fixture.py")
        self.write("Tests/fixture.py", f"PEM = '{PEM_HEADER}'\nREAL = '{PEM_HEADER}'\n")
        report = self.run_scan()
        self.assertEqual(len([f for f in report.findings if f.path == "Tests/fixture.py"]), 2)
        self.assertEqual(report.excepted, [])
        self.assertErrorContains(report, "reviewed for 1 finding(s) but 2 are present")

    def test_second_secret_on_an_excepted_line_fails(self):
        self.except_("private-key:Tests/fixture.py")
        self.write("Tests/fixture.py", f"PEM = '{PEM_HEADER} {PEM_HEADER}'\n")
        report = self.run_scan()
        self.assertErrorContains(report, "but 2 are present")
        self.assertFinding(report, "Tests/fixture.py", "private-key")

    def test_reviewed_count_must_ratchet_down(self):
        self.except_("private-key:Tests/fixture.py", count=2)
        report = self.run_scan()
        self.assertErrorContains(report, "lower 'count' to 1")

    def test_count_is_required_and_bounded(self):
        for count in (None, 0, -1, True, "1", 1.0, scan.MAX_EXCEPTION_COUNT + 1):
            with self.subTest(count=count):
                self.exceptions = []
                self.except_("private-key:Tests/fixture.py", count=count)
                if count is None:
                    del self.exceptions[0]["count"]
                report = self.run_scan()
                self.assertErrorContains(report, ".count: must be the exact reviewed finding count")
                self.assertFinding(report, "Tests/fixture.py", "private-key")

    def test_exception_is_rule_specific(self):
        self.except_("github-token:Tests/fixture.py")
        report = self.run_scan()
        self.assertFinding(report, "Tests/fixture.py", "private-key")
        self.assertErrorContains(report, "matches no finding")

    def test_expired_exception_fails(self):
        self.except_("private-key:Tests/fixture.py", expires=(TODAY - timedelta(days=1)).isoformat())
        report = self.run_scan()
        self.assertErrorContains(report, "expired on")
        self.assertFinding(report, "Tests/fixture.py", "private-key")

    def test_exception_beyond_the_horizon_fails(self):
        too_far = (TODAY + timedelta(days=scan.MAX_EXCEPTION_HORIZON_DAYS + 1)).isoformat()
        self.except_("private-key:Tests/fixture.py", expires=too_far)
        report = self.run_scan()
        self.assertErrorContains(report, "days ahead")

    def test_stale_exception_fails(self):
        self.except_("private-key:Tests/fixture.py")
        self.write("README2.md", "clean\n")
        self.except_("structured-credential:README2.md")
        report = self.run_scan()
        self.assertErrorContains(report, "README2.md) matches no finding")

    def test_wildcard_and_directory_scopes_are_rejected(self):
        for scope in (
            "private-key:Tests/*",
            "private-key:Tests/*.py",
            "private-key:Tests/",
            "private-key:Tests",
            "*:Tests/fixture.py",
            "private-key:Tests/../Tests/fixture.py",
            "private-key:./Tests/fixture.py",
            "Tests/fixture.py",
            "vulnerability:Tests/fixture.py",
        ):
            with self.subTest(scope=scope):
                self.exceptions = []
                self.except_(scope)
                report = self.run_scan()
                self.assertFalse(report.passed)
                self.assertFinding(report, "Tests/fixture.py", "private-key")
                self.assertTrue(report.errors, scope)

    def test_untracked_scope_path_is_rejected(self):
        self.except_("private-key:Tests/fixture.py")
        self.except_("private-key:Tests/missing.py")
        report = self.run_scan()
        self.assertErrorContains(report, "is not a tracked regular file")

    def test_supply_chain_schema_is_reused(self):
        cases = {
            "placeholder owner": ({"owner": "TBD"}, "owned by a named maintainer"),
            "short justification": ({"justification": "because"}, "at least 16 characters"),
            "bad expiry": ({"expires": "2027-02-30"}, "invalid ISO date"),
        }
        for label, (overrides, message) in cases.items():
            with self.subTest(label):
                self.exceptions = []
                self.except_("private-key:Tests/fixture.py", **overrides)
                report = self.run_scan()
                self.assertErrorContains(report, message)
                self.assertFinding(report, "Tests/fixture.py", "private-key")

    def test_case_insensitive_duplicate_ids_are_rejected(self):
        self.except_("private-key:Tests/fixture.py", id="SECRET-DUP")
        self.write("Tests/other.py", f"PEM = '{PEM_HEADER}'\n")
        self.except_("private-key:Tests/other.py", id="secret-dup")
        report = self.run_scan()
        self.assertErrorContains(report, "duplicate exception id")

    def test_unknown_fields_and_bad_documents_are_rejected(self):
        self.except_("private-key:Tests/fixture.py")
        self.exceptions[0]["paths"] = ["Tests/**"]
        report = self.run_scan()
        self.assertErrorContains(report, "unknown=['paths']")

        report = self.run_scan({"schemaVersion": 1, "exceptions": [], "globs": ["**"]})
        self.assertErrorContains(report, "expected exactly the keys")
        report = self.run_scan({"schemaVersion": 2, "exceptions": []})
        self.assertErrorContains(report, "schemaVersion must be 1")

        exceptions_path = Path(self._tmp.name) / "exceptions.json"
        exceptions_path.write_text('{"schemaVersion": 1, "schemaVersion": 1, "exceptions": []}', encoding="utf-8")
        report = scan.run_scan(self.root, exceptions_path, TODAY)
        self.assertErrorContains(report, "duplicate JSON key")


class SetupTests(unittest.TestCase):
    def test_non_repository_cannot_run(self):
        with tempfile.TemporaryDirectory(prefix="spark-secret-scan-norepo-") as directory:
            env = {**os.environ, "GIT_CEILING_DIRECTORIES": str(Path(directory).parent)}
            with mock.patch.dict(os.environ, env):
                code = scan.main(["--root", directory])
        self.assertEqual(code, 2)

    def test_committed_exceptions_are_owned_and_unexpired(self):
        path = PROJECT_ROOT / scan.DEFAULT_EXCEPTIONS_REL
        document = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(document["schemaVersion"], 1)
        for record in document["exceptions"]:
            self.assertGreaterEqual(date.fromisoformat(record["expires"]), TODAY, record["id"])
            self.assertNotIn("*", record["scope"])
            self.assertIsInstance(record["count"], int, record["id"])
            self.assertGreaterEqual(record["count"], 1, record["id"])


if __name__ == "__main__":
    unittest.main()
