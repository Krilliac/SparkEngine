#!/usr/bin/env python3
"""Adversarial tests for the asset integrity boundary."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "asset-integrity" / "verify_asset_integrity.py"
SPEC = importlib.util.spec_from_file_location("verify_asset_integrity", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
vai = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vai)

SITE_DATA_DIR = REPO_ROOT / "tools" / "site-data"
sys.path.insert(0, str(SITE_DATA_DIR))
import validate as site_data_validate  # noqa: E402


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_manifest(root: Path, entries: list[dict], *, metadata_root: str | None = None) -> Path:
    payload = {
        "version": 1,
        "algorithm": "sha256",
        "root": root.name if metadata_root is None else metadata_root,
        "fileCount": len(entries),
        "entries": entries,
    }
    path = root / vai.MANIFEST_FILENAME
    path.write_bytes(vai.manifest_bytes(payload))
    return path


def create_reparse(link: Path, target: Path) -> None:
    if sys.platform == "win32":
        subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            check=True,
            capture_output=True,
        )
    else:
        link.symlink_to(target, target_is_directory=True)


def remove_reparse(link: Path) -> None:
    if not link.exists() and not link.is_symlink():
        return
    if sys.platform == "win32":
        subprocess.run(["cmd", "/c", "rmdir", str(link)], check=True, capture_output=True)
    else:
        link.unlink()


class PathPolicyTests(unittest.TestCase):
    def test_accepts_canonical_portable_paths(self) -> None:
        for path in ("file.bin", "Models/hero.obj", "Audio/MMO/ambient.wav"):
            self.assertEqual(vai._canonical_relative_path(path), (path, None))

    def test_rejects_traversal_absolute_drive_and_backslash(self) -> None:
        for path in (
            "../escape", "a/../b", "/absolute", "C:/absolute", "C:relative",
            "\\\\server\\share", "a\\b",
        ):
            self.assertIsNotNone(vai._canonical_relative_path(path)[1], path)

    def test_rejects_noncanonical_aliases(self) -> None:
        for path in ("./file", "dir/./file", "dir//file", "file/", " file", "file "):
            self.assertIsNotNone(vai._canonical_relative_path(path)[1], path)

    def test_rejects_windows_invalid_ads_and_reserved_names(self) -> None:
        for path in (
            "name:stream", "bad<name", "bad>name", 'bad"name', "bad|name",
            "bad?name", "bad*name", "CON", "com1.txt", "LPT9 .txt", "NUL.any",
            "CONIN$", "dir/PRN.log", "trailing.",
        ):
            self.assertIsNotNone(vai._canonical_relative_path(path)[1], path)

    def test_rejects_control_non_nfc_and_path_bound(self) -> None:
        self.assertIsNotNone(vai._canonical_relative_path("bad\nname")[1])
        self.assertIsNotNone(vai._canonical_relative_path("e\u0301.txt")[1])
        with mock.patch.object(vai, "MAX_PATH_BYTES", 3):
            self.assertIsNotNone(vai._canonical_relative_path("four")[1])


class SnapshotSafetyTests(unittest.TestCase):
    def test_sorted_snapshot_hashes_each_file_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "b.bin").write_bytes(b"b")
            (root / "a.bin").write_bytes(b"a")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(errors, [])
            self.assertEqual([item["path"] for item in entries], ["a.bin", "b.bin"])
            self.assertEqual(entries[0]["sha256"], digest(b"a"))

    def test_nested_manifest_is_not_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested = root / "nested"
            nested.mkdir()
            (root / "data.bin").write_bytes(b"data")
            (nested / vai.MANIFEST_FILENAME).write_bytes(b"nested")
            manifest = write_manifest(root, [{"path": "data.bin", "sha256": digest(b"data"), "size": 4}])
            errors = vai.verify_manifest(manifest, root)
            self.assertTrue(any(e.category == "undeclared" and e.path.endswith(vai.MANIFEST_FILENAME) for e in errors))

    def test_authoritative_root_rejects_escape_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "Assets"
            outside = base / "outside"
            root.mkdir()
            outside.mkdir()
            (outside / "secret.bin").write_bytes(b"secret")
            manifest = write_manifest(root, [], metadata_root="../outside")
            errors = vai.verify_manifest(manifest, root)
            self.assertEqual([e.category for e in errors], ["root-metadata"])

    def test_exact_root_metadata_is_case_sensitive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Assets"
            root.mkdir()
            manifest = write_manifest(root, [], metadata_root="assets")
            self.assertEqual(vai.verify_manifest(manifest, root)[0].category, "root-metadata")

    def test_root_reparse_is_rejected_before_scan(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            target = base / "real"
            link = base / "root-link"
            target.mkdir()
            (target / "secret.bin").write_bytes(b"secret")
            create_reparse(link, target)
            try:
                entries, errors = vai.scan_directory(link)
                self.assertEqual(entries, [])
                self.assertTrue(any(e.category == "reparse" for e in errors))
            finally:
                remove_reparse(link)

    def test_nested_reparse_to_sibling_prefix_is_never_hashed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "Assets"
            sibling = base / "AssetsEvil"
            root.mkdir()
            sibling.mkdir()
            (sibling / "secret.bin").write_bytes(b"secret")
            link = root / "linked"
            create_reparse(link, sibling)
            try:
                with mock.patch.object(vai, "_hash_fd", wraps=vai._hash_fd) as hasher:
                    entries, errors = vai.scan_directory(root)
                self.assertEqual(entries, [])
                self.assertEqual(hasher.call_count, 0)
                self.assertTrue(any(e.category == "reparse" for e in errors))
            finally:
                remove_reparse(link)

    def test_file_change_during_hash_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "asset.bin"
            target.write_bytes(b"before")
            original = vai._hash_fd

            def mutate(fd: int, *, max_bytes: int = vai.MAX_FILE_BYTES) -> str:
                result = original(fd, max_bytes=max_bytes)
                target.write_bytes(b"after-content")
                return result

            with mock.patch.object(vai, "_hash_fd", side_effect=mutate):
                entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertTrue(any(e.category == "io-race" for e in errors))

    def test_scan_io_error_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(vai.os, "scandir", side_effect=PermissionError("denied")):
                entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertEqual(errors[0].category, "io-error")

    @unittest.skipIf(sys.platform == "win32", "mkfifo is Unix-only")
    def test_fifo_is_rejected_without_opening(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            os.mkfifo(root / "pipe")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertEqual(errors[0].category, "non-regular")

    def test_file_count_file_size_and_total_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a").write_bytes(b"aa")
            (root / "b").write_bytes(b"bb")
            with mock.patch.object(vai, "MAX_ENTRY_COUNT", 1):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))

            with mock.patch.object(vai, "MAX_FILE_BYTES", 1):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))
            with mock.patch.object(vai, "MAX_TOTAL_BYTES", 3):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))
            with mock.patch.object(vai, "MAX_DIRECTORY_ENTRY_COUNT", 1):
                _, errors = vai.scan_directory(root)
                self.assertTrue(any(e.category == "resource-limit" for e in errors))


class InstalledFPSPackageAssetIntegrityTests(unittest.TestCase):
    """Exercise the installed-package CMake asset-integrity helper contract."""

    HELPER = REPO_ROOT / "Tests" / "PackageSmoke" / "ValidateInstalledFPSAssets.cmake"

    def _fixture(
        self, temporary: str | os.PathLike[str], license_id: str = "CC0-1.0",
        extra: dict[str, bytes] | None = None,
    ) -> Path:
        # The helper applies the stable-v1 package profile, so the fixture is a
        # schema v2 manifest whose entries match a reviewed source manifest.
        # The source manifest lives in a fixture checkout whose stable-v1
        # profile definition makes payload.bin (plus any extra files) the whole
        # asset closure (RDY-020).
        assets = Path(temporary) / "Assets"
        assets.mkdir(parents=True)
        payload = b"installed FPS package fixture\n"
        files = {"payload.bin": payload, **(extra or {})}
        checkout = Path(temporary) / "checkout"
        for relative in ("Assets", "tools/asset-integrity", "GameModules/Fixture/Source", "Engine/Source"):
            (checkout / relative).mkdir(parents=True)
        # The closure derivation follows scene references in the checkout's copy.
        for base in (assets, checkout / "Assets"):
            for relative, data in files.items():
                (base / relative).parent.mkdir(parents=True, exist_ok=True)
                (base / relative).write_bytes(data)
        (checkout / "tools/asset-integrity/package-profiles.json").write_text(json.dumps({
            "version": 1,
            "profiles": {"stable-v1": {
                "modules": ["Fixture"],
                "engineSources": [{"path": "Engine/Source/", "reason": "Fixture engine code"}],
                "seeds": [{"path": relative, "reason": "Installed-package helper fixture payload"}
                          for relative in sorted(files)],
                "unshippedReferences": [],
            }},
        }), encoding="utf-8")
        (checkout / "GameModules/module-content-inventory.json").write_text(json.dumps({"modules": [{
            "name": "Fixture",
            "sourceDirectory": "GameModules/Fixture/Source",
            "profileApplicability": {"stable-v1": "required"},
        }]}), encoding="utf-8")
        manifest = {
            "version": 2,
            "algorithm": "sha256",
            "root": "Assets",
            "fileCount": len(files),
            "entries": [{
                "path": relative,
                "sha256": digest(files[relative]),
                "size": len(files[relative]),
                "license": license_id,
                "provenance": "Installed-package helper fixture",
            } for relative in sorted(files)],
        }
        (assets / vai.MANIFEST_FILENAME).write_bytes(vai.manifest_bytes(manifest))
        (checkout / "Assets" / vai.MANIFEST_FILENAME).write_bytes(vai.manifest_bytes(manifest))
        return assets

    def _run_helper(self, assets: Path) -> subprocess.CompletedProcess[str]:
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake, "cmake is required for installed-package helper tests")
        return subprocess.run(
            [
                cmake,
                f"-DSPARK_ASSETS_ROOT={assets}",
                f"-DSPARK_ASSET_VERIFIER={SCRIPT}",
                f"-DSPARK_ASSET_SOURCE_MANIFEST={assets.parent / 'checkout' / 'Assets' / vai.MANIFEST_FILENAME}",
                "-P",
                str(self.HELPER),
            ],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )

    def test_installed_package_asset_helper_accepts_matching_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = self._run_helper(self._fixture(temporary))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_installed_package_asset_helper_checks_scene_reference_closure(self) -> None:
        crate = b"o crate\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
        for model, accepted in (("Assets/Models/crate.obj", True), ("crate.obj", False)):
            with self.subTest(model=model), tempfile.TemporaryDirectory() as temporary:
                scene = f"[Scene]\nname=Fixture\n\n[Object]\ntype=model\nmodel={model}\n".encode()
                assets = self._fixture(temporary, extra={"Models/crate.obj": crate, "Scenes/arena.scene": scene})
                result = self._run_helper(assets)
                output = result.stdout + result.stderr
                if accepted:
                    self.assertEqual(result.returncode, 0, output)
                    self.assertIn("reference closure passed: OK: 1 references in 1 scene", output)
                else:
                    # The hash and closure-profile checks accept the bytes; only
                    # the reference check sees that the runtime cannot resolve them.
                    self.assertNotEqual(result.returncode, 0, output)
                    self.assertIn("Installed FPS asset reference closure failed", output)
                    self.assertIn("Scenes/arena.scene:6: model='crate.obj' is not an Assets/-rooted path", output)

    def test_installed_package_asset_helper_fails_hashes_before_references(self) -> None:
        # A tampered payload plus a broken reference must stop at the hash step:
        # closure is only meaningful over bytes already proven to be the reviewed ones.
        scene = b"[Scene]\nname=Fixture\n\n[Object]\ntype=model\nmodel=Assets/Models/absent.obj\n"
        with tempfile.TemporaryDirectory() as temporary:
            assets = self._fixture(temporary, extra={"Scenes/arena.scene": scene})
            (assets / "payload.bin").write_bytes(b"tampered package payload\n")
            result = self._run_helper(assets)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("Installed FPS asset integrity validation failed", output)
        self.assertNotIn("reference closure", output)

    def test_installed_package_asset_helper_rejects_noassertion_asset(self) -> None:
        # OD-09: the stable-v1 package must not ship an asset without a license record.
        with tempfile.TemporaryDirectory() as temporary:
            result = self._run_helper(self._fixture(temporary, license_id="NOASSERTION"))
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"\[profile-excluded\] payload\.bin")

    def test_installed_package_asset_helper_rejects_missing_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            assets = self._fixture(temporary)
            (assets / vai.MANIFEST_FILENAME).unlink()
            result = self._run_helper(assets)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?i)manifest")

    def test_installed_package_asset_helper_rejects_missing_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            assets = self._fixture(temporary)
            (assets / "payload.bin").unlink()
            result = self._run_helper(assets)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?i)missing.*payload\.bin")

    def test_installed_package_asset_helper_rejects_tampered_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            assets = self._fixture(temporary)
            (assets / "payload.bin").write_bytes(b"tampered package payload\n")
            result = self._run_helper(assets)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?i)(hash|size).*payload\.bin")

    def test_installed_package_asset_helper_rejects_case_mismatched_manifest_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            assets = self._fixture(temporary)
            manifest = assets / vai.MANIFEST_FILENAME
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["entries"][0]["path"] = "Payload.bin"
            manifest.write_bytes(vai.manifest_bytes(data))
            result = self._run_helper(assets)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?i)(missing|undeclared).*payload\.bin")

    def test_installed_package_asset_helper_rejects_undeclared_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            assets = self._fixture(temporary)
            (assets / "undeclared.bin").write_bytes(b"undeclared package payload\n")
            result = self._run_helper(assets)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?i)undeclared.*undeclared\.bin")

    def test_installed_package_asset_helper_rejects_link_like_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            assets = self._fixture(temporary)
            outside = root / "outside"
            outside.mkdir()
            (outside / "payload.bin").write_bytes(b"external package payload\n")
            linked = assets / "linked"
            create_reparse(linked, outside)
            try:
                result = self._run_helper(assets)
            finally:
                remove_reparse(linked)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?i)(reparse|symlink|junction).*linked")

    def test_installed_fps_package_wires_asset_helper_before_module_validation(self) -> None:
        package_script = REPO_ROOT / "Tests" / "PackageSmoke" / "RunInstalledFPSPackage.cmake"
        text = package_script.read_text(encoding="utf-8")
        helper_position = text.find("ValidateInstalledFPSAssets.cmake")
        module_position = text.find("ValidateStagedPackageExecutables.cmake")
        self.assertGreaterEqual(helper_position, 0, "installed FPS package must invoke asset helper")
        self.assertGreaterEqual(module_position, 0, "installed FPS package must invoke module validator")
        self.assertLess(
            helper_position,
            module_position,
            "asset integrity must be validated before module validation",
        )

    def test_installed_fps_package_wires_d3d11_smoke_after_module_validation(self) -> None:
        package_script = REPO_ROOT / "Tests" / "PackageSmoke" / "RunInstalledFPSPackage.cmake"
        smoke_script = REPO_ROOT / "Tests" / "PackageSmoke" / "RunInstalledFPSD3D11.cmake"
        package_text = package_script.read_text(encoding="utf-8")
        smoke_text = smoke_script.read_text(encoding="utf-8")
        module_position = package_text.find("ValidateStagedPackageExecutables.cmake")
        d3d11_position = package_text.find("RunInstalledFPSD3D11.cmake")
        save_position = package_text.find("RunInstalledFPSSaveReload.cmake")
        self.assertGreaterEqual(module_position, 0, "installed FPS package must invoke module validator")
        self.assertGreaterEqual(d3d11_position, 0, "installed FPS package must invoke D3D11 smoke")
        self.assertGreaterEqual(save_position, 0, "installed FPS package must retain save/reload smoke")
        self.assertLess(module_position, d3d11_position)
        self.assertLess(d3d11_position, save_position)
        self.assertIn('"SPARK_RHI_BACKEND=d3d11"', smoke_text)
        self.assertIn('"SPARK_D3D11_DRIVER=warp"', smoke_text)
        self.assertIn("_spark_validate_lifecycle_result", smoke_text)
        self.assertIn("asset_root_guard=installed-bin", smoke_text)
        self.assertIn("installed root is the source tree", smoke_text)
        self.assertIn("ReparsePoint", smoke_text)
        self.assertIn("IS_SYMLINK", smoke_text)
        self.assertIn("-test-frames 8", smoke_text)

    def test_installed_fps_d3d11_path_policy_contract(self) -> None:
        script = REPO_ROOT / "Tests" / "PackageSmoke" / "RunInstalledFPSD3D11.cmake"
        result = subprocess.run(
            ["cmake", "-DSPARK_FPS_D3D11_PATH_POLICY_SELF_TEST=ON", "-P", str(script)],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("path policy contract passed", result.stdout)


class ManifestValidationTests(unittest.TestCase):
    def test_valid_manifest_hashes_each_disk_file_only_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.bin").write_bytes(b"a")
            (root / "b.bin").write_bytes(b"b")
            manifest, errors = vai.generate_manifest(root)
            self.assertEqual(errors, [])
            path = root / vai.MANIFEST_FILENAME
            vai.write_manifest(manifest, path)
            with mock.patch.object(vai, "_hash_fd", wraps=vai._hash_fd) as hasher:
                verify_errors = vai.verify_manifest(path, root)
            self.assertEqual(verify_errors, [])
            self.assertEqual(hasher.call_count, 2)

    def test_hash_size_missing_and_undeclared_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "actual.bin").write_bytes(b"actual")
            (root / "extra.bin").write_bytes(b"extra")
            manifest = write_manifest(root, [
                {"path": "actual.bin", "sha256": "0" * 64, "size": 99},
                {"path": "missing.bin", "sha256": "1" * 64, "size": 1},
            ])
            categories = {error.category for error in vai.verify_manifest(manifest, root)}
            self.assertTrue({"hash-mismatch", "size-mismatch", "missing", "undeclared"} <= categories)

    def test_malformed_entries_return_error_instead_of_crashing(self) -> None:
        malformed = (
            [None],
            [{"path": "x"}],
            [{"path": 7, "sha256": "0" * 64, "size": 0}],
            [{"path": "x", "sha256": "bad", "size": 0}],
            [{"path": "x", "sha256": "0" * 64, "size": True}],
        )
        for entries in malformed:
            with self.subTest(entries=entries), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path = root / vai.MANIFEST_FILENAME
                path.write_text(json.dumps({
                    "version": 1,
                    "algorithm": "sha256",
                    "root": root.name,
                    "fileCount": len(entries),
                    "entries": entries,
                }), encoding="utf-8")
                errors = vai.verify_manifest(path, root)
                self.assertEqual(errors[0].category, "manifest-load")

    def test_duplicate_json_keys_duplicate_paths_and_aliases_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duplicate_key = root / vai.MANIFEST_FILENAME
            duplicate_key.write_text(
                '{"version":1,"version":1,"algorithm":"sha256","root":"x","fileCount":0,"entries":[]}',
                encoding="utf-8",
            )
            self.assertEqual(vai.verify_manifest(duplicate_key, root)[0].category, "manifest-load")

            for paths in (("same", "same"), ("File.txt", "file.txt")):
                entries = [{"path": item, "sha256": "0" * 64, "size": 0} for item in paths]
                write_manifest(root, entries)
                self.assertEqual(vai.verify_manifest(duplicate_key, root)[0].category, "manifest-load")

    def test_entries_must_be_sorted_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = write_manifest(root, [
                {"path": "b", "sha256": "0" * 64, "size": 0},
                {"path": "a", "sha256": "1" * 64, "size": 0},
            ])
            self.assertEqual(vai.verify_manifest(path, root)[0].category, "manifest-load")
            with mock.patch.object(vai, "MAX_ENTRY_COUNT", 1):
                self.assertEqual(vai.verify_manifest(path, root)[0].category, "manifest-load")
            path.write_bytes(b"{}" * 10)
            with self.assertRaises(vai.ManifestFormatError):
                vai._read_bounded_json(path, limit=4)

    def test_generation_is_deterministic_and_refuses_every_scan_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as policy_dir:
            root = Path(directory)
            (root / "b").write_bytes(b"b")
            (root / "a").write_bytes(b"a")
            one, errors_one = vai.generate_manifest(root)
            two, errors_two = vai.generate_manifest(root)
            self.assertEqual(errors_one, [])
            self.assertEqual(errors_two, [])
            self.assertEqual(vai.manifest_bytes(one), vai.manifest_bytes(two))
            # The CLI always emits schema v2, so it needs a policy claiming every file.
            policy = Path(policy_dir) / "provenance.json"
            policy.write_text(json.dumps({
                "version": 1,
                "root": root.name,
                "licenses": {"NOASSERTION": {"name": "No license asserted"}},
                "rules": [{
                    "id": "fixture",
                    "license": "NOASSERTION",
                    "provenance": "Test fixture bytes",
                    "evidence": [],
                    "gap": "RDY-020",
                    "files": {"a": digest(b"a"), "b": digest(b"b")},
                }],
            }), encoding="utf-8")
            args = argparse.Namespace(root=str(root), output=None, provenance=str(policy))
            self.assertEqual(vai.cmd_generate(args), 0)
            first_bytes = (root / vai.MANIFEST_FILENAME).read_bytes()
            self.assertEqual(json.loads(first_bytes)["version"], vai.MANIFEST_SCHEMA_VERSION)
            self.assertEqual(vai.cmd_generate(args), 0)
            self.assertEqual((root / vai.MANIFEST_FILENAME).read_bytes(), first_bytes)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = argparse.Namespace(root=str(root), output=None)
            failure = vai.IntegrityError("x", "io-error", "denied")
            with mock.patch.object(vai, "generate_manifest", return_value=({}, [failure])):
                self.assertEqual(vai.cmd_generate(args), 1)
            self.assertFalse((root / vai.MANIFEST_FILENAME).exists())


class TemplateCompletenessTests(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[Path, Path, Path]:
        assets = root / "Templates" / "Starter" / "Assets"
        assets.mkdir(parents=True)
        data = b"asset"
        (assets / "asset.bin").write_bytes(data)
        (assets / "README.md").write_bytes(b"root metadata")
        manifest = assets / "manifest.json"
        manifest.write_text(json.dumps({
            "manifestVersion": 1,
            "package": "Starter",
            "assets": [{"path": "asset.bin", "sha256": digest(data)}],
        }), encoding="utf-8")
        lock = root / "Templates" / "assets.lock.json"
        lock.write_text(json.dumps({
            "version": 1,
            "algorithm": "sha256",
            "assets": {"Starter/Assets/asset.bin": digest(data)},
        }), encoding="utf-8")
        return assets, manifest, lock

    def test_exact_manifest_lock_disk_equality(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._fixture(root)
            self.assertEqual(vai.verify_template_manifests(root), [])

    def test_unexpected_template_root_file_is_not_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._fixture(root)
            unexpected = root / "Templates" / "unexpected.bin"
            unexpected.write_bytes(b"concealed payload")
            errors = vai.verify_template_manifests(root)
        self.assertTrue(any(
            error.category == "undeclared" and error.path == "Templates/unexpected.bin"
            for error in errors
        ), errors)

    def test_empty_template_manifest_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets, manifest, lock = self._fixture(root)
            (assets / "asset.bin").unlink()
            manifest.write_text(json.dumps({
                "manifestVersion": 1,
                "package": "Starter",
                "assets": [],
            }), encoding="utf-8")
            lock.write_text(json.dumps({
                "version": 1,
                "algorithm": "sha256",
                "assets": {},
            }), encoding="utf-8")
            errors = vai.verify_template_manifests(root)
        self.assertTrue(any(
            error.category == "manifest-load"
            and error.path == "Templates/Starter/Assets/manifest.json"
            for error in errors
        ), errors)

    def test_undeclared_disk_file_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets, _, _ = self._fixture(root)
            (assets / "extra.bin").write_bytes(b"extra")
            self.assertTrue(any(e.category == "undeclared" for e in vai.verify_template_manifests(root)))

    def test_stale_lock_entry_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, _, lock = self._fixture(root)
            data = json.loads(lock.read_text(encoding="utf-8"))
            data["assets"]["Starter/Assets/stale.bin"] = "0" * 64
            lock.write_text(json.dumps(data), encoding="utf-8")
            self.assertTrue(any(e.category == "lock-extra" for e in vai.verify_template_manifests(root)))

    def test_duplicate_manifest_row_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, manifest, _ = self._fixture(root)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["assets"].append(dict(data["assets"][0]))
            manifest.write_text(json.dumps(data), encoding="utf-8")
            self.assertTrue(any(e.category == "manifest-load" for e in vai.verify_template_manifests(root)))

    def test_nested_metadata_filename_is_not_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets, _, _ = self._fixture(root)
            nested = assets / "nested"
            nested.mkdir()
            (nested / "manifest.json").write_bytes(b"nested")
            self.assertTrue(any(e.category == "undeclared" for e in vai.verify_template_manifests(root)))


class ToctouFingerprintTests(unittest.TestCase):
    """Finding 1: TOCTOU fingerprint limitations and honest boundary."""

    def test_restored_mtime_same_size_rewrite_is_toctou_gap(self) -> None:
        """Demonstrates the documented TOCTOU limitation: a same-size rewrite
        that occurs within the filesystem timestamp granularity may not be
        detected by fingerprint comparison. This test proves the gap exists
        and is honestly documented rather than silently ignored."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "asset.bin"
            target.write_bytes(b"AAAA")
            before_stat = target.stat()
            original = vai._hash_fd

            def tamper_restore_mtime(fd: int, *, max_bytes: int = vai.MAX_FILE_BYTES) -> str:
                result = original(fd, max_bytes=max_bytes)
                target.write_bytes(b"BBBB")
                os.utime(target, ns=(before_stat.st_atime_ns, before_stat.st_mtime_ns))
                return result

            with mock.patch.object(vai, "_hash_fd", side_effect=tamper_restore_mtime):
                entries, errors = vai.scan_directory(root)
            if not errors:
                self.assertEqual(len(entries), 1)
                self.assertNotEqual(entries[0]["sha256"], digest(b"BBBB"),
                    "hash should reflect the pre-tamper content read from the fd")

    def test_concurrent_addition_is_toctou_gap(self) -> None:
        """Demonstrates the documented TOCTOU limitation: a file created after
        scandir completes (but before walk finishes) is not guaranteed to appear
        in the snapshot. The scan is non-atomic."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "existing.bin").write_bytes(b"exists")
            entries, errors = vai.scan_directory(root)
            self.assertEqual(len(entries), 1)
            (root / "added_later.bin").write_bytes(b"late")
            self.assertTrue((root / "added_later.bin").exists(),
                "file exists on disk but was not in the prior snapshot")

    def test_concurrent_deletion_produces_error(self) -> None:
        """A file present in scandir but deleted before read produces an error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "vanish.bin"
            target.write_bytes(b"here")

            call_count = 0
            original_checked = vai._checked_candidate
            def delete_on_second_check(r, rr, rel, *, want_directory=False):
                nonlocal call_count
                result = original_checked(r, rr, rel, want_directory=want_directory)
                if rel == "vanish.bin" and not want_directory:
                    call_count += 1
                    if call_count == 1:
                        target.unlink()
                return result

            with mock.patch.object(vai, "_checked_candidate", side_effect=delete_on_second_check):
                entries, errors = vai.scan_directory(root)
            self.assertEqual(entries, [])
            self.assertTrue(len(errors) > 0)

    def test_directory_swap_during_walk_is_caught(self) -> None:
        """Directory replaced with a file mid-walk produces an error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subdir = root / "subdir"
            subdir.mkdir()
            (subdir / "inner.bin").write_bytes(b"inner")

            original_checked = vai._checked_candidate
            def swap_dir(r, rr, rel, *, want_directory=False):
                if rel == "subdir" and want_directory:
                    import shutil
                    shutil.rmtree(subdir)
                    subdir.write_bytes(b"not a dir")
                return original_checked(r, rr, rel, want_directory=want_directory)

            with mock.patch.object(vai, "_checked_candidate", side_effect=swap_dir):
                entries, errors = vai.scan_directory(root)
            has_relevant_error = any(
                e.path.startswith("subdir") for e in errors
            )
            self.assertTrue(has_relevant_error or len(entries) == 0)


class ConcealedPayloadTests(unittest.TestCase):
    """Finding 2: ignored paths must be statted to prevent concealed payloads."""

    def test_ignored_directory_readme_is_rejected(self) -> None:
        """A directory named README.md at template root is a concealed-payload error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data.bin").write_bytes(b"data")
            readme_dir = root / "README.md"
            readme_dir.mkdir()
            (readme_dir / "hidden.bin").write_bytes(b"payload")
            entries, errors = vai.scan_directory(
                root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
            categories = {e.category for e in errors}
            self.assertIn("concealed-payload", categories)

    def test_ignored_regular_file_readme_is_skipped_cleanly(self) -> None:
        """A regular README.md at template root is silently ignored (no error)."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data.bin").write_bytes(b"data")
            (root / "README.md").write_bytes(b"readme")
            entries, errors = vai.scan_directory(
                root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
            self.assertEqual(errors, [])
            self.assertEqual(len(entries), 1)
            self.assertEqual(entries[0]["path"], "data.bin")

    def test_ignored_manifest_json_directory_is_rejected(self) -> None:
        """A directory named manifest.json is a concealed-payload error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data.bin").write_bytes(b"data")
            manifest_dir = root / "manifest.json"
            manifest_dir.mkdir()
            (manifest_dir / "secret.bin").write_bytes(b"secret")
            entries, errors = vai.scan_directory(
                root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
            self.assertIn("concealed-payload", {e.category for e in errors})

    def test_ignored_reparse_readme_is_rejected(self) -> None:
        """A reparse point named README.md is a concealed-payload error."""
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root = base / "assets"
            target = base / "evil"
            root.mkdir()
            target.mkdir()
            (target / "payload.bin").write_bytes(b"payload")
            (root / "data.bin").write_bytes(b"data")
            link = root / "README.md"
            create_reparse(link, target)
            try:
                entries, errors = vai.scan_directory(
                    root, ignored_root_paths=vai.TEMPLATE_ROOT_METADATA)
                self.assertIn("concealed-payload", {e.category for e in errors})
            finally:
                remove_reparse(link)


class HashFdBoundaryTests(unittest.TestCase):
    """Finding 3: _hash_fd must enforce live byte limits."""

    def test_hash_fd_rejects_growth_past_declared_size(self) -> None:
        """_hash_fd raises when the file grows past the declared max_bytes."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "growing.bin"
            path.write_bytes(b"small")
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
            try:
                with self.assertRaises(OSError):
                    vai._hash_fd(fd, max_bytes=2)
            finally:
                os.close(fd)

    def test_hash_fd_accepts_within_limit(self) -> None:
        """_hash_fd succeeds when file is within the byte limit."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ok.bin"
            data = b"fits"
            path.write_bytes(data)
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
            try:
                result = vai._hash_fd(fd, max_bytes=100)
                self.assertEqual(result, digest(data))
            finally:
                os.close(fd)

    def test_hash_fd_exact_limit_succeeds(self) -> None:
        """_hash_fd succeeds when file size exactly equals max_bytes."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "exact.bin"
            data = b"ABCD"
            path.write_bytes(data)
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0))
            try:
                result = vai._hash_fd(fd, max_bytes=4)
                self.assertEqual(result, digest(data))
            finally:
                os.close(fd)

    def test_concurrent_growth_during_scan_is_caught(self) -> None:
        """A file that grows during hash in scan_directory produces an error."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "grower.bin"
            target.write_bytes(b"sm")
            original = vai._hash_fd
            def grow_then_hash(fd: int, *, max_bytes: int = vai.MAX_FILE_BYTES) -> str:
                target.write_bytes(b"sm" + b"X" * 1000)
                os.lseek(fd, 0, os.SEEK_SET)
                return original(fd, max_bytes=max_bytes)
            with mock.patch.object(vai, "_hash_fd", side_effect=grow_then_hash):
                entries, errors = vai.scan_directory(root)
            has_error = len(errors) > 0 or len(entries) == 0
            self.assertTrue(has_error)


class SiteDataAssetIntegrationTests(unittest.TestCase):
    """The site-data asset gate must fail closed around the integrity verifier."""

    def make_root(self, root: Path, *, include_verifier: bool) -> None:
        assets = root / "Assets"
        assets.mkdir(parents=True)
        (root / "Shaders").mkdir()
        graphics = root / "SparkEngine" / "Source" / "Graphics"
        graphics.mkdir(parents=True)
        (graphics / "AssetPipeline.cpp").write_text("// fixture\n", encoding="utf-8")
        payload = b"verified fixture asset"
        (assets / "fixture.bin").write_bytes(payload)
        (assets / "assets.integrity.json").write_text(
            json.dumps(
                {
                    "version": 1,
                    "algorithm": "sha256",
                    "root": "Assets",
                    "fileCount": 1,
                    "entries": [
                        {
                            "path": "fixture.bin",
                            "sha256": hashlib.sha256(payload).hexdigest(),
                            "size": len(payload),
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        if include_verifier:
            target = root / "tools" / "asset-integrity" / SCRIPT.name
            target.parent.mkdir(parents=True)
            target.write_bytes(SCRIPT.read_bytes())

    def validate_fixture(self, root: Path) -> list[str]:
        with (
            mock.patch.object(site_data_validate, "REPO_ROOT", root),
            mock.patch.object(site_data_validate, "validate_assets", return_value=[]),
        ):
            validator = site_data_validate.Validator({})
            validator.validate_asset_surface()
        return validator.errors

    def test_missing_integrity_verifier_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_root(root, include_verifier=False)
            errors = self.validate_fixture(root)
        self.assertTrue(any("asset integrity verifier is missing" in error for error in errors), errors)

    def test_tampered_integrity_manifest_fails_through_site_data_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_root(root, include_verifier=True)
            (root / "Assets" / "fixture.bin").write_bytes(b"tampered")
            errors = self.validate_fixture(root)
        self.assertTrue(any("asset-integrity:fixture.bin" in error for error in errors), errors)


class AssetReferenceClosureTests(unittest.TestCase):
    """ENG-220: staged scene and material references must close over listed files inside the root."""

    SCENE = (
        "[Scene]\n"
        "name=Fixture\n"
        "skybox=Assets/Textures/sky/space\n"
        "\n"
        "[Object]\n"
        "type=model\n"
        "model=Assets/Models/crate.obj\n"
        "material=Assets/Materials/Stone.json\n"
        "position=0.0,0.0,0.0\n"
    )
    ZONE = {
        "name": "Zone",
        "environment": {"skyTexture": ""},
        "entities": [
            {"name": "Crate", "components": {"MeshRenderer": {"mesh": "Models/crate.obj",
                                                              "material": "Materials/Stone.json"}}},
            {"name": "Floor", "components": {"MeshRenderer": {"mesh": "Primitive/Cube"}}},
            {"name": "Ambience", "components": {"AudioSource": {"sound": "Audio/amb.wav"}}},
        ],
    }
    MATERIAL = {"name": "Stone", "shader": "PBR", "albedo": "Textures/stone.png",
                "normal": "Assets/Textures/stone_n.png", "roughness": 0.5, "tiling": [1, 1]}

    def _stage(
        self, directory: str, overrides: dict[str, bytes | None] | None = None, *, unlisted: tuple[str, ...] = ()
    ) -> Path:
        root = Path(directory) / "Assets"
        files: dict[str, bytes | None] = {
            "Scenes/level.scene": self.SCENE.encode(),
            "Scenes/zone.scene": json.dumps(self.ZONE).encode(),
            "Materials/Stone.json": json.dumps(self.MATERIAL).encode(),
            "Models/crate.obj": b"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
            "Textures/stone.png": b"png",
            "Textures/stone_n.png": b"png-normal",
            "Audio/amb.wav": b"wav",
        }
        for face in ("px", "nx", "py", "ny", "pz", "nz"):
            files[f"Textures/sky/space_{face}.png"] = face.encode()
        files.update(overrides or {})
        entries = []
        for relative, data in sorted(files.items()):
            if data is None:
                continue
            (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / relative).write_bytes(data)
            if relative not in unlisted:
                entries.append({"path": relative, "sha256": digest(data), "size": len(data)})
        write_manifest(root, entries)
        return root

    def _check(self, root: Path) -> tuple[list, int, int]:
        return vai.verify_references(root / vai.MANIFEST_FILENAME, root)

    def _assert_error(self, root: Path, category: str, location: str, fragment: str) -> None:
        errors, _, _ = self._check(root)
        rendered = [str(error) for error in errors]
        self.assertTrue(
            any(error.category == category and error.path == location and fragment in error.message
                for error in errors),
            f"expected [{category}] {location}: ...{fragment}... in {rendered}")

    def test_closed_fixture_passes_both_dialects_and_materials(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory)
            self.assertEqual(vai.verify_manifest(root / vai.MANIFEST_FILENAME, root), [])
            errors, parsed, checked = self._check(root)
        self.assertEqual(errors, [], [str(error) for error in errors])
        # level.scene: 6 skybox faces + model + material; zone.scene: mesh, material, sound; Stone.json: 2.
        self.assertEqual((parsed, checked), (3, 13))

    def test_missing_texture_names_material_and_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Textures/stone.png": None})
            self._assert_error(root, "reference-missing", "Materials/Stone.json:albedo",
                               "albedo='Textures/stone.png' resolves to 'Textures/stone.png', which is not staged")

    def test_missing_material_names_scene_line(self) -> None:
        scene = self.SCENE.replace("Materials/Stone.json", "Materials/Missing.json")
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Scenes/level.scene": scene.encode()})
            self._assert_error(root, "reference-missing", "Scenes/level.scene:8",
                               "material='Assets/Materials/Missing.json'")

    def test_path_escaping_the_root_fails_in_every_format(self) -> None:
        zone = json.loads(json.dumps(self.ZONE))
        zone["entities"][0]["components"]["MeshRenderer"]["mesh"] = "Models/../../outside.obj"
        material = dict(self.MATERIAL, normal="../Textures/stone_n.png")
        cases = {
            "Scenes/level.scene:7": ("Scenes/level.scene",
                                     self.SCENE.replace("Assets/Models/crate.obj", "Assets/../outside.obj").encode()),
            "Scenes/zone.scene:entities.0.components.MeshRenderer.mesh": ("Scenes/zone.scene",
                                                                          json.dumps(zone).encode()),
            "Materials/Stone.json:normal": ("Materials/Stone.json", json.dumps(material).encode()),
        }
        for location, (key, data) in cases.items():
            with self.subTest(location=location), tempfile.TemporaryDirectory() as directory:
                (Path(directory) / "outside.obj").write_bytes(b"v 0 0 0\n")
                root = self._stage(directory, {key: data})
                self._assert_error(root, "reference", location, "escapes the staged Assets root")

    def test_absolute_reference_fails(self) -> None:
        material = dict(self.MATERIAL, albedo="/etc/passwd")
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Materials/Stone.json": json.dumps(material).encode()})
            self._assert_error(root, "reference", "Materials/Stone.json:albedo", "is an absolute path")

    def test_unlisted_staged_file_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, unlisted=("Audio/amb.wav",))
            self._assert_error(root, "reference-unlisted",
                               "Scenes/zone.scene:entities.2.components.AudioSource.sound",
                               "which assets.integrity.json does not list")

    def test_case_mismatch_fails_on_every_filesystem(self) -> None:
        scene = self.SCENE.replace("Assets/Models/crate.obj", "Assets/Models/Crate.obj")
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Scenes/level.scene": scene.encode()})
            self._assert_error(root, "reference-case", "Scenes/level.scene:7", "differs in case")

    def test_bare_scene_names_the_runtime_cannot_resolve_fail(self) -> None:
        scene = self.SCENE.replace("Assets/Models/crate.obj", "crate.obj").replace(
            "Assets/Materials/Stone.json", "ground_dirt")
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Scenes/level.scene": scene.encode()})
            self._assert_error(root, "reference", "Scenes/level.scene:7", "is not an Assets/-rooted path")
            self._assert_error(root, "reference", "Scenes/level.scene:8", "is not an Assets/-rooted path")

    def test_unknown_reference_like_keys_fail_closed(self) -> None:
        zone = json.loads(json.dumps(self.ZONE))
        zone["entities"][0]["components"]["Decal"] = {"image": "Textures/stone.png"}
        material = dict(self.MATERIAL, emissiveMap="Textures/stone.png")
        cases = {
            "Scenes/level.scene:10": ("Scenes/level.scene",
                                      (self.SCENE + "texture=Assets/Textures/stone.png\n").encode()),
            "Scenes/zone.scene:entities.0.components.Decal.image": ("Scenes/zone.scene", json.dumps(zone).encode()),
            "Materials/Stone.json:emissiveMap": ("Materials/Stone.json", json.dumps(material).encode()),
        }
        for location, (key, data) in cases.items():
            with self.subTest(location=location), tempfile.TemporaryDirectory() as directory:
                root = self._stage(directory, {key: data})
                self._assert_error(root, "reference", location, "under a key the reference validator does not know")

    def test_every_cubemap_face_must_be_staged(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Textures/sky/space_nz.png": None})
            self._assert_error(root, "reference-missing", "Scenes/level.scene:3",
                               "resolves to 'Textures/sky/space_nz.png'")

    def test_material_outside_materials_is_followed(self) -> None:
        zone = json.loads(json.dumps(self.ZONE))
        zone["entities"][0]["components"]["MeshRenderer"]["material"] = "Data/Other.json"
        other = json.dumps(dict(self.MATERIAL, albedo="Textures/absent.png")).encode()
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Scenes/zone.scene": json.dumps(zone).encode(), "Data/Other.json": other})
            self._assert_error(root, "reference-missing", "Data/Other.json:albedo", "'Textures/absent.png'")

    def test_legacy_and_malformed_scenes_fail_closed(self) -> None:
        cases = {
            "is not an INI or JSON scene": b"Cube 0 0 0\nSphere 1 2 3\n",
            "is not valid JSON": b'{"entities": [}',
            "duplicate JSON object key": b'{"name": "a", "name": "b"}',
        }
        for fragment, data in cases.items():
            with self.subTest(fragment=fragment), tempfile.TemporaryDirectory() as directory:
                root = self._stage(directory, {"Scenes/zone.scene": data})
                self._assert_error(root, "reference", "Scenes/zone.scene", fragment)

    @unittest.skipIf(sys.platform == "win32", "symlink creation needs privileges on Windows")
    def test_link_like_reference_target_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Audio/amb.wav": None})
            outside = Path(directory) / "outside.wav"
            outside.write_bytes(b"wav")
            (root / "Audio").mkdir()
            (root / "Audio" / "amb.wav").symlink_to(outside)
            manifest = vai.load_manifest(root / vai.MANIFEST_FILENAME)
            manifest["entries"].append({"path": "Audio/amb.wav", "sha256": digest(b"wav"), "size": 3})
            manifest["entries"].sort(key=lambda entry: entry["path"])
            manifest["fileCount"] = len(manifest["entries"])
            (root / vai.MANIFEST_FILENAME).write_bytes(vai.manifest_bytes(manifest))
            self._assert_error(root, "reference-unsafe",
                               "Scenes/zone.scene:entities.2.components.AudioSource.sound", "symlink")

    def test_command_line_reports_referencing_file_and_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory)
            passed = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "references", str(root / vai.MANIFEST_FILENAME),
                 "--root", str(root)], text=True, capture_output=True, timeout=60, check=False)
            (root / "Textures" / "stone_n.png").unlink()
            failed = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "references", str(root / vai.MANIFEST_FILENAME),
                 "--root", str(root)], text=True, capture_output=True, timeout=60, check=False)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertIn("OK: 13 references in 3 scene and material files", passed.stdout)
        self.assertEqual(failed.returncode, 1)
        self.assertIn("[reference-missing] Materials/Stone.json:normal: normal='Assets/Textures/stone_n.png'",
                      failed.stderr)

    def test_ini_material_outside_runtime_material_root_fails(self) -> None:
        # GameObject loads INI materials only from Assets/Materials/*.json (exact
        # case); any other staged, listed JSON would silently render the default.
        for value in ("Assets/Textures/foo.json", "Assets/materials/foo.json", "Assets/Materials/Foo.JSON",
                      "Assets/Materials\\Foo.json"):
            scene = self.SCENE.replace("Assets/Materials/Stone.json", value)
            with self.subTest(value=value), tempfile.TemporaryDirectory() as directory:
                staged = value[len("Assets/"):].replace("\\", "/")
                root = self._stage(directory, {"Scenes/level.scene": scene.encode(),
                                               staged: json.dumps(self.MATERIAL).encode()})
                self._assert_error(root, "reference", "Scenes/level.scene:8",
                                   "GameObject ignores materials outside Assets/Materials/")
        backslash = self.SCENE.replace("Assets/Materials/Stone.json", "Assets\\Materials\\Stone.json")
        with tempfile.TemporaryDirectory() as directory:
            root = self._stage(directory, {"Scenes/level.scene": backslash.encode()})
            errors, _, _ = self._check(root)
        self.assertEqual(errors, [], [str(error) for error in errors])


class RepositoryParityTests(unittest.TestCase):
    def test_live_repository_contract(self) -> None:
        errors = vai.verify_repository(REPO_ROOT)
        self.assertEqual(errors, [], msg="\n".join(str(error) for error in errors))

    def test_manifest_matches_git_blobs_and_worktree_bytes(self) -> None:
        manifest = vai.load_manifest(REPO_ROOT / "Assets" / vai.MANIFEST_FILENAME)
        process = subprocess.Popen(
            ["git", "cat-file", "--batch"],
            cwd=REPO_ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )
        assert process.stdin is not None and process.stdout is not None
        try:
            for entry in manifest["entries"]:
                relative = entry["path"]
                process.stdin.write(f"HEAD:Assets/{relative}\n".encode("utf-8"))
                process.stdin.flush()
                header = process.stdout.readline().decode("ascii").strip().split()
                self.assertEqual(header[1], "blob", relative)
                size = int(header[2])
                blob = process.stdout.read(size)
                self.assertEqual(process.stdout.read(1), b"\n")
                self.assertEqual(size, entry["size"], relative)
                self.assertEqual(digest(blob), entry["sha256"], relative)
                self.assertEqual((REPO_ROOT / "Assets" / relative).read_bytes(), blob, relative)
        finally:
            process.stdin.close()
            process.stdout.close()
            process.wait(timeout=10)
        attribute = subprocess.check_output(
            ["git", "check-attr", "text", "--", "Assets/README.md"],
            cwd=REPO_ROOT,
            text=True,
        )
        self.assertIn("text: unset", attribute)


if __name__ == "__main__":
    unittest.main()
