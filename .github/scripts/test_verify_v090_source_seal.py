import json
import subprocess
import unittest
from unittest.mock import Mock

from verify_v090_source_seal import _api_variable, validate_seal, verify


SOURCE = "a" * 40
BASELINE = "b" * 40


class SourceSealTests(unittest.TestCase):
    def test_metadata_only_single_parent_seal_passes(self):
        evidence = validate_seal(SOURCE, BASELINE, [BASELINE],
                                 ["docs/site/readiness.json", "docs/readiness/work-items/00-truth-ci-release.json"], SOURCE)
        self.assertEqual(evidence["reviewed_sha"], SOURCE)

    def test_missing_or_mismatched_protected_sha_fails(self):
        for reviewed in ("", "c" * 40):
            with self.subTest(reviewed=reviewed):
                with self.assertRaisesRegex(ValueError, "reviewed SHA"):
                    validate_seal(SOURCE, BASELINE, [BASELINE], [], reviewed)

    def test_wrong_parent_fails(self):
        with self.assertRaisesRegex(ValueError, "exactly one parent"):
            validate_seal(SOURCE, BASELINE, ["c" * 40], [], SOURCE)
        with self.assertRaisesRegex(ValueError, "exactly one parent"):
            validate_seal(SOURCE, BASELINE, [BASELINE, "c" * 40], [], SOURCE)

    def test_code_change_in_seal_fails(self):
        with self.assertRaisesRegex(ValueError, "non-readiness"):
            validate_seal(SOURCE, BASELINE, [BASELINE], ["CMakeLists.txt"], SOURCE)

    def test_generated_handoff_is_allowed_seal_metadata(self):
        evidence = validate_seal(
            SOURCE, BASELINE, [BASELINE],
            ["docs/readiness/ENGINE_READINESS_HANDOFF.md"], SOURCE,
        )
        self.assertIn("docs/readiness/ENGINE_READINESS_HANDOFF.md", evidence["changed_paths"])

    def test_environment_variable_is_read_from_protected_endpoint(self):
        api = Mock(return_value=subprocess.CompletedProcess([], 0, json.dumps({"name": "SPARKENGINE_V090_REVIEWED_SHA", "value": SOURCE}), ""))
        git = Mock(side_effect=[
            subprocess.CompletedProcess([], 0, f"{SOURCE} {BASELINE}\n", ""),
            subprocess.CompletedProcess([], 0, "docs/site/readiness.json\n", ""),
        ])
        evidence = verify("owner/repo", SOURCE, BASELINE, token="secret", runner=api, git_runner=git)
        self.assertEqual(evidence["source_sha"], SOURCE)
        self.assertEqual(api.call_args.args[0][:3], ["gh", "api", "repos/owner/repo/environments/stable-release/variables/SPARKENGINE_V090_REVIEWED_SHA"])
        self.assertEqual(api.call_args.kwargs["env"]["GH_TOKEN"], "secret")

    def test_missing_environment_variable_fails_closed(self):
        api = Mock(return_value=subprocess.CompletedProcess([], 1, "", "not found"))
        git = Mock(side_effect=[
            subprocess.CompletedProcess([], 0, f"{SOURCE} {BASELINE}\n", ""),
            subprocess.CompletedProcess([], 0, "", ""),
        ])
        with self.assertRaisesRegex(ValueError, "protected stable-release source seal"):
            verify("owner/repo", SOURCE, BASELINE, token="secret", runner=api, git_runner=git)

    def test_duplicate_environment_variable_fields_fail_closed(self):
        response = (
            '{"name":"SPARKENGINE_V090_REVIEWED_SHA","value":"' + SOURCE
            + '","value":"' + ("c" * 40) + '"}'
        )
        api = Mock(return_value=subprocess.CompletedProcess([], 0, response, ""))
        with self.assertRaisesRegex(ValueError, "invalid source-seal evidence"):
            _api_variable("owner/repo", "secret", runner=api)


if __name__ == "__main__":
    unittest.main()
