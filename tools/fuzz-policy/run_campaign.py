#!/usr/bin/env python3
"""Run a time-budgeted SEC-120 libFuzzer mutation campaign.

The blocking ``fuzz-policy`` job only replays each reviewed seed once
(``-runs=<seed count>``) so the merge gate stays deterministic.  This runner is
the exploration half: it is driven by the scheduled ``fuzz-scheduled`` workflow
and by developers who want a longer local campaign.

Targets are discovered from the configured fuzz build itself -- every CTest
test labelled exactly ``fuzz`` -- so the campaign mutates precisely the
binaries, resource limits and corpora the blocking smoke replays, and a new
target joins the campaign as soon as its smoke test is registered.

For each target the runner:

* copies the committed seed corpus into a disposable working directory and
  hands libFuzzer that copy as its only writable corpus, so new units never
  land in the repository;
* adds the target's committed generated corpus (``FuzzerTests/generated/<name>``
  beside ``FuzzerTests/corpora/<name>``), coverage-minimized units kept from
  earlier campaigns, as a read-only second corpus so a campaign resumes from
  that coverage instead of the seeds alone;
* keeps the smoke's ``-max_len``/``-timeout``/``-rss_limit_mb`` limits, drops
  its replay-only ``-runs``/``-max_total_time`` and mutates for ``--seconds``;
* records duration, executed units, crash-free wall time and peak RSS;
* makes UndefinedBehaviorSanitizer reports fatal (``halt_on_error=1``) so a
  target built with recoverable UBSan still stops and leaves a reproducer, and
  treats any ``runtime error:`` report left in the log as a finding;
* runs ``-minimize_crash=1`` on every crash/leak/timeout/OOM artifact and
  retains both the raw and minimized reproducer for upload;
* re-hashes the committed seed and generated corpora afterwards and treats any
  change as a finding.

``campaign-summary.json`` is rewritten after every target (``complete`` is
false until the last one finishes), so a job cancelled mid-campaign still
leaves the results of the targets that ran.

Exit status: 0 when every target ran its full budget without a finding; 1 when
any target crashed, hung, exited abnormally, printed a sanitizer report or its
committed corpus changed; 2 when the campaign could not be set up (bad
arguments, no targets, a budget above ``--max-campaign-seconds``) or, absent any
finding, when a target could not be run (missing binary or unusable corpus).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1
FUZZ_LABEL = "fuzz"

# libFuzzer names every reproducer it writes with one of these prefixes.
# slow-unit-* is informational (the input finished under -timeout) and is not a
# failure on its own.
FAILURE_ARTIFACT_PREFIXES = ("crash-", "leak-", "timeout-", "oom-")

# Smoke flags that make a run a bounded replay rather than a campaign.
REPLAY_ONLY_FLAGS = ("-runs=", "-max_total_time=")
# Flags this runner owns; a smoke test that sets them would silently fight it.
RUNNER_OWNED_FLAGS = (
    "-artifact_prefix=",
    "-exact_artifact_path=",
    "-minimize_crash=",
    "-print_final_stats=",
    "-jobs=",
    "-workers=",
    "-fork=",
    "-merge=",
)

MIN_SECONDS = 1
MAX_SECONDS = 6 * 60 * 60
MAX_CORPUS_FILES = 20_000
MAX_LOG_BYTES = 8 * 1024 * 1024
MAX_CTEST_JSON_BYTES = 16 * 1024 * 1024

_STAT_LINE = re.compile(r"^stat::([a-z_]+):\s*(\d+)\s*$", re.MULTILINE)
_PROGRESS_LINE = re.compile(r"^#(\d+)\s", re.MULTILINE)
# UBSan's report format is "<file>:<line>:<col>: runtime error: <what>".
_UBSAN_REPORT = re.compile(r": runtime error: ")
# Per-target states that are findings about the code under test, as opposed to
# "clean" and "setup-error".
FINDING_STATUSES = ("crash", "hang", "abnormal-exit", "sanitizer-report", "corpus-mutated")
# Target names become output directory names.
_TARGET_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")


class CampaignError(RuntimeError):
    """The campaign cannot be set up; nothing about target safety is known."""


@dataclass(frozen=True)
class FuzzTarget:
    """One libFuzzer smoke test as registered with CTest."""

    name: str
    binary: Path
    options: tuple[str, ...]
    corpus: Path
    working_directory: Path | None


@dataclass
class TargetResult:
    name: str
    binary: str
    corpus: str
    seed_count: int = 0
    generated_count: int = 0
    status: str = "not-run"
    exit_code: int | None = None
    duration_seconds: float = 0.0
    crash_free_seconds: float = 0.0
    executed_units: int | None = None
    peak_rss_mb: int | None = None
    new_units: int | None = None
    corpus_unchanged: bool = False
    artifacts: list[dict[str, Any]] = field(default_factory=list)
    log: str = ""
    detail: str = ""

    def as_json(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "binary": self.binary,
            "corpus": self.corpus,
            "seed_count": self.seed_count,
            "generated_count": self.generated_count,
            "status": self.status,
            "exit_code": self.exit_code,
            "duration_seconds": round(self.duration_seconds, 3),
            "crash_free_seconds": round(self.crash_free_seconds, 3),
            "executed_units": self.executed_units,
            "peak_rss_mb": self.peak_rss_mb,
            "new_units": self.new_units,
            "committed_corpus_unchanged": self.corpus_unchanged,
            "artifacts": self.artifacts,
            "log": self.log,
            "detail": self.detail,
        }


def parse_ctest_tests(document: Any) -> list[FuzzTarget]:
    """Turn ``ctest --show-only=json-v1`` output into campaign targets.

    Every test must look like a libFuzzer invocation: an absolute binary, only
    ``-flag=value`` options, and exactly one positional corpus directory.  A
    shape this runner does not understand is a setup error rather than a
    target that is quietly skipped.
    """
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
        raise CampaignError("ctest JSON has no 'tests' array")
    targets: list[FuzzTarget] = []
    for entry in document["tests"]:
        if not isinstance(entry, dict):
            raise CampaignError("ctest JSON test entry is not an object")
        name = entry.get("name")
        command = entry.get("command")
        if not isinstance(name, str) or not _TARGET_NAME.match(name):
            raise CampaignError(f"ctest JSON test name is missing or unsafe: {name!r}")
        if not isinstance(command, list) or len(command) < 2 or not all(isinstance(a, str) for a in command):
            raise CampaignError(f"{name}: test command must be a binary followed by arguments")
        working_directory: Path | None = None
        for prop in entry.get("properties", []) or []:
            if isinstance(prop, dict) and prop.get("name") == "WORKING_DIRECTORY":
                if isinstance(prop.get("value"), str) and prop["value"]:
                    working_directory = Path(prop["value"])

        options: list[str] = []
        positionals: list[str] = []
        for argument in command[1:]:
            if argument.startswith("-"):
                if "=" not in argument:
                    raise CampaignError(f"{name}: unsupported libFuzzer option shape {argument!r}")
                if argument.startswith(RUNNER_OWNED_FLAGS):
                    raise CampaignError(f"{name}: smoke sets runner-owned option {argument!r}")
                if argument.startswith(REPLAY_ONLY_FLAGS):
                    continue
                options.append(argument)
            else:
                positionals.append(argument)
        if len(positionals) != 1:
            raise CampaignError(f"{name}: expected exactly one corpus directory, found {len(positionals)}")
        corpus = Path(positionals[0])
        if not corpus.is_absolute() and working_directory is not None:
            corpus = working_directory / corpus
        targets.append(
            FuzzTarget(
                name=name,
                binary=Path(command[0]),
                options=tuple(options),
                corpus=corpus,
                working_directory=working_directory,
            )
        )
    return targets


def discover_targets(build_dir: Path, config: str | None) -> list[FuzzTarget]:
    command = ["ctest", "--test-dir", str(build_dir), "-L", f"^{FUZZ_LABEL}$", "--show-only=json-v1"]
    if config:
        command += ["-C", config]
    try:
        completed = subprocess.run(command, capture_output=True, check=False, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CampaignError(f"cannot query ctest: {error}") from error
    if completed.returncode != 0:
        raise CampaignError(f"ctest --show-only failed: {completed.stderr.decode(errors='replace').strip()}")
    if len(completed.stdout) > MAX_CTEST_JSON_BYTES:
        raise CampaignError("ctest JSON output exceeds the size limit")
    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise CampaignError(f"ctest JSON output is not valid JSON: {error}") from error
    return parse_ctest_tests(document)


def generated_corpus(corpus: Path) -> Path | None:
    """The committed generated corpus paired with a seed corpus, if there is one.

    ``FuzzerTests/corpora/<name>`` pairs with ``FuzzerTests/generated/<name>``.
    """
    candidate = corpus.parent.parent / "generated" / corpus.name
    return candidate if candidate.is_dir() and not candidate.is_symlink() else None


def corpus_snapshot(corpus: Path) -> dict[str, str]:
    """Relative path -> SHA-256 for every file under the committed corpus."""
    if not corpus.is_dir() or corpus.is_symlink():
        raise CampaignError(f"corpus is not a real directory: {corpus}")
    snapshot: dict[str, str] = {}
    for path in sorted(corpus.rglob("*")):
        if path.is_symlink():
            raise CampaignError(f"corpus contains a symlink: {path}")
        if not path.is_file():
            continue
        if len(snapshot) >= MAX_CORPUS_FILES:
            raise CampaignError(f"corpus exceeds {MAX_CORPUS_FILES} files: {corpus}")
        snapshot[path.relative_to(corpus).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return snapshot


def _read_log(path: Path) -> str:
    with path.open("rb") as handle:
        return handle.read(MAX_LOG_BYTES).decode("utf-8", errors="replace")


def parse_run_stats(log_text: str) -> dict[str, int]:
    """Extract libFuzzer's -print_final_stats counters from a run log."""
    stats = {key: int(value) for key, value in _STAT_LINE.findall(log_text)}
    if "number_executed_units" not in stats:
        # A run killed before printing final stats still reports progress lines.
        progress = _PROGRESS_LINE.findall(log_text)
        if progress:
            stats["number_executed_units"] = int(progress[-1])
    return stats


def failure_artifacts(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.iterdir() if p.is_file() and p.name.startswith(FAILURE_ARTIFACT_PREFIXES))


def _is_inside(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
    except ValueError:
        return False
    return True


def campaign_environment() -> dict[str, str]:
    """Child environment for fuzz processes.

    ASan's default 256 MB quarantine alone can trip the smoke's
    -rss_limit_mb=256 during a long mutation run on the larger seeds (observed on
    scene-manifest), which would report a false OOM.  A 32 MB quarantine keeps
    use-after-free detection and leaves the RSS limit meaningful.  An explicit
    quarantine_size_mb in the caller's ASAN_OPTIONS wins.

    UBSan checks compiled without -fno-sanitize-recover print a report and keep
    running, so libFuzzer would neither stop nor write a reproducer.
    halt_on_error=1 makes every UBSan report fatal; an explicit halt_on_error
    in the caller's UBSAN_OPTIONS wins (the log scan still flags any report).
    """
    environment = dict(os.environ)
    asan_options = environment.get("ASAN_OPTIONS", "")
    if "quarantine_size_mb=" not in asan_options:
        environment["ASAN_OPTIONS"] = ":".join(part for part in (asan_options, "quarantine_size_mb=32") if part)
    ubsan_options = environment.get("UBSAN_OPTIONS", "")
    if "halt_on_error=" not in ubsan_options:
        environment["UBSAN_OPTIONS"] = ":".join(
            part for part in (ubsan_options, "halt_on_error=1:print_stacktrace=1") if part
        )
    return environment


def minimize_artifact(
    target: FuzzTarget, artifact: Path, destination: Path, seconds: int, log_path: Path, output: Path
) -> dict[str, Any]:
    """Shrink one reproducer with libFuzzer's -minimize_crash mode.

    Paths in the returned record are relative to the campaign output root so
    the summary stays valid after the directory is uploaded and downloaded.
    """
    command = [
        str(target.binary),
        *target.options,
        "-minimize_crash=1",
        f"-max_total_time={seconds}",
        f"-exact_artifact_path={destination}",
        str(artifact),
    ]
    record: dict[str, Any] = {
        "raw": artifact.relative_to(output).as_posix(),
        "raw_bytes": artifact.stat().st_size,
        "raw_sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
        "minimize_log": log_path.relative_to(output).as_posix(),
    }
    with log_path.open("wb") as log:
        try:
            completed = subprocess.run(
                command,
                stdout=log,
                stderr=subprocess.STDOUT,
                cwd=target.working_directory,
                env=campaign_environment(),
                timeout=seconds * 2 + 60,
                check=False,
            )
            record["minimize_exit_code"] = completed.returncode
        except subprocess.TimeoutExpired:
            record["minimize_exit_code"] = None
    if destination.is_file():
        record["minimized"] = destination.relative_to(output).as_posix()
        record["minimized_bytes"] = destination.stat().st_size
        record["minimized_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
    else:
        # The raw reproducer is still retained; a failed minimization must not
        # hide the finding.
        record["minimized"] = None
    return record


def run_target(target: FuzzTarget, seconds: int, minimize_seconds: int, output: Path) -> TargetResult:
    result = TargetResult(name=target.name, binary=str(target.binary), corpus=str(target.corpus))
    target_out = output / target.name
    artifacts_dir = target_out / "artifacts"
    minimized_dir = target_out / "minimized"
    artifacts_dir.mkdir(parents=True)
    minimized_dir.mkdir()
    log_path = target_out / "fuzz.log"
    result.log = log_path.relative_to(output).as_posix()

    if not target.binary.is_file() or not os.access(target.binary, os.X_OK):
        result.status = "setup-error"
        result.detail = f"fuzz binary is missing or not executable: {target.binary}"
        return result
    before = corpus_snapshot(target.corpus)
    result.seed_count = len(before)
    generated = generated_corpus(target.corpus)
    generated_before = corpus_snapshot(generated) if generated else {}
    result.generated_count = len(generated_before)
    if not before:
        result.status = "setup-error"
        result.detail = "committed corpus has no seeds"
        return result

    with tempfile.TemporaryDirectory(prefix=f"spark-fuzz-{target.name}-") as scratch:
        work_corpus = Path(scratch) / "corpus"
        shutil.copytree(target.corpus, work_corpus, symlinks=False)
        command = [
            str(target.binary),
            *target.options,
            f"-max_total_time={seconds}",
            "-print_final_stats=1",
            f"-artifact_prefix={artifacts_dir}/",
            str(work_corpus),  # libFuzzer writes new units only into its first corpus
            *([str(generated)] if generated else []),
        ]
        started = time.monotonic()
        with log_path.open("wb") as log:
            log.write(("$ " + " ".join(command) + "\n").encode())
            log.flush()
            try:
                completed = subprocess.run(
                    command,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    cwd=target.working_directory,
                    env=campaign_environment(),
                    timeout=seconds * 2 + 60,
                    check=False,
                )
                result.exit_code = completed.returncode
            except subprocess.TimeoutExpired:
                result.exit_code = None
        result.duration_seconds = time.monotonic() - started
        new_units = sum(1 for p in work_corpus.rglob("*") if p.is_file()) - len(before)
        result.new_units = max(new_units, 0)

    log_text = _read_log(log_path)
    stats = parse_run_stats(log_text)
    result.executed_units = stats.get("number_executed_units")
    result.peak_rss_mb = stats.get("peak_rss_mb")

    found = failure_artifacts(artifacts_dir)
    for index, artifact in enumerate(found):
        destination = minimized_dir / f"{artifact.name}.min"
        log = target_out / f"minimize-{index}.log"
        result.artifacts.append(minimize_artifact(target, artifact, destination, minimize_seconds, log, output))

    after = corpus_snapshot(target.corpus)
    generated_after = corpus_snapshot(generated) if generated else {}
    result.corpus_unchanged = after == before and generated_after == generated_before

    if result.exit_code is None:
        result.status = "hang"
        result.detail = f"fuzzer did not exit within {seconds * 2 + 60} seconds"
    elif found:
        result.status = "crash"
        result.detail = f"{len(found)} failure artifact(s) retained"
    elif result.exit_code != 0:
        result.status = "abnormal-exit"
        result.detail = f"fuzzer exited {result.exit_code} without writing a reproducer"
    elif _UBSAN_REPORT.search(log_text):
        # Only reachable when the caller overrode halt_on_error: UB was reported
        # but the run continued to a clean exit.
        result.status = "sanitizer-report"
        result.detail = "UBSan 'runtime error:' report in fuzz.log without a reproducer"
    else:
        result.status = "clean"
    # libFuzzer stops at its first failure, so the crash-free time is the wall
    # time up to that exit; for a clean run it is the whole budget.
    result.crash_free_seconds = result.duration_seconds
    if not result.corpus_unchanged:
        result.status = "corpus-mutated" if result.status == "clean" else result.status
        result.detail = (result.detail + "; " if result.detail else "") + "committed corpus changed during the run"
    return result


def source_revision(root: Path) -> str | None:
    if os.environ.get("GITHUB_SHA"):
        return os.environ["GITHUB_SHA"]
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, check=False, timeout=30
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.decode().strip() or None


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path, help="configured tools/fuzz-policy build tree")
    parser.add_argument("--output", required=True, type=Path, help="new directory for logs, reproducers, summary")
    parser.add_argument("--seconds", required=True, type=int, help="mutation budget per target")
    parser.add_argument(
        "--minimize-seconds", type=int, default=60, help="-minimize_crash budget per reproducer (default 60)"
    )
    parser.add_argument("--target", action="append", default=[], help="run only this smoke test (repeatable)")
    parser.add_argument("--config", help="CTest configuration for multi-config generators")
    parser.add_argument(
        "--max-campaign-seconds",
        type=int,
        help="refuse to start when --seconds times the selected target count exceeds this (fit a job timeout)",
    )
    return parser.parse_args(argv)


def write_summary(output: Path, args: argparse.Namespace, started_at: datetime, results: list[TargetResult],
                  target_count: int) -> None:
    """Atomically (re)write campaign-summary.json with the targets run so far."""
    summary = {
        "schema_version": SCHEMA_VERSION,
        "source_revision": source_revision(Path(__file__).resolve().parents[2]),
        "started_at": started_at.isoformat(),
        "finished_at": datetime.now(timezone.utc).isoformat(),
        "seconds_per_target": args.seconds,
        "minimize_seconds": args.minimize_seconds,
        "planned_targets": target_count,
        "complete": len(results) == target_count,
        "passed": len(results) == target_count and all(r.status == "clean" for r in results),
        "targets": [r.as_json() for r in results],
    }
    staging = output / "campaign-summary.json.tmp"
    staging.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    staging.replace(output / "campaign-summary.json")


def exit_status(results: list[TargetResult]) -> int:
    """1 for any finding, else 2 if a target could not run, else 0."""
    if any(r.status in FINDING_STATUSES for r in results):
        return 1
    if any(r.status != "clean" for r in results):
        return 2
    return 0


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        for label, value in (("--seconds", args.seconds), ("--minimize-seconds", args.minimize_seconds)):
            if not MIN_SECONDS <= value <= MAX_SECONDS:
                raise CampaignError(f"{label} must be between {MIN_SECONDS} and {MAX_SECONDS}")
        if args.max_campaign_seconds is not None and args.max_campaign_seconds < MIN_SECONDS:
            raise CampaignError(f"--max-campaign-seconds must be at least {MIN_SECONDS}")
        if args.output.exists():
            raise CampaignError(f"output directory already exists: {args.output}")
        targets = discover_targets(args.build_dir, args.config)
        if args.target:
            known = {t.name for t in targets}
            unknown = sorted(set(args.target) - known)
            if unknown:
                raise CampaignError(f"unknown fuzz target(s): {', '.join(unknown)}")
            targets = [t for t in targets if t.name in set(args.target)]
        if not targets:
            raise CampaignError(f"no CTest tests labelled '{FUZZ_LABEL}' in {args.build_dir}")
        planned = args.seconds * len(targets)
        if args.max_campaign_seconds is not None and planned > args.max_campaign_seconds:
            raise CampaignError(
                f"--seconds {args.seconds} x {len(targets)} target(s) = {planned}s exceeds "
                f"--max-campaign-seconds {args.max_campaign_seconds}"
            )
        for target in targets:
            for committed in (target.corpus, generated_corpus(target.corpus)):
                if committed is not None and _is_inside(args.output, committed):
                    raise CampaignError(f"output directory lies inside committed corpus {committed}")
        args.output.mkdir(parents=True)
    except CampaignError as error:
        print(f"run_campaign: {error}", file=sys.stderr)
        return 2

    started_at = datetime.now(timezone.utc)
    results: list[TargetResult] = []
    write_summary(args.output, args, started_at, results, len(targets))
    for target in targets:
        print(f"run_campaign: {target.name}: mutating for {args.seconds}s", flush=True)
        try:
            outcome = run_target(target, args.seconds, args.minimize_seconds, args.output)
        except CampaignError as error:
            outcome = TargetResult(name=target.name, binary=str(target.binary), corpus=str(target.corpus))
            outcome.status = "setup-error"
            outcome.detail = str(error)
        results.append(outcome)
        print(
            f"run_campaign: {target.name}: {outcome.status} "
            f"(execs={outcome.executed_units}, {outcome.duration_seconds:.1f}s) {outcome.detail}",
            flush=True,
        )
        write_summary(args.output, args, started_at, results, len(targets))

    return exit_status(results)


if __name__ == "__main__":
    sys.exit(main())
