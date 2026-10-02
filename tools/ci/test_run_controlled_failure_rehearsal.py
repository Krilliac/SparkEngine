#!/usr/bin/env python3
"""Unit tests for the CI-100 controlled-failure rehearsal driver.

Every git/gh interaction is faked, so these run with no network, no gh, no
process spawn, and no repository mutation. They exercise the driver's control
flow: dry-run safety, branch/PR/evidence sequencing, gate-conclusion recording,
the "gate did not go red" failure path, and cleanup-always.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_controlled_failure_rehearsal as driver  # noqa: E402

BASE_SHA = "6a95b684f765119004b98067afa5f1c366c03223"
PATCH_SHA = "a" * 40
RUN_ID = "123456abcdef123456abcdef123456ab"


class FakeRunner(driver.CommandRunner):
    """Records every command and returns scripted gh JSON; never spawns a process."""

    def __init__(self, *, dry_run=False, gate_conclusion="failure", run_present=True,
                 fail_on=None, pr_number=4242, run_id=99887766, wrong_sha=False,
                 wrong_pr=False, wrong_job=False, wrong_step=False,
                 preexisting_local=False, preexisting_remote=False, remote_changed=False,
                 remote_repointed=False, pr_url_missing=False, push_response_failed=False,
                 pr_create_nonzero_url=False):
        super().__init__(dry_run=dry_run, echo=lambda _message: None)
        self.calls: list[tuple[tuple[str, ...], bool]] = []
        self.executed: list[tuple[tuple[str, ...], bool]] = []
        self.gate_conclusion = gate_conclusion
        self.run_present = run_present
        self.fail_on = fail_on  # substring; raise CalledProcessError when a command contains it
        self.pr_number = pr_number
        self.run_id = run_id
        self.wrong_sha = wrong_sha
        self.wrong_pr = wrong_pr
        self.wrong_job = wrong_job
        self.wrong_step = wrong_step
        self.preexisting_local = preexisting_local
        self.preexisting_remote = preexisting_remote
        self.remote_changed = remote_changed
        self.remote_repointed = remote_repointed
        self.pr_url_missing = pr_url_missing
        self.push_response_failed = push_response_failed
        self.pr_create_nonzero_url = pr_create_nonzero_url
        self.branch = None
        self.local_sha = BASE_SHA if preexisting_local else None
        self.remote_sha = BASE_SHA if preexisting_remote else None
        self.pr_state = "OPEN"

    def run(self, command, *, cwd=None, check=True, capture=True, text_input=None, mutating=False):
        self.calls.append((tuple(command), mutating))
        if self.dry_run and mutating:
            return subprocess.CompletedProcess(list(command), 0, "", "")
        self.executed.append((tuple(command), mutating))
        if command[:2] == ["git", "show-ref"]:
            return subprocess.CompletedProcess(list(command), 0 if self.local_sha else 1, "", "")
        if command[:2] == ["git", "ls-remote"]:
            ref = command[-1]
            output = f"{self.remote_sha}\t{ref}\n" if self.remote_sha else ""
            return subprocess.CompletedProcess(list(command), 0, output, "")
        if command[:3] == ["git", "switch", "-c"]:
            self.branch = command[-1]
            self.local_sha = BASE_SHA
        if command[:2] == ["git", "rev-parse"]:
            sha = PATCH_SHA if command[-1] == "HEAD" else self.local_sha
            return subprocess.CompletedProcess(list(command), 0, (sha or "") + "\n", "")
        if self.fail_on is not None and self.fail_on in " ".join(command):
            if check:
                raise subprocess.CalledProcessError(1, command, output="", stderr="boom")
            return subprocess.CompletedProcess(list(command), 1, "", "boom")
        if command[0] == "git" and "commit" in command:
            self.local_sha = PATCH_SHA
        if command[:3] == ["git", "branch", "-D"]:
            self.local_sha = None
        if (command[:2] == ["git", "push"] and self.branch is not None
                and command[-1] == f"{self.branch}:refs/heads/{self.branch}"):
            self.remote_sha = PATCH_SHA
            if self.push_response_failed:
                raise subprocess.CalledProcessError(1, command, output="", stderr="response lost")
        if command[:2] == ["git", "push"] and any(x.startswith(":refs/heads/") for x in command):
            if not self.remote_changed:
                self.remote_sha = None
        if command[:3] == ["gh", "pr", "close"]:
            self.pr_state = "CLOSED"
            if self.remote_repointed:
                self.remote_sha = BASE_SHA
        if command[:3] == ["gh", "pr", "create"]:
            self.pr_state = "OPEN"
            if self.pr_create_nonzero_url:
                return subprocess.CompletedProcess(list(command), 1, self._stdout(command), "response lost")
        return subprocess.CompletedProcess(list(command), 0, self._stdout(command), "")

    def _stdout(self, command) -> str:
        if command[:3] == ["gh", "pr", "create"]:
            if self.pr_url_missing:
                return ""
            return f"https://github.com/Krilliac/SparkEngine/pull/{self.pr_number}\n"
        if command[:3] == ["gh", "pr", "view"]:
            return json.dumps({"number": self.pr_number, "state": self.pr_state,
                               "url": f"https://github.com/Krilliac/SparkEngine/pull/{self.pr_number}",
                               "headRefName": self.branch})
        if command[:3] == ["gh", "run", "list"]:
            if not self.run_present:
                return "[]"
            return json.dumps([
                {"databaseId": self.run_id, "status": "completed",
                 "conclusion": "failure", "headSha": BASE_SHA if self.wrong_sha else PATCH_SHA,
                 "event": "pull_request"},
            ])
        if command[:2] == ["gh", "api"]:
            return json.dumps({"id": self.run_id, "head_sha": PATCH_SHA, "head_branch": self.branch,
                               "event": "pull_request", "name": driver.BUILD_WORKFLOW_NAME,
                               "pull_requests": [{"number": self.pr_number + (1 if self.wrong_pr else 0)}]})
        if command[:3] == ["gh", "run", "view"]:
            failure_class = next(k for k in driver.CLASS_PLAN if f"ci100-{k}-" in self.branch)
            _patch, _job_id, job_name, step_name = driver.CLASS_PLAN[failure_class]
            return json.dumps({
                "conclusion": "failure",
                "jobs": [
                    {"name": "build-linux-gcc" if self.wrong_job else job_name,
                     "conclusion": "failure",
                     "steps": [{"name": "Wrong step" if self.wrong_step else step_name,
                                "conclusion": "failure"}]},
                    {"name": "validate-prompts", "conclusion": "success"},
                    {"name": driver.REQUIRED_GATE_JOB, "conclusion": self.gate_conclusion},
                ],
            })
        return ""

    def commands(self) -> list[str]:
        return [" ".join(command) for command, _mutating in self.calls]

    def mutating_commands(self) -> list[str]:
        return [" ".join(command) for command, mutating in self.calls if mutating]

    def executed_commands(self) -> list[str]:
        return [" ".join(command) for command, _mutating in self.executed]


def make_config(evidence_file: Path, **overrides) -> driver.RehearsalConfig:
    params = dict(
        base_sha=BASE_SHA,
        repo="Krilliac/SparkEngine",
        evidence_file=evidence_file,
        poll_interval_seconds=0.0,
        run_timeout_seconds=1.0,
        sleep=lambda _seconds: None,
        run_id=RUN_ID,
    )
    params.update(overrides)
    return driver.RehearsalConfig(**params)


class NamingTests(unittest.TestCase):
    def test_short_sha_requires_hex(self):
        self.assertEqual(driver.short_sha(BASE_SHA), BASE_SHA[:12])
        with self.assertRaises(driver.RehearsalError):
            driver.short_sha("not-a-sha")

    def test_branch_name_format(self):
        self.assertEqual(
            driver.branch_name("format", BASE_SHA, RUN_ID),
            f"rehearsal/ci100-format-{BASE_SHA[:12]}-{RUN_ID}",
        )

    def test_every_class_has_a_real_patch(self):
        for failure_class in driver.CLASS_PLAN:
            self.assertTrue(driver.patch_path(failure_class).is_file(), failure_class)

    def test_class_plan_jobs_are_the_six_classes(self):
        self.assertEqual(
            set(driver.CLASS_PLAN),
            {"test", "sanitizer", "format", "threshold", "registration", "validation"},
        )


class DryRunTests(unittest.TestCase):
    def test_dry_run_makes_no_mutating_calls_and_writes_no_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "evidence.json"
            runner = FakeRunner(dry_run=True)
            config = make_config(evidence)
            outcomes = driver.run_rehearsal(runner, config, ["format"])
            # A dry run spawns NOTHING: no mutating command and no gh read were
            # actually executed (the plan is only printed).
            self.assertEqual(runner.executed_commands(), [])
            # The plan still records the intended mutating commands.
            self.assertTrue(any("git push --force-with-lease=" in c for c in runner.mutating_commands()))
            # gh run list/view (read-only) are never reached in a dry run.
            self.assertNotIn("gh run list", " ".join(runner.commands()))
            self.assertFalse(evidence.exists())
            self.assertEqual(len(outcomes), 1)
            self.assertFalse(outcomes[0].demonstrated)


class ExecuteHappyPathTests(unittest.TestCase):
    def test_records_red_gate_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "evidence.json"
            runner = FakeRunner(gate_conclusion="failure")
            config = make_config(evidence)
            outcomes = driver.run_rehearsal(runner, config, ["test"])
            outcome = outcomes[0]
            self.assertTrue(outcome.demonstrated)
            self.assertEqual(outcome.required_gate_conclusion, "failure")
            self.assertEqual(outcome.run_id, runner.run_id)
            self.assertIn("build-linux-gcc", outcome.failing_jobs)
            self.assertEqual(outcome.pr_number, runner.pr_number)

            commands = runner.commands()
            # Core sequence present and ordered.
            self.assertTrue(any("git worktree add" in c for c in commands))
            self.assertTrue(any("git apply" in c for c in commands))
            self.assertTrue(any("git push --force-with-lease=refs/heads/rehearsal/ci100-test-" in c
                                for c in commands))
            self.assertTrue(any(c.startswith("gh pr create") for c in commands))
            self.assertTrue(any("gh run list" in c for c in commands))
            # Cleanup verifies PR closure and deletes only our pushed SHA.
            self.assertTrue(any("gh pr close 4242" in c for c in commands))
            self.assertTrue(any("git push --force-with-lease=" in c for c in commands))
            self.assertEqual(outcome.cleanup_errors, [])
            self.assertEqual(outcome.patch_sha, PATCH_SHA)
            self.assertEqual(outcome.failing_step, "Run Tests")
            # Worktree removed.
            self.assertTrue(any("git worktree remove" in c for c in commands))

            document = json.loads(evidence.read_text(encoding="utf-8"))
            self.assertEqual(document["schemaVersion"], driver.EVIDENCE_SCHEMA_VERSION)
            record = document["classes"]["test"]
            self.assertTrue(record["demonstrated"])
            self.assertEqual(record["requiredGateConclusion"], "failure")
            self.assertEqual(record["patch"], "tools/ci/controlled-failures/test.patch")
            self.assertEqual(record["expectedJob"], "build-linux-gcc")
            self.assertEqual(record["patchSha"], PATCH_SHA)
            self.assertEqual(record["prUrl"], f"https://github.com/Krilliac/SparkEngine/pull/{runner.pr_number}")

    def test_evidence_accumulates_across_classes(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "evidence.json"
            runner = FakeRunner()
            config = make_config(evidence)
            driver.run_rehearsal(runner, config, ["format", "registration"])
            document = json.loads(evidence.read_text(encoding="utf-8"))
            self.assertEqual(set(document["classes"]), {"format", "registration"})


class ExecuteFailurePathTests(unittest.TestCase):
    def test_gate_not_red_is_not_demonstrated(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "evidence.json"
            runner = FakeRunner(gate_conclusion="success")
            config = make_config(evidence)
            outcomes = driver.run_rehearsal(runner, config, ["validation"])
            self.assertFalse(outcomes[0].demonstrated)
            self.assertEqual(outcomes[0].required_gate_conclusion, "success")

    def test_timeout_when_no_run_appears(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "evidence.json"
            runner = FakeRunner(run_present=False)
            config = make_config(evidence, run_timeout_seconds=0.0)
            outcomes = driver.run_rehearsal(runner, config, ["format"])
            self.assertIsNotNone(outcomes[0].error)
            self.assertFalse(outcomes[0].demonstrated)
            # Cleanup still ran.
            self.assertTrue(any("gh pr close" in c for c in runner.commands()))

    def test_push_failure_cleans_up_without_a_pr(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "evidence.json"
            runner = FakeRunner(fail_on="git push --force-with-lease=")
            config = make_config(evidence)
            outcomes = driver.run_rehearsal(runner, config, ["threshold"])
            self.assertIsNotNone(outcomes[0].error)
            self.assertIsNone(outcomes[0].pr_number)
            commands = runner.commands()
            # No PR was created; failed push did not establish remote ownership.
            self.assertFalse(any(c.startswith("gh pr create") for c in commands))
            self.assertFalse(any(" origin :refs/heads/" in c for c in commands))
            self.assertTrue(outcomes[0].cleanup_errors)
            # The worktree is still removed despite the failure.
            self.assertTrue(any("git worktree remove" in c for c in commands))

    def test_wrong_patch_sha_run_is_not_selected(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(wrong_sha=True)
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json",
                                           run_timeout_seconds=0), ["test"])[0]
            self.assertFalse(outcome.demonstrated)
            self.assertIsNone(outcome.run_id)

    def test_other_pr_run_is_not_selected(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(wrong_pr=True)
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json",
                                           run_timeout_seconds=0), ["test"])[0]
            self.assertFalse(outcome.demonstrated)
            self.assertIsNone(outcome.run_id)

    def test_wrong_job_or_step_cannot_demonstrate_class(self):
        for options in ({"wrong_job": True}, {"wrong_step": True}):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as temp:
                runner = FakeRunner(**options)
                outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["sanitizer"])[0]
                self.assertFalse(outcome.demonstrated)
                self.assertIsNone(outcome.failing_step)

    def test_every_class_requires_its_job_and_step(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner()
            outcomes = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"),
                                            list(driver.CLASS_PLAN))
            self.assertTrue(all(outcome.demonstrated for outcome in outcomes))
            self.assertTrue(all(not outcome.cleanup_errors for outcome in outcomes))

    def test_preexisting_local_or_remote_branch_is_refused(self):
        for options in ({"preexisting_local": True}, {"preexisting_remote": True}):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as temp:
                runner = FakeRunner(**options)
                outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["test"])[0]
                self.assertIn("already exists", outcome.error)
                self.assertFalse(any("git worktree add" in c for c in runner.commands()))
                self.assertFalse(any("--force-with-lease=" in c for c in runner.commands()))

    def test_failed_pr_close_is_reported(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(fail_on="gh pr close")
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["format"])[0]
            self.assertTrue(outcome.cleanup_errors)
            self.assertIn("PR close", outcome.cleanup_errors[0])

    def test_remote_delete_must_be_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(remote_changed=True)
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["format"])[0]
            self.assertTrue(any("remote branch deletion not verified" in e for e in outcome.cleanup_errors))

    def test_repointed_remote_is_not_deleted(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(remote_repointed=True)
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["format"])[0]
            self.assertTrue(any("remote branch changed" in e for e in outcome.cleanup_errors))
            self.assertFalse(any(" origin :refs/heads/" in c for c in runner.commands()))

    def test_missing_create_url_reports_possible_open_pr(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(pr_url_missing=True)
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["format"])[0]
            self.assertIn("no usable PR URL", outcome.error)
            self.assertTrue(any("PR identity unavailable" in e for e in outcome.cleanup_errors))

    def test_lost_push_response_can_clean_only_verified_sha(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(push_response_failed=True)
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["format"])[0]
            self.assertIsNotNone(outcome.error)
            self.assertTrue(any("--force-with-lease=" in c for c in runner.commands()))
            self.assertEqual(outcome.cleanup_errors, [])

    def test_create_url_is_retained_even_on_nonzero_result(self):
        with tempfile.TemporaryDirectory() as temp:
            runner = FakeRunner(pr_create_nonzero_url=True)
            outcome = driver.run_rehearsal(runner, make_config(Path(temp) / "e.json"), ["format"])[0]
            self.assertIsNotNone(outcome.error)
            self.assertEqual(outcome.pr_number, runner.pr_number)
            self.assertTrue(any("gh pr close 4242" in c for c in runner.commands()))
            self.assertEqual(outcome.cleanup_errors, [])


class CliTests(unittest.TestCase):
    def test_resolve_classes_all(self):
        self.assertEqual(driver.resolve_classes("all"), list(driver.CLASS_PLAN))

    def test_resolve_classes_subset(self):
        self.assertEqual(driver.resolve_classes("format,test"), ["format", "test"])

    def test_resolve_classes_rejects_unknown(self):
        with self.assertRaises(SystemExit):
            driver.resolve_classes("format,bogus")

    def test_main_dry_run_returns_zero_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "evidence.json"
            # main() builds a real CommandRunner(dry_run=True); mutating calls are skipped,
            # and gh read calls would spawn — but a dry run never reaches them.
            code = driver.main(["--base", BASE_SHA, "--classes", "format", "--evidence-file", str(evidence)])
            self.assertEqual(code, 0)
            self.assertFalse(evidence.exists())

    def test_main_rejects_bad_base(self):
        with self.assertRaises(SystemExit):
            driver.main(["--base", "nope", "--classes", "format"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
