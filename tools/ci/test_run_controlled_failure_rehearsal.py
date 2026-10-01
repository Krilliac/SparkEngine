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


class FakeRunner(driver.CommandRunner):
    """Records every command and returns scripted gh JSON; never spawns a process."""

    def __init__(self, *, dry_run=False, gate_conclusion="failure", run_present=True,
                 fail_on=None, pr_number=4242, run_id=99887766):
        super().__init__(dry_run=dry_run, echo=lambda _message: None)
        self.calls: list[tuple[tuple[str, ...], bool]] = []
        self.executed: list[tuple[tuple[str, ...], bool]] = []
        self.gate_conclusion = gate_conclusion
        self.run_present = run_present
        self.fail_on = fail_on  # substring; raise CalledProcessError when a command contains it
        self.pr_number = pr_number
        self.run_id = run_id

    def run(self, command, *, cwd=None, check=True, capture=True, text_input=None, mutating=False):
        self.calls.append((tuple(command), mutating))
        if self.dry_run and mutating:
            return subprocess.CompletedProcess(list(command), 0, "", "")
        self.executed.append((tuple(command), mutating))
        if self.fail_on is not None and self.fail_on in " ".join(command):
            if check:
                raise subprocess.CalledProcessError(1, command, output="", stderr="boom")
            return subprocess.CompletedProcess(list(command), 1, "", "boom")
        return subprocess.CompletedProcess(list(command), 0, self._stdout(command), "")

    def _stdout(self, command) -> str:
        joined = " ".join(command)
        if command[:3] == ["gh", "pr", "view"]:
            return json.dumps({"number": self.pr_number})
        if command[:3] == ["gh", "run", "list"]:
            if not self.run_present:
                return "[]"
            return json.dumps([
                {"databaseId": self.run_id, "status": "completed",
                 "conclusion": "failure", "headSha": BASE_SHA, "event": "pull_request"},
            ])
        if command[:3] == ["gh", "run", "view"]:
            return json.dumps({
                "conclusion": "failure",
                "jobs": [
                    {"name": "build-linux-gcc", "conclusion": "failure"},
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
            driver.branch_name("format", BASE_SHA),
            f"rehearsal/ci100-format-{BASE_SHA[:12]}",
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
            self.assertTrue(any("git push -u origin" in c for c in runner.mutating_commands()))
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
            self.assertTrue(any("git push -u origin rehearsal/ci100-test-" in c for c in commands))
            self.assertTrue(any(c.startswith("gh pr create") for c in commands))
            self.assertTrue(any("gh run list" in c for c in commands))
            # Cleanup always: PR closed with branch delete.
            self.assertTrue(any("gh pr close 4242" in c and "--delete-branch" in c for c in commands))
            # Worktree removed.
            self.assertTrue(any("git worktree remove" in c for c in commands))

            document = json.loads(evidence.read_text(encoding="utf-8"))
            self.assertEqual(document["schemaVersion"], driver.EVIDENCE_SCHEMA_VERSION)
            record = document["classes"]["test"]
            self.assertTrue(record["demonstrated"])
            self.assertEqual(record["requiredGateConclusion"], "failure")
            self.assertEqual(record["patch"], "tools/ci/controlled-failures/test.patch")
            self.assertEqual(record["expectedJob"], "build-linux-gcc")

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
            runner = FakeRunner(fail_on="git push -u origin")
            config = make_config(evidence)
            outcomes = driver.run_rehearsal(runner, config, ["threshold"])
            self.assertIsNotNone(outcomes[0].error)
            self.assertIsNone(outcomes[0].pr_number)
            commands = runner.commands()
            # No PR was created, so cleanup deletes the remote branch directly.
            self.assertFalse(any(c.startswith("gh pr create") for c in commands))
            self.assertTrue(any("git push origin --delete rehearsal/ci100-threshold-" in c for c in commands))
            # The worktree is still removed despite the failure.
            self.assertTrue(any("git worktree remove" in c for c in commands))


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
