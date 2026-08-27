#!/usr/bin/env python3
"""Regression tests for the orchestration process-smoke harness."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
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

    def test_posix_cleanup_reaches_child_group_after_daemon_group_is_gone(self) -> None:
        daemon = mock.Mock()
        daemon.pid = 100
        daemon.poll.return_value = 0
        group_signals: list[tuple[int, int]] = []
        process_signals: list[tuple[int, int]] = []

        def killpg(pid: int, signal_number: int) -> None:
            group_signals.append((pid, signal_number))
            raise ProcessLookupError

        def kill(pid: int, signal_number: int) -> None:
            process_signals.append((pid, signal_number))
            raise ProcessLookupError

        with (
            mock.patch.object(HARNESS.os, "name", "posix"),
            mock.patch.object(HARNESS.os, "killpg", side_effect=killpg, create=True),
            mock.patch.object(HARNESS.os, "kill", side_effect=kill),
        ):
            HARNESS.force_terminate_process_tree(daemon, {200})

        self.assertIn((100, HARNESS.signal.SIGTERM), group_signals)
        self.assertIn((200, HARNESS.signal.SIGTERM), group_signals)
        self.assertIn((200, HARNESS.signal.SIGTERM), process_signals)

    def test_posix_cleanup_force_kills_a_child_group_that_does_not_exit(self) -> None:
        daemon = mock.Mock()
        daemon.pid = 100
        daemon.poll.return_value = 0
        group_signals: list[tuple[int, int]] = []
        live_groups = {200}
        sigkill = 9

        def killpg(pid: int, signal_number: int) -> None:
            group_signals.append((pid, signal_number))
            if pid not in live_groups:
                raise ProcessLookupError
            if signal_number == sigkill:
                live_groups.remove(pid)

        with (
            mock.patch.object(HARNESS.os, "name", "posix"),
            mock.patch.object(HARNESS.os, "killpg", side_effect=killpg, create=True),
            mock.patch.object(HARNESS.os, "kill", side_effect=ProcessLookupError),
            mock.patch.object(HARNESS.signal, "SIGKILL", sigkill, create=True),
            mock.patch.object(HARNESS.time, "monotonic", side_effect=[0.0, 4.0]),
        ):
            HARNESS.force_terminate_process_tree(daemon, {200})

        self.assertIn((200, HARNESS.signal.SIGTERM), group_signals)
        self.assertIn((200, sigkill), group_signals)
        self.assertNotIn(200, live_groups)

    def test_udp_reservation_is_held_until_context_exit(self) -> None:
        with HARNESS.reserve_udp_port() as port:
            contender = HARNESS.socket.socket(
                HARNESS.socket.AF_INET, HARNESS.socket.SOCK_DGRAM
            )
            try:
                with self.assertRaises(OSError):
                    contender.bind(("127.0.0.1", port))
            finally:
                contender.close()

        with HARNESS.socket.socket(
            HARNESS.socket.AF_INET, HARNESS.socket.SOCK_DGRAM
        ) as successor:
            successor.bind(("127.0.0.1", port))


class SupervisedEngineHostTests(unittest.TestCase):
    """The graceful-stop assertion must fail closed on a terminated host.

    SparkDaemon stops a supervised process with CTRL_BREAK_EVENT (Windows) or
    SIGTERM (POSIX). Both leave the child `stopped pid=0`, so process state alone
    cannot tell a graceful shutdown from a kill. Only the host's own final
    live=false health snapshot can, which is what these tests pin down.
    """

    def _run(self, statuses: str, final_health: dict):
        recorded: list[tuple[str, ...]] = []

        with tempfile.TemporaryDirectory(prefix="spark-harness-") as temporary:
            scratch = Path(temporary)
            health_path = scratch / "supervised-server-health.json"
            health_path.write_text(json.dumps({"live": True, "ready": True}), encoding="utf-8")

            def invoke(*command: str, **_: object):
                recorded.append(command)
                if command[0] == "stop":
                    health_path.write_text(json.dumps(final_health), encoding="utf-8")
                return mock.Mock(stdout=statuses, returncode=0)

            error: Exception | None = None
            try:
                HARNESS.run_supervised_engine_host(
                    invoke,
                    server=scratch / "SparkServer",
                    game_module=scratch / "SparkGame",
                    working_dir=scratch,
                    scratch=scratch,
                )
            except Exception as raised:  # noqa: BLE001 - the assertion under test
                error = raised
            return recorded, error

    def test_terminated_host_is_reported_as_a_failed_graceful_stop(self) -> None:
        recorded, error = self._run(
            "live-server\tstopped\tpid=0\n", {"live": True, "ready": True}
        )
        self.assertIsInstance(error, RuntimeError)
        self.assertIn("terminated instead of honouring", str(error))
        self.assertNotIn(("undefine", "live-server"), recorded)

    def test_gracefully_stopped_host_passes_and_is_undefined(self) -> None:
        recorded, error = self._run(
            "live-server\tstopped\tpid=0\n",
            {"live": False, "ready": False, "stopping": False},
        )
        self.assertIsNone(error)
        self.assertEqual(recorded[0][0], "define")
        self.assertIn(("start", "live-server"), recorded)
        self.assertIn(("stop", "live-server"), recorded)
        self.assertIn(("undefine", "live-server"), recorded)

    def test_health_snapshot_read_tolerates_a_rename_race(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spark-harness-") as temporary:
            missing = Path(temporary) / "absent.json"
            self.assertIsNone(HARNESS.read_health(missing))
            partial = Path(temporary) / "partial.json"
            partial.write_text('{"live": tru', encoding="utf-8")
            self.assertIsNone(HARNESS.read_health(partial))
            complete = Path(temporary) / "complete.json"
            complete.write_text('{"live": false}', encoding="utf-8")
            self.assertEqual(HARNESS.read_health(complete), {"live": False})


if __name__ == "__main__":
    unittest.main()
