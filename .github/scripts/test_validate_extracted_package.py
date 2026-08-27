#!/usr/bin/env python3
"""Regression tests for portable-archive and extracted-package validation."""

from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile


SCRIPT = Path(__file__).with_name("validate-extracted-package.py")
ROOT = Path(__file__).resolve().parents[2]
RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
SPEC = importlib.util.spec_from_file_location("validate_extracted_package", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ArchiveMemberPreflightTests(unittest.TestCase):
    def _write_zip(self, path: Path, entries: tuple[tuple[str, bytes], ...]) -> None:
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
            for name, data in entries:
                archive.writestr(name, data)

    def _write_tar_gz(self, path: Path, entries: tuple[tuple[str, bytes], ...]) -> None:
        with tarfile.open(path, "w:gz") as archive:
            for name, data in entries:
                member = tarfile.TarInfo(name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))

    def test_clean_zip_and_tar_gz_archives_pass(self) -> None:
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            archives = (root / "package.zip", root / "package.tar.gz")
            entries = (("SparkEngine/bin/engine", b"binary"),)
            self._write_zip(archives[0], entries)
            self._write_tar_gz(archives[1], entries)

            for archive in archives:
                with self.subTest(archive=archive.name):
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output):
                        MODULE.validate_archive(archive)
                    self.assertIn("Archive member preflight passed", output.getvalue())

    def test_preflight_archive_cli_mode(self) -> None:
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            archive = Path(temporary) / "package.zip"
            self._write_zip(archive, (("SparkEngine/bin/engine", b"binary"),))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = MODULE.main(["--preflight-archive", str(archive)])
            self.assertEqual(result, 0)
            self.assertIn("Archive member preflight passed", output.getvalue())

    def test_rejects_absolute_drive_unc_and_traversal_paths(self) -> None:
        for name in (
            "/absolute/file",
            "\\absolute\\file",
            "\\\\server\\share\\file",
            "C:\\package\\file",
            "C:relative-file",
            "../escape",
            "root/../../escape",
            "root/.. /escape",
        ):
            with self.subTest(name=name), self.assertRaises(MODULE.ValidationError):
                MODULE._normalize_archive_destination(name)

        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            archives = (root / "traversal.zip", root / "traversal.tar.gz")
            entries = (("../escape", b"payload"),)
            self._write_zip(archives[0], entries)
            self._write_tar_gz(archives[1], entries)
            for archive in archives:
                with self.subTest(archive=archive.name), self.assertRaisesRegex(
                    MODULE.ValidationError, "Traversal"
                ):
                    MODULE.validate_archive(archive)

    def test_rejects_duplicate_case_colliding_and_type_conflicting_destinations(self) -> None:
        tracker = MODULE._ArchiveMemberTracker()
        tracker.add("Root/file.txt", "file", 1)
        with self.assertRaisesRegex(MODULE.ValidationError, "duplicate"):
            tracker.add("Root/file.txt", "file", 1)

        tracker = MODULE._ArchiveMemberTracker()
        tracker.add("Root/file.txt", "file", 1)
        with self.assertRaisesRegex(MODULE.ValidationError, "case-colliding"):
            tracker.add("root/FILE.txt", "file", 1)

        tracker = MODULE._ArchiveMemberTracker()
        tracker.add("Root", "file", 1)
        with self.assertRaisesRegex(MODULE.ValidationError, "non-directory"):
            tracker.add("Root/child.txt", "file", 1)

        tracker = MODULE._ArchiveMemberTracker()
        tracker.add("Root/child.txt", "file", 1)
        with self.assertRaisesRegex(MODULE.ValidationError, "conflicts with a directory"):
            tracker.add("Root", "file", 1)

    def test_rejects_zip_symlink_entries(self) -> None:
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            archive_path = Path(temporary) / "links.zip"
            member = zipfile.ZipInfo("link")
            member.create_system = 3
            member.external_attr = (MODULE.stat.S_IFLNK | 0o777) << 16
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr(member, b"target")
            with self.assertRaisesRegex(MODULE.ValidationError, "link/device/special"):
                MODULE.validate_archive(archive_path)

    def test_rejects_tar_symlink_hardlink_device_and_special_entries(self) -> None:
        entry_types = (
            ("symlink", tarfile.SYMTYPE),
            ("hardlink", tarfile.LNKTYPE),
            ("character-device", tarfile.CHRTYPE),
            ("block-device", tarfile.BLKTYPE),
            ("fifo", tarfile.FIFOTYPE),
        )
        for name, entry_type in entry_types:
            with self.subTest(name=name), tempfile.TemporaryDirectory(
                dir=Path.cwd()
            ) as temporary:
                archive_path = Path(temporary) / "special.tar.gz"
                member = tarfile.TarInfo(name)
                member.type = entry_type
                member.linkname = "target"
                with tarfile.open(archive_path, "w:gz") as archive:
                    archive.addfile(member)
                with self.assertRaisesRegex(
                    MODULE.ValidationError, "link|device|special"
                ):
                    MODULE.validate_archive(archive_path)

    def test_rejects_member_size_total_size_and_member_count_limits(self) -> None:
        tracker = MODULE._ArchiveMemberTracker()
        with self.assertRaisesRegex(MODULE.ValidationError, "smaller than"):
            tracker.add("huge.bin", "file", MODULE.TWO_GIB)

        with mock.patch.object(MODULE, "MAX_TOTAL_EXPANDED_BYTES", 3):
            tracker = MODULE._ArchiveMemberTracker()
            tracker.add("one.bin", "file", 2)
            with self.assertRaisesRegex(MODULE.ValidationError, "expanded size"):
                tracker.add("two.bin", "file", 2)

        with mock.patch.object(MODULE, "MAX_ARCHIVE_MEMBERS", 1):
            tracker = MODULE._ArchiveMemberTracker()
            tracker.add("one.bin", "file", 0)
            with self.assertRaisesRegex(MODULE.ValidationError, "more than"):
                tracker.add("two.bin", "file", 0)

    def test_rejects_format_extension_mismatch_and_unsupported_extension(self) -> None:
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            zip_path = root / "package.zip"
            tar_path = root / "package.tar.gz"
            self._write_zip(zip_path, (("file", b"zip"),))
            self._write_tar_gz(tar_path, (("file", b"tar"),))

            wrong_tar = root / "zip-data.tar.gz"
            wrong_tar.write_bytes(zip_path.read_bytes())
            wrong_zip = root / "tar-data.zip"
            wrong_zip.write_bytes(tar_path.read_bytes())
            unsupported = root / "package.tgz"
            unsupported.write_bytes(tar_path.read_bytes())

            for archive in (wrong_tar, wrong_zip):
                with self.subTest(archive=archive.name), self.assertRaisesRegex(
                    MODULE.ValidationError, "format/extension mismatch"
                ):
                    MODULE.validate_archive(archive)
            with self.assertRaisesRegex(MODULE.ValidationError, r"\.zip or \.tar\.gz"):
                MODULE.validate_archive(unsupported)

    def test_rejects_malformed_and_corrupt_archives(self) -> None:
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            malformed_zip = root / "malformed.zip"
            malformed_zip.write_bytes(b"PK\x03\x04broken")
            malformed_tar = root / "malformed.tar.gz"
            malformed_tar.write_bytes(b"\x1f\x8b\x08broken")

            valid_zip = root / "corrupt.zip"
            self._write_zip(valid_zip, (("file.txt", b"unique-payload"),))
            corrupted = bytearray(valid_zip.read_bytes())
            payload_offset = corrupted.index(b"unique-payload")
            corrupted[payload_offset] ^= 0xFF
            valid_zip.write_bytes(corrupted)

            valid_tar = root / "truncated.tar.gz"
            self._write_tar_gz(valid_tar, (("file.txt", b"payload"),))
            valid_tar.write_bytes(valid_tar.read_bytes()[:-8])

            for archive in (malformed_zip, malformed_tar, valid_zip, valid_tar):
                with self.subTest(archive=archive.name), self.assertRaisesRegex(
                    MODULE.ValidationError, "Malformed"
                ):
                    MODULE.validate_archive(archive)


class ExtractedPackageValidationTests(unittest.TestCase):
    def _package(self, root: Path, library_data: bytes = b"library") -> Path:
        package = root / "package"
        templates = package / "share" / "SparkEngine" / "templates" / "EmptyProject"
        templates.mkdir(parents=True)
        (templates / "CMakeLists.txt").write_text("project(EmptyProject)\n", encoding="utf-8")
        models = templates / "Assets" / "Models"
        models.mkdir(parents=True)
        (models / "starter_mesh.obj").write_text("o StarterMesh\n", encoding="utf-8")
        library = package / "lib" / "SparkEngineLib.lib"
        library.parent.mkdir(parents=True)
        library.write_bytes(library_data)
        return package

    def test_clean_package_and_matching_stage_pass(self) -> None:
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            package = self._package(root)
            stage = root / "stage"
            (stage / "lib").mkdir(parents=True)
            (stage / "lib" / "SparkEngineLib.lib").write_bytes(b"library")

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                MODULE.validate_package(package, stage, None)

            self.assertIn("staged/extracted SHA-256 match", output.getvalue())
            self.assertIn("headroom below 2 GiB", output.getvalue())

    def test_rejects_template_build_and_runtime_debris(self) -> None:
        for relative in (
            "build/CMakeCache.txt",
            "Logs/session.log",
            "game.pdb",
            "mmo_data.db",
            "spark_trace.json",
        ):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory(
                dir=Path.cwd()
            ) as temporary:
                package = self._package(Path(temporary))
                debris = package / "share" / "SparkEngine" / "templates" / relative
                debris.parent.mkdir(parents=True, exist_ok=True)
                debris.write_text("debris", encoding="utf-8")
                with self.assertRaisesRegex(MODULE.ValidationError, "debris"):
                    MODULE.validate_package(package, None, None)

    def test_rejects_known_source_tree_leaks_and_top_level_debris(self) -> None:
        forbidden_paths = (
            "tools/accept_lit.cfg",
            "tools/assetgen/generate.py",
            "bin/Assets/Textures/MMOFPS/fx/blob_shadow.png",
            "_CPack_Packages/staging/file.txt",
        )
        for relative in forbidden_paths:
            with self.subTest(relative=relative), tempfile.TemporaryDirectory(
                dir=Path.cwd()
            ) as temporary:
                package = self._package(Path(temporary))
                leaked = package / relative
                leaked.parent.mkdir(parents=True, exist_ok=True)
                leaked.write_text("leak", encoding="utf-8")
                with self.assertRaisesRegex(MODULE.ValidationError, "Forbidden package content"):
                    MODULE.validate_package(package, None, None)

        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            package = self._package(Path(temporary))
            (package / "tools" / "assetgen").mkdir(parents=True)
            with self.assertRaisesRegex(MODULE.ValidationError, "tools/assetgen/"):
                MODULE.validate_package(package, None, None)

    def test_rejects_link_like_package_entries(self) -> None:
        for entry_kind in ("file", "directory"):
            with self.subTest(entry_kind=entry_kind), tempfile.TemporaryDirectory(
                dir=Path.cwd()
            ) as temporary:
                package = self._package(Path(temporary))
                linked = package / "linked-assets"
                if entry_kind == "file":
                    linked.write_text("placeholder", encoding="utf-8")
                else:
                    linked.mkdir()
                original_is_link_like = MODULE._is_link_like

                def classify(path: Path) -> bool:
                    return path == linked or original_is_link_like(path)

                with mock.patch.object(MODULE, "_is_link_like", side_effect=classify):
                    with self.assertRaisesRegex(
                        MODULE.ValidationError, f"Link-like package {entry_kind}"
                    ):
                        MODULE.validate_package(package, None, None)

        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            package = self._package(Path(temporary))
            with mock.patch.object(MODULE, "_is_link_like", return_value=True):
                with self.assertRaisesRegex(MODULE.ValidationError, "package root"):
                    MODULE.validate_package(package, None, None)

    def test_detects_symlinks_and_windows_reparse_attributes(self) -> None:
        class ReparseStat:
            st_file_attributes = MODULE.WINDOWS_REPARSE_POINT

        reparse_path = mock.Mock(spec=Path)
        reparse_path.is_symlink.return_value = False
        reparse_path.is_junction.return_value = False
        reparse_path.lstat.return_value = ReparseStat()
        self.assertTrue(MODULE._is_link_like(reparse_path))

        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            target = root / "target.txt"
            target.write_text("target", encoding="utf-8")
            link = root / "link.txt"
            try:
                link.symlink_to(target)
            except OSError:
                pass
            else:
                self.assertTrue(MODULE._is_link_like(link))

    def test_two_gib_boundary_is_exclusive_and_reports_warning_headroom(self) -> None:
        path = Path("SparkEngineLib.lib")
        warning = MODULE._check_library_boundary(path, MODULE.LIBRARY_WARNING_BYTES)
        self.assertIn("WARNING", warning)
        self.assertIn("headroom", warning)
        with self.assertRaisesRegex(MODULE.ValidationError, "smaller than"):
            MODULE._check_library_boundary(path, MODULE.TWO_GIB)

    def test_rejects_static_library_hash_or_inventory_mismatch(self) -> None:
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            root = Path(temporary)
            package = self._package(root, b"archive")
            stage = root / "stage"
            (stage / "lib").mkdir(parents=True)
            (stage / "lib" / "SparkEngineLib.lib").write_bytes(b"stage")
            with self.assertRaisesRegex(MODULE.ValidationError, "SHA-256 mismatch"):
                MODULE.validate_package(package, stage, None)

        with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary:
            package = self._package(Path(temporary))
            stage = Path(temporary) / "stage"
            stage.mkdir()
            with self.assertRaisesRegex(MODULE.ValidationError, "no SparkEngine static library"):
                MODULE.validate_package(package, stage, None)


class ExtractedPackageWorkflowWiringTests(unittest.TestCase):
    def test_all_portable_platforms_run_the_full_extracted_gate(self) -> None:
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(workflow.count("validate-extracted-package.py"), 6)
        self.assertEqual(workflow.count("--preflight-archive"), 3)
        self.assertEqual(workflow.count("--stage-root"), 3)
        self.assertEqual(workflow.count("--archive"), 3)
        self.assertEqual(workflow.count("package-template-smoke-build"), 3)
        self.assertIn(
            'SPARK_TEMPLATE_ROOT="$packageRoot/share/SparkEngine/templates"', workflow
        )
        self.assertEqual(
            workflow.count(
                'SPARK_TEMPLATE_ROOT="$package_root/share/SparkEngine/templates"'
            ),
            2,
        )
        self.assertEqual(workflow.count("ValidateStagedPackageExecutables.cmake"), 6)
        self.assertEqual(workflow.count("SPARK_EXECUTABLE_SUFFIX:STRING=.exe"), 2)
        self.assertNotIn("SPARK_EXECUTABLE_SUFFIX=.exe", workflow)
        self.assertEqual(workflow.count("cmake -S Tests/PackageSmoke"), 6)
        self.assertEqual(workflow.count("VerifyInstalledTemplates.cmake"), 6)

        portable_steps = workflow.split(
            "    - name: Extract and smoke-test portable package\n"
        )[1:]
        self.assertEqual(len(portable_steps), 3)
        for step in portable_steps:
            extractor = "Expand-Archive" if "Expand-Archive" in step else "tar -xzf"
            self.assertLess(step.index("--preflight-archive"), step.index(extractor))
            self.assertLess(step.index(extractor), step.index("--package-root"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
