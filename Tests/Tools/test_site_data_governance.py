"""Offline governance policy checks for GOV-400."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("site_policy", ROOT / "tools/site-data/policy.py")
assert SPEC and SPEC.loader
policy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(policy)
sys.path.insert(0, str(ROOT / "tools/site-data"))
import validate as site_validate
from common import load_contract


class GovernancePolicyTests(unittest.TestCase):
    def test_repository_policy_matches_configured_development_channel(self) -> None:
        self.assertEqual(policy.validate(ROOT), [])

    def test_unpublished_version_is_rejected(self) -> None:
        errors = policy.validate_security_text(
            "## Supported Versions\n| Release line | Status |\n|---|---|\n| `v1.2.3` | supported |\nGitHub Security Advisories\ndoes not currently promise a timeline",
            {"Working"},
        )
        self.assertTrue(any("v1.2.3" in error for error in errors))

    def test_stable_profile_must_be_explicitly_unpublished(self) -> None:
        errors = policy.validate_support_text(
            "## Supported Versions\n| Release line | Status |\n|---|---|\n| `stable-v1` | supported |\nhttps://github.com/Krilliac/SparkEngine/issues\nbest-effort",
            {"Working"},
        )
        self.assertTrue(any("stable-v1" in error for error in errors))

    def test_unknown_channel_is_rejected(self) -> None:
        errors = policy.validate_support_text(
            "## Supported Versions\n| Release line | Status |\n|---|---|\n| `lts` | best-effort |\nhttps://github.com/Krilliac/SparkEngine/issues\nbest-effort",
            {"Working", "nightly"},
        )
        self.assertTrue(any("unavailable channel or release line lts" in error for error in errors), errors)

    def test_nightly_row_requires_a_publishing_release_workflow(self) -> None:
        """nightly is a real prerelease channel only while release.yml publishes it."""
        text = (ROOT / "SUPPORT.md").read_text(encoding="utf-8")
        self.assertEqual(policy.validate_support_text(text, {"Working", "nightly"}), [])
        errors = policy.validate_support_text(text, {"Working"})
        self.assertTrue(any("unavailable channel or release line nightly" in error for error in errors), errors)

    def test_published_nightly_channel_must_be_disclosed(self) -> None:
        """Dropping the nightly row would hide a published prerelease channel's status."""
        channels = policy.configured_development_channels(ROOT)
        self.assertIn("nightly", channels)
        text = "\n".join(
            line for line in (ROOT / "SECURITY.md").read_text(encoding="utf-8").splitlines()
            if not line.startswith("| `nightly` |")
        )
        errors = policy.validate_security_text(text, channels)
        self.assertTrue(any("every published channel" in error and "nightly" in error for error in errors), errors)

    def test_local_version_tag_cannot_authorize_a_release_claim(self) -> None:
        errors = policy.validate_support_text(
            "## Supported Versions\n| Release line | Status |\n|---|---|\n| `v1.2.3` | published |\nhttps://github.com/Krilliac/SparkEngine/issues\nbest-effort",
            {"Working", "v1.2.3"},
        )
        self.assertTrue(any("unpublished release" in error for error in errors), errors)

    def test_disclaimer_does_not_permit_contradictory_status(self) -> None:
        text = (ROOT / "SUPPORT.md").read_text(encoding="utf-8").replace(
            policy.POLICY_STATUS["stable-v1"], policy.POLICY_STATUS["stable-v1"] + "; fully supported"
        )
        self.assertTrue(policy.validate_support_text(text, {"Working"}))

    def test_missing_supported_versions_heading_is_rejected(self) -> None:
        text = (ROOT / "SUPPORT.md").read_text(encoding="utf-8").replace("## Supported Versions", "## Versions")
        self.assertTrue(policy.validate_support_text(text, {"Working"}))

    def test_unprefixed_release_claim_in_prose_is_rejected(self) -> None:
        text = (ROOT / "SUPPORT.md").read_text(encoding="utf-8") + "\nSupported release: 1.2.3\n"
        errors = policy.validate_support_text(text, {"Working"})
        self.assertTrue(any("1.2.3" in error for error in errors), errors)


class GovernanceIntegrationTests(unittest.TestCase):
    def test_legal_gate_rejects_unpublished_channel_in_security_table(self) -> None:
        """The real --legal path used to ignore an invented support row."""
        security = ROOT / "SECURITY.md"
        original_read = Path.read_text
        text = original_read(security, encoding="utf-8").replace("| `nightly` |", "| `lts` |")
        validator = site_validate.Validator(load_contract())

        def read(path, *args, **kwargs):
            return text if path == security else original_read(path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", read):
            validator.validate_legal(strict_public_wording=True)
        self.assertTrue(any("lts" in error for error in validator.errors), validator.errors)


if __name__ == "__main__":
    unittest.main()
