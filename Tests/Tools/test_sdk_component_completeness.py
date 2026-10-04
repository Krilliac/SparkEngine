"""Adversarial fixture tests for the SDK component completeness CMake script."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECK_SCRIPT = ROOT / "Tests" / "PackageSmoke" / "TestSparkSDKComponentCompleteness.cmake"

REQUIRED_FILES = (
    "include/Spark/SparkSDK.h",
    "include/Spark/Version.h",
    "include/Spark/GeneratedVersion.h",
    "include/Spark/IModule.h",
    "include/Spark/ModuleABI.h",
    "include/Spark/IWeatherService.h",
    "lib/cmake/SparkEngine/SparkEngineConfig.cmake",
    "lib/cmake/SparkEngine/SparkEngineConfigVersion.cmake",
    "lib/cmake/SparkEngine/SparkEngineTargets.cmake",
    "lib/cmake/SparkEngine/SparkGameModule.cmake",
    "share/SparkEngine/sdk/README.md",
    "share/SparkEngine/sdk/API-REFERENCE.md",
    "share/SparkEngine/sdk/MIGRATION.md",
    "share/SparkEngine/sdk/LICENSE.txt",
    "share/SparkEngine/sdk/THIRD_PARTY_NOTICES.txt",
    "share/SparkEngine/sdk/examples/EmptyProject/CMakeLists.txt",
)


class SdkComponentCompletenessTests(unittest.TestCase):
    def _write_fixture(self, root: Path, *, fail_runtime: bool = False) -> tuple[Path, Path]:
        source = root / "source"
        build = root / "build"
        for relative in REQUIRED_FILES:
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if relative.endswith("README.md"):
                path.write_text(
                    "## Public headers and ABI\n"
                    "## Package contents\n"
                    "## Compatibility diagnostics\n"
                    "SPARK_SDK_VERSION\n"
                    "there is no N-1 load or migration path\n",
                    encoding="utf-8",
                )
            elif relative.endswith("API-REFERENCE.md"):
                path.write_text(
                    "## Module lifecycle\nSparkModuleCompatibilityDescriptor\n",
                    encoding="utf-8",
                )
            elif relative.endswith("MIGRATION.md"):
                path.write_text(
                    "## Current version: SDK ABI v10\nThere is no N-1 module load\n",
                    encoding="utf-8",
                )
            else:
                path.write_text("fixture\n", encoding="utf-8")

        cmake = source / "CMakeLists.txt"
        runtime_code = (
            'message(FATAL_ERROR "fixture runtime install failed")'
            if fail_runtime
            else 'file(WRITE "${CMAKE_INSTALL_PREFIX}/runtime-installed" "runtime\\n")'
        )
        install_lines = [
            f'install(CODE [[{runtime_code}]] COMPONENT runtime)',
            'install(CODE [[\n'
            'file(WRITE "${CMAKE_INSTALL_PREFIX}/sdk-started" "sdk\\n")\n'
            'if(NOT EXISTS "${CMAKE_INSTALL_PREFIX}/runtime-installed")\n'
            '  message(FATAL_ERROR "SDK fixture requires runtime to be installed first")\n'
            'endif()\n'
            ']] COMPONENT sdk)',
        ]
        for relative in REQUIRED_FILES:
            install_lines.append(
                f'install(FILES "${{CMAKE_CURRENT_SOURCE_DIR}}/{relative}" OPTIONAL '
                f'DESTINATION "{Path(relative).parent.as_posix()}" COMPONENT sdk)'
            )
        cmake.write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(SdkFixture LANGUAGES NONE)\n"
            + "\n".join(install_lines)
            + "\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["cmake", "-S", str(source), "-B", str(build)],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
        return build, source

    def _run_check(self, build: Path, test_root: Path) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["TMP"] = str(test_root.parent)
        environment["TEMP"] = str(test_root.parent)
        return subprocess.run(
            [
                "cmake",
                f"-DSPARK_ENGINE_BUILD_DIR={build}",
                "-DSPARK_CONFIG=Release",
                f"-DSPARK_TEST_ROOT={test_root}",
                "-P",
                str(CHECK_SCRIPT),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=30,
        )

    def test_every_required_file_is_fail_closed(self) -> None:
        temp_parent = os.environ.get("TMP") or os.environ.get("TEMP")
        with tempfile.TemporaryDirectory(prefix="spark-sdk-completeness-", dir=temp_parent) as temporary:
            root = Path(temporary)
            build, source = self._write_fixture(root)
            valid = self._run_check(build, root / "valid")
            self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)

            for relative in REQUIRED_FILES:
                with self.subTest(missing_file=relative):
                    path = source / relative
                    contents = path.read_bytes()
                    path.unlink()
                    result = self._run_check(build, root / "missing" / Path(relative).name)
                    self.assertNotEqual(result.returncode, 0, relative)
                    output = result.stdout + result.stderr
                    self.assertIn("SDK component is missing required public-package files", output)
                    self.assertIn(relative, output)
                    path.write_bytes(contents)

            readme = source / "share" / "SparkEngine" / "sdk" / "README.md"
            original_readme = readme.read_text(encoding="utf-8")
            for token in (
                "## Public headers and ABI",
                "## Package contents",
                "## Compatibility diagnostics",
                "SPARK_SDK_VERSION",
                "there is no N-1 load or migration path",
            ):
                with self.subTest(missing_documentation=token):
                    readme.write_text(original_readme.replace(token, ""), encoding="utf-8")
                    result = self._run_check(build, root / "missing-doc" / Path(token.replace(" ", "_")))
                    self.assertNotEqual(result.returncode, 0, token)
                    output = result.stdout + result.stderr
                    self.assertIn("SDK documentation is missing required API/migration guidance", output)
                    self.assertIn(token, output)
            readme.write_text(original_readme, encoding="utf-8")

            for relative, token in (
                ("share/SparkEngine/sdk/API-REFERENCE.md", "## Module lifecycle"),
                ("share/SparkEngine/sdk/MIGRATION.md", "## Current version: SDK ABI v10"),
            ):
                with self.subTest(missing_documentation=relative):
                    path = source / relative
                    original = path.read_text(encoding="utf-8")
                    path.write_text(original.replace(token, ""), encoding="utf-8")
                    result = self._run_check(build, root / "missing-doc" / Path(relative).name)
                    self.assertNotEqual(result.returncode, 0, relative)
                    output = result.stdout + result.stderr
                    self.assertIn("SDK documentation is missing required API/migration guidance", output)
                    self.assertIn(token, output)
                    path.write_text(original, encoding="utf-8")

    def test_failed_runtime_dependency_stops_before_sdk_install(self) -> None:
        temp_parent = os.environ.get("TMP") or os.environ.get("TEMP")
        with tempfile.TemporaryDirectory(prefix="spark-sdk-dependency-", dir=temp_parent) as temporary:
            root = Path(temporary)
            build, _ = self._write_fixture(root, fail_runtime=True)
            test_root = root / "failed-runtime"
            result = self._run_check(build, test_root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Installing the runtime component failed", result.stdout + result.stderr)
            self.assertIn("fixture runtime install failed", result.stdout + result.stderr)
            self.assertFalse((test_root / "prefix/sdk-started").exists())


if __name__ == "__main__":
    unittest.main()
