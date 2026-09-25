#!/usr/bin/env python3
"""BLD-100 / OD-04 stable-v1 CPU floor contract.

The stable-v1 CPU floor is x86-64 with SSE4.2 (x86-64-v2). These tests drive
cmake/SparkCpuFloor.cmake through real CMake runs:

* the flag classifier rejects AVX/AVX2/AVX-512/FMA/F16C/LZCNT/BMI selections,
  non-floor -march values, MSVC /arch:AVX* and Jolt's JPH_USE_* selectors, and
  accepts the SSE4.2/POPCNT floor;
* spark_assert_cpu_floor() fails configuration when any target carries a flag
  above the floor, passes for floor-only flags, and stands down only for the
  host-tuned SPARK_NATIVE_ARCH=ON escape hatch;
* the vendored Jolt build configured through spark_configure_jolt_cpu_floor()
  publishes no above-floor flags, while Jolt's upstream defaults are rejected;
* the root CMakeLists.txt pins Jolt before add_subdirectory() and asserts the
  floor after every subdirectory, and both Shipping presets keep
  SPARK_NATIVE_ARCH OFF.

These are configure-level checks. They do not execute a Shipping binary on an
SSE4.2-only CPU; that evidence needs hardware or an emulator run.
"""

from __future__ import annotations

import json
import platform
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
FLOOR_MODULE = REPO_ROOT / "cmake" / "SparkCpuFloor.cmake"
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"
PRESETS = REPO_ROOT / "CMakePresets.json"
JOLT_BUILD_DIR = REPO_ROOT / "ThirdParty" / "Physics" / "JoltPhysics" / "Build"

CMAKE = shutil.which("cmake")
X86_HOST = platform.machine().lower() in {"x86_64", "amd64"}
FLOOR_ERROR = "BLD-100/OD-04"


def _cmake_path(path: Path) -> str:
    return path.as_posix()


def _run(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True, check=False)


@unittest.skipUnless(CMAKE, "cmake is required")
class CpuFloorClassifierTests(unittest.TestCase):
    def classify(self, *entries: str) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "classify.cmake"
            quoted = " ".join(f'"{entry}"' for entry in entries)
            script.write_text(
                f'include("{_cmake_path(FLOOR_MODULE)}")\n'
                f"spark_cpu_floor_violations(result {quoted})\n"
                'foreach(item IN LISTS result)\n  message("VIOLATION=${item}")\nendforeach()\n',
                encoding="utf-8",
            )
            result = _run([CMAKE, "-P", str(script)], Path(tmp))
            self.assertEqual(result.returncode, 0, result.stderr)
            return [line.removeprefix("VIOLATION=") for line in result.stderr.splitlines() if line.startswith("VIOLATION=")]

    def test_above_floor_selections_are_rejected(self) -> None:
        above = [
            "-mavx",
            "-mavx2",
            "-mavx512f",
            "-mfma",
            "-mf16c",
            "-mlzcnt",
            "-mbmi",
            "-mbmi2",
            "-mmovbe",
            "-maes",
            "-mpclmul",
            "-msha",
            "-mgfni",
            "-mrdrnd",
            "-mrdseed",
            "-madx",
            "-mvaes",
            "-mvpclmulqdq",
            "$<$<CONFIG:Release>:-maes>",
            "-march=native",
            "-march=haswell",
            "-march=x86-64-v3",
            "/arch:AVX",
            "/arch:AVX2",
            "-arch:AVX512",
            "$<$<NOT:$<CONFIG:Debug>>:-mavx2>",
            "JPH_USE_AVX2",
            "JPH_USE_AVX",
            "JPH_USE_LZCNT",
            "JPH_USE_TZCNT",
            "JPH_USE_F16C",
            "JPH_USE_FMADD",
        ]
        self.assertEqual(self.classify(*above), above)

    def test_floor_selections_are_accepted(self) -> None:
        within = [
            "-msse4.2",
            "-msse4.1",
            "-mpopcnt",
            "-mfpmath=sse",
            "-mtune=generic",
            "-mshstk",
            "-msahf",
            "-mcx16",
            "-march=x86-64",
            "-march=x86-64-v2",
            "$<$<NOT:$<CONFIG:Debug>>:-msse4.2>",
            "JPH_USE_SSE4_1",
            "JPH_USE_SSE4_2",
            "/W3",
        ]
        self.assertEqual(self.classify(*within), [])


def configure_probe(body: str, *extra: str, languages: str = "NONE") -> subprocess.CompletedProcess[str]:
    """Configure a throwaway project that includes the floor module and asserts it last."""
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "src"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            f"project(CpuFloorProbe LANGUAGES {languages})\n"
            f'include("{_cmake_path(FLOOR_MODULE)}")\n' + textwrap.dedent(body) + "spark_assert_cpu_floor()\n",
            encoding="utf-8",
        )
        return _run([CMAKE, "-S", str(source), "-B", str(Path(tmp) / "build"), *extra], Path(tmp))


@unittest.skipUnless(CMAKE, "cmake is required")
@unittest.skipUnless(X86_HOST, "the CPU floor applies to x86-64 hosts")
class CpuFloorAssertionTests(unittest.TestCase):
    def configure(self, body: str, *extra: str) -> subprocess.CompletedProcess[str]:
        return configure_probe(body, *extra)

    def test_target_above_floor_fails_configuration(self) -> None:
        result = self.configure(
            """
            add_library(probe INTERFACE)
            target_compile_options(probe INTERFACE $<$<CONFIG:Release>:-mavx2>)
            """
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(FLOOR_ERROR, result.stderr)
        self.assertIn("probe INTERFACE_COMPILE_OPTIONS", result.stderr)

    def test_global_flag_above_floor_fails_configuration(self) -> None:
        result = self.configure("", "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -mfma")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CMAKE_CXX_FLAGS_RELEASE: -mfma", result.stderr)

    def test_jolt_selector_definition_fails_configuration(self) -> None:
        result = self.configure(
            """
            add_library(probe INTERFACE)
            target_compile_definitions(probe INTERFACE JPH_USE_SSE4_2 JPH_USE_F16C)
            """
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("probe INTERFACE_COMPILE_DEFINITIONS: JPH_USE_F16C", result.stderr)

    def test_floor_flags_pass_and_native_arch_stands_down(self) -> None:
        within = """
            add_library(probe INTERFACE)
            target_compile_options(probe INTERFACE -msse4.2 -mpopcnt)
            target_compile_definitions(probe INTERFACE JPH_USE_SSE4_1 JPH_USE_SSE4_2)
            """
        result = self.configure(within)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("CPU floor: x86-64 SSE4.2 verified", result.stdout)

        above = """
            add_library(probe INTERFACE)
            target_compile_options(probe INTERFACE -march=native)
            """
        self.assertNotEqual(self.configure(above).returncode, 0)
        result = self.configure(above, "-DSPARK_NATIVE_ARCH=ON")
        self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(CMAKE, "cmake is required")
@unittest.skipUnless(X86_HOST, "the CPU floor applies to x86-64 hosts")
@unittest.skipUnless((JOLT_BUILD_DIR / "CMakeLists.txt").is_file(), "vendored Jolt is not checked out")
class VendoredJoltFloorTests(unittest.TestCase):
    JOLT_SETUP = """
        set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
        set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
        set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
        set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
        set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
        set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
        set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
        {floor}
        add_subdirectory("{jolt}" "${{CMAKE_BINARY_DIR}}/Jolt")
        get_target_property(_opts Jolt INTERFACE_COMPILE_OPTIONS)
        get_target_property(_defs Jolt INTERFACE_COMPILE_DEFINITIONS)
        message(STATUS "JOLT_OPTIONS=${{_opts}}")
        message(STATUS "JOLT_DEFINITIONS=${{_defs}}")
        """

    def configure_jolt(self, apply_floor: bool) -> subprocess.CompletedProcess[str]:
        body = self.JOLT_SETUP.format(
            floor="spark_configure_jolt_cpu_floor()" if apply_floor else "",
            jolt=_cmake_path(JOLT_BUILD_DIR),
        )
        return configure_probe(body, languages="C CXX")

    def test_jolt_upstream_defaults_are_rejected(self) -> None:
        result = self.configure_jolt(apply_floor=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(FLOOR_ERROR, result.stderr)
        self.assertIn("Jolt INTERFACE_COMPILE_DEFINITIONS: JPH_USE_AVX2", result.stderr)

    def test_jolt_configured_through_floor_stays_within_floor(self) -> None:
        result = self.configure_jolt(apply_floor=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        definitions = re.search(r"JOLT_DEFINITIONS=(.*)", result.stdout)
        self.assertIsNotNone(definitions)
        assert definitions is not None
        self.assertIn("JPH_USE_SSE4_2", definitions.group(1))
        self.assertNotRegex(definitions.group(1), r"JPH_USE_(AVX|LZCNT|TZCNT|F16C|FMADD)")
        options = re.search(r"JOLT_OPTIONS=(.*)", result.stdout)
        assert options is not None
        self.assertNotRegex(options.group(1), r"-m(avx|fma|f16c|lzcnt|bmi)|/arch:AVX")


class RootWiringTests(unittest.TestCase):
    def test_root_pins_jolt_before_add_subdirectory_and_asserts_last(self) -> None:
        text = ROOT_CMAKE.read_text(encoding="utf-8")
        configure = text.find("spark_configure_jolt_cpu_floor()")
        jolt_subdirectory = text.find('add_subdirectory("${JOLT_PHYSICS_SOURCE_DIR}/Build"')
        self.assertGreater(configure, 0, "root CMakeLists.txt must pin Jolt to the CPU floor")
        self.assertGreater(jolt_subdirectory, configure, "Jolt must be pinned before add_subdirectory()")

        assertion = text.rfind("spark_assert_cpu_floor()")
        self.assertGreater(assertion, 0, "root CMakeLists.txt must assert the CPU floor")
        last_subdirectory = max(match.start() for match in re.finditer(r"^\s*add_subdirectory\(", text, re.M))
        self.assertGreater(assertion, last_subdirectory, "the floor must be asserted after every subdirectory")

    def test_shipping_presets_keep_native_arch_off(self) -> None:
        presets = {preset["name"]: preset for preset in json.loads(PRESETS.read_text(encoding="utf-8"))["configurePresets"]}
        for name in ("windows-shipping", "linux-shipping"):
            with self.subTest(preset=name):
                self.assertIn(name, presets)
                self.assertEqual(presets[name].get("cacheVariables", {}).get("SPARK_NATIVE_ARCH"), "OFF")


if __name__ == "__main__":
    unittest.main()
