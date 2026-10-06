#!/usr/bin/env python3
"""CI-100 controlled-failure rehearsal driver.

Produces the hosted evidence CI-100[0] needs: a controlled test, sanitizer,
format, threshold, registration, and validation failure each turning the
``Required CI Gate`` red, *without* changing production CI behaviour. For each
failure class it:

1. creates a unique throwaway branch ``rehearsal/ci100-<class>-<shortsha>-<run-id>`` from a given
   base SHA,
2. applies the matching patch from ``tools/ci/controlled-failures/<class>.patch``,
3. pushes it and opens a DRAFT pull request against ``Working`` labelled
   ``do-not-merge``,
4. waits for that branch's ``Build SparkEngine`` run to finish,
5. records the exact patch SHA, PR, run id, expected failing job/step, and ``Required CI Gate``
   conclusion into ``docs/readiness/evidence/ci100-controlled-failures.json``,
6. then closes the PR and deletes the branch.

Nothing here is a production gate: the branches are draft, labelled
do-not-merge, and deleted. The committed patches never reach ``Working``.

SAFETY: the default is a dry run that only prints the plan. Mutating the remote
requires ``--execute`` and a GitHub CLI (``gh``) already authenticated by the
operator. This script does not weaken, bypass, or reconfigure any gate.

Every git/gh call goes through :class:`CommandRunner`, so the unit tests in
``tools/ci/test_run_controlled_failure_rehearsal.py`` drive the whole flow with a
fake runner and no network, process, or repository mutation.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import re
import subprocess
import sys
import tempfile
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]
PATCH_DIR = REPO_ROOT / "tools" / "ci" / "controlled-failures"
DEFAULT_EVIDENCE = REPO_ROOT / "docs" / "readiness" / "evidence" / "ci100-controlled-failures.json"
DEFAULT_REPO = "Krilliac/SparkEngine"
DEFAULT_BASE_BRANCH = "Working"
BUILD_WORKFLOW_NAME = "Build SparkEngine"
REQUIRED_GATE_JOB = "Required CI Gate"
DO_NOT_MERGE_LABEL = "do-not-merge"
EVIDENCE_SCHEMA_VERSION = 1

# class -> patch, required job id, displayed job name, failing step name.
CLASS_PLAN: dict[str, tuple[str, str, str, str]] = {
    "test": ("test.patch", "build-linux-gcc", "build-linux-gcc", "Run Tests"),
    "sanitizer": ("sanitizer.patch", "build-linux-asan", "build-linux-asan", "Run Tests under ASan + UBSan + LSan"),
    "format": ("format.patch", "check-format", "check-format", "Check formatting"),
    "threshold": ("threshold.patch", "coverage", "Code Coverage (GCC + per-subsystem thresholds)",
                  "Per-subsystem coverage analysis"),
    "registration": ("registration.patch", "validate-ci-tools", "Validate CI tooling",
                     "Verify every test source is registered"),
    "validation": ("validation.patch", "docs-health", "Documentation exact-currentness",
                   "Run hostile documentation contract tests"),
}


class RehearsalError(Exception):
    """A rehearsal step failed in a way that stops this class (cleanup still runs)."""


@dataclass
class CommandRunner:
    """The only process boundary. Tests subclass or replace :meth:`run`."""

    dry_run: bool = False
    echo: Callable[[str], None] = print

    def run(
        self,
        command: Sequence[str],
        *,
        cwd: Optional[Path] = None,
        check: bool = True,
        capture: bool = True,
        text_input: Optional[str] = None,
        mutating: bool = False,
    ) -> subprocess.CompletedProcess:
        """Run one git/gh command. A mutating command is skipped on a dry run."""

        printable = " ".join(command)
        if self.dry_run and mutating:
            self.echo(f"DRY-RUN (skipped): {printable}")
            return subprocess.CompletedProcess(list(command), 0, "", "")
        self.echo(f"$ {printable}")
        return subprocess.run(
            list(command),
            cwd=str(cwd) if cwd else None,
            check=check,
            capture_output=capture,
            text=True,
            input=text_input,
            timeout=600,
        )


@dataclass
class RehearsalConfig:
    base_sha: str
    repo: str = DEFAULT_REPO
    base_branch: str = DEFAULT_BASE_BRANCH
    evidence_file: Path = DEFAULT_EVIDENCE
    poll_interval_seconds: float = 30.0
    run_timeout_seconds: float = 7200.0
    sleep: Callable[[float], None] = time.sleep
    now: Callable[[], _dt.datetime] = lambda: _dt.datetime.now(_dt.timezone.utc)
    run_id: str = field(default_factory=lambda: uuid.uuid4().hex)


@dataclass
class ClassOutcome:
    failure_class: str
    patch: str
    expected_job: str
    branch: str
    base_sha: str
    patch_sha: Optional[str] = None
    pr_number: Optional[int] = None
    pr_url: Optional[str] = None
    run_id: Optional[int] = None
    run_conclusion: Optional[str] = None
    required_gate_conclusion: Optional[str] = None
    failing_jobs: list[str] = field(default_factory=list)
    failing_step: Optional[str] = None
    demonstrated: bool = False
    cleanup_errors: list[str] = field(default_factory=list)
    local_owned: bool = False
    remote_owned: bool = False
    pr_create_attempted: bool = False
    recorded_at: Optional[str] = None
    error: Optional[str] = None

    def to_record(self) -> dict:
        record = {
            "class": self.failure_class,
            "patch": f"tools/ci/controlled-failures/{self.patch}",
            "expectedJob": self.expected_job,
            "branch": self.branch,
            "baseSha": self.base_sha,
            "patchSha": self.patch_sha,
            "prNumber": self.pr_number,
            "prUrl": self.pr_url,
            "runId": self.run_id,
            "runConclusion": self.run_conclusion,
            "requiredGateConclusion": self.required_gate_conclusion,
            "failingJobs": sorted(self.failing_jobs),
            "failingStep": self.failing_step,
            "demonstrated": self.demonstrated,
            "cleanupErrors": self.cleanup_errors,
            "recordedAt": self.recorded_at,
        }
        if self.error:
            record["error"] = self.error
        return record


def short_sha(base_sha: str) -> str:
    cleaned = base_sha.strip()
    if len(cleaned) < 7 or any(character not in "0123456789abcdefABCDEF" for character in cleaned):
        raise RehearsalError(f"base SHA must be a hex commit id, got {base_sha!r}")
    return cleaned[:12]


def branch_name(failure_class: str, base_sha: str, run_id: str) -> str:
    if not re.fullmatch(r"[0-9a-f]{32}", run_id):
        raise RehearsalError("run id must be a lowercase UUID hex string")
    return f"rehearsal/ci100-{failure_class}-{short_sha(base_sha)}-{run_id}"


def patch_path(failure_class: str) -> Path:
    filename = CLASS_PLAN[failure_class][0]
    path = PATCH_DIR / filename
    if not path.is_file():
        raise RehearsalError(f"missing patch for {failure_class}: {path}")
    return path


def _gh_json(runner: CommandRunner, args: Sequence[str]) -> object:
    result = runner.run(["gh", *args], mutating=False)
    stdout = (result.stdout or "").strip()
    if not stdout:
        return None
    try:
        return json.loads(stdout)
    except json.JSONDecodeError as error:
        raise RehearsalError(f"gh returned unparseable JSON for {' '.join(args)}: {error}") from error


def ensure_label(runner: CommandRunner, repo: str) -> None:
    """Best-effort: the do-not-merge label marks the draft PR. A missing label
    must not abort the rehearsal, but the operator is warned."""

    try:
        runner.run(
            ["gh", "label", "create", DO_NOT_MERGE_LABEL, "--repo", repo,
             "--color", "B60205", "--description", "CI-100 rehearsal branch; never merge",
             "--force"],
            mutating=True,
        )
    except subprocess.CalledProcessError:
        runner.echo(f"::warning:: could not ensure '{DO_NOT_MERGE_LABEL}' label on {repo}; PR will be opened without it")


def remote_branch_sha(runner: CommandRunner, branch: str) -> Optional[str]:
    ref = f"refs/heads/{branch}"
    result = runner.run(["git", "ls-remote", "--heads", "origin", ref], cwd=REPO_ROOT)
    lines = (result.stdout or "").strip().splitlines()
    if not lines:
        return None
    if len(lines) != 1:
        raise RehearsalError(f"ambiguous remote ref {ref}")
    parts = lines[0].split()
    if len(parts) != 2 or parts[1] != ref or not re.fullmatch(r"[0-9a-fA-F]{40}", parts[0]):
        raise RehearsalError(f"invalid remote ref response for {ref}")
    return parts[0].lower()


def create_branch_with_patch(
    runner: CommandRunner, config: RehearsalConfig, outcome: ClassOutcome, work_dir: Path
) -> None:
    """Create a fresh branch and push its exact patch commit."""

    branch = outcome.branch
    failure_class = outcome.failure_class
    patch = patch_path(failure_class)
    worktree = work_dir / f"wt-{failure_class}"
    if runner.dry_run:
        for command in (
            ["git", "fetch", "origin", config.base_sha],
            ["git", "worktree", "add", "--detach", str(worktree), config.base_sha],
            ["git", "switch", "-c", branch],
            ["git", "apply", "--index", "--whitespace=nowarn", str(patch)],
            ["git", "commit", "-m", f"ci100 rehearsal: controlled {failure_class} failure"],
            ["git", "push", f"--force-with-lease=refs/heads/{branch}:", "origin",
             f"{branch}:refs/heads/{branch}"],
            ["git", "worktree", "remove", "--force", str(worktree)],
        ):
            runner.run(command, mutating=True)
        return
    local = runner.run(["git", "show-ref", "--verify", "--quiet", f"refs/heads/{branch}"],
                       cwd=REPO_ROOT, check=False)
    if local.returncode != 1:
        if local.returncode == 0:
            raise RehearsalError(f"local branch already exists: {branch}")
        raise RehearsalError(f"could not check local branch {branch}: exit {local.returncode}")
    if remote_branch_sha(runner, branch) is not None:
        raise RehearsalError(f"remote branch already exists: {branch}")
    runner.run(["git", "fetch", "origin", config.base_sha], cwd=REPO_ROOT, mutating=True)
    runner.run(
        ["git", "worktree", "add", "--detach", str(worktree), config.base_sha],
        cwd=REPO_ROOT, mutating=True,
    )
    try:
        runner.run(["git", "switch", "-c", branch], cwd=worktree, mutating=True)
        outcome.local_owned = True
        runner.run(["git", "apply", "--index", "--whitespace=nowarn", str(patch)],
                   cwd=worktree, mutating=True)
        runner.run(
            ["git", "-c", "user.name=ci100-rehearsal", "-c", "user.email=ci100-rehearsal@users.noreply.github.com",
             "commit", "-m", f"ci100 rehearsal: controlled {failure_class} failure (do not merge)"],
            cwd=worktree, mutating=True,
        )
        sha = runner.run(["git", "rev-parse", "HEAD"], cwd=worktree).stdout.strip().lower()
        if not re.fullmatch(r"[0-9a-f]{40}", sha):
            raise RehearsalError(f"invalid patch commit SHA: {sha!r}")
        outcome.patch_sha = sha
        try:
            runner.run(["git", "push", f"--force-with-lease=refs/heads/{branch}:", "origin",
                        f"{branch}:refs/heads/{branch}"], cwd=worktree, mutating=True)
        except subprocess.SubprocessError:
            # A push may reach the server even if its response fails locally.
            outcome.remote_owned = remote_branch_sha(runner, branch) == outcome.patch_sha
            raise
        if remote_branch_sha(runner, branch) != outcome.patch_sha:
            raise RehearsalError(f"pushed branch SHA does not match patch commit: {branch}")
        outcome.remote_owned = True
    finally:
        result = runner.run(["git", "worktree", "remove", "--force", str(worktree)],
                            cwd=REPO_ROOT, check=False, mutating=True)
        if result.returncode != 0:
            outcome.cleanup_errors.append(f"temporary worktree removal failed: {result.returncode}")


def open_draft_pr(runner: CommandRunner, config: RehearsalConfig, outcome: ClassOutcome) -> None:
    failure_class, branch = outcome.failure_class, outcome.branch
    title = f"[CI-100 rehearsal] controlled {failure_class} failure (do not merge)"
    body = (
        f"Automated CI-100[0] controlled-failure rehearsal for the **{failure_class}** class.\n\n"
        f"Patch: `tools/ci/controlled-failures/{CLASS_PLAN[failure_class][0]}`\n"
        f"Expected failing job: `{CLASS_PLAN[failure_class][1]}`\n\n"
        "This draft PR exists only to prove the Required CI Gate turns red. It is "
        "labelled `do-not-merge` and is closed automatically by the driver.\n"
    )
    outcome.pr_create_attempted = True
    result = runner.run(
        ["gh", "pr", "create", "--repo", config.repo, "--draft", "--base", config.base_branch,
         "--head", branch, "--title", title, "--body", body, "--label", DO_NOT_MERGE_LABEL],
        check=False, mutating=True,
    )
    if runner.dry_run:
        return
    url = (result.stdout or "").strip()
    match = re.fullmatch(rf"https://github\.com/{re.escape(config.repo)}/pull/([1-9][0-9]*)/?", url)
    outcome.pr_url = url if match else None
    if not match:
        raise RehearsalError(f"gh pr create returned no usable PR URL: {url!r}")
    outcome.pr_number = int(match.group(1))
    if result.returncode != 0:
        raise RehearsalError(f"gh pr create returned exit {result.returncode} after reporting {url}")


def wait_for_run(runner: CommandRunner, config: RehearsalConfig, outcome: ClassOutcome) -> dict:
    """Poll for this PR's Build run at exactly the pushed patch commit."""

    deadline = config.run_timeout_seconds
    waited = 0.0
    while True:
        runs = _gh_json(
            runner,
            ["run", "list", "--repo", config.repo, "--branch", outcome.branch,
             "--event", "pull_request", "--workflow", BUILD_WORKFLOW_NAME, "--json",
             "databaseId,status,conclusion,headSha,event", "--limit", "20"],
        )
        for run in runs or []:
            if not isinstance(run, dict) or run.get("event") != "pull_request":
                continue
            if run.get("headSha") != outcome.patch_sha or not isinstance(run.get("databaseId"), int):
                continue
            run_id = run["databaseId"]
            detail = _gh_json(runner, ["api", f"repos/{config.repo}/actions/runs/{run_id}"])
            if not isinstance(detail, dict):
                continue
            prs = detail.get("pull_requests")
            if (detail.get("id") != run_id or detail.get("head_sha") != outcome.patch_sha
                    or detail.get("head_branch") != outcome.branch
                    or detail.get("event") != "pull_request" or detail.get("name") != BUILD_WORKFLOW_NAME
                    or not isinstance(prs, list)
                    or outcome.pr_number not in [pr.get("number") for pr in prs if isinstance(pr, dict)]):
                continue
            if run.get("status") == "completed":
                return run
        if waited >= deadline:
            raise RehearsalError(
                f"timed out after {deadline:.0f}s waiting for this PR's Build run at {outcome.patch_sha}"
            )
        if config.poll_interval_seconds <= 0:
            raise RehearsalError("poll interval must be positive while waiting for a run")
        config.sleep(config.poll_interval_seconds)
        waited += config.poll_interval_seconds


def collect_run_evidence(
    runner: CommandRunner, config: RehearsalConfig, run_id: int, failure_class: str
) -> tuple[list[str], Optional[str], bool]:
    data = _gh_json(
        runner,
        ["run", "view", str(run_id), "--repo", config.repo, "--json", "jobs,conclusion"],
    )
    failing: list[str] = []
    gate_conclusion: Optional[str] = None
    expected_failed = False
    _patch, _job_id, display_name, expected_step = CLASS_PLAN[failure_class]
    jobs = data.get("jobs") if isinstance(data, dict) else None
    for job in jobs or []:
        if not isinstance(job, dict):
            continue
        name = job.get("name", "")
        conclusion = job.get("conclusion")
        if name == REQUIRED_GATE_JOB:
            gate_conclusion = conclusion
        if conclusion == "failure":
            failing.append(name)
        if (name == display_name or name.startswith(display_name + " (")) and conclusion == "failure":
            steps = job.get("steps")
            expected_failed = expected_failed or any(
                isinstance(step, dict) and step.get("name") == expected_step
                and step.get("conclusion") == "failure" for step in (steps or [])
            )
    return failing, gate_conclusion, expected_failed


def load_evidence(evidence_file: Path) -> dict:
    if evidence_file.is_file():
        try:
            document = json.loads(evidence_file.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            document = {}
    else:
        document = {}
    if not isinstance(document, dict):
        document = {}
    document.setdefault("schemaVersion", EVIDENCE_SCHEMA_VERSION)
    document.setdefault("description",
                        "CI-100[0] controlled-failure rehearsal evidence. Each class records the "
                        "hosted Build run whose controlled failure turned the Required CI Gate red. "
                        "Written by tools/ci/run_controlled_failure_rehearsal.py.")
    records = document.get("classes")
    if not isinstance(records, dict):
        document["classes"] = {}
    return document


def write_evidence(evidence_file: Path, document: dict, outcome: ClassOutcome) -> None:
    document["classes"][outcome.failure_class] = outcome.to_record()
    document["baseSha"] = outcome.base_sha
    evidence_file.parent.mkdir(parents=True, exist_ok=True)
    serialized = json.dumps(document, indent=2, sort_keys=True) + "\n"
    evidence_file.write_text(serialized, encoding="utf-8", newline="\n")


def rehearse_class(
    runner: CommandRunner, config: RehearsalConfig, failure_class: str, work_dir: Path
) -> ClassOutcome:
    patch_file, expected_job, _display_name, expected_step = CLASS_PLAN[failure_class]
    outcome = ClassOutcome(
        failure_class=failure_class,
        patch=patch_file,
        expected_job=expected_job,
        branch=branch_name(failure_class, config.base_sha, config.run_id),
        base_sha=config.base_sha,
    )
    try:
        create_branch_with_patch(runner, config, outcome, work_dir)
        ensure_label(runner, config.repo)
        open_draft_pr(runner, config, outcome)
        if runner.dry_run:
            runner.echo(f"DRY-RUN: would wait for the Build run and record evidence for {failure_class}")
            outcome.recorded_at = config.now().isoformat()
            return outcome
        run = wait_for_run(runner, config, outcome)
        outcome.run_id = int(run["databaseId"])
        outcome.run_conclusion = run.get("conclusion")
        failing, gate, step_failed = collect_run_evidence(runner, config, outcome.run_id, failure_class)
        outcome.failing_jobs = failing
        outcome.required_gate_conclusion = gate
        outcome.failing_step = expected_step if step_failed else None
        outcome.demonstrated = gate == "failure" and step_failed and outcome.run_conclusion == "failure"
        outcome.recorded_at = config.now().isoformat()
    except (RehearsalError, subprocess.SubprocessError) as error:
        outcome.error = str(error)
        outcome.recorded_at = config.now().isoformat()
    finally:
        if not runner.dry_run:
            _cleanup(runner, config, outcome)
    return outcome


def _cleanup(runner: CommandRunner, config: RehearsalConfig, outcome: ClassOutcome) -> None:
    """Close the created PR and delete only refs still at our recorded commit."""

    pr_ref = str(outcome.pr_number) if outcome.pr_number is not None else outcome.pr_url
    if pr_ref:
        try:
            closed = runner.run(["gh", "pr", "close", pr_ref, "--repo", config.repo,
                                 "--comment", "CI-100 rehearsal complete; closing the do-not-merge draft."],
                                check=False, mutating=True)
            detail = _gh_json(runner, ["pr", "view", pr_ref, "--repo", config.repo,
                                       "--json", "state,number,url,headRefName"])
            if (closed.returncode != 0 or not isinstance(detail, dict) or detail.get("state") != "CLOSED"
                    or detail.get("headRefName") != outcome.branch
                    or detail.get("url") != outcome.pr_url
                    or (outcome.pr_number is not None and detail.get("number") != outcome.pr_number)):
                outcome.cleanup_errors.append(f"PR close could not be verified: {pr_ref}")
        except (RehearsalError, subprocess.SubprocessError) as error:
            outcome.cleanup_errors.append(f"PR close verification failed: {error}")
    elif outcome.pr_create_attempted:
        outcome.cleanup_errors.append("PR identity unavailable after creation attempt; check for an open draft PR")

    if outcome.remote_owned and outcome.patch_sha:
        ref = f"refs/heads/{outcome.branch}"
        try:
            current = remote_branch_sha(runner, outcome.branch)
            if current != outcome.patch_sha:
                outcome.cleanup_errors.append(f"remote branch changed; refusing deletion: {outcome.branch}")
            else:
                deleted = runner.run(["git", "push", f"--force-with-lease={ref}:{outcome.patch_sha}",
                                      "origin", f":{ref}"], cwd=REPO_ROOT, check=False, mutating=True)
                if deleted.returncode != 0 or remote_branch_sha(runner, outcome.branch) is not None:
                    outcome.cleanup_errors.append(f"remote branch deletion not verified: {outcome.branch}")
        except (RehearsalError, subprocess.SubprocessError) as error:
            outcome.cleanup_errors.append(f"remote branch cleanup failed: {error}")
    elif outcome.patch_sha:
        outcome.cleanup_errors.append(f"remote branch ownership unverified; inspect {outcome.branch} manually")

    if outcome.local_owned:
        try:
            local = runner.run(["git", "rev-parse", f"refs/heads/{outcome.branch}"], cwd=REPO_ROOT)
            if (local.stdout or "").strip().lower() != outcome.patch_sha:
                outcome.cleanup_errors.append(f"local branch SHA unverified; refusing deletion: {outcome.branch}")
            else:
                deleted = runner.run(["git", "branch", "-D", outcome.branch],
                                     cwd=REPO_ROOT, check=False, mutating=True)
                remaining = runner.run(["git", "show-ref", "--verify", "--quiet",
                                        f"refs/heads/{outcome.branch}"], cwd=REPO_ROOT, check=False)
                if deleted.returncode != 0 or remaining.returncode != 1:
                    outcome.cleanup_errors.append(f"local branch deletion failed: {outcome.branch}")
        except subprocess.SubprocessError as error:
            outcome.cleanup_errors.append(f"local branch cleanup failed: {error}")


def run_rehearsal(
    runner: CommandRunner, config: RehearsalConfig, classes: Sequence[str]
) -> list[ClassOutcome]:
    outcomes: list[ClassOutcome] = []
    if runner.dry_run:
        # The planned paths are printed, never created.
        work_dir = REPO_ROOT / ".ci100-dry-run"
        return [rehearse_class(runner, config, failure_class, work_dir) for failure_class in classes]
    document = load_evidence(config.evidence_file)
    with tempfile.TemporaryDirectory(prefix="ci100-rehearsal-") as temp:
        work_dir = Path(temp)
        for failure_class in classes:
            outcome = rehearse_class(runner, config, failure_class, work_dir)
            outcomes.append(outcome)
            if not runner.dry_run:
                write_evidence(config.evidence_file, document, outcome)
                if outcome.cleanup_errors:
                    break
    return outcomes


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", required=True, help="base commit SHA to branch each rehearsal from")
    parser.add_argument("--repo", default=DEFAULT_REPO, help="owner/name of the GitHub repository")
    parser.add_argument("--base-branch", default=DEFAULT_BASE_BRANCH, help="PR base branch (default Working)")
    parser.add_argument(
        "--classes",
        default="all",
        help="comma-separated subset of: " + ",".join(CLASS_PLAN) + " (default all)",
    )
    parser.add_argument("--evidence-file", type=Path, default=DEFAULT_EVIDENCE)
    parser.add_argument("--poll-interval", type=float, default=30.0, help="seconds between run-status polls")
    parser.add_argument("--run-timeout", type=float, default=7200.0, help="seconds to wait for a Build run")
    parser.add_argument(
        "--execute",
        action="store_true",
        help="actually push branches, open PRs, and record evidence. Without it this is a dry run.",
    )
    return parser.parse_args(argv)


def resolve_classes(selector: str) -> list[str]:
    if selector.strip() == "all":
        return list(CLASS_PLAN)
    chosen = [piece.strip() for piece in selector.split(",") if piece.strip()]
    unknown = [piece for piece in chosen if piece not in CLASS_PLAN]
    if unknown:
        raise SystemExit(f"unknown failure class(es): {', '.join(unknown)}")
    return chosen


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    classes = resolve_classes(args.classes)
    config = RehearsalConfig(
        base_sha=args.base,
        repo=args.repo,
        base_branch=args.base_branch,
        evidence_file=args.evidence_file,
        poll_interval_seconds=args.poll_interval,
        run_timeout_seconds=args.run_timeout,
    )
    runner = CommandRunner(dry_run=not args.execute)
    if runner.dry_run:
        print("=== DRY RUN — no branches, PRs, or evidence will be created. Pass --execute to run. ===")
    else:
        print("=== EXECUTE — creating draft rehearsal PRs; requires an authenticated gh. ===")
    try:
        short_sha(args.base)
    except RehearsalError as error:
        raise SystemExit(str(error))
    outcomes = run_rehearsal(runner, config, classes)
    print("\n=== Rehearsal summary ===")
    exit_code = 0
    for outcome in outcomes:
        status = ("DRY-RUN" if runner.dry_run else "CLEANUP FAILED" if outcome.cleanup_errors
                  else "RED (ok)" if outcome.demonstrated else "NOT RED")
        detail = outcome.error or (f"gate={outcome.required_gate_conclusion} run={outcome.run_id} "
                                   f"expected={outcome.expected_job} failingStep={outcome.failing_step}")
        if outcome.cleanup_errors:
            detail += f"; cleanup: {'; '.join(outcome.cleanup_errors)}"
        print(f"  {outcome.failure_class:<13} {status:<14} {detail}")
        if not runner.dry_run and (not outcome.demonstrated or outcome.cleanup_errors):
            exit_code = 1
    if not runner.dry_run:
        print(f"\nEvidence written to {config.evidence_file}")
        if len(outcomes) < len(classes):
            print("Stopped after a cleanup failure; remaining classes were not started.")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
