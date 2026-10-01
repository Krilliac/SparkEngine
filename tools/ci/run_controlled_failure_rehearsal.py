#!/usr/bin/env python3
"""CI-100 controlled-failure rehearsal driver.

Produces the hosted evidence CI-100[0] needs: a controlled test, sanitizer,
format, threshold, registration, and validation failure each turning the
``Required CI Gate`` red, *without* changing production CI behaviour. For each
failure class it:

1. creates a throwaway branch ``rehearsal/ci100-<class>-<shortsha>`` from a given
   base SHA,
2. applies the matching patch from ``tools/ci/controlled-failures/<class>.patch``,
3. pushes it and opens a DRAFT pull request against ``Working`` labelled
   ``do-not-merge``,
4. waits for that branch's ``Build SparkEngine`` run to finish,
5. records the run id, the failing job(s), and the ``Required CI Gate``
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
import subprocess
import sys
import tempfile
import time
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

# class -> (patch filename, the build.yml job expected to turn red). The job is
# documentation for the recorded evidence; the gate turning red is the proof.
CLASS_PLAN: dict[str, tuple[str, str]] = {
    "test": ("test.patch", "build-linux-gcc"),
    "sanitizer": ("sanitizer.patch", "build-linux-asan"),
    "format": ("format.patch", "check-format"),
    "threshold": ("threshold.patch", "coverage"),
    "registration": ("registration.patch", "validate-ci-tools"),
    "validation": ("validation.patch", "docs-health"),
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


@dataclass
class ClassOutcome:
    failure_class: str
    patch: str
    expected_job: str
    branch: str
    base_sha: str
    pr_number: Optional[int] = None
    run_id: Optional[int] = None
    run_conclusion: Optional[str] = None
    required_gate_conclusion: Optional[str] = None
    failing_jobs: list[str] = field(default_factory=list)
    demonstrated: bool = False
    recorded_at: Optional[str] = None
    error: Optional[str] = None

    def to_record(self) -> dict:
        record = {
            "class": self.failure_class,
            "patch": f"tools/ci/controlled-failures/{self.patch}",
            "expectedJob": self.expected_job,
            "branch": self.branch,
            "baseSha": self.base_sha,
            "prNumber": self.pr_number,
            "runId": self.run_id,
            "runConclusion": self.run_conclusion,
            "requiredGateConclusion": self.required_gate_conclusion,
            "failingJobs": sorted(self.failing_jobs),
            "demonstrated": self.demonstrated,
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


def branch_name(failure_class: str, base_sha: str) -> str:
    return f"rehearsal/ci100-{failure_class}-{short_sha(base_sha)}"


def patch_path(failure_class: str) -> Path:
    filename, _job = CLASS_PLAN[failure_class]
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


def create_branch_with_patch(
    runner: CommandRunner, config: RehearsalConfig, failure_class: str, work_dir: Path
) -> str:
    """Create the rehearsal branch at the base SHA with only the class patch applied."""

    branch = branch_name(failure_class, config.base_sha)
    patch = patch_path(failure_class)
    worktree = work_dir / f"wt-{failure_class}"

    runner.run(["git", "fetch", "origin", config.base_sha], cwd=REPO_ROOT, mutating=True)
    runner.run(
        ["git", "worktree", "add", "--detach", str(worktree), config.base_sha],
        cwd=REPO_ROOT, mutating=True,
    )
    try:
        runner.run(["git", "switch", "-c", branch], cwd=worktree, mutating=True)
        runner.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=worktree, mutating=True)
        runner.run(["git", "add", "-A"], cwd=worktree, mutating=True)
        runner.run(
            ["git", "-c", "user.name=ci100-rehearsal", "-c", "user.email=ci100-rehearsal@users.noreply.github.com",
             "commit", "-m", f"ci100 rehearsal: controlled {failure_class} failure (do not merge)"],
            cwd=worktree, mutating=True,
        )
        runner.run(["git", "push", "-u", "origin", branch, "--force-with-lease"], cwd=worktree, mutating=True)
    finally:
        runner.run(["git", "worktree", "remove", "--force", str(worktree)], cwd=REPO_ROOT, check=False, mutating=True)
    return branch


def open_draft_pr(runner: CommandRunner, config: RehearsalConfig, failure_class: str, branch: str) -> Optional[int]:
    title = f"[CI-100 rehearsal] controlled {failure_class} failure (do not merge)"
    body = (
        f"Automated CI-100[0] controlled-failure rehearsal for the **{failure_class}** class.\n\n"
        f"Patch: `tools/ci/controlled-failures/{CLASS_PLAN[failure_class][0]}`\n"
        f"Expected failing job: `{CLASS_PLAN[failure_class][1]}`\n\n"
        "This draft PR exists only to prove the Required CI Gate turns red. It is "
        "labelled `do-not-merge` and is closed automatically by the driver.\n"
    )
    runner.run(
        ["gh", "pr", "create", "--repo", config.repo, "--draft", "--base", config.base_branch,
         "--head", branch, "--title", title, "--body", body, "--label", DO_NOT_MERGE_LABEL],
        mutating=True,
    )
    if runner.dry_run:
        # A dry run never created the PR, so there is nothing to look up and no
        # reason to spawn gh.
        return None
    data = _gh_json(
        runner,
        ["pr", "view", branch, "--repo", config.repo, "--json", "number"],
    )
    if isinstance(data, dict) and isinstance(data.get("number"), int):
        return data["number"]
    return None


def wait_for_run(runner: CommandRunner, config: RehearsalConfig, branch: str) -> dict:
    """Poll until the branch's Build SparkEngine run reaches a terminal status."""

    deadline = config.run_timeout_seconds
    waited = 0.0
    while True:
        runs = _gh_json(
            runner,
            ["run", "list", "--repo", config.repo, "--branch", branch,
             "--workflow", BUILD_WORKFLOW_NAME, "--json",
             "databaseId,status,conclusion,headSha,event", "--limit", "20"],
        )
        candidates = [r for r in (runs or []) if isinstance(r, dict)]
        if candidates:
            # Newest first is gh's default order; take the most recent run.
            run = candidates[0]
            if run.get("status") == "completed":
                return run
        if waited >= deadline:
            raise RehearsalError(
                f"timed out after {deadline:.0f}s waiting for a completed Build run on {branch}"
            )
        config.sleep(config.poll_interval_seconds)
        waited += config.poll_interval_seconds


def collect_run_evidence(runner: CommandRunner, config: RehearsalConfig, run_id: int) -> tuple[list[str], Optional[str]]:
    data = _gh_json(
        runner,
        ["run", "view", str(run_id), "--repo", config.repo, "--json", "jobs,conclusion"],
    )
    failing: list[str] = []
    gate_conclusion: Optional[str] = None
    jobs = data.get("jobs") if isinstance(data, dict) else None
    for job in jobs or []:
        if not isinstance(job, dict):
            continue
        name = job.get("name", "")
        conclusion = job.get("conclusion")
        if name == REQUIRED_GATE_JOB:
            gate_conclusion = conclusion
        if conclusion not in (None, "success", "skipped", "neutral"):
            failing.append(name)
    return failing, gate_conclusion


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
    patch_file, expected_job = CLASS_PLAN[failure_class]
    outcome = ClassOutcome(
        failure_class=failure_class,
        patch=patch_file,
        expected_job=expected_job,
        branch=branch_name(failure_class, config.base_sha),
        base_sha=config.base_sha,
    )
    branch = outcome.branch
    pr_number: Optional[int] = None
    try:
        ensure_label(runner, config.repo)
        create_branch_with_patch(runner, config, failure_class, work_dir)
        pr_number = open_draft_pr(runner, config, failure_class, branch)
        outcome.pr_number = pr_number
        if runner.dry_run:
            runner.echo(f"DRY-RUN: would wait for the Build run and record evidence for {failure_class}")
            outcome.recorded_at = config.now().isoformat()
            return outcome
        run = wait_for_run(runner, config, branch)
        outcome.run_id = int(run["databaseId"])
        outcome.run_conclusion = run.get("conclusion")
        failing, gate = collect_run_evidence(runner, config, outcome.run_id)
        outcome.failing_jobs = failing
        outcome.required_gate_conclusion = gate
        # The rehearsal is demonstrated only when the gate actually went red.
        outcome.demonstrated = gate == "failure"
        outcome.recorded_at = config.now().isoformat()
    except (RehearsalError, subprocess.SubprocessError) as error:
        outcome.error = str(error)
        outcome.recorded_at = config.now().isoformat()
    finally:
        _cleanup(runner, config, branch, pr_number)
    return outcome


def _cleanup(runner: CommandRunner, config: RehearsalConfig, branch: str, pr_number: Optional[int]) -> None:
    if pr_number is not None:
        try:
            runner.run(
                ["gh", "pr", "close", str(pr_number), "--repo", config.repo,
                 "--comment", "CI-100 rehearsal complete; closing the do-not-merge draft.", "--delete-branch"],
                check=False, mutating=True,
            )
            return
        except subprocess.SubprocessError:
            runner.echo(f"::warning:: failed to close PR #{pr_number}; attempting branch delete")
    runner.run(
        ["git", "push", "origin", "--delete", branch],
        cwd=REPO_ROOT, check=False, mutating=True,
    )


def run_rehearsal(
    runner: CommandRunner, config: RehearsalConfig, classes: Sequence[str]
) -> list[ClassOutcome]:
    outcomes: list[ClassOutcome] = []
    document = load_evidence(config.evidence_file)
    with tempfile.TemporaryDirectory(prefix="ci100-rehearsal-") as temp:
        work_dir = Path(temp)
        for failure_class in classes:
            outcome = rehearse_class(runner, config, failure_class, work_dir)
            outcomes.append(outcome)
            if not runner.dry_run:
                write_evidence(config.evidence_file, document, outcome)
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
        status = "DRY-RUN" if runner.dry_run else ("RED (ok)" if outcome.demonstrated else "NOT RED")
        detail = outcome.error or f"gate={outcome.required_gate_conclusion} run={outcome.run_id}"
        print(f"  {outcome.failure_class:<13} {status:<9} {detail}")
        if not runner.dry_run and not outcome.demonstrated:
            exit_code = 1
    if not runner.dry_run:
        print(f"\nEvidence written to {config.evidence_file}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
