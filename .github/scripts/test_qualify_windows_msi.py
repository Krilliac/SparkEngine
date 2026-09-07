#!/usr/bin/env python3
"""Native-process fixtures exercise orchestration, not Windows qualification."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("qualify_windows_msi", Path(__file__).with_name("qualify-windows-msi.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class WindowsMSILifecycleTests(unittest.TestCase):
    def test_lifecycle_failures_preserve_original_error_and_attempt_uninstall(self):
        for case in ("success", "install", "validate", "fps", "uninstall", "residue", "install_and_uninstall",
                     "reboot", "changed_msi", "wrong_identity", "wrong_version", "invalid_identity", "preexisting", "wrong_root", "timeout", "registration_remains", "no_marker", "zero_marker", "older_related_product"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                packages = root / "packages"
                packages.mkdir()
                msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
                msi.write_bytes(b"fixture MSI")
                manifest = root / "SparkEngineGameModules.cmake"
                manifest.write_text("fixture manifest")
                logs = root / "logs"
                calls = []
                install_root = None
                installed = False

                def runner(argv, log, *, timeout, env=None, cwd=None):
                    nonlocal install_root, installed
                    calls.append(argv)
                    log.write_text("fixture command output")
                    if "-EncodedCommand" in argv:
                        if case == "invalid_identity":
                            log.write_text("[]")
                            return 0
                        log.write_text(json.dumps({"ProductName": "Wrong" if case == "wrong_identity" else "SparkEngine",
                                                  "ProductVersion": "9.8.7" if case == "wrong_version" else "1.2.3", "ProductCode": "{12345678-1234-1234-1234-123456789ABC}",
                                                  "UpgradeCode": "{ABCDEF01-1234-1234-1234-123456789ABC}",
                                                  "RelatedProducts": ["{98765432-1234-1234-1234-123456789ABC}"] if case == "older_related_product" else [],
                                                  "ProductState": 5 if case == "preexisting" or installed or (
                                                      case == "registration_remains" and any("/x" in call for call in calls)) else -1,
                                                  "InstallRoot": "WRONG" if case == "wrong_root" else "INSTALL_ROOT"}))
                        return 0
                    if "/i" in argv:
                        install_root = Path(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("INSTALL_ROOT=")))
                        self.assertTrue(install_root.parent.is_relative_to(root))
                        self.assertFalse(install_root.exists())
                        install_root.mkdir()
                        (install_root / "owned-file").write_text("payload")
                        installed = True
                        if case == "timeout":
                            raise subprocess.TimeoutExpired(argv, timeout)
                        return 1603 if case in ("install", "install_and_uninstall") else 3010 if case == "reboot" else 0
                    if "/x" in argv:
                        self.assertEqual(Path(argv[argv.index("/x") + 1]), msi)
                        if case not in ("residue", "uninstall", "install_and_uninstall"):
                            shutil.rmtree(install_root)
                            installed = False
                        return 1603 if case in ("uninstall", "install_and_uninstall") else 0
                    if argv[0].endswith("SparkEngine.exe"):
                        self.assertEqual(cwd, install_root / "bin")
                        self.assertEqual(argv[argv.index("-game") + 1], str(install_root / "bin/SparkGameFPS.dll"))
                        self.assertIn("-require-game", argv)
                        self.assertIn("-headless", argv)
                        self.assertEqual(argv[argv.index("-test-frames") + 1], "5")
                        self.assertEqual(timeout, 120)
                        log.write_text("no readiness marker" if case == "no_marker" else
                                       "SPARK_MODULE_READY count=0\n" if case == "zero_marker" else
                                       "SPARK_MODULE_READY count=1\n")
                        return 9 if case == "fps" else 0
                    self.assertTrue(any(arg.startswith("-DSPARK_PACKAGE_ROOT=") for arg in argv))
                    self.assertIn("-DSPARK_PACKAGE_LAYOUT=runtime", argv)
                    self.assertNotIn("-DSPARK_PACKAGE_VALIDATE_MODULES_ONLY=ON", argv)
                    if case == "changed_msi":
                        msi.write_bytes(b"changed")
                    return 9 if case == "validate" else 0

                with contextlib.redirect_stdout(io.StringIO()):
                    result = MODULE.qualify(packages, "1.2.3", manifest, root, logs, runner=runner,
                                            msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake")
                report = json.loads((logs / "result.json").read_text())
                if case == "success":
                    self.assertEqual(result, 0, report)
                    self.assertEqual(report["errors"], [])
                else:
                    self.assertNotEqual(result, 0, report)
                    self.assertTrue(report["errors"])
                uninstall_calls = [argv for argv in calls if "/x" in argv]
                if case in ("wrong_identity", "wrong_version", "invalid_identity", "preexisting", "wrong_root", "older_related_product"):
                    self.assertFalse(uninstall_calls)
                    self.assertFalse(any("/i" in argv for argv in calls))
                elif case == "changed_msi":
                    self.assertFalse(uninstall_calls, "Never execute a substituted MSI for cleanup")
                    self.assertIn("changed", " ".join(report["errors"]))
                else:
                    self.assertEqual(len(uninstall_calls), 1)
                    self.assertIn("/qn", uninstall_calls[0])
                    self.assertIn("/norestart", uninstall_calls[0])
                if case == "install_and_uninstall":
                    self.assertIn("install", report["errors"][0])
                    self.assertTrue(any("uninstall" in error for error in report["errors"][1:]))
                if case == "residue":
                    self.assertTrue(any("residue" in error for error in report["errors"]))

    def test_native_runner_retains_output_exit_status_and_working_directory(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            log = root / "native.log"
            code = MODULE.run_command(
                [sys.executable, "-c", "import os,sys; print(os.getcwd()); print('native stderr', file=sys.stderr); sys.exit(9)"],
                log, timeout=10, cwd=root)
            self.assertEqual(code, 9)
            self.assertIn(str(root), log.read_text())
            self.assertIn("native stderr", log.read_text())

    def test_rejects_wrong_or_duplicate_package_before_native_execution(self):
        for names in ([], ["wrong.msi"], ["SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi", "second.msi"]):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                packages = root / "packages"
                packages.mkdir()
                for name in names:
                    (packages / name).write_bytes(b"fixture")
                def forbidden(*args, **kwargs):
                    self.fail("Native process must not execute for rejected artifact identity")
                with contextlib.redirect_stdout(io.StringIO()):
                    result = MODULE.qualify(packages, "1.2.3", root / "manifest", root, root / "logs", runner=forbidden,
                                            msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake")
                self.assertNotEqual(result, 0)
                self.assertIn("MSI", json.loads((root / "logs/result.json").read_text())["errors"][0])


if __name__ == "__main__":
    unittest.main()
