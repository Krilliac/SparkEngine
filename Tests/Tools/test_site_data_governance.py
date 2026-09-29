"""Offline governance policy checks for GOV-400."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
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


FIXTURES = ROOT / "Tests/Tools/fixtures/governance"


def fixture(name: str) -> object:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


class PublishedReleasePolicyTests(unittest.TestCase):
    """GOV-400: the policy tables name the versions and channels GitHub actually publishes."""

    def setUp(self) -> None:
        self.security = (ROOT / "SECURITY.md").read_text(encoding="utf-8")
        self.support = (ROOT / "SUPPORT.md").read_text(encoding="utf-8")

    def check(self, releases: object, security: str | None = None, support: str | None = None) -> list[str]:
        return policy.validate_against_releases(security or self.security, support or self.support, releases)

    def test_current_tables_match_a_published_nightly_prerelease(self) -> None:
        self.assertEqual([], self.check(fixture("releases-nightly-only.json")))
        self.assertEqual([], policy.validate(ROOT, fixture("releases-nightly-only.json")))

    def test_nightly_row_without_a_published_nightly_is_rejected(self) -> None:
        errors = self.check(fixture("releases-none.json"))
        for label in ("SECURITY.md", "SUPPORT.md"):
            self.assertIn(
                f"{label} names an unpublished channel nightly: no nightly prerelease is published", errors
            )
        # The offline check alone passes the same tables: only publication can catch this.
        self.assertEqual([], policy.validate(ROOT))
        self.assertTrue(policy.validate(ROOT, fixture("releases-none.json")))

    def test_published_stable_release_makes_the_tables_stale(self) -> None:
        errors = self.check(fixture("releases-stable-published.json"))
        for label in ("SECURITY.md", "SUPPORT.md"):
            self.assertIn(f"{label} does not name published release v1.0.0", errors)
            self.assertIn(f"{label} says no supported version has been published, but v1.0.0 is", errors)
        # The run-identity nightly tag counts as the nightly channel; only the stable release is stale.
        self.assertEqual(4, len(errors), errors)

    def test_drafts_are_ignored_and_other_prereleases_are_named(self) -> None:
        draft_only = [{"tag_name": "nightly", "draft": True, "prerelease": True}]
        self.assertIn(
            "SUPPORT.md names an unpublished channel nightly: no nightly prerelease is published",
            self.check(draft_only),
        )
        candidate = fixture("releases-nightly-only.json") + [
            [{"tag_name": "v1.1.0-rc1", "draft": False, "prerelease": True}]
        ]
        self.assertIn("SECURITY.md does not name published prerelease v1.1.0-rc1", self.check(candidate))
        # A tag that only resembles the run-identity pattern is not a nightly.
        lookalike = [{"tag_name": "nightly-0-1-0123456789ab", "draft": False, "prerelease": True}]
        self.assertIn("SUPPORT.md does not name published prerelease nightly-0-1-0123456789ab", self.check(lookalike))

    def test_published_nightly_must_keep_its_row(self) -> None:
        support = "\n".join(line for line in self.support.splitlines() if not line.startswith("| `nightly` |"))
        errors = self.check(fixture("releases-nightly-only.json"), support=support)
        self.assertEqual(["SUPPORT.md must declare the published nightly prerelease channel"], errors)

    def test_malformed_release_lists_are_errors_not_empty(self) -> None:
        for malformed in (
            {"message": "Bad credentials"},
            None,
            [{"tag_name": "nightly", "draft": False}],
            [{"tag_name": "", "draft": False, "prerelease": True}],
            [{"tag_name": "nightly", "draft": "false", "prerelease": True}],
            ["nightly"],
        ):
            with self.subTest(releases=malformed), self.assertRaises(ValueError):
                self.check(malformed)

    def run_policy(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(ROOT / "tools/site-data/policy.py"), *arguments],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )

    def test_cli_accepts_the_current_tables_for_the_published_nightly(self) -> None:
        result = self.run_policy("--published-releases", str(FIXTURES / "releases-nightly-only.json"))
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("match the published releases", result.stdout)

    def test_cli_rejects_stale_tables_and_unusable_release_files(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            empty = Path(raw) / "empty.json"
            empty.write_bytes(b"")
            broken = Path(raw) / "broken.json"
            broken.write_text('{"message": "API rate limit exceeded"}', encoding="utf-8")
            for path, fragment in (
                (FIXTURES / "releases-none.json", "names an unpublished channel nightly"),
                (FIXTURES / "releases-stable-published.json", "does not name published release v1.0.0"),
                (empty, "cannot check the policy tables against published releases"),
                (broken, "published releases must be a JSON array"),
                (Path(raw) / "missing.json", "cannot check the policy tables against published releases"),
            ):
                with self.subTest(releases=path.name):
                    result = self.run_policy("--published-releases", str(path))
                    self.assertEqual(1, result.returncode, result.stdout + result.stderr)
                    self.assertIn(fragment, result.stdout)

    def test_license_compliance_job_feeds_the_live_release_list(self) -> None:
        try:
            import yaml  # noqa: PLC0415
        except ImportError:
            self.skipTest("PyYAML is not installed")
        workflow = yaml.safe_load((ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8"))
        job = workflow["jobs"]["license-compliance"]
        self.assertEqual({"contents": "read"}, job["permissions"])
        steps = [step for step in job["steps"] if step.get("name") == "Check support policy against published releases"]
        self.assertEqual(1, len(steps))
        check = steps[0]
        self.assertNotIn("if", check)
        self.assertNotIn("continue-on-error", check)
        self.assertEqual("${{ github.token }}", check["env"]["GH_TOKEN"])
        run = check["run"]
        self.assertTrue(run.startswith("set -euo pipefail\n"))
        self.assertIn('gh api --paginate --slurp "repos/${GITHUB_REPOSITORY}/releases" > "$RELEASES_JSON"', run)
        self.assertIn("exit 1", run)
        self.assertTrue(run.rstrip().endswith('python3 tools/site-data/policy.py --published-releases "$RELEASES_JSON"'))
        self.assertNotIn("|| true", run)


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
