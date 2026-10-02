"""Contract tests for the fail-closed Windows token launcher."""

from __future__ import annotations

import importlib.util
import ctypes
from pathlib import Path
import unittest
from unittest import mock
import contextlib
import subprocess
import sys
import types


MODULE_PATH = Path(__file__).with_name("run-non-elevated-windows.py")
SPEC = importlib.util.spec_from_file_location("run_non_elevated_windows", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


class NonElevatedLauncherTests(unittest.TestCase):
    def test_non_windows_fails_closed(self) -> None:
        with mock.patch.object(MODULE.os, "name", "posix"):
            with self.assertRaisesRegex(OSError, "only supported on Windows"):
                MODULE.run(["msiexec.exe"], "unused.log", 1)

    def test_empty_argv_is_rejected_before_platform_work(self) -> None:
        with mock.patch.object(MODULE.os, "name", "posix"):
            with self.assertRaisesRegex(OSError, "only supported on Windows"):
                MODULE.run([], "unused.log", 1)

    def test_source_has_no_elevation_fallback(self) -> None:
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn("CreateRestrictedToken", source)
        self.assertIn("TokenIntegrityLevel", source)
        self.assertIn("child token was not same-user medium-integrity", source)
        self.assertNotIn("ShellExecute", source)
        self.assertNotIn("runas", source.lower())

    def test_child_is_verified_before_resume(self) -> None:
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertLess(source.index("_same_user(source, child_token)"), source.index("ResumeThread"))
        self.assertIn("CREATE_SUSPENDED", source)

    def test_all_child_failure_paths_reap(self) -> None:
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertIn("child_created and not child_reaped", source)
        self.assertIn("timeout cleanup failed", source)
        self.assertIn("WaitForSingleObject failed", source)

    def test_safe_caller_uses_normal_subprocess_without_msvcrt_import_at_module_load(self) -> None:
        source = MODULE_PATH.read_text(encoding="utf-8")
        self.assertNotIn("import msvcrt\n", source.split("def run", 1)[0])
        self.assertIn("subprocess.run(argv", source)

    def test_mandatory_label_size_matches_documented_formula(self) -> None:
        sid_length = 28
        self.assertEqual(ctypes.sizeof(MODULE._TOKEN_MANDATORY_LABEL) + sid_length,
                         MODULE._mandatory_label_size(sid_length))

    def test_cleanup_failure_is_surfaced(self) -> None:
        terminate = mock.Mock(return_value=True)
        wait = mock.Mock(return_value=MODULE.WAIT_TIMEOUT)
        with mock.patch.object(MODULE.ctypes, "get_last_error", return_value=5, create=True), \
                self.assertRaisesRegex(OSError, "child did not terminate.*Win32 error 5"):
            MODULE._reap_child(17, terminate=terminate, wait=wait)
        terminate.assert_called_once_with(17, 1)
        self.assertEqual(wait.call_args_list, [mock.call(17, 0), mock.call(17, 5000)])

    def test_already_exited_child_does_not_get_terminated_again(self) -> None:
        terminate = mock.Mock()
        MODULE._reap_child(17, terminate=terminate, wait=mock.Mock(return_value=MODULE.WAIT_OBJECT_0))
        terminate.assert_not_called()

    def test_suspended_child_lifecycle_and_failure_cleanup(self) -> None:
        for scenario in ("success", "invalid-token", "wait-failed", "timeout", "resume-failed"):
            with self.subTest(scenario=scenario), contextlib.ExitStack() as stack:
                events = []

                def create(*args):
                    args[-1]._obj.hProcess = 42
                    args[-1]._obj.hThread = 43
                    events.append("created-suspended")
                    self.assertTrue(args[6] & MODULE.CREATE_SUSPENDED)
                    return True

                def validate(token):
                    if token == 11:
                        return False
                    events.append("validated-child")
                    return scenario != "invalid-token"

                def resume(handle):
                    events.append("resume")
                    return 0xFFFFFFFF if scenario == "resume-failed" else 1

                def wait(handle, milliseconds):
                    events.append(("wait", milliseconds))
                    if milliseconds == 1000:
                        return {"wait-failed": 0xFFFFFFFF, "timeout": MODULE.WAIT_TIMEOUT}.get(
                            scenario, MODULE.WAIT_OBJECT_0)
                    return MODULE.WAIT_TIMEOUT if milliseconds == 0 else MODULE.WAIT_OBJECT_0

                def terminate(handle, code):
                    events.append("terminate")
                    return True

                def exit_code(handle, code):
                    code._obj.value = 0
                    return True

                functions = {"GetCurrentProcess": lambda: 55, "SetHandleInformation": lambda *a: True,
                             "CreateProcessAsUserW": create, "ResumeThread": resume,
                             "WaitForSingleObject": wait, "TerminateProcess": terminate,
                             "GetExitCodeProcess": exit_code}
                fake_file = mock.Mock()
                fake_file.fileno.return_value = 9
                stack.enter_context(mock.patch.object(MODULE.os, "name", "nt"))
                stack.enter_context(mock.patch.object(MODULE, "Path", return_value=mock.Mock(open=mock.Mock(return_value=fake_file))))
                stack.enter_context(mock.patch("builtins.open", return_value=fake_file))
                stack.enter_context(mock.patch.dict(sys.modules, {"msvcrt": types.SimpleNamespace(get_osfhandle=lambda fd: fd)}))
                stack.enter_context(mock.patch.object(MODULE, "_api", side_effect=lambda name, *args: functions[name]))
                stack.enter_context(mock.patch.object(MODULE, "_token", side_effect=[11, 12]))
                stack.enter_context(mock.patch.object(MODULE, "_restricted_token", return_value=29))
                stack.enter_context(mock.patch.object(MODULE, "_token_is_medium_non_elevated", side_effect=validate))
                stack.enter_context(mock.patch.object(MODULE, "_token_is_elevated", return_value=True))
                stack.enter_context(mock.patch.object(MODULE, "_same_user", return_value=True))
                stack.enter_context(mock.patch.object(MODULE, "_close"))
                stack.enter_context(mock.patch.object(MODULE.ctypes, "get_last_error", return_value=5, create=True))
                if scenario == "success":
                    self.assertEqual(MODULE.run(["msiexec.exe"], "unused.log", 1), 0)
                    self.assertNotIn("terminate", events)
                else:
                    expected = subprocess.TimeoutExpired if scenario == "timeout" else OSError
                    with self.assertRaises(expected):
                        MODULE.run(["msiexec.exe"], "unused.log", 1)
                    self.assertEqual(events.count("terminate"), 1)
                    self.assertIn(("wait", 5000), events)
                if scenario == "invalid-token":
                    self.assertNotIn("resume", events)
                else:
                    self.assertLess(events.index("validated-child"), events.index("resume"))


if __name__ == "__main__":
    unittest.main()
