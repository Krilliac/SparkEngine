"""Configure-only contracts for the production C++ runtime export helper.

LANGUAGES NONE deliberately performs no compiler probes or native builds.
These tests verify export/import properties, not driver acceptance or ABI loads.
"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "cmake" / "SparkCxxRuntime.cmake"


class SdkCxxRuntimeTests(unittest.TestCase):
    def configure(self, source, build):
        command = [os.environ.get("SPARK_CMAKE_COMMAND", "cmake")]
        if shutil.which("ninja"):
            command += ["-G", "Ninja"]
        return subprocess.run(
            command + ["-S", str(source), "-B", str(build)],
            capture_output=True, text=True, timeout=60,
        )

    def round_trip(self, runtime, windows=False):
        with tempfile.TemporaryDirectory(prefix="spark-sdk-runtime-") as directory:
            root = Path(directory)
            producer = root / "producer"
            consumer = root / "consumer"
            producer.mkdir()
            consumer.mkdir()
            export = root / "producer-build" / "SparkRuntimeTargets.cmake"
            # Interface stand-ins exercise the same application/export seam
            # without introducing an actual archive build or compiler lookup.
            (producer / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(RuntimeProducer LANGUAGES NONE)\n'
                f'set(WIN32 {"TRUE" if windows else "FALSE"})\n'
                f'include("{HELPER.as_posix()}")\n'
                'add_library(EngineLib INTERFACE)\n'
                'add_library(EngineInterface INTERFACE)\n'
                f'spark_apply_cxx_runtime(EngineLib INTERFACE "{runtime}")\n'
                f'spark_apply_cxx_runtime(EngineInterface INTERFACE "{runtime}")\n'
                f'export(TARGETS EngineLib EngineInterface NAMESPACE Spark:: FILE "{export.as_posix()}")\n',
                encoding="utf-8",
            )
            result = self.configure(producer, root / "producer-build")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            snapshot = root / "consumer-build" / "properties.txt"
            (consumer / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(RuntimeConsumer LANGUAGES NONE)\n'
                f'include("{export.as_posix()}")\n'
                f'file(WRITE "{snapshot.as_posix()}" "")\n'
                'foreach(target EngineLib EngineInterface)\n'
                '  foreach(property INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS INTERFACE_LINK_LIBRARIES)\n'
                '    get_target_property(value Spark::${target} ${property})\n'
                f'    file(APPEND "{snapshot.as_posix()}" "${{target}}.${{property}}=${{value}}\\n")\n'
                '  endforeach()\n'
                'endforeach()\n',
                encoding="utf-8",
            )
            result = self.configure(consumer, root / "consumer-build")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            properties = dict(line.split("=", 1) for line in snapshot.read_text().splitlines())
            for target in ("EngineLib", "EngineInterface"):
                self.assertEqual(properties[f"{target}.INTERFACE_LINK_LIBRARIES"], "value-NOTFOUND")
            self.assertNotIn(str(producer), export.read_text())
            return properties

    def test_libcxx_compile_and_link_requirements_survive_export(self):
        properties = self.round_trip("libc++")
        for target in ("EngineLib", "EngineInterface"):
            self.assertEqual(properties[f"{target}.INTERFACE_COMPILE_OPTIONS"],
                             "$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
            self.assertEqual(properties[f"{target}.INTERFACE_LINK_OPTIONS"],
                             "$<$<LINK_LANGUAGE:CXX>:-stdlib=libc++>")

    def test_libstdcxx_selection_is_only_needed_for_clang(self):
        properties = self.round_trip("libstdc++")
        for target in ("EngineLib", "EngineInterface"):
            self.assertEqual(properties[f"{target}.INTERFACE_COMPILE_OPTIONS"],
                             "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-stdlib=libstdc++>")
            self.assertEqual(properties[f"{target}.INTERFACE_LINK_OPTIONS"],
                             "$<$<LINK_LANG_AND_ID:CXX,Clang,AppleClang>:-stdlib=libstdc++>")

    def test_windows_contract_adds_no_runtime_options(self):
        properties = self.round_trip("", windows=True)
        for target in ("EngineLib", "EngineInterface"):
            self.assertEqual(properties[f"{target}.INTERFACE_COMPILE_OPTIONS"], "value-NOTFOUND")
            self.assertEqual(properties[f"{target}.INTERFACE_LINK_OPTIONS"], "value-NOTFOUND")

    def test_unsupported_runtime_fails_instead_of_exporting_arbitrary_flags(self):
        self.reject_application('INTERFACE "-fsanitize=address"', "Unsupported Spark C++ runtime")

    def test_private_requirement_is_rejected(self):
        self.reject_application('PRIVATE "libc++"', "must be PUBLIC or INTERFACE")

    def detect(self, setup, selection, expected=None, diagnostic=None):
        # Replace only the header-check module. Exercise the production loop,
        # state isolation and diagnostics without enabling any compiler.
        with tempfile.TemporaryDirectory(prefix="spark-sdk-detection-") as directory:
            root = Path(directory)
            (root / "CheckCXXSourceCompiles.cmake").write_text(
                'macro(check_cxx_source_compiles source result)\n'
                '  if(DEFINED ${result})\n'
                '    message(FATAL_ERROR "stale probe result")\n'
                '  endif()\n'
                '  if(NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")\n'
                '    message(FATAL_ERROR "probe must be compile-only")\n'
                '  endif()\n'
                '  if(CMAKE_REQUIRED_FLAGS)\n'
                '    message(FATAL_ERROR "check flags leaked into probe")\n'
                '  endif()\n'
                '  set_property(GLOBAL APPEND PROPERTY probe_configs "[${CMAKE_TRY_COMPILE_CONFIGURATION}]")\n'
                f'{selection}\n'
                '  set(${result} "${answer}" CACHE INTERNAL "fixture result")\n'
                'endmacro()\n', encoding="utf-8",
            )
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(RuntimeDetection LANGUAGES NONE)\n'
                'set(WIN32 FALSE)\n'
                'list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")\n'
                f'include("{HELPER.as_posix()}")\n'
                'set(CMAKE_REQUIRED_FLAGS "caller-check-flags")\n'
                'set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)\n'
                f'{setup}\n'
                'spark_detect_cxx_runtime(actual)\n'
                'if(NOT CMAKE_REQUIRED_FLAGS STREQUAL "caller-check-flags" OR\n'
                '   NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "EXECUTABLE")\n'
                '  message(FATAL_ERROR "caller state was changed")\n'
                'endif()\n'
                'if(DEFINED CACHE{_spark_has_libcxx} OR DEFINED CACHE{_spark_has_libstdcxx})\n'
                '  message(FATAL_ERROR "probe cache was not cleared")\n'
                'endif()\n'
                'get_property(configs GLOBAL PROPERTY probe_configs)\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/result.txt" "${actual}\n${configs}")\n',
                encoding="utf-8",
            )
            result = self.configure(root, root / "build")
            output = result.stdout + result.stderr
            if diagnostic:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(diagnostic, output)
            else:
                self.assertEqual(result.returncode, 0, output)
                self.assertEqual((root / "build" / "result.txt").read_text(), expected)

    def test_detection_clears_cached_configuration_for_untyped_build(self):
        self.detect(
            'set(CMAKE_BUILD_TYPE "")\nset(CMAKE_CONFIGURATION_TYPES "")\n'
            'set(CMAKE_TRY_COMPILE_CONFIGURATION Release CACHE STRING "caller setting")',
            'set(answer FALSE)\nif("${result}" STREQUAL "_spark_has_libstdcxx")\n'
            '  set(answer TRUE)\nendif()',
            expected="libstdc++\n[];[]",
        )

    def test_detection_checks_every_configuration_without_stale_results(self):
        self.detect(
            'set(CMAKE_CONFIGURATION_TYPES Debug Release)',
            'set(answer FALSE)\nif("${result}" STREQUAL "_spark_has_libcxx")\n'
            '  set(answer TRUE)\nendif()',
            expected="libc++\n[Debug];[Debug];[Release];[Release]",
        )

    def test_detection_rejects_mixed_configuration_families(self):
        self.detect(
            'set(CMAKE_CONFIGURATION_TYPES Debug Release)',
            'set(answer FALSE)\n'
            'if((CMAKE_TRY_COMPILE_CONFIGURATION STREQUAL "Debug" AND "${result}" STREQUAL "_spark_has_libcxx") OR\n'
            '   (CMAKE_TRY_COMPILE_CONFIGURATION STREQUAL "Release" AND "${result}" STREQUAL "_spark_has_libstdcxx"))\n'
            '  set(answer TRUE)\nendif()',
            diagnostic="Spark SDK configurations must use the same C++ runtime",
        )

    def test_detection_rejects_unknown_and_ambiguous_header_identity(self):
        for answer in ("FALSE", "TRUE"):
            with self.subTest(answer=answer):
                self.detect('set(CMAKE_BUILD_TYPE Debug)', f'set(answer {answer})',
                            diagnostic="Cannot identify the Spark C++ runtime")

    def test_windows_detection_never_probes(self):
        self.detect('set(WIN32 TRUE)', 'message(FATAL_ERROR "unexpected probe")', expected="\n")

    def reject_application(self, arguments, diagnostic):
        with tempfile.TemporaryDirectory(prefix="spark-sdk-runtime-invalid-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(RuntimeContract LANGUAGES NONE)\n'
                'set(WIN32 FALSE)\n'
                f'include("{HELPER.as_posix()}")\n'
                'add_library(EngineInterface INTERFACE)\n'
                f'spark_apply_cxx_runtime(EngineInterface {arguments})\n',
                encoding="utf-8",
            )
            result = self.configure(root, root / "build")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(diagnostic, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
