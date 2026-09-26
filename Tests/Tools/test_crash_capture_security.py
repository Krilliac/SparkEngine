"""Focused ownership and fail-closed contracts for the native capture driver."""
from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import run_crash_capture_security as driver


class CrashCaptureDriverTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="crash-driver-test-")
        self.root = Path(self.scratch.name).resolve()
        self.host_fields = driver.PRODUCER_ARTIFACT_FIELDS
        # Host-independent: exercise the Windows minidump contract by default;
        # POSIX-specific tests switch to the log-only producer contract.
        self.use_producer_fields(driver.WINDOWS_PRODUCER_FIELDS)

    def use_producer_fields(self, fields):
        patcher = mock.patch.object(driver, "PRODUCER_ARTIFACT_FIELDS", fields)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        self.scratch.cleanup()

    def write_manifest(self, **updates):
        manifest = dict(enginePID="123", logFile="crash.log", dumpFile="crash.dmp",
                        screenshotFile="", zipFile="", requireConsent=False,
                        promptUserDescription=False, fullMemoryDump=False)
        manifest.update(updates)
        path = self.root / "crash_manifest_0123456789abcdef.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return path

    def test_child_environment_removes_inherited_selection_and_owns_temp(self):
        with mock.patch.dict(os.environ, {"SPARK_TEST_LIMIT": "0", "SPARK_TEST_EXCLUDE": "Crash",
                                          "SPARK_TEST_FILE": "wrong.cpp"}):
            env = driver.producer_environment(self.root)
        self.assertNotIn("SPARK_TEST_LIMIT", env)
        self.assertNotIn("SPARK_TEST_EXCLUDE", env)
        self.assertNotIn("SPARK_TEST_FILE", env)
        self.assertEqual(env["SPARK_TEST_NAME"], driver.PRODUCER_TEST)
        self.assertEqual(env["SPARK_TEST_EXPECT_COUNT"], "1")
        self.assertEqual(env["TEMP"], str(self.root))
        self.assertEqual(env["TMP"], str(self.root))

    def test_discovery_requires_exact_pid_name_and_single_directory(self):
        (self.root / ("spark_crash_999_" + "a" * 32)).mkdir()
        with self.assertRaises(driver.CaptureError):
            driver.discover_capture(self.root, 123)
        expected = self.root / ("spark_crash_123_" + "a" * 32)
        expected.mkdir()
        self.assertEqual(driver.discover_capture(self.root, 123), expected)
        (self.root / ("spark_crash_123_" + "b" * 32)).mkdir()
        with self.assertRaises(driver.CaptureError):
            driver.discover_capture(self.root, 123)

    def test_discovery_rejects_a_file_with_the_root_name(self):
        (self.root / ("spark_crash_123_" + "a" * 32)).write_bytes(b"not a directory")
        with self.assertRaises(driver.CaptureError):
            driver.discover_capture(self.root, 123)

    def test_manifest_requires_real_pid_dump_and_noninteractive_flags(self):
        path = self.write_manifest()
        self.assertEqual(driver.read_manifest(self.root, 123)[0], path.name)
        for change in ({"enginePID": "999"}, {"dumpFile": ""}, {"requireConsent": True},
                       {"screenshotFile": "screen.png"}, {"fullMemoryDump": True}):
            self.write_manifest(**change)
            with self.assertRaises(driver.CaptureError, msg=str(change)):
                driver.read_manifest(self.root, 123)

    def test_host_selects_its_production_producer_contract(self):
        expected = driver.WINDOWS_PRODUCER_FIELDS if os.name == "nt" else driver.POSIX_PRODUCER_FIELDS
        self.assertEqual(self.host_fields, expected)

    def test_posix_manifest_requires_log_and_an_empty_dump_reference(self):
        self.use_producer_fields(driver.POSIX_PRODUCER_FIELDS)
        path = self.write_manifest(dumpFile="")
        self.assertEqual(driver.read_manifest(self.root, 123)[0], path.name)
        for change in ({"dumpFile": "crash.core"}, {"dumpFile": None}, {"logFile": ""},
                       {"enginePID": "999"}, {"requireConsent": True}, {"zipFile": "crash.zip"}):
            self.write_manifest(**{"dumpFile": "", **change})
            with self.assertRaises(driver.CaptureError, msg=str(change)):
                driver.read_manifest(self.root, 123)
        manifest = json.loads(path.read_text(encoding="utf-8"))
        del manifest["dumpFile"]
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(driver.CaptureError):
            driver.read_manifest(self.root, 123)

    def test_manifest_reports_bounded_dump_failure_diagnostic(self):
        path = self.write_manifest(dumpFile="")
        (self.root / "crash.log").write_text(
            "prefix\nMinidump capture failed (Win32=5, HRESULT=0x80070005)\n" + "x" * 10000,
            encoding="utf-8",
        )
        with self.assertRaisesRegex(driver.CaptureError, r"Win32=5, HRESULT=0x80070005"):
            driver.read_manifest(self.root, 123)
        self.assertEqual(path.name, "crash_manifest_0123456789abcdef.json")

    def test_manifest_reports_post_write_probe_failure(self):
        self.write_manifest(dumpFile="")
        (self.root / "crash.log").write_text(
            "Minidump probe failed after writer reported success\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(driver.CaptureError, r"Minidump probe failed after writer reported success"):
            driver.read_manifest(self.root, 123)

    def test_empty_or_ambiguous_capture_cannot_pass(self):
        with self.assertRaises(driver.CaptureError):
            driver.read_manifest(self.root, 123)
        self.write_manifest()
        (self.root / "crash_manifest_fedcba9876543210.json").write_text("{}", encoding="utf-8")
        with self.assertRaises(driver.CaptureError):
            driver.read_manifest(self.root, 123)

    def test_snapshot_detects_same_bytes_with_replaced_identity(self):
        artifact = self.root / "crash.log"
        artifact.write_bytes(b"same content")
        before = driver.snapshot(self.root)
        replacement = self.root / "replacement"
        replacement.write_bytes(b"same content")
        replacement.replace(artifact)
        self.assertNotEqual(driver.snapshot(self.root), before)

    def test_snapshot_rejects_hardlinks(self):
        artifact = self.root / "crash.log"
        artifact.write_bytes(b"log")
        os.link(artifact, self.root / "alias.log")
        with self.assertRaises(driver.FilesystemPolicyError):
            driver.snapshot(self.root)

    def test_copy_changes_only_adverse_manifest_and_preserves_original(self):
        manifest_path = self.write_manifest()
        (self.root / "crash.log").write_bytes(b"original log")
        before = driver.snapshot(self.root)
        with tempfile.TemporaryDirectory(prefix="crash-adverse-test-") as separate:
            destination = Path(separate) / "case"
            driver.copy_case(self.root, destination, manifest_path.name, {"logFile": "../outside.log"})
            self.assertEqual(json.loads((destination / manifest_path.name).read_text()),
                             {"logFile": "../outside.log"})
            self.assertEqual((destination / "crash.log").read_bytes(), b"original log")
        self.assertEqual(driver.snapshot(self.root), before)

    def test_cleanup_refuses_replaced_root_without_deleting_either_tree(self):
        owned = self.root / "owned"
        owned.mkdir()
        identity = driver.directory_identity(owned)
        previous = self.root / "previous"
        owned.rename(previous)
        owned.mkdir()
        with self.assertRaises(driver.CaptureError):
            driver.remove_owned_tree(owned, identity)
        self.assertTrue(previous.is_dir())
        self.assertTrue(owned.is_dir())

    def test_cleanup_never_deletes_a_replacement_after_identity_validation(self):
        owned = self.root / "owned"
        owned.mkdir()
        identity = driver.directory_identity(owned)
        original = self.root / "original"
        replacement = self.root / "replacement"
        replacement.mkdir()
        (replacement / "must-survive.txt").write_text("foreign data", encoding="utf-8")
        real_identity = driver.directory_identity

        def swap_after_validation(path):
            result = real_identity(path)
            owned.rename(original)
            replacement.rename(owned)
            return result

        with mock.patch.object(driver, "directory_identity", side_effect=swap_after_validation), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(driver.remove_owned_tree(owned, identity))
        self.assertTrue(original.is_dir())
        self.assertEqual((owned / "must-survive.txt").read_text(encoding="utf-8"), "foreign data")

    @unittest.skipUnless(driver.posix_identity_bound_removal_available(), "requires dir_fd-relative removal")
    def test_posix_cleanup_removes_owned_tree_without_following_symlinks(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "keep.txt").write_text("outside data", encoding="utf-8")
        owned = self.root / "owned"
        (owned / "tmp" / "nested").mkdir(parents=True)
        (owned / "tmp" / "nested" / "crash.log").write_bytes(b"log")
        (owned / "cases").mkdir()
        (owned / "cases" / "dir-link").symlink_to(outside, target_is_directory=True)
        (owned / "cases" / "file-link").symlink_to(outside / "keep.txt")
        identity = driver.directory_identity(owned)
        self.assertTrue(driver.remove_owned_tree(owned, identity))
        self.assertFalse(owned.exists())
        self.assertEqual((outside / "keep.txt").read_text(encoding="utf-8"), "outside data")

    def test_traversal_requires_the_loader_rejection_exit_not_any_failure(self):
        source = self.root / "source"
        source.mkdir()
        manifest_name = "crash_manifest_0123456789abcdef.json"
        manifest = {"logFile": "crash.log", "dumpFile": "crash.dmp"}
        (source / manifest_name).write_text(json.dumps(manifest), encoding="utf-8")
        (source / "crash.log").write_bytes(b"log")
        (source / "crash.dmp").write_bytes(b"dump")
        for status in (0, 2, -11, 0xC0000005, 1):
            with self.subTest(status=status):
                owned = self.root / ("status-" + str(status))
                owned.mkdir()
                with mock.patch.object(driver.tempfile, "mkdtemp", return_value=str(owned)), \
                        mock.patch.object(driver, "discover_capture", return_value=source), \
                        mock.patch.object(driver, "read_manifest", return_value=(manifest_name, manifest)), \
                        mock.patch.object(driver, "validate_package", return_value=[]), \
                        mock.patch.object(driver, "run_process", side_effect=[
                            (123, 0, ""), (124, 0, ""), (125, status, ""), (126, 0, "")]), \
                        contextlib.redirect_stderr(io.StringIO()):
                    if status == 1:
                        self.assertTrue(driver.capture_security(
                            Path(sys.executable), Path(sys.executable), keep_work=True)["passed"])
                    else:
                        with self.assertRaises(driver.CaptureError):
                            driver.capture_security(Path(sys.executable), Path(sys.executable), keep_work=True)

    def test_validator_requires_exit_status_and_specific_negative_reason(self):
        for response in ((0, '{"passed": false, "errors": []}'),
                         (1, '{"passed": true, "errors": []}'), (0, 'not JSON'), (0, '[]')):
            with mock.patch.object(driver, "run_process", return_value=(123, *response)):
                with self.assertRaises(driver.CaptureError):
                    driver.validate_package(self.root, self.root)
        result = json.dumps({"passed": False, "errors": [{"check": "writer-field"}]})
        with mock.patch.object(driver, "run_process", return_value=(123, 1, result)):
            self.assertEqual(driver.validate_package(self.root, self.root, expected_check="writer-field"),
                             ["writer-field"])
            with self.assertRaises(driver.CaptureError):
                driver.validate_package(self.root, self.root, expected_check="artifact-path")

    def test_process_timeout_fails_and_waits_for_child_exit(self):
        with self.assertRaises(driver.CaptureError):
            driver.run_process([sys.executable, "-c", "import time; time.sleep(5)"], self.root, timeout=0.05)
        self.assertEqual(len(list(self.root.glob("process-*.log"))), 1)

    def test_process_diagnostics_are_bounded_inside_owned_root(self):
        command = [sys.executable, "-c",
                   f"import sys; sys.stdout.buffer.write(b'x' * {driver.MAX_PROCESS_OUTPUT + 50})"]
        with self.assertRaisesRegex(driver.CaptureError, "output limit"):
            driver.run_process(command, self.root)
        logs = list(self.root.glob("process-*.log"))
        self.assertEqual(len(logs), 1)
        self.assertEqual(logs[0].stat().st_size, driver.MAX_PROCESS_OUTPUT)

    def test_process_uses_file_backed_output_without_communicate(self):
        real_popen = subprocess.Popen

        def start(*args, **kwargs):
            self.assertNotEqual(kwargs.get("stdout"), subprocess.PIPE)
            self.assertTrue(hasattr(kwargs["stdout"], "fileno"))
            process = real_popen(*args, **kwargs)
            process.communicate = mock.Mock(side_effect=AssertionError("communicate must not buffer output"))
            return process

        with mock.patch.object(driver.subprocess, "Popen", side_effect=start):
            _, status, output = driver.run_process([sys.executable, "-c", "print('file-backed')"], self.root)
        self.assertEqual(status, 0)
        self.assertEqual(output.strip(), "file-backed")

    def test_timeout_has_only_bounded_waits_even_when_kill_does_not_complete(self):
        process = mock.MagicMock(pid=123, returncode=None)
        process.__enter__.return_value = process
        process.poll.return_value = None
        process.wait.side_effect = subprocess.TimeoutExpired("probe", 0.01)

        def forbidden_unbounded_communicate(timeout=None):
            if timeout is None:
                raise AssertionError("second unbounded wait")
            raise subprocess.TimeoutExpired("probe", timeout)

        process.communicate.side_effect = forbidden_unbounded_communicate
        with mock.patch.object(driver.subprocess, "Popen", return_value=process), \
                mock.patch.object(driver, "PROCESS_KILL_TIMEOUT", 0.02, create=True):
            with self.assertRaises(driver.CaptureError):
                driver.run_process(["probe"], self.root, timeout=0.01)
        process.communicate.assert_not_called()
        process.__exit__.assert_not_called()
        self.assertTrue(process.wait.call_args_list)
        for call in process.wait.call_args_list:
            self.assertGreater(call.kwargs["timeout"], 0)
            self.assertLessEqual(call.kwargs["timeout"], 0.02)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required for registration semantics")
    def test_registration_covers_native_hosts_and_excludes_cross_compiles_and_stub_builds(self):
        source = (driver.ROOT / "Tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        target = source.index("NAME CrashCapturePackageSecurity")
        start = source.rfind("\nif(", 0, target) + 1
        end = source.index("\nendif()", target) + len("\nendif()")
        registration = source[start:end]
        for label, windows, cross, miniz in (("native", True, False, True), ("cross", True, True, True),
                                             ("posix", False, False, True), ("posix-cross", False, True, True),
                                             ("native-stub", True, False, False), ("posix-stub", False, False, False)):
            with self.subTest(configuration=label):
                project = self.root / label
                project.mkdir()
                executable = Path(sys.executable).as_posix()
                preamble = (
                    "cmake_minimum_required(VERSION 3.25)\nproject(CrashRegistration NONE)\nenable_testing()\n"
                    f"set(WIN32 {'TRUE' if windows else 'FALSE'})\n"
                    f"set(CMAKE_CROSSCOMPILING {'TRUE' if cross else 'FALSE'})\n"
                    f"set(MINIZ_FOUND {'TRUE' if miniz else 'FALSE'})\n"
                    f'set(Python3_EXECUTABLE "{executable}")\n'
                    "add_executable(SparkTests IMPORTED)\nadd_executable(SparkCrashReporter IMPORTED)\n"
                    f'set_target_properties(SparkTests SparkCrashReporter PROPERTIES IMPORTED_LOCATION "{executable}")\n'
                )
                (project / "CMakeLists.txt").write_text(preamble + registration + "\n", encoding="utf-8")
                result = subprocess.run([shutil.which("cmake"), "-S", str(project), "-B", str(project / "build")],
                                        capture_output=True, text=True, timeout=30, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                generated = (project / "build" / "CTestTestfile.cmake").read_text(encoding="utf-8")
                self.assertEqual("CrashCapturePackageSecurity" in generated, miniz and not cross)

    def test_integration_failure_preserves_owned_root_without_cleanup(self):
        owned = self.root / "capture"
        owned.mkdir()
        with mock.patch.object(driver.tempfile, "mkdtemp", return_value=str(owned)), \
                mock.patch.object(driver, "run_process", return_value=(123, 1, "failed")), \
                mock.patch.object(driver, "remove_owned_tree") as cleanup:
            with self.assertRaises(driver.CaptureError):
                driver.capture_security(Path(sys.executable), Path(sys.executable))
        cleanup.assert_not_called()
        self.assertTrue(owned.is_dir())


if __name__ == "__main__":
    unittest.main()
