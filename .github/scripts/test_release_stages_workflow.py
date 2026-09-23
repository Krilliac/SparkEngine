"""Verify publication authority and consumer isolation in the executable workflow."""
from pathlib import Path
import re
import unittest
import yaml


class WorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = yaml.safe_load((Path(__file__).parents[1] / "workflows/release.yml").read_text())

    def test_stable_publisher_is_bound_to_named_environment(self):
        publisher = self.workflow["jobs"]["release"]
        self.assertEqual(publisher["environment"], "${{ needs.prepare.outputs.is_versioned == 'true' && 'stable-release' || 'nightly-release' }}")
        scripts = "\n".join(step.get("run", "") for step in publisher["steps"])
        self.assertEqual(scripts.count("--require-candidate-ready"), 2)
        self.assertNotIn("--require-ready", scripts)
        self.assertEqual(scripts.count("verify_release_environment.py"), 2)
        publish = next(step["run"] for step in publisher["steps"] if step["name"] == "Publish complete stable versioned release")
        self.assertLess(publish.index("verify_release_environment.py"), publish.index("release-acceptance-gate.py"))

    def test_authority_preflight_precedes_builds(self):
        prepare = self.workflow["jobs"]["prepare"]
        preflight = next(step for step in prepare["steps"] if step["name"] == "Preflight stable publication authority before building")
        self.assertEqual(preflight["if"], "steps.meta.outputs.is_versioned == 'true'")
        self.assertEqual(preflight["run"], "python3 .github/scripts/verify_release_environment.py")

    def test_incompatible_nightly_policy_fails_before_publication_mutations(self):
        publisher = self.workflow["jobs"]["release"]
        steps = publisher["steps"]
        policy = next(step for step in steps if step["name"] == "Verify release channel policy before any publication mutation")
        self.assertNotIn("if", policy)
        self.assertEqual(policy["run"], "python3 .github/scripts/verify_release_policy.py")
        self.assertLess(steps.index(policy), next(i for i, step in enumerate(steps)
                                                if step["name"] == "Checkout canonical badge branch"))
        self.assertNotIn("RELEASE_POLICY_READ_TOKEN", publisher.get("env", {}))
        self.assertEqual(policy["env"]["RELEASE_POLICY_READ_TOKEN"], "${{ secrets.RELEASE_POLICY_READ_TOKEN }}")
        self.assertNotIn("verify_release_policy.py", str(self.workflow["jobs"]["prepare"]))

    def test_policy_secret_is_bound_only_to_steps_that_need_it(self):
        helpers = ("guard_release_mutation.py", "verify_release_policy.py", "stage_release_draft.py",
                   "release-acceptance-gate.py", "recover_release_publication.py")
        recovery = yaml.safe_load((Path(__file__).parents[1] / "workflows/release-recovery.yml").read_text())
        count = 0
        for workflow in (self.workflow, recovery):
            self.assertNotIn("RELEASE_POLICY_READ_TOKEN", workflow.get("env", {}))
            for job in workflow["jobs"].values():
                self.assertNotIn("RELEASE_POLICY_READ_TOKEN", job.get("env", {}))
                for step in job.get("steps", []):
                    needs_policy = any(helper in step.get("run", "") for helper in helpers)
                    binding = step.get("env", {}).get("RELEASE_POLICY_READ_TOKEN")
                    if needs_policy:
                        self.assertEqual(binding, "${{ secrets.RELEASE_POLICY_READ_TOKEN }}", step["name"])
                        count += 1
                    else:
                        self.assertIsNone(binding, step["name"])
                    if "uses" in step:
                        self.assertIsNone(binding, step["name"])
        self.assertEqual(count, 18)

    def test_every_shell_release_write_is_guarded_and_staging_is_not_opaque(self):
        steps = self.workflow["jobs"]["release"]["steps"]
        writes = []
        for step in steps:
            self.assertNotIn("softprops/action-gh-release", step.get("uses", ""))
            for line in step.get("run", "").splitlines():
                if re.search(r'(?:git -c .*\bpush\b|gh api --method (?:POST|PATCH|DELETE|PUT))', line):
                    writes.append(line)
                    self.assertIn("guard_release_mutation.py", line)
        self.assertEqual(len(writes), 11)
        for name in ("Stage new or interrupted stable versioned release as draft", "Stage nightly rolling release as draft"):
            step = next(step for step in steps if step["name"] == name)
            self.assertEqual(step["run"], "python3 .github/scripts/stage_release_draft.py")

    def test_immutable_stable_failure_never_uses_redraft_recovery(self):
        steps = self.workflow["jobs"]["release"]["steps"]
        stable = next(step for step in steps if step["name"] == "Publish complete stable versioned release")
        self.assertNotIn("recover_release_publication.py", stable["run"])
        self.assertIn("quarantines only a proven mutable target", stable["run"])
        recovery = next(step for step in steps if step["name"] == "Recover incomplete public release")
        self.assertEqual(recovery["if"], "failure() && needs.prepare.outputs.is_versioned == 'false'")
        attestation = next(step for step in steps if step["name"] == "Verify published release attestation as a consumer")
        self.assertEqual(attestation["if"], "needs.prepare.outputs.is_versioned == 'true'")

    def test_independent_consumer_has_no_publication_authority(self):
        consumer = self.workflow["jobs"]["verify-stable-publication"]
        self.assertEqual(consumer["needs"], ["prepare", "release"])
        self.assertEqual(consumer["permissions"], {"actions": "read", "contents": "read", "attestations": "read"})
        self.assertNotIn("environment", consumer)
        self.assertNotIn("RELEASE_POLICY_READ_TOKEN", str(consumer))
        scripts = "\n".join(step.get("run", "") for step in consumer["steps"])
        self.assertIn("verify_published_stable_release.py", scripts)
        self.assertIn("verify-exact-required-gate.py", scripts)
        self.assertNotIn("release-acceptance-gate.py", scripts)
        self.assertNotIn("git push", scripts)
        self.assertNotIn("download-artifact", str(consumer))
        self.assertIn("--source-commit", scripts)
        self.assertIn("--run-attempt", scripts)
        retained = consumer["steps"][-1]
        self.assertEqual(retained["with"]["if-no-files-found"], "error")
        self.assertGreaterEqual(retained["with"]["retention-days"], 90)

    def test_real_predecessor_requirement_is_retained(self):
        build = self.workflow["jobs"]["build-windows"]
        scripts = "\n".join(step.get("run", "") for step in build["steps"])
        self.assertIn("provision-previous-windows-msi.py", scripts)
        self.assertIn("--previous-package-manifest", scripts)

    def test_v09_uses_only_the_explicit_bootstrap_qualifier_path(self):
        build = self.workflow["jobs"]["build-windows"]
        bootstrap = next(step for step in build["steps"]
                         if step["name"].startswith("Qualify Windows v0.9.0 predecessor"))
        self.assertEqual(
            bootstrap["if"],
            "needs.prepare.outputs.is_versioned == 'true' && needs.prepare.outputs.version == '0.9.0'",
        )
        self.assertIn("qualify-windows-msi.py", bootstrap["run"])
        self.assertIn("--bootstrap-repair", bootstrap["run"])
        self.assertNotIn("--previous-packages", bootstrap["run"])
        self.assertNotIn("provision-previous-windows-msi.py", bootstrap["run"])

        v1 = next(step for step in build["steps"]
                  if step["name"] == "Qualify Windows stable MSI install upgrade rollback repair and uninstall")
        self.assertEqual(
            v1["if"],
            "needs.prepare.outputs.is_versioned == 'true' && needs.prepare.outputs.version != '0.9.0'",
        )
        self.assertIn("provision-previous-windows-msi.py", v1["run"])
        self.assertIn("--previous-package-manifest", v1["run"])

    def test_all_versioned_readiness_boundaries_select_the_matching_stage(self):
        release = self.workflow["jobs"]["release"]
        scripts = "\n".join(step.get("run", "") for step in release["steps"])
        self.assertEqual(scripts.count("--require-predecessor-candidate"), 2)
        self.assertEqual(scripts.count("--require-candidate-ready"), 2)
        self.assertIn('needs.prepare.outputs.version }}" == "0.9.0"', scripts)

        consumer = self.workflow["jobs"]["verify-stable-publication"]
        consumer_scripts = "\n".join(step.get("run", "") for step in consumer["steps"])
        self.assertIn("--require-predecessor-candidate", consumer_scripts)
        self.assertIn("--require-candidate-ready", consumer_scripts)


if __name__ == "__main__":
    unittest.main()
