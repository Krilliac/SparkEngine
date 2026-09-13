#!/usr/bin/env python3
"""Native-process fixtures exercise orchestration, not Windows qualification."""
import contextlib
import hashlib
import importlib.util
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

SPEC = importlib.util.spec_from_file_location("qualify_windows_msi", Path(__file__).with_name("qualify-windows-msi.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

SOURCE_SHA = "0123456789abcdef0123456789abcdef01234567"


def write_shipping_package_manifest(path, msi, *, source_sha=SOURCE_SHA):
    path.write_text(json.dumps({
        "schemaVersion": "spark-shipping-package-v1",
        "commitSHA": source_sha,
        "profile": "stable-v1",
        "configuration": "MinSizeRel",
        "version": "1.2.3",
        "msi": msi.name,
        "sha256": hashlib.sha256(msi.read_bytes()).hexdigest(),
    }), encoding="utf-8")


@unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
class WindowsMSILifecycleTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows CLI alias preservation")
    def test_main_preserves_raw_package_and_manifest_aliases_for_no_follow_validation(self):
        """CLI parsing must not resolve away a symlink before qualification rejects it."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            module_manifest = root / "SparkEngineGameModules.cmake"
            module_manifest.write_text("fixture manifest")
            package_manifest = root / "shipping-package-manifest.json"
            package_manifest.write_text("{}", encoding="utf-8")
            packages_alias = root / "packages-alias"
            manifest_alias = root / "manifest-alias.json"
            try:
                os.symlink(packages, packages_alias, target_is_directory=True)
                os.symlink(package_manifest, manifest_alias)
            except OSError as exc:
                self.skipTest(f"symlink fixture is unavailable: {exc}")
            argv = [
                "qualify-windows-msi.py", "--packages", str(packages_alias),
                "--version", "1.2.3", "--manifest", str(module_manifest),
                "--package-manifest", str(manifest_alias),
                "--runner-temp", str(root), "--logs", str(root / "logs"),
                "--source-sha", SOURCE_SHA,
            ]
            with mock.patch.object(MODULE, "qualify", return_value=0) as qualify, \
                    mock.patch.object(sys, "argv", argv), \
                    mock.patch.dict(os.environ, {"SystemRoot": str(root)}):
                self.assertEqual(MODULE.main(), 0)

            positional = qualify.call_args.args
            self.assertEqual(positional[0], packages_alias)
            self.assertEqual(qualify.call_args.kwargs["package_manifest"], manifest_alias)

    def test_success_runs_both_installed_backends_and_writes_canonical_log(self):
        """One clean install proves NullRHI and D3D11/WARP before package success."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
            msi.write_bytes(b"fixture MSI")
            manifest = root / "SparkEngineGameModules.cmake"
            manifest.write_text("fixture manifest")
            package_manifest = root / "shipping-package-manifest.json"
            write_shipping_package_manifest(package_manifest, msi)
            logs = root / "logs"
            installed = False
            install_root = None
            backend_runs = []

            def runner(argv, log, *, timeout, env=None, cwd=None):
                nonlocal installed, install_root
                if "-EncodedCommand" in argv:
                    log.write_text(json.dumps({
                        "ProductName": "SparkEngine",
                        "ProductVersion": "1.2.3",
                        "ProductCode": "{12345678-1234-1234-1234-123456789ABC}",
                        "UpgradeCode": "{ABCDEF01-1234-1234-1234-123456789ABC}",
                        "RelatedProducts": [],
                        "ProductState": 5 if installed else -1,
                        "InstallRoot": "INSTALL_ROOT",
                    }))
                    return 0
                if "/i" in argv:
                    install_root = Path(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("INSTALL_ROOT=")))
                    install_root.mkdir()
                    installed = True
                    return 0
                if "/x" in argv:
                    shutil.rmtree(install_root)
                    installed = False
                    return 0
                if argv[0].endswith("SparkEngine.exe"):
                    if "-headless" in argv:
                        backend_runs.append("nullrhi")
                        log.write_text(
                            "SPARK_MODULE_READY count=1\n"
                            "SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n"
                            "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=5 fixed=4 rendered=0 unloaded=1 faults=0\n",
                        )
                    else:
                        backend_runs.append("d3d11-warp")
                        log.write_text(
                            "SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                            "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=5 fixed=4 render=5 unload=1 destroy=1 faults=0\n",
                        )
                    self.assertEqual(cwd, install_root / "bin")
                    self.assertEqual(timeout, 120)
                    return 0
                return 0

            with contextlib.redirect_stdout(io.StringIO()):
                result = MODULE.qualify(
                    packages, "1.2.3", manifest, root, logs, runner=runner,
                    msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake",
                    source_sha=SOURCE_SHA,
                    package_manifest=package_manifest,
                )

            self.assertEqual(result, 0)
            self.assertFalse((logs / "result.json").exists())
            self.assertEqual(backend_runs, ["nullrhi", "d3d11-warp"])
            smoke = logs / "package-smoke.log"
            self.assertEqual(smoke.read_text(encoding="utf-8"), (
                "[package-smoke] schema=package-smoke-v1\n"
                "[package-smoke] product=SparkEngine\n"
                "[package-smoke] module=SparkGameFPS\n"
                "[package-smoke] profile=stable-v1\n"
                f"[package-smoke] commit_sha={SOURCE_SHA}\n"
                f"[package-smoke] msi_sha256={hashlib.sha256(b'fixture MSI').hexdigest()}\n"
                "[package-smoke] backend=nullrhi result=PASS\n"
                "[package-smoke] backend=d3d11-warp result=PASS\n"
                "[package-smoke] exit_code=0\n"
                "[package-smoke] PASS\n"
            ))

    def test_rejects_mismatched_shipping_manifest_before_native_execution(self):
        """The separate package identity document must bind the MSI before install."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
            msi.write_bytes(b"fixture MSI")
            module_manifest = root / "SparkEngineGameModules.cmake"
            module_manifest.write_text("fixture manifest")
            package_manifest = root / "shipping-package-manifest.json"
            package_manifest.write_text(json.dumps({
                "schemaVersion": "spark-shipping-package-v1",
                "commitSHA": SOURCE_SHA,
                "profile": "stable-v1",
                "configuration": "MinSizeRel",
                "version": "1.2.3",
                "msi": msi.name,
                "sha256": "0" * 64,
            }), encoding="utf-8")

            def forbidden(*args, **kwargs):
                self.fail("identity mismatch invoked a native process")

            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    result = MODULE.qualify(
                        packages, "1.2.3", module_manifest, root, root / "logs",
                        runner=forbidden, msiexec="msiexec.exe", powershell="powershell.exe",
                        cmake="cmake", source_sha=SOURCE_SHA,
                        package_manifest=package_manifest,
                    )
            except TypeError as exc:
                self.fail(f"qualification has no shipping manifest contract: {exc}")

            report = json.loads((root / "logs" / "result.json").read_text())
            self.assertNotEqual(result, 0, report)
            self.assertIn("hash", " ".join(report["errors"]).lower())
            self.assertFalse((root / "logs" / "package-smoke.log").exists())

    @unittest.skipUnless(os.name == "nt", "Windows private-MSI identity lock")
    def test_installs_a_locked_private_verified_copy_not_the_mutable_artifact_path(self):
        """A private-copy replacement at install time cannot change the MSI Installer reads."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
            msi.write_bytes(b"verified fixture MSI")
            module_manifest = root / "SparkEngineGameModules.cmake"
            module_manifest.write_text("fixture manifest")
            package_manifest = root / "shipping-package-manifest.json"
            write_shipping_package_manifest(package_manifest, msi)
            logs = root / "logs"
            installed = False
            install_root = None
            install_target = None
            private_copy_swapped = False
            identity_copy_swapped = False
            identity_attempted = False
            identity_replacement = root / "private-identity-replacement.msi"
            identity_replacement.write_bytes(b"attacker identity replacement")
            install_replacement = root / "private-install-replacement.msi"
            install_replacement.write_bytes(b"attacker install replacement")

            def runner(argv, log, *, timeout, env=None, cwd=None):
                nonlocal installed, install_root, install_target, private_copy_swapped, identity_copy_swapped, identity_attempted
                if "-EncodedCommand" in argv:
                    if not identity_attempted:
                        identity_attempted = True
                        private_identity = Path(env["SPARK_MSI_PATH"])
                        try:
                            os.chmod(private_identity, 0o666)
                            os.replace(identity_replacement, private_identity)
                            identity_copy_swapped = True
                        except OSError:
                            pass
                    log.write_text(json.dumps({
                        "ProductName": "SparkEngine",
                        "ProductVersion": "1.2.3",
                        "ProductCode": "{12345678-1234-1234-1234-123456789ABC}",
                        "UpgradeCode": "{ABCDEF01-1234-1234-1234-123456789ABC}",
                        "RelatedProducts": [],
                        "ProductState": 5 if installed else -1,
                        "InstallRoot": "INSTALL_ROOT",
                    }))
                    return 0
                if "/i" in argv:
                    install_target = Path(argv[argv.index("/i") + 1])
                    try:
                        os.chmod(install_target, 0o666)
                        os.replace(install_replacement, install_target)
                        private_copy_swapped = True
                    except OSError:
                        pass
                    install_root = Path(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("INSTALL_ROOT=")))
                    install_root.mkdir()
                    installed = True
                    return 0
                if "/x" in argv:
                    shutil.rmtree(install_root)
                    installed = False
                    return 0
                if argv[0].endswith("SparkEngine.exe"):
                    if "-headless" in argv:
                        log.write_text(
                            "SPARK_MODULE_READY count=1\n"
                            "SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n"
                            "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=5 fixed=4 rendered=0 unloaded=1 faults=0\n",
                        )
                    else:
                        log.write_text(
                            "SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                            "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=5 fixed=4 render=5 unload=1 destroy=1 faults=0\n",
                        )
                    return 0
                return 0

            with contextlib.redirect_stdout(io.StringIO()):
                result = MODULE.qualify(
                    packages, "1.2.3", module_manifest, root, logs, runner=runner,
                    msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake",
                    source_sha=SOURCE_SHA, package_manifest=package_manifest,
                )

            self.assertEqual(result, 0)
            self.assertFalse((logs / "result.json").exists())
            self.assertNotEqual(install_target, msi.absolute())
            self.assertTrue(install_target.is_relative_to(logs))
            self.assertFalse(identity_copy_swapped, "private MSI was replaceable during identity parsing")
            self.assertFalse(private_copy_swapped, "private MSI was replaceable during Installer launch")
            self.assertTrue((logs / "package-smoke.log").is_file())

    def test_refuses_a_stale_log_directory_before_native_execution(self):
        """A previous success log cannot be reused by a failed new qualification."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
            msi.write_bytes(b"fixture MSI")
            module_manifest = root / "SparkEngineGameModules.cmake"
            module_manifest.write_text("fixture manifest")
            package_manifest = root / "shipping-package-manifest.json"
            write_shipping_package_manifest(package_manifest, msi)
            logs = root / "logs"
            logs.mkdir()
            stale = logs / "package-smoke.log"
            stale.write_text("old success", encoding="utf-8")

            def forbidden(*args, **kwargs):
                self.fail("stale evidence directory invoked a native process")

            with self.assertRaisesRegex(ValueError, "fresh"):
                MODULE.qualify(
                    packages, "1.2.3", module_manifest, root, logs, runner=forbidden,
                    msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake",
                    source_sha=SOURCE_SHA, package_manifest=package_manifest,
                )
            self.assertEqual(stale.read_text(encoding="utf-8"), "old success")

    def test_rejects_duplicate_shipping_manifest_keys_before_native_execution(self):
        """Last-wins JSON parsing cannot smuggle a second package identity into CI."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
            msi.write_bytes(b"fixture MSI")
            module_manifest = root / "SparkEngineGameModules.cmake"
            module_manifest.write_text("fixture manifest")
            package_manifest = root / "shipping-package-manifest.json"
            digest = hashlib.sha256(msi.read_bytes()).hexdigest()
            package_manifest.write_text(
                "{"
                "\"schemaVersion\":\"spark-shipping-package-v1\","
                f"\"commitSHA\":\"{SOURCE_SHA}\","
                "\"profile\":\"stable-v1\","
                "\"configuration\":\"MinSizeRel\","
                "\"version\":\"1.2.3\","
                f"\"msi\":\"{msi.name}\","
                "\"sha256\":\"0\","
                f"\"sha256\":\"{digest}\""
                "}",
                encoding="utf-8",
            )

            def forbidden(*args, **kwargs):
                self.fail("duplicate manifest key invoked a native process")

            with contextlib.redirect_stdout(io.StringIO()):
                result = MODULE.qualify(
                    packages, "1.2.3", module_manifest, root, root / "logs", runner=forbidden,
                    msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake",
                    source_sha=SOURCE_SHA, package_manifest=package_manifest,
                )
            report = json.loads((root / "logs" / "result.json").read_text())
            self.assertNotEqual(result, 0, report)
            self.assertIn("duplicate", " ".join(report["errors"]).lower())
            self.assertFalse((root / "logs" / "package-smoke.log").exists())

    def test_racing_package_smoke_destination_is_never_overwritten(self):
        """Qualification log publication loses cleanly if a destination appears late."""
        with tempfile.TemporaryDirectory() as raw:
            output = Path(raw) / "package-smoke.log"

            def race_link(source, destination):
                Path(destination).write_text("attacker", encoding="utf-8")
                raise FileExistsError("destination appeared during publish")

            with mock.patch.object(MODULE.package_evidence_io.os, "link", side_effect=race_link):
                with self.assertRaises(MODULE.package_evidence_io.PackageEvidenceIOError):
                    MODULE._write_package_smoke_log(output, SOURCE_SHA, "a" * 64)

            self.assertEqual(output.read_text(encoding="utf-8"), "attacker")

    def test_success_does_not_publish_a_secondary_result_after_smoke(self):
        """The smoke leaf is final, leaving no later result publication to fail it."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            packages = root / "packages"
            packages.mkdir()
            msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
            msi.write_bytes(b"fixture MSI")
            module_manifest = root / "SparkEngineGameModules.cmake"
            module_manifest.write_text("fixture manifest")
            package_manifest = root / "shipping-package-manifest.json"
            write_shipping_package_manifest(package_manifest, msi)
            logs = root / "logs"
            installed = False
            install_root = None

            def runner(argv, log, *, timeout, env=None, cwd=None):
                nonlocal installed, install_root
                if "-EncodedCommand" in argv:
                    log.write_text(json.dumps({
                        "ProductName": "SparkEngine",
                        "ProductVersion": "1.2.3",
                        "ProductCode": "{12345678-1234-1234-1234-123456789ABC}",
                        "UpgradeCode": "{ABCDEF01-1234-1234-1234-123456789ABC}",
                        "RelatedProducts": [],
                        "ProductState": 5 if installed else -1,
                        "InstallRoot": "INSTALL_ROOT",
                    }))
                    return 0
                if "/i" in argv:
                    install_root = Path(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("INSTALL_ROOT=")))
                    install_root.mkdir()
                    installed = True
                    return 0
                if "/x" in argv:
                    shutil.rmtree(install_root)
                    installed = False
                    return 0
                if argv[0].endswith("SparkEngine.exe"):
                    log.write_text(
                        "SPARK_MODULE_READY count=1\n"
                        "SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n"
                        "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=5 fixed=4 rendered=0 unloaded=1 faults=0\n"
                        if "-headless" in argv else
                        "SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                        "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=5 fixed=4 render=5 unload=1 destroy=1 faults=0\n",
                    )
                    return 0
                return 0

            with mock.patch.object(MODULE, "_write_result_report", side_effect=AssertionError("success wrote a late result report")), \
                    contextlib.redirect_stdout(io.StringIO()):
                result = MODULE.qualify(
                    packages, "1.2.3", module_manifest, root, logs, runner=runner,
                    msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake",
                    source_sha=SOURCE_SHA, package_manifest=package_manifest,
                )

            self.assertEqual(result, 0)
            self.assertTrue((logs / "package-smoke.log").exists())
            self.assertFalse((logs / "result.json").exists())

    def test_lifecycle_failures_preserve_original_error_and_attempt_uninstall(self):
        for case in ("success", "install", "validate", "fps", "d3d11", "d3d11_bad_marker", "d3d11_duplicate", "d3d11_logger_copy", "nullrhi_bad_lifecycle", "nullrhi_d3d11_logger_copy", "uninstall", "residue", "install_and_uninstall",
                     "reboot", "changed_msi", "wrong_identity", "wrong_version", "invalid_identity", "preexisting", "wrong_root", "timeout", "registration_remains", "no_marker", "zero_marker", "older_related_product", "unregistered_install", "absent_install", "advertised_install", "broken_install"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                packages = root / "packages"
                packages.mkdir()
                msi = packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
                msi.write_bytes(b"fixture MSI")
                manifest = root / "SparkEngineGameModules.cmake"
                manifest.write_text("fixture manifest")
                package_manifest = root / "shipping-package-manifest.json"
                write_shipping_package_manifest(package_manifest, msi)
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
                        product_state = 5 if case == "preexisting" or installed or (
                            case == "registration_remains" and any("/x" in call for call in calls)) else -1
                        if installed:
                            product_state = {"unregistered_install": -1, "absent_install": 2,
                                             "advertised_install": 1, "broken_install": 0}.get(case, product_state)
                        log.write_text(json.dumps({"ProductName": "Wrong" if case == "wrong_identity" else "SparkEngine",
                                                  "ProductVersion": "9.8.7" if case == "wrong_version" else "1.2.3", "ProductCode": "{12345678-1234-1234-1234-123456789ABC}",
                                                  "UpgradeCode": "{ABCDEF01-1234-1234-1234-123456789ABC}",
                                                  "RelatedProducts": ["{98765432-1234-1234-1234-123456789ABC}"] if case == "older_related_product" else [],
                                                  "ProductState": product_state,
                                                  "InstallRoot": "WRONG" if case == "wrong_root" else "INSTALL_ROOT"}))
                        return 0
                    if "/i" in argv:
                        install_root = Path(next(arg.split("=", 1)[1] for arg in argv if arg.startswith("INSTALL_ROOT=")))
                        self.assertTrue(install_root.parent.resolve().is_relative_to(root.resolve()))
                        self.assertFalse(install_root.exists())
                        install_root.mkdir()
                        (install_root / "owned-file").write_text("payload")
                        installed = True
                        if case == "timeout":
                            raise subprocess.TimeoutExpired(argv, timeout)
                        return 1603 if case in ("install", "install_and_uninstall") else 3010 if case == "reboot" else 0
                    if "/x" in argv:
                        private_msi = Path(argv[argv.index("/x") + 1])
                        self.assertNotEqual(private_msi, msi)
                        self.assertTrue(private_msi.is_relative_to(logs))
                        self.assertEqual(private_msi.name, msi.name)
                        if case not in ("residue", "uninstall", "install_and_uninstall"):
                            shutil.rmtree(install_root)
                            installed = False
                        return 1603 if case in ("uninstall", "install_and_uninstall") else 0
                    if argv[0].endswith("SparkEngine.exe"):
                        self.assertEqual(cwd, install_root / "bin")
                        self.assertEqual(argv[argv.index("-game") + 1], str(install_root / "bin/SparkGameFPS.dll"))
                        self.assertIn("-require-game", argv)
                        self.assertEqual(timeout, 120)
                        if "-headless" in argv:
                            self.assertEqual(env["SPARK_RHI_BACKEND"], "null")
                            self.assertEqual(argv[argv.index("-test-frames") + 1], "5")
                            log.write_text("[INFO] SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                                           "SPARK_MODULE_READY count=1\n"
                                           "SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n"
                                           "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=5 fixed=4 rendered=0 unloaded=1 faults=0\n"
                                           if case == "nullrhi_d3d11_logger_copy" else
                                           "no readiness marker" if case == "no_marker" else
                                           "SPARK_MODULE_READY count=0\n" if case == "zero_marker" else
                                           "SPARK_MODULE_READY count=1\n"
                                           "SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n"
                                           "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=5 fixed=4 rendered=1 unloaded=1 faults=0\n" if case == "nullrhi_bad_lifecycle" else
                                           "SPARK_MODULE_READY count=1\n"
                                           "SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n"
                                           "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=5 fixed=4 rendered=0 unloaded=1 faults=0\n")
                            return 9 if case == "fps" else 0
                        self.assertEqual(env["SPARK_RHI_BACKEND"], "d3d11")
                        self.assertEqual(env["SPARK_D3D11_DRIVER"], "warp")
                        self.assertIn("-test-seconds", argv)
                        self.assertEqual(argv[argv.index("-test-seconds") + 1], "1.0")
                        d3d_log = (
                            "SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                            "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=5 fixed=4 render=5 unload=1 destroy=1 faults=0\n"
                        )
                        if case == "d3d11_bad_marker":
                            d3d_log = (
                                "SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                                "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=0 load=1 update=5 fixed=4 render=5 unload=1 destroy=1 faults=0\n"
                            )
                        elif case == "d3d11_duplicate":
                            d3d_log = d3d_log + d3d_log
                        elif case == "d3d11_logger_copy":
                            d3d_log = (
                                "[INFO] SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                                "[INFO] SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=5 fixed=4 render=5 unload=1 destroy=1 faults=0\n"
                            )
                        log.write_text(d3d_log)
                        return 9 if case == "d3d11" else 0
                    self.assertTrue(any(arg.startswith("-DSPARK_PACKAGE_ROOT=") for arg in argv))
                    self.assertIn("-DSPARK_PACKAGE_LAYOUT=runtime", argv)
                    self.assertNotIn("-DSPARK_PACKAGE_VALIDATE_MODULES_ONLY=ON", argv)
                    if case == "changed_msi":
                        msi.write_bytes(b"changed")
                    return 9 if case == "validate" else 0

                with contextlib.redirect_stdout(io.StringIO()):
                    result = MODULE.qualify(packages, "1.2.3", manifest, root, logs, runner=runner,
                                            msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake",
                                            source_sha=SOURCE_SHA, package_manifest=package_manifest)
                expected_success = case in ("success", "changed_msi")
                if expected_success:
                    self.assertEqual(result, 0)
                    self.assertFalse((logs / "result.json").exists())
                else:
                    report = json.loads((logs / "result.json").read_text())
                    self.assertNotEqual(result, 0, report)
                    self.assertTrue(report["errors"])
                smoke = logs / "package-smoke.log"
                self.assertEqual(smoke.exists(), expected_success)
                uninstall_calls = [argv for argv in calls if "/x" in argv]
                if case in ("wrong_identity", "wrong_version", "invalid_identity", "preexisting", "wrong_root", "older_related_product"):
                    self.assertFalse(uninstall_calls)
                    self.assertFalse(any("/i" in argv for argv in calls))
                else:
                    self.assertEqual(len(uninstall_calls), 1)
                    self.assertIn("/qn", uninstall_calls[0])
                    self.assertIn("/norestart", uninstall_calls[0])
                if case == "install_and_uninstall":
                    self.assertIn("install", report["errors"][0])
                    self.assertTrue(any("uninstall" in error for error in report["errors"][1:]))
                if case in ("unregistered_install", "absent_install", "advertised_install", "broken_install"):
                    self.assertTrue(any("registration" in error for error in report["errors"]))
                    self.assertFalse(any(any(arg.startswith("-DSPARK_PACKAGE_ROOT=") for arg in argv) for argv in calls))
                    self.assertFalse(any(argv[0].endswith("SparkEngine.exe") for argv in calls))
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
                                            msiexec="msiexec.exe", powershell="powershell.exe", cmake="cmake",
                                            source_sha=SOURCE_SHA,
                                            package_manifest=root / "shipping-package-manifest.json")
                self.assertNotEqual(result, 0)
                self.assertIn("MSI", json.loads((root / "logs/result.json").read_text())["errors"][0])


if __name__ == "__main__":
    unittest.main()
