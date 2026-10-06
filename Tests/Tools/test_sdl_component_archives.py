#!/usr/bin/env python3
"""Exercise production SDL2 component rules with real CMake and CPack."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile


REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform in ("linux", "darwin"), "POSIX shared-library component rules")
class SDLComponentArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = shutil.which("cmake")
        cls.cpack = shutil.which("cpack")
        cls.compiler = shutil.which("cc")
        cls.generator = "Ninja" if shutil.which("ninja") else "Unix Makefiles"
        if not all((cls.cmake, cls.cpack, cls.compiler)) or (
            cls.generator == "Unix Makefiles" and not shutil.which("make")
        ):
            raise unittest.SkipTest("CMake, CPack, a C compiler and a build tool are required")
        cls.fixture = tempfile.TemporaryDirectory(prefix="spark-sdl-component-")
        cls.addClassCleanup(cls.fixture.cleanup)
        cls.root = Path(cls.fixture.name).resolve()
        cls.source = cls.root / "source"
        cls.build = cls.root / "build"
        cls.source.mkdir()

        # Use the actual root-owned install rules and RPATH policy. The small
        # shared library replaces only SDL's compilation, not package logic.
        production = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        start = production.index("if(APPLE)\n    set(CMAKE_MACOSX_RPATH ON)")
        rpath = production[start:production.index("\nendif()", start) + len("\nendif()")]
        start = production.index('if(TARGET SDL2 AND EXISTS "${SDL2_SUBMODULE_DIR}/include")')
        start = production.index("\n", start) + 1
        end = production.index("    install(FILES ${_SDL2_PUBLIC_HEADERS}", start)
        install_rules = production[start:end]
        (cls.source / "sdl.c").write_text("int spark_sdl_fixture(void) { return 0; }\n", encoding="utf-8")
        (cls.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(SparkSDLComponentFixture C)\n"
            + rpath + "\n"
            "set(SPARK_INSTALL_COMPONENT_RUNTIME runtime)\n"
            "set(SPARK_INSTALL_COMPONENT_SDK sdk)\n"
            "add_library(SDL2 SHARED sdl.c)\n"
            "set_target_properties(SDL2 PROPERTIES VERSION 1.2 SOVERSION 1)\n"
            + install_rules + "\n"
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/names.txt" CONTENT '
            '"$<TARGET_FILE_NAME:SDL2>\\n$<TARGET_SONAME_FILE_NAME:SDL2>\\n'
            '$<TARGET_LINKER_FILE_NAME:SDL2>\\n")\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/image.txt" CONTENT "$<TARGET_FILE:SDL2>")\n'
            "set(CPACK_PACKAGE_NAME SparkSDLComponentFixture)\n"
            "set(CPACK_PACKAGE_VERSION 1.0.0)\n"
            "set(CPACK_COMPONENTS_ALL runtime sdk)\n"
            "set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)\n"
            "set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)\n"
            f'set(CPACK_PROJECT_CONFIG_FILE "{(REPO / "cmake/SparkCPackOptions.cmake").as_posix()}")\n'
            "include(CPack)\n",
            encoding="utf-8",
        )
        cls.checked(cls.cmake, "-S", cls.source, "-B", cls.build, "-G", cls.generator)
        cls.checked(cls.cmake, "--build", cls.build)
        cls.real_name, cls.soname, cls.linker_name = (cls.build / "names.txt").read_text().splitlines()
        cls.image = Path((cls.build / "image.txt").read_text())

    @classmethod
    def checked(cls, *arguments: object) -> subprocess.CompletedProcess:
        result = subprocess.run([str(argument) for argument in arguments], capture_output=True,
                                text=True, check=False, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="case-", dir=self.root))
        self.addCleanup(shutil.rmtree, self.work)

    def install(self, prefix: Path, component: str) -> None:
        self.checked(self.cmake, "--install", self.build, "--prefix", prefix, "--component", component)

    def consumer(self, prefix: Path) -> subprocess.CompletedProcess:
        source = self.work / "consumer"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\nproject(SDLConsumer NONE)\n"
            f'include("{(prefix / "lib/cmake/SDL2/SDL2Targets.cmake").as_posix()}")\n',
            encoding="utf-8",
        )
        return subprocess.run([self.cmake, "-S", str(source), "-B", str(self.work / "consumer-build")],
                              capture_output=True, text=True, check=False, timeout=30)

    def assert_regular_payload(self, prefix: Path) -> None:
        expected = self.image.read_bytes()
        for name in (self.real_name, self.soname, self.linker_name):
            with self.subTest(library=name):
                file = prefix / "lib" / name
                self.assertTrue(file.is_file())
                self.assertFalse(file.is_symlink())
                self.assertEqual(expected, file.read_bytes())

    def test_runtime_then_sdk_preserves_names_bytes_and_exported_target(self) -> None:
        prefix = self.work / "prefix"
        self.install(prefix, "runtime")
        self.install(prefix, "sdk")
        self.assert_regular_payload(prefix)
        result = self.consumer(prefix)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_real_cpack_archives_pass_unchanged_production_preflight(self) -> None:
        for generator, pattern in (("TGZ", "*.tar.gz"), ("ZIP", "*.zip")):
            with self.subTest(generator=generator):
                output = self.work / generator
                self.checked(self.cpack, "--config", self.build / "CPackConfig.cmake",
                             "-G", generator, "-B", output)
                archives = list(output.glob(pattern))
                self.assertEqual(1, len(archives))
                archive = archives[0]
                self.checked(sys.executable, "-B", REPO / ".github/scripts/validate-extracted-package.py",
                             "--preflight-archive", archive)
                extracted = output / "extracted"
                if generator == "TGZ":
                    with tarfile.open(archive) as payload:
                        payload.extractall(extracted, filter="data")
                else:
                    with zipfile.ZipFile(archive) as payload:
                        payload.extractall(extracted)
                roots = list(extracted.iterdir())
                self.assertEqual(1, len(roots))
                self.assert_regular_payload(roots[0])

    def test_sdk_alone_does_not_satisfy_declared_runtime_dependency(self) -> None:
        prefix = self.work / "prefix"
        self.install(prefix, "sdk")
        self.assertFalse((prefix / "lib" / self.real_name).exists())
        result = self.consumer(prefix)
        self.assertNotEqual(0, result.returncode)
        self.assertIn(self.real_name, result.stdout + result.stderr)
        self.assertIn("does not exist", result.stdout + result.stderr)

    def test_sdk_replaces_a_legacy_in_prefix_developer_link(self) -> None:
        prefix = self.work / "prefix"
        self.install(prefix, "runtime")
        real = prefix / "lib" / self.real_name
        alias = prefix / "lib" / self.linker_name
        alias.symlink_to(real.name)
        # CMake's plain file install can retain an up-to-date symlink.
        future = self.image.stat().st_mtime_ns + 10_000_000_000
        os.utime(real, ns=(future, future))
        self.install(prefix, "sdk")
        self.assert_regular_payload(prefix)

    def test_sdk_rejects_a_legacy_developer_link_outside_library_directory(self) -> None:
        prefix = self.work / "prefix"
        library = prefix / "lib"
        library.mkdir(parents=True)
        sentinel = self.work / "outside-library"
        sentinel.write_bytes(b"preserve outside bytes")
        alias = library / self.linker_name
        alias.symlink_to(sentinel)
        result = subprocess.run([self.cmake, "--install", str(self.build), "--prefix", str(prefix),
                                 "--component", "sdk"], capture_output=True, text=True,
                                check=False, timeout=30)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("SDL2 install link escapes its library directory", result.stdout + result.stderr)
        self.assertTrue(alias.is_symlink())
        self.assertEqual(b"preserve outside bytes", sentinel.read_bytes())


if __name__ == "__main__":
    unittest.main()
