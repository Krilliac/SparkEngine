import copy
import json
import subprocess
import unittest
from unittest.mock import Mock

from verify_release_environment import protection_errors, verify


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.environment = {
            "name": "stable-release",
            "can_admins_bypass": False,
            "protection_rules": [{"type": "required_reviewers", "prevent_self_review": False,
                                  "reviewers": [{"type": "User", "reviewer": {"id": 1}}]}],
            "deployment_branch_policy": {"protected_branches": False, "custom_branch_policies": True},
        }
        self.policies = {"total_count": 1, "branch_policies": [{"name": "Working", "type": "branch"}]}
        self.repository = {"name": "repo", "owner": {"login": "Krilliac", "id": 1}}

    def test_exact_protection_passes(self):
        self.assertEqual(protection_errors(self.environment, self.policies, self.repository), [])

    def test_verifier_fetches_only_the_expected_read_only_endpoints(self):
        runner = Mock(side_effect=[subprocess.CompletedProcess([], 0, json.dumps(value), "")
                                   for value in (self.repository, self.environment, self.policies)])
        verify("owner/repo", runner=runner)
        self.assertEqual([call.args[0] for call in runner.call_args_list], [
            ["gh", "api", "repos/owner/repo"],
            ["gh", "api", "repos/owner/repo/environments/stable-release"],
            ["gh", "api", "repos/owner/repo/environments/stable-release/deployment-branch-policies?per_page=100"],
        ])

    def test_duplicate_api_protection_fields_fail_closed(self):
        runner = Mock(return_value=subprocess.CompletedProcess([], 0, '{"name":"bad","name":"stable-release"}', ""))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            verify("owner/repo", runner=runner)

    def test_absent_reviewers_self_review_and_unrestricted_deployment_fail(self):
        for mutation in ("absent", "reviewers", "self", "branches"):
            environment = copy.deepcopy(self.environment)
            if mutation == "absent":
                environment = {}
            elif mutation == "reviewers":
                environment["protection_rules"] = []
            elif mutation == "self":
                environment["protection_rules"][0]["prevent_self_review"] = True
            else:
                environment["deployment_branch_policy"] = None
            self.assertTrue(protection_errors(environment, self.policies, self.repository), mutation)

    def test_wildcard_tag_and_extra_deployment_policy_fail(self):
        self.assertEqual(protection_errors(self.environment, self.policies, self.repository), [])
        for policy in ({"name": "*", "type": "branch"}, {"name": "Working", "type": "tag"}):
            self.assertTrue(protection_errors(self.environment, {"total_count": 1, "branch_policies": [policy]}, self.repository))
        self.policies["total_count"] = 101
        self.assertTrue(protection_errors(self.environment, self.policies, self.repository))

    def test_api_denial_never_falls_back_to_unprotected_publication(self):
        runner = Mock(return_value=subprocess.CompletedProcess([], 1, "", "forbidden"))
        with self.assertRaisesRegex(ValueError, "API denial is not an approval waiver"):
            verify("owner/repo", runner=runner)
        self.assertEqual(runner.call_count, 1)

    def test_malformed_protection_evidence_is_rejected(self):
        self.environment["protection_rules"] = None
        self.assertTrue(protection_errors(self.environment, self.policies, self.repository))

    def test_administrative_bypass_and_unknown_bypass_state_are_rejected(self):
        for value in (True, None, "false"):
            self.environment["can_admins_bypass"] = value
            self.assertTrue(protection_errors(self.environment, self.policies, self.repository), value)

    def test_non_owner_reviewer_is_rejected(self):
        environment = copy.deepcopy(self.environment)
        environment["protection_rules"][0]["reviewers"][0]["reviewer"]["id"] = 2
        self.assertTrue(protection_errors(environment, self.policies, self.repository))

    def test_owner_only_identity_is_explicit(self):
        repository = copy.deepcopy(self.repository)
        repository["owner"]["login"] = "someone-else"
        self.assertTrue(protection_errors(self.environment, self.policies, repository))


if __name__ == "__main__":
    unittest.main()
