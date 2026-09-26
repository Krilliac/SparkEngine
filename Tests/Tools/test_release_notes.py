#!/usr/bin/env python3
"""REL-190 stable release notes: rendering, fail-closed cases, and workflow wiring.

Acceptance criterion: release notes enumerate support, limitations, migrations,
hashes, signatures, SBOM, and provenance. These tests drive the real
tools/release_notes.py against fixture readiness, changelog, SHA256SUMS, asset
inventory, and a real gzip signature control asset carrying a real RSA public
key, then render against the repository's own readiness contract and changelog.
"""
from __future__ import annotations

import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "release_notes.py"
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
REPOSITORY = "Owner/SparkEngine"
ZIP = "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel.zip"
SBOM = "SparkEngine-SBOM.spdx.json"
EVIDENCE = "SparkEngine-Exact-CI-Evidence.json"
CONTROL = "SparkEngine-release-signature-bundle.tar.gz"

READINESS = {
    "capabilities": [
        {"id": "platform.windows", "name": "Windows host"},
        {"id": "platform.linux", "name": "Linux host"},
        {"id": "platform.mobile", "name": "Mobile platforms"},
    ],
    "gates": [{"id": "G11", "name": "Gameplay scripting closure"}, {"id": "G17", "name": "Release rehearsal"}],
    "releaseProfiles": [{
        "id": "stable-v1",
        "name": "Stable v1",
        "state": "certified",
        "scope": [
            {"id": "host", "label": "Host operating system", "value": "Windows 11 x64 only."},
            {"id": "renderer", "label": "Rendered path", "value": "Direct3D 11"},
        ],
        "supportedHosts": ["Windows 11 x64"],
        "boundaries": {"experimentalCapabilityIds": ["platform.linux"],
                       "unsupportedCapabilityIds": ["platform.mobile"]},
        "excludedGates": [{"gateId": "G11", "reason": "Scripting is outside the profile."}],
        "limitations": ["Fixture limitation one | with a pipe.", "Fixture limitation two."],
    }],
}

CHANGELOG = """# Changelog

## [Unreleased]

### Added
- Not in the release.

## [1.2.3] - 2026-10-01

Stable fixture release.

### Changed
- Save format v5.

### Migration notes
- Open v4 saves once to migrate them to v5.

```text
## [9.9.9] inside a fence is not a heading
```

### Fixed
- A fixed bug.

## [1.2.2] - 2026-09-01

### Added
- Older entry.
"""


def rsa_public_key_pem() -> str:
    private = subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048"],
                             check=True, capture_output=True).stdout
    return subprocess.run(["openssl", "pkey", "-pubout"], input=private, check=True,
                          capture_output=True).stdout.decode("ascii")


def spki_fingerprint(pem: str) -> str:
    der = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "DER"], input=pem.encode("ascii"), check=True,
                         capture_output=True).stdout
    return hashlib.sha256(der).hexdigest()


class ReleaseNotesTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pem = rsa_public_key_pem()
        cls.fingerprint = spki_fingerprint(cls.pem)

    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="spark-release-notes-"))
        self.addCleanup(shutil.rmtree, self.temp, True)
        self.readiness = self.temp / "readiness.json"
        self.changelog = self.temp / "CHANGELOG.md"
        self.sums = self.temp / "SHA256SUMS"
        self.assets = self.temp / "expected-release-assets.txt"
        self.control = self.temp / "control.tar.gz"
        self.write_readiness(READINESS)
        self.changelog.write_text(CHANGELOG, encoding="utf-8")
        self.sums.write_text(f"{'a' * 64}  {ZIP}\n{'b' * 64}  {SBOM}\n", encoding="utf-8")
        self.assets.write_text(f"{EVIDENCE}\n{SBOM}\n{ZIP}\nSHA256SUMS\n", encoding="utf-8")
        self.write_control({"release-signatures.json": b"{}\n", "spark-release-public-key.pem": self.pem.encode(),
                            f"{ZIP}.sig": b"\x01" * 256, f"{SBOM}.sig": b"\x02" * 256})

    def write_readiness(self, data):
        self.readiness.write_text(json.dumps(data), encoding="utf-8")

    def write_control(self, members):
        with tarfile.open(self.control, mode="w:gz") as archive:
            for name, payload in members.items():
                info = tarfile.TarInfo(name)
                info.size = len(payload)
                archive.addfile(info, io.BytesIO(payload))

    def run_tool(self, *extra, version="1.2.3", readiness=None, changelog=None, fingerprint=None):
        args = [sys.executable, str(TOOL), "--version", version,
                "--readiness", str(readiness or self.readiness), "--changelog", str(changelog or self.changelog),
                "--sha256sums", str(self.sums), "--expected-assets-file", str(self.assets),
                "--signature-control-asset", str(self.control),
                "--signer-fingerprint", fingerprint or self.fingerprint, "--source-commit", COMMIT,
                "--built-at", "2026-10-01T12:00:00Z", "--repository", REPOSITORY,
                "--title-suffix", " (Windows Shipping)", *extra]
        return subprocess.run(args, capture_output=True, text=True, encoding="utf-8", check=False, timeout=60)

    def assert_rejected(self, fragment, **kwargs):
        result = self.run_tool(**kwargs)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn(fragment, result.stderr)


class HappyPathTests(ReleaseNotesTestCase):
    def test_renders_every_required_section_from_its_source(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        body = result.stdout
        self.assertTrue(body.startswith("## SparkEngine 1.2.3 — Stable Release (Windows Shipping)\n"))
        self.assertIn(f"[`0123456`](https://github.com/{REPOSITORY}/commit/{COMMIT})", body)
        # Support matrix and profile state come from the readiness contract.
        self.assertIn("### Support matrix: Stable v1 (`stable-v1`)", body)
        self.assertIn("at this commit: `certified`.", body)
        self.assertIn("Supported hosts: Windows 11 x64.", body)
        self.assertIn("| Rendered path | Direct3D 11 |", body)
        # Exclusions and limitations are copied verbatim (pipes escaped in tables only).
        self.assertIn("- Linux host (`platform.linux`)", body)
        self.assertIn("- Mobile platforms (`platform.mobile`)", body)
        self.assertIn("- G11 Gameplay scripting closure: Scripting is outside the profile.", body)
        self.assertIn("- Fixture limitation one | with a pipe.\n- Fixture limitation two.\n", body)
        # Exactly the requested changelog section, headings demoted, migrations extracted.
        self.assertIn("### Changes in 1.2.3 - 2026-10-01\n\nStable fixture release.", body)
        self.assertIn("#### Changed\n- Save format v5.", body)
        self.assertIn("#### Fixed\n- A fixed bug.", body)
        self.assertIn("## [9.9.9] inside a fence is not a heading", body)
        self.assertNotIn("Older entry.", body)
        self.assertNotIn("Not in the release.", body)
        migrations = body.split("### Migrations\n", 1)[1].split("\n### ", 1)[0]
        self.assertIn("- Open v4 saves once to migrate them to v5.", migrations)
        self.assertIn("```text", migrations)
        self.assertNotIn("A fixed bug", migrations)
        # Hashes, signatures, SBOM, and provenance.
        self.assertIn(f"```text\n{'a' * 64}  {ZIP}\n{'b' * 64}  {SBOM}\n```", body)
        self.assertIn(f"- `{CONTROL}` (detached signature control asset)", body)
        self.assertIn("sha256sum --ignore-missing -c SHA256SUMS", body)
        self.assertIn(f"# expected: {self.fingerprint}", body)
        self.assertIn("openssl dgst -sha256 -verify spark-release-public-key.pem -signature <asset>.sig <asset>", body)
        signer = f"--repo {REPOSITORY} --signer-workflow {REPOSITORY}/.github/workflows/release.yml"
        self.assertIn(f"gh attestation verify {SBOM} {signer}", body)
        self.assertIn(f"gh attestation verify <asset> {signer}", body)
        self.assertIn(f"gh release verify v1.2.3 --repo {REPOSITORY}", body)

    def test_output_is_deterministic_and_bounded(self):
        first = self.run_tool()
        second = self.run_tool()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        output = self.temp / "notes.md"
        written = self.run_tool("--output", str(output))
        self.assertEqual(written.returncode, 0, written.stderr)
        self.assertEqual(output.read_text(encoding="utf-8"), first.stdout)

    def test_missing_migration_section_is_stated_not_invented(self):
        self.changelog.write_text(CHANGELOG.replace("### Migration notes\n- Open v4 saves once to migrate them to v5.\n",
                                                    ""), encoding="utf-8")
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CHANGELOG.md records no migration subsection for 1.2.3.", result.stdout)

    def test_renders_against_the_repository_contract_and_changelog(self):
        readiness = ROOT / "docs" / "site" / "readiness.json"
        changelog = ROOT / "CHANGELOG.md"
        self.assets.write_text(f"SparkEngine-0.9.0-Windows-AMD64-MinSizeRel.zip\n{SBOM}\n{EVIDENCE}\nSHA256SUMS\n",
                               encoding="utf-8")
        self.sums.write_text(f"{'c' * 64}  SparkEngine-0.9.0-Windows-AMD64-MinSizeRel.zip\n{'d' * 64}  {SBOM}\n",
                             encoding="utf-8")
        result = self.run_tool(version="0.9.0", readiness=readiness, changelog=changelog)
        self.assertEqual(result.returncode, 0, result.stderr)
        profile = next(entry for entry in json.loads(readiness.read_text(encoding="utf-8"))["releaseProfiles"]
                       if entry["id"] == "stable-v1")
        for limitation in profile["limitations"]:
            self.assertIn(f"- {limitation}\n", result.stdout)
        for entry in profile["excludedGates"]:
            self.assertIn(entry["reason"], result.stdout)
        self.assertIn(f"at this commit: `{profile['state']}`.", result.stdout)
        self.assertIn("### Changes in 0.9.0", result.stdout)


class FailClosedTests(ReleaseNotesTestCase):
    def test_missing_version_section(self):
        self.assert_rejected("has no ## [4.5.6] section", version="4.5.6")

    def test_duplicated_version_section(self):
        self.changelog.write_text(CHANGELOG + "\n## [1.2.3] - again\n\n- Duplicate.\n", encoding="utf-8")
        self.assert_rejected("has 2 ## [1.2.3] sections")

    def test_empty_version_section(self):
        self.changelog.write_text("# Changelog\n\n## [1.2.3] - 2026-10-01\n\n## [1.2.2]\n- x\n", encoding="utf-8")
        self.assert_rejected("section [1.2.3] is empty")

    def test_empty_sha256sums(self):
        self.sums.write_text("\n", encoding="utf-8")
        self.assert_rejected("SHA256SUMS is empty")

    def test_malformed_or_foreign_sha256sums(self):
        self.sums.write_text(f"{'a' * 63}  {ZIP}\n", encoding="utf-8")
        self.assert_rejected("malformed SHA256SUMS line")
        self.sums.write_text(f"{'a' * 64}  {ZIP}\n{'b' * 64}  {SBOM}\n{'e' * 64}  Other.zip\n", encoding="utf-8")
        self.assert_rejected("lists Other.zip, which is not an expected release asset")

    def test_sha256sums_must_cover_every_distributable(self):
        self.sums.write_text(f"{'b' * 64}  {SBOM}\n", encoding="utf-8")
        self.assert_rejected(f"does not cover expected assets: {ZIP}")

    def test_sbom_absent_from_expected_asset_list(self):
        self.assets.write_text(f"{EVIDENCE}\n{ZIP}\nSHA256SUMS\n", encoding="utf-8")
        self.assert_rejected(f"does not contain the SBOM {SBOM}")

    def test_sha256sums_absent_from_expected_asset_list(self):
        self.assets.write_text(f"{EVIDENCE}\n{SBOM}\n{ZIP}\n", encoding="utf-8")
        self.assert_rejected("expected asset inventory does not contain SHA256SUMS")

    def test_signature_control_asset_absent(self):
        self.control.unlink()
        self.assert_rejected("signature control asset is missing or empty")

    def test_signature_control_asset_without_key_or_signatures(self):
        self.write_control({"release-signatures.json": b"{}\n", f"{ZIP}.sig": b"\x01"})
        self.assert_rejected("does not contain spark-release-public-key.pem")
        self.write_control({"release-signatures.json": b"{}\n", "spark-release-public-key.pem": self.pem.encode()})
        self.assert_rejected("has no detached signature")

    def test_signature_control_asset_is_not_an_archive(self):
        self.control.write_bytes(b"not a tarball")
        self.assert_rejected("not a readable gzip tar archive")

    def test_signature_key_must_match_pinned_fingerprint(self):
        self.assert_rejected("does not match the pinned signer fingerprint", fingerprint="f" * 64)

    def test_missing_profile_or_limitations(self):
        data = copy.deepcopy(READINESS)
        data["releaseProfiles"][0]["id"] = "other"
        self.write_readiness(data)
        self.assert_rejected("must define release profile 'stable-v1' exactly once")
        data = copy.deepcopy(READINESS)
        data["releaseProfiles"][0]["limitations"] = []
        self.write_readiness(data)
        self.assert_rejected("declares no limitations")
        data = copy.deepcopy(READINESS)
        data["releaseProfiles"][0]["boundaries"]["experimentalCapabilityIds"].append("platform.unknown")
        self.write_readiness(data)
        self.assert_rejected("unknown capability 'platform.unknown'")

    def test_body_over_bound_is_rejected(self):
        self.changelog.write_text(CHANGELOG.replace("Stable fixture release.", "x" * 130_000), encoding="utf-8")
        self.assert_rejected("the bound is 120000")

    def test_invalid_identity_arguments(self):
        self.assert_rejected("--version must be X.Y.Z", version="v1.2.3")
        self.assert_rejected("--signer-fingerprint must be a lowercase SHA-256 digest", fingerprint="A" * 64)


class WorkflowWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.steps = yaml.safe_load(cls.text)["jobs"]["release"]["steps"]

    def step(self, name):
        return next(step for step in self.steps if step.get("name") == name)

    def test_stable_body_is_the_generated_notes(self):
        notes = self.step("Render fail-closed stable release notes")
        self.assertEqual(notes["id"], "stable-notes")
        self.assertEqual(notes["if"], "needs.prepare.outputs.is_versioned == 'true'")
        self.assertNotIn("continue-on-error", notes)
        self.assertIn("set -euo pipefail", notes["run"])
        self.assertIn("tools/release_notes.py", notes["run"])
        for flag in ("--sha256sums", "--expected-assets-file", "--signature-control-asset", "--signer-fingerprint"):
            self.assertIn(flag, notes["run"])
        stable = self.step("Stage new or interrupted stable versioned release as draft")
        self.assertEqual(stable["env"]["RELEASE_BODY"], "${{ steps.stable-notes.outputs.body }}")
        self.assertEqual(stable["run"], "python3 .github/scripts/stage_release_draft.py")

    def test_notes_render_after_signing_and_before_staging(self):
        names = [step.get("name") for step in self.steps]
        notes = names.index("Render fail-closed stable release notes")
        self.assertLess(names.index("Generate protected stable signature bundle"), notes)
        self.assertLess(names.index("Resolve workflow run start time"), notes)
        self.assertLess(notes, names.index("Stage new or interrupted stable versioned release as draft"))

    def test_nightly_keeps_its_short_body(self):
        nightly = self.step("Stage nightly rolling release as draft")
        self.assertIn("## Nightly Build", nightly["env"]["RELEASE_BODY"])
        self.assertNotIn("stable-notes", nightly["env"]["RELEASE_BODY"])


if __name__ == "__main__":
    unittest.main()
