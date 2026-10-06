#!/usr/bin/env python3
"""Behavioral tests for tools/build-lock.sh (the shared build-directory lock).

Every case runs the real script against a private lock file. The no-inherit
case is the regression for a 2026-09-24 deadlock: a detached helper inherited
the build lock descriptor and waited on a `pgrep -f` pattern that matched the
queued waiters themselves, so the lock was never released.
"""
import fcntl
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "build-lock.sh"

HOLD_LOCK = (
    "import fcntl, sys, time\n"
    "f = open(sys.argv[1], 'a+')\n"
    "fcntl.flock(f, fcntl.LOCK_EX)\n"
    "print('held', flush=True)\n"
    "time.sleep(float(sys.argv[2]))\n"
)


def lock_is_free(path):
    with open(path, "a+") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return False
        fcntl.flock(handle, fcntl.LOCK_UN)
        return True


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


@unittest.skipUnless(shutil.which("flock") and os.name == "posix", "flock(1) required")
class BuildLockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="spark-build-lock-"))
        self.lock = self.tmp / "build.lock"
        self.cleanup_pids = []
        self.cleanup_procs = []

    def tearDown(self):
        for pid in self.cleanup_pids:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for proc in self.cleanup_procs:
            proc.kill()
            proc.wait()
            for stream in (proc.stdout, proc.stderr):
                if stream:
                    stream.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_lock(self, *args, timeout=60):
        return subprocess.run(
            ["bash", str(SCRIPT), "--lock", str(self.lock), *args],
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def hold_lock_externally(self, seconds):
        holder = subprocess.Popen(
            [sys.executable, "-c", HOLD_LOCK, str(self.lock), str(seconds)],
            stdout=subprocess.PIPE,
            text=True,
        )
        self.cleanup_procs.append(holder)
        self.assertEqual(holder.stdout.readline().strip(), "held")
        return holder

    def test_propagates_command_status_and_clears_holder_record(self):
        record = self.tmp / "record.txt"
        result = self.run_lock("--", "bash", "-c", f'cat "{self.lock}" > "{record}"; exit 3')
        self.assertEqual(result.returncode, 3, result.stderr)
        text = record.read_text()
        self.assertIn("pid=", text)
        self.assertIn("command=bash -c", text)
        self.assertEqual(self.lock.read_text(), "", "holder record must be cleared on release")
        self.assertTrue(lock_is_free(self.lock))

    def test_times_out_with_exit_75_and_names_the_holder(self):
        holder = self.hold_lock_externally(30)
        started = time.monotonic()
        result = self.run_lock("--timeout", "1", "--", "true")
        self.assertEqual(result.returncode, 75, result.stderr)
        self.assertLess(time.monotonic() - started, 15)
        self.assertIn("timed out", result.stderr)
        self.assertIn(str(holder.pid), result.stderr, "diagnostics must list the process holding the lock")

    def test_spawned_background_process_cannot_keep_the_lock(self):
        pid_file = self.tmp / "orphan.pid"
        result = self.run_lock("--", "bash", "-c", f'sleep 30 >/dev/null 2>&1 & echo $! > "{pid_file}"')
        self.assertEqual(result.returncode, 0, result.stderr)
        orphan = int(pid_file.read_text())
        self.cleanup_pids.append(orphan)
        self.assertTrue(pid_alive(orphan), "fixture orphan should still be running")
        self.assertTrue(lock_is_free(self.lock), "a lingering child must not inherit the lock descriptor")

    def test_status_reports_free_and_held(self):
        free = self.run_lock("--status")
        self.assertEqual(free.returncode, 0, free.stderr)
        self.assertIn("free", free.stderr)

        holder = self.hold_lock_externally(30)
        held = self.run_lock("--status")
        self.assertEqual(held.returncode, 1, held.stderr)
        self.assertIn("held", held.stderr)
        self.assertIn(str(holder.pid), held.stderr)

    def test_status_flags_lock_held_after_recorded_holder_died(self):
        dead = subprocess.Popen(["true"])  # reaped below, so its pid is known-dead
        dead.wait()
        self.lock.write_text(f"pid={dead.pid}\ncommand=cmake --build build\n")
        self.hold_lock_externally(30)
        result = self.run_lock("--status")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("orphaned", result.stderr)

    def test_term_is_forwarded_to_the_command(self):
        pid_file = self.tmp / "child.pid"
        wrapper = subprocess.Popen(
            ["bash", str(SCRIPT), "--lock", str(self.lock), "--", "bash", "-c",
             f'echo $$ > "{pid_file}"; exec sleep 30'],
            stderr=subprocess.PIPE,
        )
        self.cleanup_procs.append(wrapper)
        deadline = time.monotonic() + 10
        while not (pid_file.exists() and pid_file.read_text().strip()) and time.monotonic() < deadline:
            time.sleep(0.05)
        child = int(pid_file.read_text())
        self.cleanup_pids.append(child)

        wrapper.send_signal(signal.SIGTERM)
        wrapper.wait(timeout=10)
        deadline = time.monotonic() + 5
        while pid_alive(child) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(pid_alive(child), "the command must not outlive a terminated wrapper")
        self.assertTrue(lock_is_free(self.lock))

    def test_rejects_symlinked_lock_and_missing_command(self):
        target = self.tmp / "elsewhere"
        target.write_text("")
        self.lock.symlink_to(target)
        result = self.run_lock("--", "true")
        self.assertEqual(result.returncode, 64, result.stderr)
        self.lock.unlink()

        result = self.run_lock()
        self.assertEqual(result.returncode, 64, result.stderr)


if __name__ == "__main__":
    unittest.main()
