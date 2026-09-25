#!/usr/bin/env python3
"""HEAD-220 headless shutdown and restart-recovery harness.

Drives the production headless host (``SparkEngine -headless -game <FPS module>
-require-game``) on NullRHI through three process-level scenarios:

graceful
    Boot, wait until the 60 Hz loop has dispatched a scripted command, then
    request shutdown the way an operator does (SIGTERM). The host must pass
    the CanShutdownEngine checkpoint on its own, exit 0 within the bound, and
    print the strict NullRHI lifecycle records (unloaded=1, faults=0,
    rendered=0) accepted by cmake/RunSparkHeadlessNullRHILifecycle.cmake.

forced-recovery
    Boot the same way, then kill the process without warning (SIGKILL)
    at a seeded point in the running loop. The persisted
    user-data tree is then left the way a writer killed mid-save leaves it
    (an orphaned ``<slot>.spark_save.tmp`` and a torn primary with no
    retained copy). A fresh process on the same user-data tree must boot
    cleanly, report the torn slot as unreadable instead of crashing or
    listing it, and shut down with the strict lifecycle records.

boot-interrupted
    Kill the process during engine/module bring-up (before the loop
    dispatches its first scripted command), then require the next boot on the
    same user-data tree to complete a bounded run with the strict records.

Every run gets private user directories (XDG_* and HOME) and a working directory in a fresh temp
directory outside the source tree. Every wait is wall-clock bounded; a hang
or crash is a failure, never a retry. Only a kill that landed in the wrong
phase (a race the harness cannot prevent) is retried, and the seed plus every
attempt's timing is printed so a failure can be replayed with ``--seed``.

Scope note: the headless FPS module registers no quicksave/quickload console
commands (SparkGameFPS OnLoad returns before gameplay setup when the context
is headless), so this harness cannot kill a real FPS quicksave in flight. The
torn-slot injection covers the reader side of that contract until a headless
save path exists.

Platform note: the process scenarios run on Linux only. On Windows,
SparkEngine is a GUI-subsystem executable that skips AllocConsole when its
output is redirected, so it has no console to receive CTRL_BREAK_EVENT and
the graceful stop cannot be delivered. Windows coverage needs a
console-attached or non-console stop channel plus a real Windows run.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

IS_LINUX = sys.platform.startswith("linux")

BOOT_BOUND_SECONDS = 90.0
EXIT_BOUND_SECONDS = 45.0
BOUNDED_RUN_SECONDS = 120.0
BOOT_RETRY_LIMIT = 4

READY_FRAME = 30
TORN_SLOT = "fps_quicksave"

# Boot-phase log lines the host prints on its line-buffered stderr sink, in
# boot order. boot-interrupted kills on a seeded one of these.
BOOT_MARKERS = (
    "PhysicsSystem initializing",
    "SaveSystem initializing",
    "InitConsole: InitDebugSystems",
    "InitConsole: InitGameplaySystems",
    "Loading module:",
)

AUDIT_HEADER = re.compile(r"^frame (\d+) t=(\d+\.\d)s \| (ok |ERR) \| (.+)$")


class HarnessFailure(Exception):
    """A scenario requirement was not met."""


@dataclass
class RunResult:
    returncode: int | None
    stdout: str
    stderr: str
    audit: str
    elapsed: float

    @property
    def combined(self) -> str:
        return f"{self.stdout}\n{self.stderr}"


# ---------------------------------------------------------------------------
# Pure validators (covered by --self-test)
# ---------------------------------------------------------------------------


def parse_audit(audit: str) -> list[tuple[int, str, str, list[str]]]:
    """Split an -exec audit trail into (frame, status, command, output lines) entries."""
    entries: list[tuple[int, str, str, list[str]]] = []
    for line in audit.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        match = AUDIT_HEADER.match(line)
        if match:
            entries.append((int(match.group(1)), match.group(3).strip(), match.group(4), []))
        elif line.startswith("frame "):
            raise HarnessFailure(f"malformed audit header '{line}'")
        elif entries and line:
            entries[-1][3].append(line)
    return entries


def require_audit_commands(audit: str, expected: list[str]) -> list[tuple[int, str, str, list[str]]]:
    """Require exactly @p expected commands, in order, each dispatched ok with its own [exec] marker."""
    entries = parse_audit(audit)
    commands = [entry[2] for entry in entries]
    if commands != expected:
        raise HarnessFailure(f"audit commands were {commands}, expected {expected}")
    for frame, status, command, lines in entries:
        if status != "ok":
            raise HarnessFailure(f"command '{command}' was dispatched with status {status}")
        marker_prefix = f"    > [exec] frame {frame} (t="
        markers = [line for line in lines if line.startswith(marker_prefix) and line.endswith(f"): {command}")]
        if len(markers) != 1:
            raise HarnessFailure(f"command '{command}' had {len(markers)} [exec] markers, expected 1")
    return entries


def command_output_after_marker(entry: tuple[int, str, str, list[str]]) -> list[str]:
    """Console lines the command itself produced (everything after its [exec] marker)."""
    frame, _status, command, lines = entry
    for index, line in enumerate(lines):
        if line.startswith(f"    > [exec] frame {frame} (t=") and line.endswith(f"): {command}"):
            return lines[index + 1 :]
    return []


def require_clean_shutdown_text(combined: str, *, signal_driven: bool) -> None:
    """Host-level shutdown requirements beyond the strict NullRHI record parser.

    The loop only leaves through a passed CanShutdownEngine checkpoint, and the
    strict records prove teardown ran (unloaded=1, NullRHI shutdown=1); this adds
    that no module vetoed a checkpoint and, for a signal-driven stop, that the
    exit was not a test limit.
    """
    text = combined.replace("\r\n", "\n")
    if "Shutdown request cancelled" in text or "Exit postponed" in text:
        raise HarnessFailure("a module vetoed the CanShutdownEngine checkpoint")
    if signal_driven and "[TEST] Limit reached" in text:
        raise HarnessFailure("the host exited on a test limit, not on the shutdown request")
    fatal = [line for line in text.split("\n") if "[FATAL" in line]
    if fatal:
        raise HarnessFailure(f"host logged a fatal line: {fatal[0]!r}")


def require_torn_slot_rejected(entries: list[tuple[int, str, str, list[str]]]) -> None:
    """The recovery boot must neither list the torn slot as a save nor report metadata for it.

    The user-data tree is private to this harness, so the torn slot is the only
    candidate: the listing must be empty and save_info must answer "not found".
    """
    by_command = {entry[2]: entry for entry in entries}
    listing = command_output_after_marker(by_command["save_list"])
    # Slot rows are raw continuation lines ("  <slot> — ..."); SaveSystem's own
    # "[Save] ... too small" rejection warning may name the file and is expected.
    slot_rows = [line for line in listing if line.startswith(f"  {TORN_SLOT} ")]
    if listing.count("    > === Save Slots (0) ===") != 1 or slot_rows:
        raise HarnessFailure(f"save_list did not report an empty slot list: {listing}")
    info = command_output_after_marker(by_command[f"save_info {TORN_SLOT}"])
    if info.count(f"    > Save '{TORN_SLOT}' not found") != 1 or any("=== Save:" in line for line in info):
        raise HarnessFailure(f"save_info did not reject the torn slot: {info}")


# ---------------------------------------------------------------------------
# Process control
# ---------------------------------------------------------------------------


class Harness:
    def __init__(self, args: argparse.Namespace) -> None:
        self.engine = args.engine.resolve(strict=True)
        self.module = args.module.resolve(strict=True)
        self.cmake = args.cmake
        self.parser_script = (args.source_root / "cmake" / "RunSparkHeadlessNullRHILifecycle.cmake").resolve(
            strict=True
        )
        self.seed = args.seed if args.seed is not None else random.SystemRandom().randrange(1, 2**31)
        self.rng = random.Random(self.seed)
        self.root = Path(tempfile.mkdtemp(prefix="spark-headless-shutdown-"))
        self.user_root = self.root / "user"
        self.user_root.mkdir()
        self.run_index = 0

    # -- environment -------------------------------------------------------

    def environment(self) -> dict[str, str]:
        env = os.environ.copy()
        env["SPARK_RHI_BACKEND"] = "null"
        for key, name in (("HOME", "home"), ("XDG_DATA_HOME", "data"), ("XDG_CONFIG_HOME", "config"),
                          ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
            env[key] = str(self.user_root / name)
            Path(env[key]).mkdir(parents=True, exist_ok=True)
        return env

    def saves_dir(self) -> Path:
        return self.user_root / "data" / "SparkEngine" / "Saves"

    # -- launching ---------------------------------------------------------

    def launch(self, script_lines: list[str], extra_args: list[str]) -> tuple[subprocess.Popen, Path, Path, Path]:
        self.run_index += 1
        run_dir = self.root / f"run{self.run_index}"
        run_dir.mkdir()
        script = run_dir / "script.exec"
        script.write_text("".join(f"{line}\n" for line in script_lines), encoding="utf-8")
        audit = run_dir / "exec_audit.log"
        stdout_path = run_dir / "stdout.txt"
        stderr_path = run_dir / "stderr.txt"
        command = [
            str(self.engine), "-headless",
            "-game", str(self.module), "-require-game",
            "-threads", "2", "-no-subprocess",
            "-exec", str(script), "-exec-audit", str(audit),
            *extra_args,
        ]
        with open(stdout_path, "wb") as out, open(stderr_path, "wb") as err:
            process = subprocess.Popen(
                command, cwd=run_dir, env=self.environment(), stdin=subprocess.DEVNULL, stdout=out, stderr=err
            )
        return process, audit, stdout_path, stderr_path

    @staticmethod
    def read_text(path: Path) -> str:
        try:
            return path.read_bytes().decode("utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def collect(self, process: subprocess.Popen, audit: Path, stdout_path: Path, stderr_path: Path,
                started: float) -> RunResult:
        return RunResult(process.returncode, self.read_text(stdout_path), self.read_text(stderr_path),
                         self.read_text(audit), time.monotonic() - started)

    @staticmethod
    def hard_kill(process: subprocess.Popen) -> None:
        if process.poll() is None:
            process.kill()
            try:
                process.wait(timeout=EXIT_BOUND_SECONDS)
            except subprocess.TimeoutExpired as error:
                raise HarnessFailure(f"process {process.pid} survived a hard kill for {EXIT_BOUND_SECONDS}s") from error

    def wait_until(self, process: subprocess.Popen, predicate, bound: float, what: str) -> float:
        started = time.monotonic()
        while time.monotonic() - started < bound:
            if predicate():
                return time.monotonic() - started
            if process.poll() is not None:
                raise HarnessFailure(f"host exited with {process.returncode} before {what}")
            time.sleep(0.005)
        self.hard_kill(process)
        raise HarnessFailure(f"host did not reach {what} within {bound:.0f}s (hang)")

    def wait_exit(self, process: subprocess.Popen, bound: float, what: str) -> None:
        try:
            process.wait(timeout=bound)
        except subprocess.TimeoutExpired as error:
            self.hard_kill(process)
            raise HarnessFailure(f"host did not exit within {bound:.0f}s after {what} (hang)") from error

    def loop_dispatched(self, audit: Path) -> bool:
        return AUDIT_HEADER.match(self.read_text(audit).split("\n", 1)[0]) is not None

    def request_graceful_shutdown(self, process: subprocess.Popen) -> None:
        process.send_signal(signal.SIGTERM)

    def require_killed(self, result: RunResult) -> None:
        expected = -signal.SIGKILL
        if result.returncode != expected:
            raise HarnessFailure(f"forced kill exit status was {result.returncode}, expected {expected}")
        if "SPARK_HEADLESS_LIFECYCLE" in result.combined:
            raise HarnessFailure("killed host still published a lifecycle record (kill landed after teardown)")

    def require_strict_records(self, result: RunResult, run_label: str) -> None:
        """Apply the shared strict NullRHI lifecycle parser (single acceptance contract)."""
        run_dir = self.root / f"parse-{run_label}"
        run_dir.mkdir(exist_ok=True)
        stdout_file = run_dir / "stdout.txt"
        stderr_file = run_dir / "stderr.txt"
        stdout_file.write_text(result.stdout, encoding="utf-8")
        stderr_file.write_text(result.stderr, encoding="utf-8")
        wrapper = run_dir / "validate.cmake"
        wrapper.write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)\n"
            f'include("{self.parser_script.as_posix()}")\n'
            f'file(READ "{stdout_file.as_posix()}" _out)\n'
            f'file(READ "{stderr_file.as_posix()}" _err)\n'
            f'_spark_validate_headless_nullrhi_result("{result.returncode}" "${{_out}}" "${{_err}}" _ok _why)\n'
            "if(NOT _ok)\n  message(FATAL_ERROR \"${_why}\")\nendif()\n",
            encoding="utf-8",
        )
        check = subprocess.run([self.cmake, "-P", str(wrapper)], capture_output=True, text=True, timeout=120)
        if check.returncode != 0:
            reason = (check.stderr or check.stdout).strip().splitlines()
            detail = next((line.strip() for line in reason if line.strip() and "CMake Error" not in line), "?")
            raise HarnessFailure(f"{run_label}: strict NullRHI lifecycle records rejected: {detail}")

    # -- scenarios ---------------------------------------------------------

    def boot_to_loop(self, script: list[str]) -> tuple[subprocess.Popen, Path, Path, Path, float, float]:
        started = time.monotonic()
        process, audit, out, err = self.launch(script, [])
        ready = self.wait_until(process, lambda: self.loop_dispatched(audit), BOOT_BOUND_SECONDS,
                                f"the frame-{READY_FRAME} scripted command")
        return process, audit, out, err, started, ready

    def scenario_graceful(self) -> list[str]:
        process, audit, out, err, started, ready = self.boot_to_loop([f"{READY_FRAME} save_list"])
        dwell = self.rng.uniform(0.05, 0.5)
        time.sleep(dwell)
        signalled = time.monotonic()
        self.request_graceful_shutdown(process)
        self.wait_exit(process, EXIT_BOUND_SECONDS, "the graceful shutdown request")
        result = self.collect(process, audit, out, err, started)
        exit_seconds = time.monotonic() - signalled
        self.require_strict_records(result, "graceful")
        require_clean_shutdown_text(result.combined, signal_driven=True)
        require_audit_commands(result.audit, ["save_list"])
        frames = re.search(r"^SPARK_HEADLESS_RHI backend=null initialized=1 frames=(\d+)", result.stdout, re.M)
        if not frames or int(frames.group(1)) <= READY_FRAME:
            raise HarnessFailure("graceful run did not keep ticking past the readiness frame before the signal")
        return [f"graceful: ready={ready:.2f}s dwell={dwell:.3f}s exit_after_signal={exit_seconds:.2f}s "
                f"frames={frames.group(1)} rc={result.returncode}"]

    def inject_torn_save(self) -> list[str]:
        """Leave the slot the way a writer killed between temp write and rename leaves it."""
        saves = self.saves_dir()
        if not saves.is_dir():
            raise HarnessFailure(f"the killed host never created its save directory {saves}")
        primary = saves / f"{TORN_SLOT}.spark_save"
        orphan = saves / f"{TORN_SLOT}.spark_save.tmp"
        torn_length = self.rng.randrange(1, 24)
        primary.write_bytes(bytes(self.rng.randrange(256) for _ in range(torn_length)))
        orphan.write_bytes(bytes(self.rng.randrange(256) for _ in range(self.rng.randrange(1, 512))))
        return [f"forced-recovery: injected torn primary ({torn_length} bytes) and orphaned .tmp in {saves}"]

    def scenario_forced_recovery(self) -> list[str]:
        process, audit, out, err, started, ready = self.boot_to_loop([f"{READY_FRAME} save_list"])
        dwell = self.rng.uniform(0.0, 0.5)
        time.sleep(dwell)
        process.kill()
        self.wait_exit(process, EXIT_BOUND_SECONDS, "a forced kill")
        killed = self.collect(process, audit, out, err, started)
        self.require_killed(killed)
        notes = [f"forced-recovery: ready={ready:.2f}s kill_dwell={dwell:.3f}s rc={killed.returncode}"]
        notes += self.inject_torn_save()

        restart_started = time.monotonic()
        process, audit, out, err = self.launch(["5 save_list", f"6 save_info {TORN_SLOT}"], ["-test-frames", "30"])
        self.wait_exit(process, BOUNDED_RUN_SECONDS, "the bounded recovery boot")
        result = self.collect(process, audit, out, err, restart_started)
        self.require_strict_records(result, "forced-recovery-restart")
        require_clean_shutdown_text(result.combined, signal_driven=False)
        entries = require_audit_commands(result.audit, ["save_list", f"save_info {TORN_SLOT}"])
        require_torn_slot_rejected(entries)
        notes.append(f"forced-recovery: restart completed in {result.elapsed:.2f}s rc={result.returncode}")
        return notes

    def scenario_boot_interrupted(self) -> list[str]:
        notes: list[str] = []
        marker_index = self.rng.randrange(len(BOOT_MARKERS))
        for attempt in range(1, BOOT_RETRY_LIMIT + 1):
            marker = BOOT_MARKERS[marker_index]
            started = time.monotonic()
            process, audit, out, err = self.launch(["1 save_list"], [])
            seen = self.wait_until(process, lambda: marker in self.read_text(err) or marker in self.read_text(out),
                                   BOOT_BOUND_SECONDS, f"boot marker '{marker}'")
            process.kill()
            self.wait_exit(process, EXIT_BOUND_SECONDS, "a forced kill during boot")
            killed = self.collect(process, audit, out, err, started)
            self.require_killed(killed)
            reached_loop = bool(parse_audit(killed.audit))
            notes.append(f"boot-interrupted: attempt={attempt} marker='{marker}' at={seen:.3f}s "
                         f"rc={killed.returncode} reached_loop={int(reached_loop)}")
            if not reached_loop:
                break
            # The kill raced past bring-up; retry on an earlier boot marker.
            marker_index = max(0, marker_index - 1)
        else:
            raise HarnessFailure(f"could not interrupt boot before the loop in {BOOT_RETRY_LIMIT} attempts")

        restart_started = time.monotonic()
        process, audit, out, err = self.launch(["5 save_list"], ["-test-frames", "30"])
        self.wait_exit(process, BOUNDED_RUN_SECONDS, "the bounded boot after interruption")
        result = self.collect(process, audit, out, err, restart_started)
        self.require_strict_records(result, "boot-interrupted-restart")
        require_clean_shutdown_text(result.combined, signal_driven=False)
        require_audit_commands(result.audit, ["save_list"])
        notes.append(f"boot-interrupted: restart completed in {result.elapsed:.2f}s rc={result.returncode}")
        return notes


# ---------------------------------------------------------------------------
# Self-test of the pure validators
# ---------------------------------------------------------------------------

_SELF_TEST_AUDIT = (
    "frame 5 t=0.1s | ok  | save_list\n"
    "    > [Core] early boot line\n"
    "    > [exec] frame 5 (t=0.1s): save_list\n"
    "    > === Save Slots (0) ===\n"
    "frame 6 t=0.1s | ok  | save_info fps_quicksave\n"
    "    > [exec] frame 6 (t=0.1s): save_info fps_quicksave\n"
    "    > Save 'fps_quicksave' not found\n"
)


def self_test() -> int:
    def expect_failure(name: str, action) -> None:
        try:
            action()
        except HarnessFailure:
            return
        raise AssertionError(f"self-test case '{name}' unexpectedly passed")

    expected = ["save_list", f"save_info {TORN_SLOT}"]
    entries = require_audit_commands(_SELF_TEST_AUDIT, expected)
    require_torn_slot_rejected(entries)
    require_audit_commands(_SELF_TEST_AUDIT.replace("\n", "\r\n"), expected)

    expect_failure("missing-command", lambda: require_audit_commands(_SELF_TEST_AUDIT, ["save_list"]))
    expect_failure("err-dispatch", lambda: require_audit_commands(
        _SELF_TEST_AUDIT.replace("| ok  | save_list", "| ERR | save_list"), expected))
    expect_failure("missing-marker", lambda: require_audit_commands(
        _SELF_TEST_AUDIT.replace("    > [exec] frame 5 (t=0.1s): save_list\n", ""), expected))
    expect_failure("malformed-header", lambda: parse_audit("frame x | ok | save_list\n"))
    expect_failure("torn-slot-listed", lambda: require_torn_slot_rejected(require_audit_commands(
        _SELF_TEST_AUDIT.replace("=== Save Slots (0) ===",
                                 "=== Save Slots (1) ===\n  fps_quicksave — Quick Save [x]"),
        expected)))
    expect_failure("torn-slot-metadata", lambda: require_torn_slot_rejected(require_audit_commands(
        _SELF_TEST_AUDIT.replace("    > Save 'fps_quicksave' not found", "    > === Save: Quick Save ==="), expected)))
    expect_failure("stale-boot-line-only", lambda: require_torn_slot_rejected(require_audit_commands(
        _SELF_TEST_AUDIT.replace("    > === Save Slots (0) ===\n", ""), expected)))

    clean = "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=40 fixed=41 rendered=0 unloaded=1 faults=0\n"
    require_clean_shutdown_text(clean, signal_driven=True)
    expect_failure("fatal-line", lambda: require_clean_shutdown_text(
        clean + "[12:00:00.000] [TID:1] [FATAL] [Core] boom\n", signal_driven=False))
    expect_failure("vetoed", lambda: require_clean_shutdown_text(
        clean + "Shutdown request cancelled: a module could not checkpoint\n", signal_driven=True))
    expect_failure("test-limit-exit", lambda: require_clean_shutdown_text(
        clean + "[TEST] Limit reached (frame 30 / t=0.5s). Exiting.\n", signal_driven=True))
    require_clean_shutdown_text(clean + "[TEST] Limit reached (frame 30 / t=0.5s). Exiting.\n", signal_driven=False)
    print("headless shutdown/recovery harness self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="validate the harness checks and exit")
    parser.add_argument("--scenario", choices=("graceful", "forced-recovery", "boot-interrupted"))
    parser.add_argument("--engine", type=Path, help="SparkEngine executable")
    parser.add_argument("--module", type=Path, help="SparkGameFPS module")
    parser.add_argument("--source-root", type=Path, help="repository root (for the strict lifecycle parser)")
    parser.add_argument("--cmake", default="cmake", help="cmake executable used to run the strict parser")
    parser.add_argument("--seed", type=int, help="replay a previous run's timing seed")
    parser.add_argument("--keep", action="store_true", help="keep the temp run directory on success")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not (args.scenario and args.engine and args.module and args.source_root):
        parser.error("--scenario, --engine, --module and --source-root are required")
    if not IS_LINUX:
        print(f"HeadlessShutdown {args.scenario}: process scenarios are Linux-only (host={sys.platform}); "
              "see the platform note in this script", file=sys.stderr)
        return 1

    harness = Harness(args)
    scenario = {
        "graceful": harness.scenario_graceful,
        "forced-recovery": harness.scenario_forced_recovery,
        "boot-interrupted": harness.scenario_boot_interrupted,
    }[args.scenario]
    print(f"HeadlessShutdown {args.scenario}: seed={harness.seed} host={sys.platform} root={harness.root}")
    try:
        notes = scenario()
    except HarnessFailure as failure:
        print(f"HeadlessShutdown {args.scenario} FAILED (seed={harness.seed}): {failure}", file=sys.stderr)
        print(f"run artefacts kept in {harness.root}", file=sys.stderr)
        return 1
    for note in notes:
        print(note)
    print(f"HeadlessShutdown {args.scenario} passed (seed={harness.seed})")
    if not args.keep:
        shutil.rmtree(harness.root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
