#!/usr/bin/env python3
"""LIFE-200 repeated headless boot/shutdown loop of the production FPS host.

Launches ``SparkEngine -headless -game <SparkGameFPS> -require-game
-test-frames 8 -threads 2 -no-subprocess`` on NullRHI ``--iterations`` times
in a row. The host leaves on its own after the frame limit, so the loop needs
no signal channel and runs on Windows and Linux alike.

Every iteration gets a fresh working directory and private user directories
(``XDG_*``/``HOME`` on POSIX, ``LOCALAPPDATA``/``APPDATA`` on Windows) and must:

* exit 0 within a hard per-run bound; a run still alive at the bound is killed
  and reported as a deadlock with its iteration index;
* pass the strict ``SPARK_MODULE_READY``/``SPARK_HEADLESS_RHI``/
  ``SPARK_HEADLESS_LIFECYCLE`` parser of
  ``cmake/RunSparkHeadlessNullRHILifecycle.cmake`` (one module, rendered=0,
  unloaded=1, faults=0) -- the single acceptance contract every NullRHI gate uses;
* print exactly one ``SPARK_FPS_HEADLESS_ARENA`` record with an active match,
  every authored spawn bound, and one arena tick per reported OnUpdate;
* print ``SPARK_HEADLESS_NULLRHI_RESOURCES live=0`` on both supported hosts;
* print exactly one ``SPARK_HEADLESS_SHUTDOWN ms=N`` teardown-time record with
  ``N`` within the ``nullrhi.headless.shutdown_time`` ceiling of
  ``perf-budgets/v1/budget.json`` (``--budget-file``);
* keep stdout/stderr free of AddressSanitizer, LeakSanitizer, ThreadSanitizer
  and UBSan reports, so the sanitizer presets fail the loop on the first one.

Across iterations the arena record (objects, spawns, bound, mode_spawns) must
be identical: the same binary and scene have to boot to the same state every
time. Per-run wall time, shutdown time and their maxima are printed.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

IS_WINDOWS = sys.platform.startswith("win")

RUN_BOUND_SECONDS = 60.0
KILL_BOUND_SECONDS = 30.0
PARSER_BOUND_SECONDS = 120.0
TEST_FRAMES = 8

ARENA_RECORD = re.compile(
    r"^SPARK_FPS_HEADLESS_ARENA objects=(\d+) spawns=(\d+) bound=(\d+) mode_spawns=(\d+) ticks=(\d+) match=([01])$"
)
RESOURCES_RECORD = re.compile(r"^SPARK_HEADLESS_NULLRHI_RESOURCES live=(0|[1-9][0-9]{0,9})$")
LIFECYCLE_UPDATED = re.compile(r"^SPARK_HEADLESS_LIFECYCLE .* updated=(\d+) ")
SHUTDOWN_RECORD = re.compile(r"^SPARK_HEADLESS_SHUTDOWN ms=(0|[1-9][0-9]{0,9})$")
SHUTDOWN_METRIC_ID = "nullrhi.headless.shutdown_time"

# UBSan prints "<file>:<line>:<col>: runtime error: ..."; the location prefix keeps
# an engine log line that merely says "runtime error" from matching.
SANITIZER_SIGNATURES = (
    re.compile(r"ERROR: AddressSanitizer"),
    re.compile(r"ERROR: LeakSanitizer"),
    re.compile(r"WARNING: ThreadSanitizer"),
    re.compile(r":\d+:\d+: runtime error: "),
    re.compile(r"SUMMARY: UndefinedBehaviorSanitizer"),
)


class HarnessFailure(Exception):
    """A loop requirement was not met."""


@dataclass(frozen=True)
class ArenaState:
    """The boot-determined part of the arena record (ticks follow the frame count)."""

    objects: int
    spawns: int
    bound: int
    mode_spawns: int


@dataclass
class RunResult:
    returncode: int
    stdout: str
    stderr: str
    elapsed: float


# ---------------------------------------------------------------------------
# Pure validators (covered by --self-test)
# ---------------------------------------------------------------------------


def _lines(text: str) -> list[str]:
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def require_no_sanitizer_reports(stdout: str, stderr: str) -> None:
    for line in _lines(f"{stdout}\n{stderr}"):
        for signature in SANITIZER_SIGNATURES:
            if signature.search(line):
                raise HarnessFailure(f"sanitizer report in host output: {line.strip()!r}")


def require_arena_record(stdout: str, stderr: str) -> ArenaState:
    """Exactly one well-formed arena record, consistent with the host's OnUpdate count."""
    lines = _lines(f"{stdout}\n{stderr}")
    mentions = [line for line in lines if "SPARK_FPS_HEADLESS_ARENA" in line]
    records = [match for match in (ARENA_RECORD.match(line) for line in lines) if match]
    if len(records) != 1 or len(mentions) != 1:
        raise HarnessFailure(f"found {len(records)} arena records in {len(mentions)} mentions, expected exactly 1")
    objects, spawns, bound, mode_spawns, ticks, match = (int(value) for value in records[0].groups())
    updated = [int(found.group(1)) for found in (LIFECYCLE_UPDATED.match(line) for line in lines) if found]
    if len(updated) != 1:
        raise HarnessFailure(f"found {len(updated)} SPARK_HEADLESS_LIFECYCLE records, expected exactly 1")
    if objects < 1 or spawns < 1:
        raise HarnessFailure(f"arena loaded {objects} scene nodes and {spawns} authored spawns")
    if bound != spawns or mode_spawns != spawns:
        raise HarnessFailure(f"arena bound {bound} respawn and {mode_spawns} GameMode spawns of {spawns}")
    if ticks < 1 or ticks != updated[0]:
        raise HarnessFailure(f"arena ticked {ticks} times but the host reported {updated[0]} OnUpdate callbacks")
    if match != 1:
        raise HarnessFailure("the Deathmatch match was not active at unload")
    return ArenaState(objects, spawns, bound, mode_spawns)


def require_resources_released(stdout: str, stderr: str, *, required: bool) -> None:
    """NullRHI must report zero live resources after teardown, once, where the host reports it."""
    lines = _lines(f"{stdout}\n{stderr}")
    mentions = [line for line in lines if "SPARK_HEADLESS_NULLRHI_RESOURCES" in line]
    records = [match for match in (RESOURCES_RECORD.match(line) for line in lines) if match]
    if not mentions and not required:
        return
    if len(records) != 1 or len(mentions) != 1:
        raise HarnessFailure(f"found {len(records)} NullRHI resource records in {len(mentions)} mentions, "
                             "expected exactly 1")
    live = int(records[0].group(1))
    if live != 0:
        raise HarnessFailure(f"NullRHI still held {live} live resources after teardown")


def load_shutdown_ceiling(budget_file: Path) -> float:
    """The provisional teardown ceiling (ms) the budget file sets for the headless host."""
    try:
        budget = json.loads(budget_file.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise HarnessFailure(f"cannot read budget file {budget_file}: {error}") from error
    metrics = budget.get("metrics") if isinstance(budget, dict) else None
    matches = [metric for metric in metrics or []
               if isinstance(metric, dict) and metric.get("id") == SHUTDOWN_METRIC_ID]
    if len(matches) != 1:
        raise HarnessFailure(f"{budget_file} defines {len(matches)} {SHUTDOWN_METRIC_ID!r} metrics, expected exactly 1")
    metric = matches[0]
    ceiling = metric.get("budget")
    if metric.get("unit") != "ms" or metric.get("direction") != "lower_is_better":
        raise HarnessFailure(f"{SHUTDOWN_METRIC_ID!r} must be a lower_is_better budget in ms")
    if isinstance(ceiling, bool) or not isinstance(ceiling, (int, float)) or not math.isfinite(ceiling) or ceiling <= 0:
        raise HarnessFailure(f"{SHUTDOWN_METRIC_ID!r} has no enforceable ceiling (budget={ceiling!r})")
    return float(ceiling)


def require_shutdown_within(stdout: str, stderr: str, ceiling_ms: float) -> int:
    """Exactly one teardown-time record, no slower than the budget ceiling; returns its ms."""
    lines = _lines(f"{stdout}\n{stderr}")
    mentions = [line for line in lines if "SPARK_HEADLESS_SHUTDOWN" in line]
    records = [match for match in (SHUTDOWN_RECORD.match(line) for line in lines) if match]
    if len(records) != 1 or len(mentions) != 1:
        raise HarnessFailure(f"found {len(records)} shutdown-time records in {len(mentions)} mentions, "
                             "expected exactly 1")
    shutdown_ms = int(records[0].group(1))
    if shutdown_ms > ceiling_ms:
        raise HarnessFailure(f"teardown took {shutdown_ms} ms, over the {ceiling_ms:g} ms "
                             f"{SHUTDOWN_METRIC_ID} budget")
    return shutdown_ms


def require_deterministic(states: list[ArenaState]) -> None:
    if len(set(states)) > 1:
        raise HarnessFailure(f"arena state differed between boots: {sorted(set(states), key=repr)}")


def validate_run(result: RunResult, *, resources_required: bool,
                 shutdown_ceiling_ms: float) -> tuple[ArenaState, int]:
    """Every Python-side check of one run (the strict lifecycle parser runs separately in CMake).

    Returns the boot-determined arena state and the reported teardown time in ms.
    """
    if result.returncode != 0:
        raise HarnessFailure(f"host exit status was {result.returncode}, expected 0")
    require_no_sanitizer_reports(result.stdout, result.stderr)
    require_resources_released(result.stdout, result.stderr, required=resources_required)
    shutdown_ms = require_shutdown_within(result.stdout, result.stderr, shutdown_ceiling_ms)
    return require_arena_record(result.stdout, result.stderr), shutdown_ms


def run_bounded(command: list[str], *, cwd: Path, env: dict[str, str], stdout_path: Path, stderr_path: Path,
                bound: float, label: str) -> RunResult:
    """Run @p command to completion; a process alive at @p bound is killed and reported as a deadlock."""
    started = time.monotonic()
    with open(stdout_path, "wb") as out, open(stderr_path, "wb") as err:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        try:
            process.wait(timeout=bound)
        except subprocess.TimeoutExpired as error:
            process.kill()
            try:
                process.wait(timeout=KILL_BOUND_SECONDS)
            except subprocess.TimeoutExpired:
                pass
            raise HarnessFailure(f"{label}: deadlock -- the host did not exit within {bound:.0f}s") from error
    return RunResult(process.returncode, read_text(stdout_path), read_text(stderr_path), time.monotonic() - started)


def read_text(path: Path) -> str:
    try:
        return path.read_bytes().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def require_strict_records(cmake: str, parser_script: Path, result: RunResult, work_dir: Path, label: str) -> None:
    """Apply the shared strict NullRHI lifecycle parser (single acceptance contract)."""
    work_dir.mkdir(parents=True, exist_ok=True)
    stdout_file = work_dir / "parse-stdout.txt"
    stderr_file = work_dir / "parse-stderr.txt"
    stdout_file.write_text(result.stdout, encoding="utf-8")
    stderr_file.write_text(result.stderr, encoding="utf-8")
    wrapper = work_dir / "validate.cmake"
    wrapper.write_text(
        "cmake_minimum_required(VERSION 3.25)\n"
        "set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)\n"
        f'include("{parser_script.as_posix()}")\n'
        f'file(READ "{stdout_file.as_posix()}" _out)\n'
        f'file(READ "{stderr_file.as_posix()}" _err)\n'
        f'_spark_validate_headless_nullrhi_result("{result.returncode}" "${{_out}}" "${{_err}}" _ok _why)\n'
        "if(NOT _ok)\n  message(FATAL_ERROR \"${_why}\")\nendif()\n",
        encoding="utf-8",
    )
    check = subprocess.run([cmake, "-P", str(wrapper)], capture_output=True, text=True, timeout=PARSER_BOUND_SECONDS)
    if check.returncode != 0:
        reason = (check.stderr or check.stdout).strip().splitlines()
        detail = next((line.strip() for line in reason if line.strip() and "CMake Error" not in line), "?")
        raise HarnessFailure(f"{label}: strict NullRHI lifecycle records rejected: {detail}")


# ---------------------------------------------------------------------------
# Process loop
# ---------------------------------------------------------------------------


def isolated_environment(user_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["SPARK_RHI_BACKEND"] = "null"
    if IS_WINDOWS:
        names = (("LOCALAPPDATA", "localappdata"), ("APPDATA", "appdata"))
    else:
        names = (("HOME", "home"), ("XDG_DATA_HOME", "data"), ("XDG_CONFIG_HOME", "config"),
                 ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state"))
    for key, name in names:
        env[key] = str(user_root / name)
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    return env


def run_loop(args: argparse.Namespace, root: Path) -> list[str]:
    engine = args.engine.resolve(strict=True)
    module = args.module.resolve(strict=True)
    parser_script = (args.source_root / "cmake" / "RunSparkHeadlessNullRHILifecycle.cmake").resolve(strict=True)
    shutdown_ceiling_ms = load_shutdown_ceiling(budget_file(args))
    command = [str(engine), "-headless", "-game", str(module), "-require-game",
               "-test-frames", str(TEST_FRAMES), "-threads", "2", "-no-subprocess"]

    notes: list[str] = []
    states: list[ArenaState] = []
    elapsed: list[float] = []
    shutdowns: list[int] = []
    for iteration in range(1, args.iterations + 1):
        label = f"iteration {iteration}/{args.iterations}"
        run_dir = root / f"run{iteration}"
        (run_dir / "cwd").mkdir(parents=True)
        result = run_bounded(command, cwd=run_dir / "cwd", env=isolated_environment(run_dir / "user"),
                             stdout_path=run_dir / "stdout.txt", stderr_path=run_dir / "stderr.txt",
                             bound=RUN_BOUND_SECONDS, label=label)
        try:
            require_strict_records(args.cmake, parser_script, result, run_dir, label)
            state, shutdown_ms = validate_run(result, resources_required=True,
                                              shutdown_ceiling_ms=shutdown_ceiling_ms)
            states.append(state)
            require_deterministic(states)
        except HarnessFailure as failure:
            raise HarnessFailure(f"{label}: {failure}") from failure
        elapsed.append(result.elapsed)
        shutdowns.append(shutdown_ms)
        notes.append(f"{label}: exit=0 wall={result.elapsed:.2f}s shutdown={shutdown_ms}ms arena={state}")
    notes.append(f"{args.iterations} boots passed; max wall={max(elapsed):.2f}s "
                 f"mean={sum(elapsed) / len(elapsed):.2f}s bound={RUN_BOUND_SECONDS:.0f}s; "
                 f"max shutdown={max(shutdowns)}ms budget={shutdown_ceiling_ms:g}ms")
    return notes


# ---------------------------------------------------------------------------
# Self-test of the validators, the strict parser hook and the deadlock bound
# ---------------------------------------------------------------------------

_READY = "SPARK_MODULE_READY count=1\n"
_ARENA = "SPARK_FPS_HEADLESS_ARENA objects=72 spawns=4 bound=4 mode_spawns=4 ticks=8 match=1\n"
_RHI = "SPARK_HEADLESS_RHI backend=null initialized=1 frames=8 shutdown=1\n"
_LIFECYCLE = "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=8 fixed=7 rendered=0 unloaded=1 faults=0\n"
_RESOURCES = "SPARK_HEADLESS_NULLRHI_RESOURCES live=0\n"
_SHUTDOWN = "SPARK_HEADLESS_SHUTDOWN ms=42\n"
_GOOD = f"{_READY}{_ARENA}{_RHI}{_LIFECYCLE}{_RESOURCES}{_SHUTDOWN}"
_CEILING_MS = 5000.0


def budget_file(args: argparse.Namespace) -> Path:
    return args.budget_file or args.source_root / "perf-budgets" / "v1" / "budget.json"


def self_test(args: argparse.Namespace) -> int:
    def expect_failure(name: str, action) -> None:
        try:
            action()
        except HarnessFailure:
            return
        raise AssertionError(f"self-test case '{name}' unexpectedly passed")

    def run(stdout: str, stderr: str = "", returncode: int = 0) -> RunResult:
        return RunResult(returncode, stdout, stderr, 0.0)

    def check(stdout: str, stderr: str = "", returncode: int = 0, *, resources_required: bool = True,
              ceiling: float = _CEILING_MS) -> tuple[ArenaState, int]:
        return validate_run(run(stdout, stderr, returncode), resources_required=resources_required,
                            shutdown_ceiling_ms=ceiling)

    good = check(_GOOD)
    assert good == (ArenaState(72, 4, 4, 4), 42), good
    check(_GOOD.replace("\n", "\r\n"))
    check(_GOOD.replace(_RESOURCES, ""), resources_required=False)
    assert check(_GOOD, ceiling=42.0)[1] == 42

    expect_failure("nonzero-exit", lambda: check(_GOOD, returncode=3))
    expect_failure("missing-arena", lambda: check(_GOOD.replace(_ARENA, "")))
    expect_failure("duplicate-arena", lambda: check(_GOOD + _ARENA))
    expect_failure("logger-only-arena", lambda: check(_GOOD.replace(_ARENA, f"[info] {_ARENA}")))
    expect_failure("match-inactive", lambda: check(_GOOD.replace("match=1", "match=0")))
    expect_failure("unbound-spawns", lambda: check(_GOOD.replace("bound=4", "bound=3")))
    expect_failure("ticks-differ-from-updates", lambda: check(_GOOD.replace("ticks=8", "ticks=5")))
    expect_failure("missing-lifecycle", lambda: check(_GOOD.replace(_LIFECYCLE, "")))
    expect_failure("live-resources", lambda: check(_GOOD.replace("live=0", "live=3")))
    expect_failure("live-resources-unrequired", lambda: check(_GOOD.replace("live=0", "live=3"),
                                                              resources_required=False))
    expect_failure("missing-resources", lambda: check(_GOOD.replace(_RESOURCES, "")))
    expect_failure("malformed-resources", lambda: check(_GOOD.replace("live=0", "live=01")))
    expect_failure("over-budget-shutdown", lambda: check(_GOOD, ceiling=41.0))
    expect_failure("missing-shutdown", lambda: check(_GOOD.replace(_SHUTDOWN, "")))
    expect_failure("duplicate-shutdown", lambda: check(_GOOD + _SHUTDOWN))
    expect_failure("malformed-shutdown", lambda: check(_GOOD.replace("ms=42", "ms=042")))
    expect_failure("logger-only-shutdown", lambda: check(_GOOD.replace(_SHUTDOWN, f"[info] {_SHUTDOWN}")))
    sanitizer_reports = (
        "==123==ERROR: AddressSanitizer: heap-use-after-free on address 0x1\n",
        "==123==ERROR: LeakSanitizer: detected memory leaks\n",
        "WARNING: ThreadSanitizer: data race (pid=123)\n",
        "Core/World.cpp:42:7: runtime error: signed integer overflow\n",
        "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior Core/World.cpp:42:7\n",
    )
    for report in sanitizer_reports:
        expect_failure(f"sanitizer report {report.strip()!r}", lambda report=report: check(_GOOD, report))
    # An engine log line that only mentions a runtime error is not a UBSan report.
    check(_GOOD, "[ERROR] [Script] runtime error: missing function\n")
    expect_failure("nondeterministic-arena", lambda: require_deterministic(
        [ArenaState(72, 4, 4, 4), ArenaState(71, 4, 4, 4)]))
    require_deterministic([ArenaState(72, 4, 4, 4)] * 3)

    with tempfile.TemporaryDirectory(prefix="spark-boot-loop-selftest-") as scratch:
        work = Path(scratch)
        # The committed budget carries the enforced ceiling; a budget that drops it,
        # nulls it or changes its unit must stop the loop instead of passing it.
        assert load_shutdown_ceiling(budget_file(args)) > 0
        for name, metric in (
            ("null-ceiling", {"id": SHUTDOWN_METRIC_ID, "unit": "ms", "direction": "lower_is_better",
                              "budget": None}),
            ("seconds-ceiling", {"id": SHUTDOWN_METRIC_ID, "unit": "s", "direction": "lower_is_better",
                                 "budget": 5}),
            ("missing-metric", {"id": "nullrhi.headless.tick_time.p50", "unit": "ms",
                                "direction": "lower_is_better", "budget": 5000}),
        ):
            bad_budget = work / f"{name}.json"
            bad_budget.write_text(json.dumps({"metrics": [metric]}), encoding="utf-8")
            expect_failure(f"budget-{name}", lambda bad_budget=bad_budget: load_shutdown_ceiling(bad_budget))
        expect_failure("budget-unreadable", lambda: load_shutdown_ceiling(work / "absent.json"))

        parser_script = (args.source_root / "cmake" / "RunSparkHeadlessNullRHILifecycle.cmake").resolve(strict=True)
        # The strict CMake parser is part of the contract: prove the hook accepts a
        # clean run and rejects a faulted one, not just that the file exists.
        require_strict_records(args.cmake, parser_script, run(_GOOD), work / "good", "good")
        expect_failure("strict-parser-faults", lambda: require_strict_records(
            args.cmake, parser_script, run(_GOOD.replace("faults=0", "faults=1")), work / "faults", "faults"))
        expect_failure("strict-parser-rendered", lambda: require_strict_records(
            args.cmake, parser_script, run(_GOOD.replace("rendered=0", "rendered=1")), work / "rendered",
            "rendered"))

        # A host that never exits is killed at the bound and reported as a deadlock.
        started = time.monotonic()
        expect_failure("deadlock-bound", lambda: run_bounded(
            [sys.executable, "-c", "import time; time.sleep(120)"], cwd=work, env=os.environ.copy(),
            stdout_path=work / "hang-out.txt", stderr_path=work / "hang-err.txt", bound=1.0, label="hang"))
        if time.monotonic() - started > 1.0 + KILL_BOUND_SECONDS:
            raise AssertionError("self-test case 'deadlock-bound' did not kill the hung process promptly")
        clean = run_bounded([sys.executable, "-c", "print('ok')"], cwd=work, env=os.environ.copy(),
                            stdout_path=work / "ok-out.txt", stderr_path=work / "ok-err.txt", bound=30.0,
                            label="ok")
        assert clean.returncode == 0 and clean.stdout.strip() == "ok", clean

    print("headless boot loop harness self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="validate the harness checks and exit")
    parser.add_argument("--engine", type=Path, help="SparkEngine executable")
    parser.add_argument("--module", type=Path, help="SparkGameFPS module")
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2],
                        help="repository root (for the strict lifecycle parser)")
    parser.add_argument("--cmake", default="cmake", help="cmake executable used to run the strict parser")
    parser.add_argument("--budget-file", type=Path, default=None,
                        help="perf budget holding the shutdown-time ceiling "
                             "(default: <source-root>/perf-budgets/v1/budget.json)")
    parser.add_argument("--iterations", type=int, default=10, help="number of consecutive boots")
    parser.add_argument("--keep", action="store_true", help="keep the temp run directory on success")
    args = parser.parse_args()

    if args.self_test:
        return self_test(args)
    if not (args.engine and args.module):
        parser.error("--engine and --module are required")
    if args.iterations < 2:
        parser.error("--iterations must be at least 2 (a loop needs a repeated boot)")

    root = Path(tempfile.mkdtemp(prefix="spark-headless-boot-loop-"))
    print(f"HeadlessBootLoop: iterations={args.iterations} host={sys.platform} root={root}")
    try:
        notes = run_loop(args, root)
    except HarnessFailure as failure:
        print(f"HeadlessBootLoop FAILED: {failure}", file=sys.stderr)
        print(f"run artefacts kept in {root}", file=sys.stderr)
        return 1
    for note in notes:
        print(note)
    print("HeadlessBootLoop passed")
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
