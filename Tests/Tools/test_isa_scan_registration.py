"""Pure CMake contract tests for the BLD-100 PE ISA scan registration."""

from __future__ import annotations

from pathlib import Path
import subprocess
import unittest
import uuid


REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE_SCRIPT = REPO_ROOT / "cmake" / "SparkIsaBaseline.cmake"


def _cmake_project(
    call: str, build_tests: bool, native_arch: bool = False
) -> tuple[subprocess.CompletedProcess[str], str]:
    review_root = REPO_ROOT / "build" / "isa-review"
    review_root.mkdir(parents=True, exist_ok=True)
    fixture = review_root / f"case-{uuid.uuid4().hex}"
    fixture.mkdir()
    capture = fixture / "capture.txt"
    cmake_lists = f"""cmake_minimum_required(VERSION 3.25)
project(IsaRegistration LANGUAGES NONE)
set(WIN32 TRUE)
set(MSVC TRUE)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(SPARK_NATIVE_ARCH {"ON" if native_arch else "OFF"})
set(BUILD_TESTS {"ON" if build_tests else "OFF"})
set(SPARK_CAPTURE_FILE "{capture.as_posix()}")

# These are real CMake targets, so the function's TARGET guard is exercised.
add_custom_target(SparkEngine)
add_custom_target(SparkSecond)
add_custom_target(SparkStatic)
add_custom_target(SparkImported)

function(get_target_property output target property)
    if(target STREQUAL "SparkEngine" AND property STREQUAL "TYPE")
        set(value EXECUTABLE)
    elseif(target STREQUAL "SparkSecond" AND property STREQUAL "TYPE")
        set(value EXECUTABLE)
    elseif(target STREQUAL "SparkStatic" AND property STREQUAL "TYPE")
        set(value STATIC_LIBRARY)
    elseif(target STREQUAL "SparkImported" AND property STREQUAL "TYPE")
        set(value SHARED_LIBRARY)
    elseif(target STREQUAL "SparkImported" AND property STREQUAL "IMPORTED")
        set(value TRUE)
    else()
        set(value "${{target}}-${{property}}-NOTFOUND")
    endif()
    set("${{output}}" "${{value}}" PARENT_SCOPE)
endfunction()

function(add_custom_target name)
    file(WRITE "${{SPARK_CAPTURE_FILE}}" "CUSTOM=${{name}}\\n")
    foreach(argument IN LISTS ARGN)
        file(APPEND "${{SPARK_CAPTURE_FILE}}" "ARG=${{argument}}\\n")
    endforeach()
endfunction()

function(add_test)
    file(APPEND "${{SPARK_CAPTURE_FILE}}" "TEST=${{ARGV}}\\n")
endfunction()

function(set_tests_properties)
    file(APPEND "${{SPARK_CAPTURE_FILE}}" "PROPERTIES=${{ARGV}}\\n")
endfunction()

include("{CMAKE_SCRIPT.as_posix()}")
{call}
"""
    (fixture / "CMakeLists.txt").write_text(cmake_lists, encoding="utf-8")
    result = subprocess.run(
        ["cmake", "-S", str(fixture), "-B", str(fixture / "out")],
        text=True,
        capture_output=True,
        timeout=120,
        check=False,
    )
    return result, capture.read_text(encoding="utf-8") if capture.exists() else ""


class IsaScanRegistrationTests(unittest.TestCase):
    def test_shipping_mode_registers_custom_scan_with_paired_pdbs(self) -> None:
        result, capture = _cmake_project(
            "spark_register_isa_baseline_scan(SparkEngine SparkSecond SparkStatic SparkImported Missing)",
            build_tests=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CUSTOM=CpuFloor_IsaBaseline", capture)
        self.assertIn("ARG=--objdump", capture)
        self.assertIn("ARG=llvm-objdump", capture)
        self.assertIn("ARG=--pdbutil", capture)
        self.assertIn("ARG=llvm-pdbutil", capture)
        self.assertIn("$<TARGET_FILE:SparkEngine>=$<TARGET_PDB_FILE:SparkEngine>", capture)
        self.assertIn("$<TARGET_FILE:SparkSecond>=$<TARGET_PDB_FILE:SparkSecond>", capture)
        self.assertNotIn("SparkStatic>", capture)
        self.assertNotIn("SparkImported>", capture)
        self.assertNotIn("TEST=", capture)

    def test_tests_mode_registers_same_scan_as_ctest(self) -> None:
        result, capture = _cmake_project("spark_register_isa_baseline_scan(SparkEngine)", build_tests=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("CUSTOM=CpuFloor_IsaBaseline", capture)
        self.assertIn("TEST=NAME;CpuFloor_IsaBaseline", capture)
        self.assertIn("TIMEOUT;600", capture)

    def test_no_configured_image_is_a_configure_error(self) -> None:
        result, capture = _cmake_project("spark_register_isa_baseline_scan(Missing)", build_tests=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no configured shipped PE image targets", result.stdout + result.stderr)
        self.assertEqual(capture, "")

    def test_native_arch_disables_distribution_floor_scan(self) -> None:
        result, capture = _cmake_project("spark_register_isa_baseline_scan(SparkEngine)", build_tests=False,
                                         native_arch=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(capture, "")


if __name__ == "__main__":
    unittest.main()
