#!/usr/bin/env python3
"""Regression tests for the orchestration process-smoke harness."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest import mock


HARNESS_PATH = Path(__file__).with_name("test_orchestration_process.py")
SPEC = importlib.util.spec_from_file_location("orchestration_process_harness", HARNESS_PATH)
assert SPEC is not None and SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HARNESS)


class ProcessLogCaptureTests(unittest.TestCase):
    def test_process_log_uses_unbuffered_append_file(self) -> None:
        log_path = Path("daemon.log")
        expected = object()
        with mock.patch.object(Path, "open", return_value=expected) as open_mock:
            self.assertIs(HARNESS.open_process_log(log_path), expected)
        open_mock.assert_called_once_with("ab", buffering=0)

    def test_supervised_pid_inventory_ignores_invalid_and_stopped_entries(self) -> None:
        known: set[int] = set()
        HARNESS.record_supervised_pids(
            "live\trunning\tpid=4123\n"
            "bad\trunning\tpid=not-a-number\n"
            "stopped\tstopped\tpid=0\n",
            known,
        )
        self.assertEqual(known, {4123})

    def test_windows_tree_cleanup_uses_taskkill_tree_mode(self) -> None:
        daemon = mock.Mock()
        daemon.pid = 100
        daemon.poll.return_value = 0
        with (
            mock.patch.object(HARNESS.os, "name", "nt"),
            mock.patch.object(HARNESS.subprocess, "run") as run_mock,
        ):
            HARNESS.force_terminate_process_tree(daemon, {200})

        self.assertEqual(run_mock.call_count, 2)
        commands = [call.args[0] for call in run_mock.call_args_list]
        self.assertEqual(commands[0], ["taskkill", "/PID", "100", "/T", "/F"])
        self.assertEqual(commands[1], ["taskkill", "/PID", "200", "/T", "/F"])


if __name__ == "__main__":
    unittest.main()
