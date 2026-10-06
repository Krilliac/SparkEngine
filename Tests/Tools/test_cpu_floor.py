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
            # LANGUAGES NONE has no compiler target identity. Give its synthetic
            # x86 test target an explicit architecture rather than rely on host env.
            + ('set(CMAKE_SYSTEM_PROCESSOR "x86_64")\n' if languages == "NONE" else "")
            +
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


# Two C sources in the probe directory; per-source properties are set on b.c.
SOURCES = """
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/a.c" "int a(void) { return 1; }\\n")
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/b.c" "int b(void) { return 2; }\\n")
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/c.c" "int c(void) { return 3; }\\n")
    """


@unittest.skipUnless(CMAKE, "cmake is required")
@unittest.skipUnless(X86_HOST, "the CPU floor applies to x86-64 hosts")
class CpuFloorPerSourceTests(unittest.TestCase):
    """spark_assert_cpu_floor() also reads per-source-file properties of every target source."""

    def configure(self, body: str, *extra: str) -> subprocess.CompletedProcess[str]:
        # A real C project: the passing cases must also generate.
        return configure_probe(SOURCES + textwrap.dedent(body), *extra, languages="C")

    def assert_rejected(self, result: subprocess.CompletedProcess[str], fragment: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        flat = " ".join(result.stderr.split())
        self.assertIn(FLOOR_ERROR, flat)
        self.assertIn(fragment, flat)
        self.assertNotIn("a.c", flat.split(FLOOR_ERROR, 1)[1].split("Remove the flag", 1)[0])

    def test_source_compile_options_above_floor_fail(self) -> None:
        result = self.configure(
            """
            add_library(probe STATIC a.c b.c)
            set_source_files_properties(b.c PROPERTIES COMPILE_OPTIONS "-O2;-mavx2")
            """
        )
        self.assert_rejected(result, "b.c COMPILE_OPTIONS: -mavx2")

    def test_source_compile_flags_above_floor_fail(self) -> None:
        result = self.configure(
            """
            add_library(probe STATIC a.c b.c)
            set_source_files_properties(b.c PROPERTIES COMPILE_FLAGS "/O2 /arch:AVX2")
            """
        )
        self.assert_rejected(result, "b.c COMPILE_FLAGS: /arch:AVX2")

    def test_source_jolt_selector_definition_fails(self) -> None:
        result = self.configure(
            """
            add_library(probe STATIC a.c b.c)
            set_source_files_properties(b.c PROPERTIES COMPILE_DEFINITIONS "JPH_USE_SSE4_2;JPH_USE_AVX2")
            """
        )
        self.assert_rejected(result, "b.c COMPILE_DEFINITIONS: JPH_USE_AVX2")

    def test_property_set_from_another_directory_is_caught(self) -> None:
        # The target lives in sub/; the root sets the property in the target's
        # directory scope, which is where the compiler reads it.
        result = self.configure(
            """
            file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/sub/b.c" "int b(void) { return 2; }\\n")
            file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/sub/CMakeLists.txt" "add_library(probe STATIC b.c)\\n")
            add_subdirectory(sub)
            set_source_files_properties(sub/b.c TARGET_DIRECTORY probe PROPERTIES COMPILE_OPTIONS -mfma)
            """
        )
        self.assert_rejected(result, "sub/b.c COMPILE_OPTIONS: -mfma")

    def test_floor_source_flags_and_object_entries_pass(self) -> None:
        result = self.configure(
            """
            add_library(other OBJECT c.c)
            add_library(probe STATIC a.c b.c $<TARGET_OBJECTS:other>)
            set_source_files_properties(b.c PROPERTIES COMPILE_OPTIONS "-msse4.2;-mpopcnt"
                COMPILE_DEFINITIONS JPH_USE_SSE4_2)
            """
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        # probe's a.c and b.c plus other's c.c; the $<TARGET_OBJECTS:other> entry is skipped.
        self.assertRegex(
            result.stdout, r"CPU floor: x86-64 SSE4\.2 verified for global flags, 2 targets\s+and 3 target sources"
        )

    def test_native_arch_stands_down_for_source_flags(self) -> None:
        body = """
            add_library(probe STATIC a.c b.c)
            set_source_files_properties(b.c PROPERTIES COMPILE_OPTIONS -march=native)
            """
        self.assert_rejected(self.configure(body), "b.c COMPILE_OPTIONS: -march=native")
        result = self.configure(body, "-DSPARK_NATIVE_ARCH=ON")
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
        # Upstream Jolt keys its MSVC ISA controls on the VS platform even with
        # Ninja. Give this control fixture the detected compiler target identity.
        if(MSVC AND "${{CMAKE_VS_PLATFORM_NAME}}" STREQUAL "")
            set(CMAKE_VS_PLATFORM_NAME "${{CMAKE_CXX_COMPILER_ARCHITECTURE_ID}}")
        endif()
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


@unittest.skipUnless(CMAKE and platform.system() == "Windows", "MSVC Windows probe is required")
class LibsodiumMsvcArchitectureProbeTests(unittest.TestCase):
    """The real libsodium wrapper must use MSVC's compiler architecture identity."""

    def test_empty_system_processor_x64_architecture_keeps_variants_disabled(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spark-sodium-arch-probe-") as temporary:
            root = Path(temporary)
            source = root / "src"
            build = root / "build"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(SparkSodiumArchitectureProbe LANGUAGES C)\n"
                'set(CMAKE_SYSTEM_PROCESSOR "")\n'
                f'include("{FLOOR_MODULE.as_posix()}")\n'
                'spark_cpu_floor_enforced(_enforced)\n'
                'if(NOT _enforced)\n  message(FATAL_ERROR "MSVC target CPU floor was bypassed")\nendif()\n'
                f'include("{(REPO_ROOT / "cmake" / "SparkLibsodium.cmake").as_posix()}")\n'
                'get_target_property(_options spark_sodium COMPILE_OPTIONS)\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/architecture-probe.txt"\n'
                '  "MSVC=${MSVC}\\nSYSTEM=${CMAKE_SYSTEM_PROCESSOR}\\nARCH=${CMAKE_C_COMPILER_ARCHITECTURE_ID}\\nOPTIONS=${_options}\\n")\n',
                encoding="utf-8",
            )
            result = _run([CMAKE, "-S", str(source), "-B", str(build), "-G", "Ninja"], root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            probe = (build / "architecture-probe.txt").read_text(encoding="utf-8")
            if "MSVC=1" not in probe:
                self.skipTest("CMake selected a non-MSVC compiler")
            self.assertIn("MSVC=1", probe)
            self.assertIn("\nSYSTEM=\n", probe)
            self.assertIn("ARCH=x64\n", probe)
            self.assertRegex(probe, r"OPTIONS=.*?/FI[^;\n]*spark_sodium_cpu_floor\.h")
            headers = list(build.rglob("spark_sodium_cpu_floor.h"))
            self.assertEqual(1, len(headers), headers)
            header = headers[0].read_text(encoding="utf-8")
            for feature in ("HAVE_AVXINTRIN_H", "HAVE_WMMINTRIN_H", "HAVE_AVX2INTRIN_H", "HAVE_AVX512FINTRIN_H"):
                self.assertIn("#undef " + feature, header)


if __name__ == "__main__":
    unittest.main()
