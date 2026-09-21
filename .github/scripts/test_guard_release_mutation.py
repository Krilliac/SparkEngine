import os
import subprocess
import unittest
from unittest.mock import Mock, patch

from guard_release_mutation import guarded_run
from verify_release_policy import validate_policy


class GuardTests(unittest.TestCase):
    def test_policy_flip_between_each_phase_prevents_the_next_mutation(self):
        writes = [
            ["gh", "api", "--method", "PATCH", "repos/owner/repo/releases/1"],
            ["gh", "api", "--method", "DELETE", "repos/owner/repo/releases/assets/2"],
            ["gh", "api", "--method", "POST", "https://uploads.github.com/repos/owner/repo/releases/1/assets?name=a.zip"],
            ["git", "push", "origin", "HEAD:refs/tags/nightly"],
        ]
        for command in writes:
            policy = {"enabled": False, "enforced_by_owner": False}
            checker = lambda repository, is_versioned: validate_policy(policy, is_versioned)
            runner = Mock(return_value=subprocess.CompletedProcess(command, 0))
            guarded_run(command, "owner/repo", False, policy_check=checker, runner=runner)
            policy["enabled"] = True
            with self.assertRaises(ValueError):
                guarded_run(command, "owner/repo", False, policy_check=checker, runner=runner)
            self.assertEqual(runner.call_count, 1, command)

    def test_policy_read_credential_is_not_given_to_the_mutation_child(self):
        with patch.dict(os.environ, {"GH_TOKEN": "generated-write-fixture", "RELEASE_POLICY_READ_TOKEN": "generated-read-fixture"}):
            runner = Mock(return_value=subprocess.CompletedProcess([], 0))
            guarded_run(["git", "push", "origin", "HEAD:refs/tags/test"], "owner/repo", True,
                        runner=runner, policy_check=Mock())
        self.assertEqual(runner.call_args.kwargs["env"]["GH_TOKEN"], "generated-write-fixture")
        self.assertNotIn("RELEASE_POLICY_READ_TOKEN", runner.call_args.kwargs["env"])

    def test_guard_refuses_unrecognized_commands_and_propagates_failure(self):
        with self.assertRaises(ValueError):
            guarded_run(["sh", "-c", "anything"], "owner/repo", True)
        runner = Mock(return_value=subprocess.CompletedProcess([], 17))
        result = guarded_run(["git", "push", "origin", "HEAD"], "owner/repo", True,
                             runner=runner, policy_check=Mock())
        self.assertEqual(result.returncode, 17)


if __name__ == "__main__":
    unittest.main()
