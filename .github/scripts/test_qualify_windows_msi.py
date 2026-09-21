#!/usr/bin/env python3
"""Native-process fixtures exercise orchestration, not Windows qualification."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
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
PREVIOUS_SIGNER_THUMBPRINT = "A" * 40


def write_shipping_package_manifest(path, msi, *, source_sha=SOURCE_SHA, version="1.2.3"):
    path.write_text(json.dumps({
        "schemaVersion": "spark-shipping-package-v1",
        "commitSHA": source_sha,
        "profile": "stable-v1",
        "configuration": "MinSizeRel",
        "version": version,
        "msi": msi.name,
        "sha256": hashlib.sha256(msi.read_bytes()).hexdigest(),
    }), encoding="utf-8")


class WindowsMSILifecycleTests(unittest.TestCase):
    def _transaction_fixture(self, root):
        """Create the two immutable package identities used by the transaction contract."""
        old_packages = root / "old-packages"
        new_packages = root / "new-packages"
        old_packages.mkdir()
        new_packages.mkdir()
        old_msi = old_packages / "SparkEngine-1.2.2-Windows-AMD64-MinSizeRel-Runtime.msi"
        new_msi = new_packages / "SparkEngine-1.2.3-Windows-AMD64-MinSizeRel-Runtime.msi"
        old_msi.write_bytes(b"old fixture MSI")
        new_msi.write_bytes(b"new fixture MSI")
        old_manifest = root / "old-shipping-package-manifest.json"
        new_manifest = root / "new-shipping-package-manifest.json"
        write_shipping_package_manifest(old_manifest, old_msi, version="1.2.2")
        write_shipping_package_manifest(new_manifest, new_msi, version="1.2.3")
        module_manifest = root / "SparkEngineGameModules.cmake"
        module_manifest.write_text("fixture manifest", encoding="utf-8")
        return old_packages, new_packages, old_manifest, new_manifest, module_manifest

    def _run_transaction_contract(self, *, root, old_packages, new_packages,
                                  old_manifest, new_manifest, module_manifest,
                                  logs, runner, previous_signer_thumbprint=PREVIOUS_SIGNER_THUMBPRINT,
                                  powershell="powershell.exe"):
        """Exercise the old->new contract with generated native-process fixtures."""
        def copy_private_verified_file(source, private_directory, destination_name):
            destination = private_directory / destination_name
            with source.open("rb") as source_stream, destination.open("xb") as destination_stream:
                shutil.copyfileobj(source_stream, destination_stream)
            return destination, hashlib.sha256(destination.read_bytes()).hexdigest()

        def publish_bytes_no_replace(destination, payload):
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open("xb") as stream:
                stream.write(payload)

        try:
            with mock.patch.object(
                MODULE.package_evidence_io,
                "copy_private_verified_file",
                side_effect=copy_private_verified_file,
            ), mock.patch.object(
                MODULE.package_evidence_io,
                "publish_bytes_no_replace",
                side_effect=publish_bytes_no_replace,
            ):
                return MODULE._qualify_impl(
                    new_packages,
                    "1.2.3",
                    module_manifest,
                    root,
                    logs,
                    runner=runner,
                    msiexec="msiexec.exe",
                    powershell=powershell,
                    cmake="cmake",
                    source_sha=SOURCE_SHA,
                    package_manifest=new_manifest,
                    previous_packages=old_packages,
                    previous_version="1.2.2",
                    previous_package_manifest=old_manifest,
                    previous_signer_thumbprint=previous_signer_thumbprint,
                )
        except TypeError as exc:
            if "unexpected keyword argument" in str(exc):
                self.fail(
                    "transaction contract is not implemented: qualify() must accept "
                    "previous_packages, previous_version, and previous_package_manifest"
                )
            raise

    def _identity_runner(self, calls, state, *, fail_new_runtime=False,
                         mismatch_upgrade_code=False, same_product_code=False,
                         fail_upgrade_command=False, foreign_related_product=False,
                         fail_previous_install_command=False, signature_changes=None):
        """Deterministic native fixture for the frozen transaction semantics."""
        def runner(argv, log, *, timeout, env=None, cwd=None):
            calls.append(list(argv))
            if env and "SPARK_SIGNATURE_PATH" in env:
                selected = Path(env["SPARK_SIGNATURE_PATH"])
                self.assertEqual(selected.parent.name, ".private-previous-package")
                self.assertEqual(selected.read_bytes(), b"old fixture MSI")
                evidence = {
                    "Status": "Valid", "SignatureType": "Authenticode",
                    "SignerThumbprint": PREVIOUS_SIGNER_THUMBPRINT,
                    "SignerSubject": "CN=Generated Fixture",
                    "TimestampThumbprint": "C" * 40, "TimestampSubject": "CN=Timestamp Fixture",
                }
                evidence.update(signature_changes or {})
                log.write_text(json.dumps(evidence), encoding="utf-8")
                state["signature_verified_before_install"] = not state["installed"]
                state["signature_call_index"] = len(calls) - 1
                return 0
            if "-EncodedCommand" in argv:
                selected_name = Path(env["SPARK_MSI_PATH"]).name
                version_match = re.search(r"SparkEngine-([0-9]+\.[0-9]+\.[0-9]+)-Windows-", selected_name)
                self.assertIsNotNone(version_match, selected_name)
                version = version_match.group(1)
                registered = state["installed"] and state["version"] == version
                product_code = ("{12345678-1234-1234-1234-123456789ABC}"
                                if (version == "1.2.3" and not same_product_code)
                                else "{87654321-4321-4321-4321-CBA987654321}")
                upgrade_code = ("{BADF00D0-0000-0000-0000-000000000001}"
                                if mismatch_upgrade_code and version == "1.2.3"
                                else "{ABCDEF01-1234-1234-1234-123456789ABC}")
                log.write_text(json.dumps({
                    "ProductName": "SparkEngine",
                    "ProductVersion": version,
                    "ProductCode": product_code,
                    "UpgradeCode": upgrade_code,
                    "RelatedProducts": (["{00000000-0000-0000-0000-000000000001}"]
                                         if foreign_related_product and version == "1.2.2"
                                         else (["{87654321-4321-4321-4321-CBA987654321}"]
                                               if version == "1.2.2" and registered else [])),
                    "ProductState": 5 if registered else -1,
                    "InstallRoot": "INSTALL_ROOT",
                }), encoding="utf-8")
                if "identity-rollback" in log.name:
                    sentinel = (Path(state["install_root"]).parent / "localappdata" / "SparkEngine"
                                / "release-qualification-sentinel.sav")
                    state["rollback_restore_observed"] = registered and sentinel.is_file() and (
                        sentinel.read_bytes() == b"SparkEngine stable-v1 external user data\n"
                    )
                return 0
            if "/i" in argv:
                if fail_upgrade_command and "1.2.3" in " ".join(map(str, argv)):
                    if state.get("old_absent_on_upgrade_failure"):
                        state["installed"] = False
                        state["version"] = None
                    return 9
                state["installed"] = True
                version_match = re.search(r"SparkEngine-([0-9]+\.[0-9]+\.[0-9]+)-Windows-", " ".join(map(str, argv)))
                self.assertIsNotNone(version_match)
                state["version"] = version_match.group(1)
                install_root = next(arg.split("=", 1)[1] for arg in argv if arg.startswith("INSTALL_ROOT="))
                state["install_root"] = install_root
                if fail_previous_install_command and state["version"] == "1.2.2":
                    if state.get("partial_previous_install"):
                        state["installed"] = True
                        Path(install_root).mkdir(parents=True, exist_ok=True)
                    elif state.get("residue_without_registration"):
                        state["installed"] = False
                        Path(install_root).mkdir(parents=True, exist_ok=True)
                    return 9
                Path(install_root).mkdir(parents=True, exist_ok=True)
                return 0
            if any(arg.lower().startswith("/f") for arg in argv):
                state["repaired"] = True
                return 0
            if "/x" in argv:
                state["installed"] = False
                state["version"] = None
                install_root = Path(state["install_root"])
                if install_root.exists():
                    shutil.rmtree(install_root)
                return 0
            if argv and argv[0].endswith("SparkEngine.exe"):
                user_data_root = Path(env["LOCALAPPDATA"]) / "SparkEngine"
                state["user_data_path"] = user_data_root / "release-qualification-sentinel.sav"
                state["user_data_preserved"] = (
                    state["user_data_path"].is_file()
                    and state["user_data_path"].read_bytes() == b"SparkEngine stable-v1 external user data\n"
                )
                if fail_new_runtime and state["version"] == "1.2.3":
                    log.write_text("post-upgrade validation failure", encoding="utf-8")
                    return 9
                if env.get("SPARK_RHI_BACKEND") == "null":
                    log.write_text(
                        "SPARK_MODULE_READY count=1\n"
                        "SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n"
                        "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=5 fixed=4 rendered=0 unloaded=1 faults=0\n",
                        encoding="utf-8",
                    )
                    return 0
                log.write_text(
                    "SPARK_D3D11_DEVICE driver=warp certification=software-only\n"
                    "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=5 fixed=4 "
                    "render=5 unload=1 destroy=1 faults=0\n",
                    encoding="utf-8",
                )
                return 0
            return 0

        return runner

    def test_unacceptable_predecessor_signature_prevents_every_msiexec(self):
        for changes in (
            {"Status": "NotSigned"},
            {"SignerThumbprint": "D" * 40},
            {"TimestampThumbprint": None},
            {"SignatureType": "Catalog"},
        ):
            with self.subTest(changes=changes), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
                logs = root / "rejected-signature"
                calls = []
                state = {"installed": False, "version": None, "repaired": False}
                result = self._run_transaction_contract(
                    root=root, old_packages=old_packages, new_packages=new_packages,
                    old_manifest=old_manifest, new_manifest=new_manifest,
                    module_manifest=module_manifest, logs=logs,
                    runner=self._identity_runner(calls, state, signature_changes=changes),
                )
                self.assertNotEqual(result, 0)
                self.assertFalse(any(call[0] == "msiexec.exe" for call in calls), calls)
                self.assertFalse(state["installed"])
                self.assertFalse((logs / "package-smoke.log").exists())

    def test_predecessor_requires_explicit_valid_publisher_before_native_commands(self):
        for thumbprint in (None, "", "invalid", "A" * 39):
            with self.subTest(thumbprint=thumbprint), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
                calls = []
                result = self._run_transaction_contract(
                    root=root, old_packages=old_packages, new_packages=new_packages,
                    old_manifest=old_manifest, new_manifest=new_manifest,
                    module_manifest=module_manifest, logs=root / "missing-publisher",
                    runner=self._identity_runner(calls, {"installed": False}),
                    previous_signer_thumbprint=thumbprint,
                )
                self.assertNotEqual(result, 0)
                self.assertEqual(calls, [])

    def test_malformed_predecessor_signature_response_prevents_installer_execution(self):
        for payload in ('{"Status":"Valid","Status":"NotSigned"}', 'not JSON'):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
                calls = []

                def signature_only_runner(argv, log, **kwargs):
                    self.assertIn("SPARK_SIGNATURE_PATH", kwargs.get("env", {}))
                    calls.append(argv)
                    log.write_text(payload, encoding="utf-8")
                    return 0

                result = self._run_transaction_contract(
                    root=root, old_packages=old_packages, new_packages=new_packages,
                    old_manifest=old_manifest, new_manifest=new_manifest,
                    module_manifest=module_manifest, logs=root / "malformed-signature",
                    runner=signature_only_runner,
                )
                self.assertNotEqual(result, 0)
                self.assertEqual(len(calls), 1)

    @unittest.skipUnless(os.name == "nt", "Requires native Windows Authenticode")
    def test_native_unsigned_predecessor_is_rejected_before_installer_execution(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []

            def signature_only_runner(argv, log, **kwargs):
                self.assertIn("SPARK_SIGNATURE_PATH", kwargs.get("env", {}),
                              "Unsigned input must stop before MSI identity or installation")
                calls.append(argv)
                return MODULE.run_command(argv, log, **kwargs)

            logs = root / "unsigned-native"
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs, runner=signature_only_runner,
                powershell=MODULE.package_signatures.trusted_powershell(),
            )
            self.assertNotEqual(result, 0)
            self.assertEqual(len(calls), 1)
            report = json.loads((logs / "result.json").read_text(encoding="utf-8"))
            self.assertIn("signature status", " ".join(report["errors"]))
            self.assertFalse((logs / "package-smoke.log").exists())

    def test_cpack_preserves_runtime_upgrade_family_without_fixed_product_code(self):
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "CMake is required for the installer configuration regression")
        options = Path(__file__).resolve().parents[2] / "cmake" / "SparkCPackOptions.cmake"
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            script = root / "identity.cmake"
            output = root / "identity.txt"
            script.write_text(
                'set(CPACK_GENERATOR "WIX")\n'
                'set(CPACK_BUILD_CONFIG "MinSizeRel")\n'
                'set(CPACK_PACKAGE_FILE_NAME "SparkEngine-${CPACK_PACKAGE_VERSION}-Windows-AMD64")\n'
                f'include("{options.as_posix()}")\n'
                'if(DEFINED CPACK_WIX_PRODUCT_GUID)\n'
                '  message(FATAL_ERROR "ProductCode must remain generated per package, never the family GUID")\n'
                'endif()\n'
                f'file(WRITE "{output.as_posix()}" "${{CPACK_WIX_UPGRADE_GUID}}\\n")\n',
                encoding="utf-8",
            )
            families = []
            for version in ("1.0.0", "1.0.1", "1.0.1"):
                result = subprocess.run([cmake, f"-DCPACK_PACKAGE_VERSION={version}", "-P", str(script)],
                                        capture_output=True, text=True, timeout=30, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                families.append(output.read_text(encoding="utf-8").strip())
            self.assertEqual(families, ["74E90DE5-B7E2-442C-9696-8CA63782F55A"] * 3)

    def test_two_version_upgrade_installs_new_product_and_uninstalls_cleanly(self):
        """The qualification contract must exercise an old->new upgrade, not only fresh install."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            logs = root / "upgrade-logs"
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "install_root": str(root / "install")}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state),
            )
            self.assertEqual(result, 0)
            self.assertTrue(state["signature_verified_before_install"])
            signature_report = json.loads((logs / "previous-signature.json").read_text(encoding="utf-8"))
            self.assertTrue(signature_report["passed"])
            self.assertEqual(signature_report["sha256"], hashlib.sha256(b"old fixture MSI").hexdigest())
            self.assertEqual(signature_report["publisher_thumbprint"], PREVIOUS_SIGNER_THUMBPRINT)
            first_install = next(index for index, call in enumerate(calls) if call[0] == "msiexec.exe")
            self.assertLess(state["signature_call_index"], first_install)
            self.assertIsNone(state["version"])
            self.assertFalse(state["installed"])
            self.assertTrue(any("1.2.3" in " ".join(call) and "/i" in call for call in calls))
            self.assertTrue(any("/x" in call for call in calls))
            self.assertFalse(Path(state["install_root"]).exists())

    def test_post_upgrade_validation_failure_rolls_back_to_old_product(self):
        """A failed new runtime must restore the old MSI/product and preserve data."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            logs = root / "rollback-logs"
            calls = []
            install_root = root / "install"
            state = {"installed": False, "version": None, "repaired": False,
                     "preserve_user_data_on_uninstall": True, "install_root": str(install_root)}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state, fail_new_runtime=True),
            )
            self.assertNotEqual(result, 0)
            self.assertIsNone(state["version"])
            self.assertTrue(state.get("user_data_preserved"))
            self.assertTrue(state.get("rollback_restore_observed"))
            self.assertTrue(state["user_data_path"].is_file())
            self.assertEqual(state["user_data_path"].read_bytes(), b"SparkEngine stable-v1 external user data\n")
            rollback_install = [index for index, call in enumerate(calls)
                                if "/i" in call and "1.2.2" in " ".join(call)]
            self.assertTrue(rollback_install)
            self.assertTrue(any(index > rollback_install[0] and "/x" in call
                                for index, call in enumerate(calls)))

    def test_rejects_upgrade_code_mismatch_before_upgrading_owned_old_product(self):
        """A package from another product family cannot trigger upgrade or cleanup."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "install_root": str(root / "install")}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=root / "upgrade-code-mismatch-logs",
                runner=self._identity_runner(calls, state, mismatch_upgrade_code=True),
            )
            self.assertNotEqual(result, 0)
            self.assertTrue(any("UpgradeCode" in error for error in json.loads(
                (root / "upgrade-code-mismatch-logs" / "result.json").read_text(encoding="utf-8")
            )["errors"]))
            self.assertTrue(any("/i" in call and "1.2.2" in " ".join(call) for call in calls))
            self.assertTrue(any("/x" in call for call in calls))
            self.assertFalse(state["installed"])
            self.assertIsNone(state["version"])

    def test_rejects_same_product_code_transition_before_upgrading_owned_old_product(self):
        """A same ProductCode cannot masquerade as an old->new transition."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "install_root": str(root / "install")}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=root / "product-code-mismatch-logs",
                runner=self._identity_runner(calls, state, same_product_code=True),
            )
            self.assertNotEqual(result, 0)
            self.assertTrue(any("ProductCode" in error for error in json.loads(
                (root / "product-code-mismatch-logs" / "result.json").read_text(encoding="utf-8")
            )["errors"]))
            self.assertTrue(any("/i" in call and "1.2.2" in " ".join(call) for call in calls))
            self.assertTrue(any("/x" in call for call in calls))
            self.assertFalse(state["installed"])
            self.assertIsNone(state["version"])

    def test_failed_upgrade_command_inspects_state_before_destructive_rollback(self):
        """A no-mutation upgrade failure preserves the old install and original error."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "install_root": str(root / "install")}
            logs = root / "upgrade-command-failure-logs"
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state, fail_upgrade_command=True),
            )
            self.assertNotEqual(result, 0)
            report = json.loads((logs / "result.json").read_text(encoding="utf-8"))
            self.assertIn("upgrade failed with exit 9", " ".join(report["errors"]))
            self.assertFalse(any("rollback-uninstall" in " ".join(call) for call in calls))
            self.assertTrue(any("/i" in call and "1.2.2" in " ".join(call) for call in calls))
            self.assertTrue(any("/x" in call for call in calls))
            self.assertFalse(state["installed"])
            self.assertIsNone(state["version"])

    def test_rejects_foreign_old_related_product_without_cleanup(self):
        """Only the old MSI's own ProductCode may appear in RelatedProducts."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "install_root": str(root / "install")}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=root / "foreign-related-logs",
                runner=self._identity_runner(calls, state, foreign_related_product=True),
            )
            self.assertNotEqual(result, 0)
            self.assertFalse(any("/i" in call for call in calls))
            self.assertFalse(any("/x" in call for call in calls))

    def test_rejects_preexisting_old_install_without_mutation_or_cleanup(self):
        """Qualification must own the old install; an existing registration is rejected untouched."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": True, "version": "1.2.2", "repaired": False,
                     "install_root": str(root / "install")}
            logs = root / "preexisting-old-logs"
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state),
            )
            self.assertNotEqual(result, 0)
            report = json.loads((logs / "result.json").read_text(encoding="utf-8"))
            self.assertTrue(any("already registered" in error for error in report["errors"]))
            self.assertFalse(any("/i" in call for call in calls))
            self.assertFalse(any("/x" in call for call in calls))
            self.assertTrue(state["installed"])
            self.assertEqual(state["version"], "1.2.2")

    def test_partial_previous_install_failure_is_inspected_and_cleaned(self):
        """A failed old-package install cannot leave a registered staged product."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "partial_previous_install": True, "install_root": str(root / "install")}
            logs = root / "partial-previous-install-logs"
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state, fail_previous_install_command=True),
            )
            self.assertNotEqual(result, 0)
            report = json.loads((logs / "result.json").read_text(encoding="utf-8"))
            self.assertIn("install-previous failed with exit 9", " ".join(report["errors"]))
            self.assertTrue(any("cleanup-partial-previous" in " ".join(call) for call in calls))
            self.assertFalse(state["installed"])
            self.assertIsNone(state["version"])

    def test_failed_upgrade_with_old_absent_restores_old_before_cleanup(self):
        """A no-new-product failure restores an absent old registration before uninstall."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "old_absent_on_upgrade_failure": True, "install_root": str(root / "install")}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=root / "old-absent-rollback-logs",
                runner=self._identity_runner(calls, state, fail_upgrade_command=True),
            )
            self.assertNotEqual(result, 0)
            self.assertTrue(state.get("rollback_restore_observed"))
            old_restore = [index for index, call in enumerate(calls)
                           if "/i" in call and "1.2.2" in " ".join(call)]
            final_uninstall = [index for index, call in enumerate(calls)
                               if "/x" in call and any(str(arg).endswith("msi-uninstall.log") for arg in call)]
            self.assertTrue(old_restore)
            self.assertTrue(final_uninstall, calls)
            self.assertLess(old_restore[-1], final_uninstall[-1])

    def test_previous_install_absent_registration_with_residue_is_reported(self):
        """Filesystem residue remains an independent failure even when registration is absent."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "residue_without_registration": True, "install_root": str(root / "install")}
            logs = root / "residue-previous-install-logs"
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state, fail_previous_install_command=True),
            )
            self.assertNotEqual(result, 0)
            report = json.loads((logs / "result.json").read_text(encoding="utf-8"))
            self.assertTrue(any("Uninstall residue remains" in error for error in report["errors"]))
            self.assertTrue(Path(state["install_root"]).exists())

    def test_main_requires_all_previous_artifact_arguments_together(self):
        """CLI transaction inputs are an all-or-none contract."""
        argv = [
            "qualify-windows-msi.py", "--packages", "packages", "--version", "1.2.3",
            "--manifest", "modules.cmake", "--package-manifest", "package.json",
            "--runner-temp", "runner-temp", "--logs", "logs", "--source-sha", SOURCE_SHA,
            "--previous-version", "1.2.2",
        ]
        with mock.patch.object(sys, "argv", argv):
            with self.assertRaises(SystemExit) as raised:
                MODULE.main()
        self.assertEqual(raised.exception.code, 2)

    def test_main_requires_publisher_even_when_ambient_signing_config_exists(self):
        argv = [
            "qualify-windows-msi.py", "--packages", "packages", "--version", "1.2.3",
            "--manifest", "modules.cmake", "--package-manifest", "package.json",
            "--runner-temp", "runner-temp", "--logs", "logs", "--source-sha", SOURCE_SHA,
            "--previous-packages", "previous", "--previous-version", "1.2.2",
            "--previous-package-manifest", "previous.json",
        ]
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, {"SPARK_RELEASE_SIGNER_THUMBPRINT": PREVIOUS_SIGNER_THUMBPRINT}), \
                mock.patch.object(MODULE, "qualify") as qualify, \
                contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                MODULE.main()
        self.assertEqual(raised.exception.code, 2)
        qualify.assert_not_called()

    def test_repair_preserves_user_data_after_upgrade(self):
        """Repair must restore product files without deleting user-owned data."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            logs = root / "repair-logs"
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "install_root": str(root / "install")}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state),
            )
            self.assertEqual(result, 0)
            self.assertTrue(state["repaired"])
            self.assertTrue(state.get("user_data_preserved"))
            self.assertTrue(state["user_data_path"].is_file())

    @unittest.skipUnless(os.name == "nt", "Windows sharing-mode mutation protection")
    def test_repair_keeps_verified_msi_locked_against_write_replace_and_parent_rename(self):
        for operation in ("write", "replace", "parent-rename"):
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
                calls = []
                state = {"installed": False, "version": None, "repaired": False}
                native_fixture = self._identity_runner(calls, state)
                boundary = {}

                def attempt_replacement(argv, log, **kwargs):
                    if "/fvomus" in argv:
                        selected = Path(argv[argv.index("/fvomus") + 1])
                        replacement = root / "replacement.msi"
                        replacement.write_bytes(b"unverified repair payload")
                        relocated = selected.parent.with_name(selected.parent.name + "-relocated")
                        try:
                            if operation == "write":
                                selected.write_bytes(replacement.read_bytes())
                            elif operation == "replace":
                                os.replace(replacement, selected)
                            else:
                                selected.parent.rename(relocated)
                                selected.parent.mkdir()
                                selected.write_bytes(replacement.read_bytes())
                        except OSError:
                            boundary["blocked"] = True
                        else:
                            boundary["blocked"] = False
                        boundary["bytes"] = selected.read_bytes()
                        # Restore only this test's private fixture if the old
                        # implementation let the adversarial mutation succeed.
                        if not boundary["blocked"]:
                            if operation == "parent-rename":
                                selected.unlink()
                                selected.parent.rmdir()
                                relocated.rename(selected.parent)
                            else:
                                selected.write_bytes(b"new fixture MSI")
                    return native_fixture(argv, log, **kwargs)

                result = self._run_transaction_contract(
                    root=root, old_packages=old_packages, new_packages=new_packages,
                    old_manifest=old_manifest, new_manifest=new_manifest,
                    module_manifest=module_manifest, logs=root / "repair-mutation",
                    runner=attempt_replacement,
                )
                self.assertEqual(result, 0)
                self.assertTrue(boundary["blocked"], f"{operation} reached the repair boundary")
                self.assertEqual(boundary["bytes"], b"new fixture MSI")

    def test_repair_rechecks_digest_after_reacquiring_identity_lock(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            calls = []
            state = {"installed": False, "version": None, "repaired": False}
            native_fixture = self._identity_runner(calls, state)
            original_hold = MODULE._hold_private_msi_identity
            replace_after_check = False

            @contextlib.contextmanager
            def replace_between_checks(path):
                nonlocal replace_after_check
                with original_hold(path):
                    yield
                if replace_after_check and "1.2.3" in path.name:
                    replace_after_check = False
                    path.write_bytes(b"unverified repair payload")

            def arm_replacement(argv, log, **kwargs):
                nonlocal replace_after_check
                result = native_fixture(argv, log, **kwargs)
                if log.name == "identity-upgraded.log":
                    replace_after_check = True
                return result

            logs = root / "repair-revalidation"
            with mock.patch.object(MODULE, "_hold_private_msi_identity", replace_between_checks):
                result = self._run_transaction_contract(
                    root=root, old_packages=old_packages, new_packages=new_packages,
                    old_manifest=old_manifest, new_manifest=new_manifest,
                    module_manifest=module_manifest, logs=logs, runner=arm_replacement,
                )
            self.assertNotEqual(result, 0)
            self.assertFalse(any("/fvomus" in call for call in calls), "Unverified repair was dispatched")
            report = json.loads((logs / "result.json").read_text(encoding="utf-8"))
            self.assertIn("private MSI changed before repair", report["errors"])

    def test_final_uninstall_removes_registration_and_all_owned_residue(self):
        """A successful transaction must finish with no registered product or owned residue."""
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            old_packages, new_packages, old_manifest, new_manifest, module_manifest = self._transaction_fixture(root)
            logs = root / "uninstall-logs"
            calls = []
            state = {"installed": False, "version": None, "repaired": False,
                     "install_root": str(root / "install")}
            result = self._run_transaction_contract(
                root=root, old_packages=old_packages, new_packages=new_packages,
                old_manifest=old_manifest, new_manifest=new_manifest,
                module_manifest=module_manifest, logs=logs,
                runner=self._identity_runner(calls, state),
            )
            self.assertEqual(result, 0)
            self.assertFalse(state["installed"])
            self.assertFalse(Path(state["install_root"]).exists())
            self.assertTrue(state["user_data_path"].is_file())
            self.assertEqual(state["user_data_path"].read_bytes(), b"SparkEngine stable-v1 external user data\n")
            self.assertTrue(any("/x" in call for call in calls))

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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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
    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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

    @unittest.skipUnless(os.name == "nt", "Windows native MSI qualification")
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
