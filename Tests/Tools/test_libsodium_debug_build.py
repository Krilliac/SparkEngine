#!/usr/bin/env python3
"""Build the real non-MSVC crypto component and exercise its password contract."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake")
COMPILER_ARGUMENTS: list[str] = []


def run(arguments: list[str]) -> str:
    result = subprocess.run(arguments, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=150)
    if result.returncode:
        raise AssertionError(f"Command failed ({result.returncode}): {shlex.join(arguments)}\n{result.stdout[-12000:]}")
    return result.stdout


@unittest.skipIf(os.name == "nt", "The optimization fix applies to the non-MSVC component")
class LibsodiumDebugBuildTests(unittest.TestCase):
    def test_debug_dependency_flags_and_existing_password_contract(self) -> None:
        self.assertIsNotNone(CMAKE, "CMake is required for this build contract")
        with tempfile.TemporaryDirectory(prefix="spark-sodium-debug-") as temporary:
            source = Path(temporary)
            source.joinpath("main.cpp").write_text(
                '#include "TestFramework.h"\n'
                'int g_assertionsPassed=0, g_assertionsFailed=0, g_assertionsWaived=0, '
                'g_assertionsNoCrash=0, g_testsWarned=0;\n'
                'std::string g_currentTest;\n'
                'int main() { for(auto* test : GetTestRegistry()) { g_currentTest=test->name; '
                'try { test->func(); } catch (...) { ++g_assertionsFailed; } '
                'std::cout << test->name << "\\n"; } '
                'std::cout << "passed=" << g_assertionsPassed << " failed=" << g_assertionsFailed << "\\n"; '
                'return g_assertionsFailed != 0 || g_assertionsPassed == 0; }\n',
                encoding="utf-8",
            )
            root = ROOT.as_posix()
            source.joinpath("CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(SparkSodiumDebugContract LANGUAGES C CXX)\n'
                'set(CMAKE_CXX_STANDARD 23)\n'
                'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n'
                f'set(SPARK_CONTRACT_ROOT [==[{root}]==])\n'
                'include("${SPARK_CONTRACT_ROOT}/cmake/SparkLibsodium.cmake")\n'
                'add_executable(password_contract main.cpp\n'
                ' "${SPARK_CONTRACT_ROOT}/Tests/TestPasswordHash.cpp"\n'
                ' "${SPARK_CONTRACT_ROOT}/SparkEngine/Source/Utils/PasswordHash.cpp"\n'
                ' "${SPARK_CONTRACT_ROOT}/SparkEngine/Source/Utils/SecureRandom.cpp")\n'
                'target_compile_definitions(password_contract PRIVATE SPARK_HAS_LIBSODIUM=1)\n'
                'target_include_directories(password_contract PRIVATE\n'
                ' "${SPARK_CONTRACT_ROOT}/Tests" "${SPARK_CONTRACT_ROOT}/SparkEngine/Source")\n'
                'target_link_libraries(password_contract PRIVATE spark_sodium)\n',
                encoding="utf-8",
            )
            generator = "Ninja" if shutil.which("ninja") else "Unix Makefiles"
            for configuration in ("Debug", "Release"):
                build = source / configuration
                run([CMAKE, "-S", str(source), "-B", str(build), "-G", generator,
                     f"-DCMAKE_BUILD_TYPE={configuration}", *COMPILER_ARGUMENTS])
                commands = json.loads(build.joinpath("compile_commands.json").read_text(encoding="utf-8"))
                sodium_commands = [entry for entry in commands if "/ThirdParty/Security/libsodium/" in entry["file"]]
                self.assertGreater(len(sodium_commands), 100, "The fixture must configure the actual vendored target")
                for entry in sodium_commands:
                    flags = entry.get("arguments") or shlex.split(entry["command"])
                    with self.subTest(configuration=configuration, source=entry["file"]):
                        for flag in ("-fno-strict-aliasing", "-fno-strict-overflow", "-fwrapv"):
                            self.assertIn(flag, flags)
                        self.assertIn("-DHAVE_INLINE_ASM=1", flags)
                        self.assertTrue(any(f in flags for f in
                                            ("-DHAVE_C11_MEMORY_FENCES=1", "-DHAVE_GCC_MEMORY_FENCES=1")))
                        self.assertFalse(any(f in flags for f in
                                             ("-DHAVE_AVXINTRIN_H=1", "-DHAVE_AVX2INTRIN_H=1",
                                              "-DHAVE_AVX512FINTRIN_H=1", "-DHAVE_WMMINTRIN_H=1")))
                        optimizations = [flag for flag in flags if flag.startswith("-O")]
                        if configuration == "Debug":
                            self.assertTrue(any(flag.startswith("-g") for flag in flags))
                            self.assertTrue(optimizations, "Debug crypto must have an explicit optimization level")
                            self.assertEqual(optimizations[-1], "-O2")
                            self.assertGreater(flags.index("-O2"), flags.index("-flax-vector-conversions"))
                        else:
                            # The Debug-only override must leave the normal Release flags intact.
                            self.assertNotIn("-O2", flags[flags.index("-flax-vector-conversions") + 1:])
                if configuration == "Debug":
                    for entry in commands:
                        if entry["file"].endswith("/Utils/PasswordHash.cpp"):
                            flags = entry.get("arguments") or shlex.split(entry["command"])
                            self.assertFalse(any(f in flags for f in ("-O1", "-O2", "-O3", "-Og")),
                                             "Only the dependency should acquire optimization")
                    run([CMAKE, "--build", str(build), "--target", "password_contract", "--parallel", "2"])
                    output = run([str(build / "password_contract")])
                    for name in ("CreatesSelfDescribingUniqueHashes", "RejectsLegacyMalformedAndUnboundedWorkFactors",
                                 "MatchesPublishedPBKDF2Sha256Construction", "PreservesPreMigrationHmacBytes"):
                        self.assertIn("SparkPasswordHash_" + name, output)
                    result = re.search(r"^passed=(\d+) failed=0$", output, re.MULTILINE)
                    self.assertIsNotNone(result)
                    self.assertGreaterEqual(int(result.group(1)), 12)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default=CMAKE)
    parser.add_argument("--c-compiler")
    parser.add_argument("--cxx-compiler")
    arguments, remaining = parser.parse_known_args()
    CMAKE = arguments.cmake
    if arguments.c_compiler:
        COMPILER_ARGUMENTS.append(f"-DCMAKE_C_COMPILER={arguments.c_compiler}")
    if arguments.cxx_compiler:
        COMPILER_ARGUMENTS.append(f"-DCMAKE_CXX_COMPILER={arguments.cxx_compiler}")
    unittest.main(argv=[__file__, *remaining])
