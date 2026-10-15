#!/usr/bin/env python3
"""HEAD-220 Windows forced-termination recovery smoke for the installed headless FPS host.

Runs the installed ``SparkEngine.exe -headless -game SparkGameFPS.dll -require-game`` on
NullRHI twice against one private user-data tree:

1. The writer boots, plays ``level / xp 37 / level / quicksave / level`` from an ``-exec`` script
   and then keeps ticking (no frame or time limit). Once the audit trail shows the quicksave was
   written, the harness calls ``TerminateProcess`` on it, so the host gets no shutdown, no
   destructors and no lifecycle records. The process must still have been alive immediately
   before the kill, and must report the harness's private exit code afterwards.
2. The save is then inspected: either the primary quicksave is a non-empty file (intact) or a
   non-empty ``.bak`` copy exists (recoverable). Neither is a failure.
3. A fresh reader boots on the same tree, runs ``level / quickload / level`` for 30 frames and
   must exit 0 with the strict NullRHI lifecycle records
   (cmake/RunSparkHeadlessNullRHILifecycle.cmake), ``SPARK_HEADLESS_NULLRHI_RESOURCES live=0``,
   a successful quickload that restores level 1 with 37 XP, and no fatal log line: the next
   boot after the kill is clean and the saved state survived.

Every wait is wall-clock bounded; a hang is a failure, never a retry. ``--self-test`` runs the
pure validators on canned host output and needs neither a build nor Windows.

Scope: the quicksave is one synchronous write inside one console dispatch, so the kill lands after
the write completed (the audit proves it), not inside it. Torn-write coverage (orphaned ``.tmp``,
torn primary) is run_headless_shutdown_recovery.py's forced-recovery scenario on Linux; this
harness covers the Windows host, TerminateProcess semantics, and recovery of a completed save.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BOOT_BOUND_SECONDS = 90.0
KILL_EXIT_BOUND_SECONDS = 30.0
RESTART_BOUND_SECONDS = 120.0
KILL_EXIT_CODE = 0x4B494C4C  # "KILL": distinguishes TerminateProcess from any exit the host chose itself
SAVE_SLOT = "fps_quicksave"

WRITER_SCRIPT = "1 level\n2 xp 37\n3 level\n4 quicksave\n5 level\n"
READER_SCRIPT = "1 level\n2 quickload\n3 level\n"
WRITER_COMMANDS = ["level", "xp 37", "level", "quicksave", "level"]
READER_COMMANDS = ["level", "quickload", "level"]

AUDIT_HEADER = re.compile(r"^frame (\d+) t=(\d+\.\d)s \| (ok |ERR) \| (.+)$")
SAVE_WRITTEN = f"    > Quick save written to slot '{SAVE_SLOT}'"
SAVE_FAILED = f"    > Quick save FAILED to write slot '{SAVE_SLOT}'"
LOAD_RESTORED = "    > Quick load restored level 1 (37 XP)"
LOAD_FAILED = f"    > Quick load FAILED for slot '{SAVE_SLOT}'"
LIVE_RESOURCES = re.compile(r"^SPARK_HEADLESS_NULLRHI_RESOURCES live=(\d+)$", re.M)


class HarnessFailure(Exception):
    """A scenario requirement was not met."""


Entry = tuple[int, str, str, list[str]]


# ---------------------------------------------------------------------------
# Pure validators (covered by --self-test)
# ---------------------------------------------------------------------------


def parse_audit(audit: str) -> list[Entry]:
    """Split an -exec audit trail into (frame, status, command, output lines) entries."""
    entries: list[Entry] = []
    for line in audit.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        match = AUDIT_HEADER.match(line)
        if match:
            entries.append((int(match.group(1)), match.group(3).strip(), match.group(4), []))
        elif line.startswith("frame "):
            raise HarnessFailure(f"malformed audit header '{line}'")
        elif entries and line:
            entries[-1][3].append(line)
    return entries


def require_commands(entries: list[Entry], expected: list[str], what: str) -> None:
    commands = [entry[2] for entry in entries]
    if commands != expected:
        raise HarnessFailure(f"{what}: audit commands were {commands}, expected {expected}")
    for frame, status, command, lines in entries:
        if status != "ok":
            raise HarnessFailure(f"{what}: command '{command}' was dispatched with status {status}")
        marker = f"    > [exec] frame {frame} (t="
        if sum(1 for line in lines if line.startswith(marker) and line.endswith(f"): {command}")) != 1:
            raise HarnessFailure(f"{what}: command '{command}' has no single [exec] marker")


def command_output(entry: Entry) -> list[str]:
    """Console lines the command itself produced (everything after its [exec] marker)."""
    frame, _status, command, lines = entry
    for index, line in enumerate(lines):
        if line.startswith(f"    > [exec] frame {frame} (t=") and line.endswith(f"): {command}"):
            return lines[index + 1 :]
    return []


def require_writer_saved(audit: str) -> None:
    """The writer's audit must show the quicksave command succeeded, exactly once, without a failure line."""
    entries = parse_audit(audit)
    saves = [entry for entry in entries if entry[2] == "quicksave"]
    if len(saves) != 1:
        raise HarnessFailure(f"writer audit has {len(saves)} quicksave entries, expected 1")
    output = command_output(saves[0])
    if saves[0][1] != "ok" or output.count(SAVE_WRITTEN) != 1 or SAVE_FAILED in output:
        raise HarnessFailure(f"writer quicksave did not report success: {output}")


def writer_saved(audit: str) -> bool:
    """True once the audit holds the success line (used to time the kill)."""
    try:
        return any(
            entry[2] == "quicksave" and entry[1] == "ok" and SAVE_WRITTEN in command_output(entry)
            for entry in parse_audit(audit)
        )
    except HarnessFailure:
        return False


def classify_save_state(primary_size: int | None, backup_size: int | None) -> str:
    """Return 'intact' or 'recoverable'; raise when the save is gone after the kill."""
    if primary_size is not None and primary_size > 0:
        return "intact"
    if backup_size is not None and backup_size > 0:
        return "recoverable"
    raise HarnessFailure(
        f"after the forced kill the quicksave is gone: primary size {primary_size}, backup size {backup_size}"
    )


def require_restart_audit(audit: str) -> None:
    """The restart's quickload must restore level 1 / 37 XP and the following level must show it."""
    entries = parse_audit(audit)
    require_commands(entries, READER_COMMANDS, "restart")
    load_output = command_output(entries[1])
    if load_output.count(LOAD_RESTORED) != 1 or LOAD_FAILED in load_output:
        raise HarnessFailure(f"restart quickload did not restore the saved state: {load_output}")
    for position, expected_xp in ((0, 0), (2, 37)):
        output = command_output(entries[position])
        if output.count("Level: 1/50") != 1 or output.count(f"XP: {expected_xp}/282") != 1:
            raise HarnessFailure(f"restart 'level' #{position} did not show level 1 with {expected_xp} XP: {output}")


def require_live_resources_zero(stdout: str) -> None:
    """SPARK_HEADLESS_NULLRHI_RESOURCES must be printed exactly once and report live=0."""
    records = LIVE_RESOURCES.findall(stdout.replace("\r\n", "\n"))
    if len(records) != 1:
        raise HarnessFailure(f"restart printed {len(records)} SPARK_HEADLESS_NULLRHI_RESOURCES records, expected 1")
    if records[0] != "0":
        raise HarnessFailure(f"restart left {records[0]} live NullRHI resources, expected 0")


def require_no_fatal(combined: str, what: str) -> None:
    fatal = [line for line in combined.replace("\r\n", "\n").split("\n") if "[FATAL" in line]
    if fatal:
        raise HarnessFailure(f"{what} logged a fatal line: {fatal[0]!r}")


# ---------------------------------------------------------------------------
# Process control
# ---------------------------------------------------------------------------


def terminate_process(process: subprocess.Popen) -> None:
    """TerminateProcess with a private exit code (no shutdown handlers, no destructors)."""
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel32.OpenProcess(0x0001, False, process.pid)  # PROCESS_TERMINATE
    if not handle:
        raise HarnessFailure(f"OpenProcess({process.pid}) failed: {ctypes.get_last_error()}")
    try:
        if not kernel32.TerminateProcess(handle, KILL_EXIT_CODE):
            raise HarnessFailure(f"TerminateProcess({process.pid}) failed: {ctypes.get_last_error()}")
    finally:
        kernel32.CloseHandle(handle)


def read_text(path: Path) -> str:
    try:
        return path.read_bytes().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def file_size(path: Path) -> int | None:
    try:
        return path.stat().st_size if path.is_file() else None
    except FileNotFoundError:
        return None


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Harness:
    def __init__(self, args: argparse.Namespace) -> None:
        self.engine = args.engine.resolve(strict=True)
        self.module = args.module.resolve(strict=True)
        self.cmake = args.cmake
        self.parser_script = (args.source_root / "cmake" / "RunSparkHeadlessNullRHILifecycle.cmake").resolve(
            strict=True
        )
        self.root = args.workdir
        self.root.mkdir(parents=True, exist_ok=True)
        self.user_root = self.root / "user"
        self.saves = self.user_root / "localappdata" / "SparkEngine" / "Saves"
        for name in ("localappdata", "appdata"):
            (self.user_root / name).mkdir(parents=True, exist_ok=True)

    def environment(self) -> dict[str, str]:
        env = os.environ.copy()
        env["SPARK_RHI_BACKEND"] = "null"
        env["LOCALAPPDATA"] = str(self.user_root / "localappdata")
        env["APPDATA"] = str(self.user_root / "appdata")
        return env

    def launch(self, name: str, script: str, extra: list[str]) -> tuple[subprocess.Popen, Path, Path, Path]:
        run_dir = self.root / name
        run_dir.mkdir()
        script_path = run_dir / "script.exec"
        script_path.write_text(script, encoding="utf-8", newline="\n")
        audit = run_dir / "exec_audit.log"
        out_path = run_dir / "stdout.txt"
        err_path = run_dir / "stderr.txt"
        command = [
            str(self.engine), "-headless", "-game", str(self.module), "-require-game",
            "-threads", "2", "-no-subprocess", "-exec", str(script_path), "-exec-audit", str(audit), *extra,
        ]
        with open(out_path, "wb") as out, open(err_path, "wb") as err:
            process = subprocess.Popen(
                command, cwd=run_dir, env=self.environment(), stdin=subprocess.DEVNULL, stdout=out, stderr=err
            )
        return process, audit, out_path, err_path

    def require_strict_records(self, returncode: int, stdout: str, stderr: str) -> None:
        """Apply the shared strict NullRHI lifecycle parser (the single acceptance contract)."""
        parse_dir = self.root / "strict-parse"
        parse_dir.mkdir(exist_ok=True)
        (parse_dir / "stdout.txt").write_text(stdout, encoding="utf-8")
        (parse_dir / "stderr.txt").write_text(stderr, encoding="utf-8")
        wrapper = parse_dir / "validate.cmake"
        wrapper.write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)\n"
            f'include("{self.parser_script.as_posix()}")\n'
            f'file(READ "{(parse_dir / "stdout.txt").as_posix()}" _out)\n'
            f'file(READ "{(parse_dir / "stderr.txt").as_posix()}" _err)\n'
            f'_spark_validate_headless_nullrhi_result("{returncode}" "${{_out}}" "${{_err}}" _ok _why)\n'
            'if(NOT _ok)\n  message(FATAL_ERROR "${_why}")\nendif()\n',
            encoding="utf-8",
        )
        check = subprocess.run([self.cmake, "-P", str(wrapper)], capture_output=True, text=True, timeout=120)
        if check.returncode != 0:
            lines = [ln.strip() for ln in (check.stderr or check.stdout).splitlines() if ln.strip()]
            detail = next((ln for ln in lines if "CMake Error" not in ln), "?")
            raise HarnessFailure(f"restart: strict NullRHI lifecycle records rejected: {detail}")

    def run_writer_and_kill(self) -> list[str]:
        started = time.monotonic()
        process, audit, out_path, err_path = self.launch("writer", WRITER_SCRIPT, [])
        try:
            while not writer_saved(read_text(audit)):
                if process.poll() is not None:
                    raise HarnessFailure(f"writer exited with {process.returncode} before its quicksave")
                if time.monotonic() - started > BOOT_BOUND_SECONDS:
                    raise HarnessFailure(f"writer did not save within {BOOT_BOUND_SECONDS:.0f}s (hang)")
                time.sleep(0.01)
            saved_after = time.monotonic() - started
            if process.poll() is not None:
                raise HarnessFailure(f"writer exited with {process.returncode} on its own before the kill")
            terminate_process(process)
            try:
                process.wait(timeout=KILL_EXIT_BOUND_SECONDS)
            except subprocess.TimeoutExpired as error:
                raise HarnessFailure(f"writer survived TerminateProcess for {KILL_EXIT_BOUND_SECONDS:.0f}s") from error
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        if process.returncode != KILL_EXIT_CODE:
            raise HarnessFailure(f"writer exit status was {process.returncode}, expected {KILL_EXIT_CODE}")
        combined = f"{read_text(out_path)}\n{read_text(err_path)}"
        if "SPARK_HEADLESS_LIFECYCLE" in combined:
            raise HarnessFailure("killed writer published lifecycle records: the kill landed after teardown")
        require_writer_saved(read_text(audit))
        return [f"writer: quicksave written after {saved_after:.2f}s, TerminateProcess exit={process.returncode:#x}"]

    def run_restart(self) -> list[str]:
        primary = self.saves / f"{SAVE_SLOT}.spark_save"
        state = classify_save_state(file_size(primary), file_size(primary.with_name(primary.name + ".bak")))
        before = sha256(primary) if state == "intact" else "n/a"
        started = time.monotonic()
        process, audit, out_path, err_path = self.launch("restart", READER_SCRIPT, ["-test-frames", "30"])
        try:
            process.wait(timeout=RESTART_BOUND_SECONDS)
        except subprocess.TimeoutExpired as error:
            process.kill()
            process.wait()
            raise HarnessFailure(f"restart did not finish within {RESTART_BOUND_SECONDS:.0f}s (hang)") from error
        stdout, stderr = read_text(out_path), read_text(err_path)
        self.require_strict_records(process.returncode, stdout, stderr)
        require_live_resources_zero(stdout)
        require_no_fatal(f"{stdout}\n{stderr}", "restart")
        require_restart_audit(read_text(audit))
        after = sha256(primary) if file_size(primary) else "n/a"
        if state == "intact" and before != after:
            raise HarnessFailure(f"restart mutated the intact quicksave: {before} -> {after}")
        return [f"save after kill: {state}", f"restart: clean boot in {time.monotonic() - started:.2f}s, live=0"]


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

_WRITER_AUDIT = (
    "frame 1 t=0.0s | ok  | level\n    > [exec] frame 1 (t=0.0s, entry=0): level\n    > === Progression ===\n"
    "Level: 1/50\nXP: 0/282\n"
    "frame 2 t=0.0s | ok  | xp 37\n    > [exec] frame 2 (t=0.0s, entry=1): xp 37\n    > Awarded 37 XP (level 1)\n"
    "frame 3 t=0.1s | ok  | level\n    > [exec] frame 3 (t=0.1s, entry=2): level\n    > === Progression ===\n"
    "Level: 1/50\nXP: 37/282\n"
    f"frame 4 t=0.1s | ok  | quicksave\n    > [exec] frame 4 (t=0.1s, entry=3): quicksave\n{SAVE_WRITTEN}\n"
)
_READER_AUDIT = (
    "frame 1 t=0.0s | ok  | level\n    > [exec] frame 1 (t=0.0s, entry=0): level\n    > === Progression ===\n"
    "Level: 1/50\nXP: 0/282\n"
    f"frame 2 t=0.0s | ok  | quickload\n    > [exec] frame 2 (t=0.0s, entry=1): quickload\n{LOAD_RESTORED}\n"
    "frame 3 t=0.1s | ok  | level\n    > [exec] frame 3 (t=0.1s, entry=2): level\n    > === Progression ===\n"
    "Level: 1/50\nXP: 37/282\n"
)


def _expect_failure(label: str, action) -> None:
    try:
        action()
    except HarnessFailure:
        return
    raise AssertionError(f"self-test case '{label}' unexpectedly passed")


def self_test() -> int:
    # Writer audit: kill is timed on the success line and only the success line.
    assert writer_saved(_WRITER_AUDIT)
    assert not writer_saved(_WRITER_AUDIT.replace(SAVE_WRITTEN, SAVE_FAILED))
    assert not writer_saved(_WRITER_AUDIT.split("frame 4")[0])
    assert not writer_saved("frame malformed\n")
    assert not writer_saved(_WRITER_AUDIT.replace("| ok  | quicksave", "| ERR | quicksave"))
    assert writer_saved(_WRITER_AUDIT.replace("\n", "\r\n"))
    require_writer_saved(_WRITER_AUDIT)
    _expect_failure("writer-failed-save", lambda: require_writer_saved(_WRITER_AUDIT.replace(SAVE_WRITTEN, SAVE_FAILED)))
    _expect_failure("writer-no-save", lambda: require_writer_saved(_WRITER_AUDIT.split("frame 4")[0]))
    _expect_failure("writer-two-saves", lambda: require_writer_saved(_WRITER_AUDIT + _WRITER_AUDIT))

    # Save state after the kill.
    assert classify_save_state(1234, None) == "intact"
    assert classify_save_state(0, 99) == "recoverable"
    assert classify_save_state(None, 99) == "recoverable"
    _expect_failure("save-gone", lambda: classify_save_state(None, None))
    _expect_failure("save-empty", lambda: classify_save_state(0, 0))

    # Restart audit.
    require_restart_audit(_READER_AUDIT)
    require_restart_audit(_READER_AUDIT.replace("\n", "\r\n"))
    _expect_failure("restart-load-failed", lambda: require_restart_audit(_READER_AUDIT.replace(LOAD_RESTORED, LOAD_FAILED)))
    _expect_failure("restart-wrong-xp", lambda: require_restart_audit(_READER_AUDIT.replace("XP: 37/282", "XP: 0/282")))
    _expect_failure("restart-missing-load", lambda: require_restart_audit(_READER_AUDIT.replace(LOAD_RESTORED + "\n", "")))
    _expect_failure("restart-wrong-command", lambda: require_restart_audit(_READER_AUDIT.replace("quickload", "quicksave")))
    _expect_failure("restart-err-dispatch", lambda: require_restart_audit(_READER_AUDIT.replace("| ok  | quickload", "| ERR | quickload")))

    # Resource record.
    require_live_resources_zero("x\nSPARK_HEADLESS_NULLRHI_RESOURCES live=0\ny\n")
    require_live_resources_zero("SPARK_HEADLESS_NULLRHI_RESOURCES live=0\r\n")
    _expect_failure("live-nonzero", lambda: require_live_resources_zero("SPARK_HEADLESS_NULLRHI_RESOURCES live=3\n"))
    _expect_failure("live-absent", lambda: require_live_resources_zero("SPARK_HEADLESS_SHUTDOWN ms=4\n"))
    _expect_failure(
        "live-twice",
        lambda: require_live_resources_zero("SPARK_HEADLESS_NULLRHI_RESOURCES live=0\n" * 2),
    )
    _expect_failure("live-embedded", lambda: require_live_resources_zero("x SPARK_HEADLESS_NULLRHI_RESOURCES live=0\n"))

    require_no_fatal("[INFO] fine\n", "restart")
    _expect_failure("fatal", lambda: require_no_fatal("[FATAL] boom\n", "restart"))
    print("forced_kill_recovery: parser self-test passed")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="run the pure validators and exit")
    parser.add_argument("--engine", type=Path, help="installed SparkEngine.exe")
    parser.add_argument("--module", type=Path, help="installed SparkGameFPS.dll")
    parser.add_argument("--source-root", type=Path, help="repository root (strict lifecycle parser)")
    parser.add_argument("--cmake", default="cmake", help="cmake executable used to run the strict parser")
    parser.add_argument("--workdir", type=Path, help="fresh directory for runs, logs and the private user tree")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    missing = [name for name in ("engine", "module", "source_root", "workdir") if getattr(args, name) is None]
    if missing:
        parser.error("missing --" + ", --".join(name.replace("_", "-") for name in missing))
    if os.name != "nt":
        print("forced_kill_recovery: TerminateProcess recovery runs on Windows only", file=sys.stderr)
        return 2
    if args.workdir.exists() and any(args.workdir.iterdir()):
        print(f"forced_kill_recovery: --workdir {args.workdir} must be empty", file=sys.stderr)
        return 2
    try:
        harness = Harness(args)
        notes = harness.run_writer_and_kill() + harness.run_restart()
    except HarnessFailure as failure:
        print(f"forced_kill_recovery: FAIL {failure}", file=sys.stderr)
        return 1
    for note in notes:
        print(f"forced_kill_recovery: {note}")
    print("forced_kill_recovery: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
