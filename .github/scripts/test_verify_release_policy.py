import unittest
import json
import os
import subprocess
from unittest.mock import Mock, patch
from verify_release_policy import validate_policy, verify


class ReleasePolicyTests(unittest.TestCase):
    def test_stable_and_rolling_nightly_require_mutually_exclusive_policies(self):
        for enabled in (True, False):
            policy = {"enabled": enabled, "enforced_by_owner": False}
            validate_policy(policy, enabled)
            with self.assertRaises(ValueError):
                validate_policy(policy, not enabled)

    def test_unknown_or_owner_enforced_policy_never_allows_rolling_mutation(self):
        for policy in ({}, {"enabled": "false"}, {"enabled": False}, {"enabled": False, "enforced_by_owner": True}):
            with self.assertRaises(ValueError):
                validate_policy(policy, False)

    def test_immutable_nightly_policy_is_required_when_explicitly_selected(self):
        immutable = {"enabled": True, "enforced_by_owner": False}
        validate_policy(immutable, False, immutable=True)
        with self.assertRaisesRegex(ValueError, "immutable publication"):
            validate_policy({"enabled": False, "enforced_by_owner": False}, False, immutable=True)

    def test_policy_authority_uses_separate_read_token_without_changing_mutation_token(self):
        runner = Mock(return_value=subprocess.CompletedProcess([], 0, json.dumps({"enabled": True}), ""))
        with patch.dict(os.environ, {"GH_TOKEN": "generated-write-fixture", "RELEASE_POLICY_READ_TOKEN": "generated-read-fixture"}):
            verify("owner/repo", True, runner=runner)
            self.assertEqual(os.environ["GH_TOKEN"], "generated-write-fixture")
        self.assertEqual(runner.call_args.kwargs["env"]["GH_TOKEN"], "generated-read-fixture")
        self.assertNotIn("generated-read-fixture", " ".join(runner.call_args.args[0]))

    def test_missing_policy_secret_does_not_fall_back_to_github_token(self):
        with patch.dict(os.environ, {"GH_TOKEN": "generated-write-fixture"}, clear=True):
            with self.assertRaisesRegex(ValueError, "Administration"):
                verify("owner/repo", True, runner=Mock())


if __name__ == "__main__":
    unittest.main()
