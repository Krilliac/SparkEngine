#!/usr/bin/env python3
"""BLD-100 / CI-120 Shipping configuration contract.

Static regressions for the strict Windows Shipping configuration surface:

* the SparkBuild configurator's "Shipping" preset (SparkBuild/src/Config.cpp)
  produces exactly the option values of the authoritative windows-shipping
  CMake preset;
* the windows-shipping preset cannot re-enable development-only toggles;
* optimized configurations request reproducible outputs (no PE timestamps,
  no absolute source paths through __FILE__);
* first-party sources never embed build-time clocks;
* every first-party CMake call that gives MinSizeRel SPARK_BUILD_SHIPPING also
  gives it SPARK_SHIPPING, the switch the debug-hook and detector headers use to
  compile their instrumentation out.

These checks read source only. They do not prove a compiled binary is
byte-for-byte reproducible; that requires the reproducibility-windows CI job
comparing two clean hosted builds.
"""

from __future__ import annotations

import copy
import re
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_ROOT = REPO_ROOT / "Tools" / "buildmatrix"
sys.path.insert(0, str(TOOLS_ROOT))

import check_parity  # noqa: E402
import inventory  # noqa: E402

CONFIG_CPP = REPO_ROOT / "SparkBuild" / "src" / "Config.cpp"
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"
REPRO_MODULE = REPO_ROOT / "cmake" / "SparkReproducibleBuild.cmake"
REPRO_INCLUDE = 'include("${CMAKE_SOURCE_DIR}/cmake/SparkReproducibleBuild.cmake")'


def _function_body(text: str, qualified_name: str) -> str:
    match = re.search(r"void\s+" + re.escape(qualified_name) + r"\s*\(\s*\)\s*\{", text)
    if not match:
        raise AssertionError(f"{qualified_name} not found in {CONFIG_CPP}")
    depth = 1
    index = match.end()
    while depth:
        if index >= len(text):
            raise AssertionError(f"unterminated body for {qualified_name}")
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
    return text[match.end() : index - 1]


def sparkbuild_shipping_values(text: str) -> tuple[dict[str, bool], str]:
    """Evaluate ConfigManager::ApplyPresetShipping statically.

    The function is ApplyPresetDefaults() followed by condition blocks of the
    form ``if (opt.cmakeVar == "X" || ...) { opt.currentValue = <bool>; }``.
    Anything else in the body is rejected so a new construct cannot be
    silently ignored.
    """

    defaults = {
        option["name"]: option["default"]
        for option in inventory.extract_sparkbuild_options(CONFIG_CPP)
    }
    body = _function_body(text, "ConfigManager::ApplyPresetShipping")
    if "ApplyPresetDefaults();" not in body:
        raise AssertionError("ApplyPresetShipping no longer starts from ApplyPresetDefaults()")
    stripped = re.sub(r"//[^\n]*", "", body)
    block_re = re.compile(
        r"if\s*\(\s*(opt\.cmakeVar\s*==\s*\"[A-Za-z0-9_]+\""
        r"(?:\s*\|\|\s*opt\.cmakeVar\s*==\s*\"[A-Za-z0-9_]+\")*)\s*\)\s*"
        r"\{\s*opt\.currentValue\s*=\s*(true|false)\s*;\s*\}"
    )
    values = dict(defaults)
    for block in block_re.finditer(stripped):
        for name in re.findall(r"\"([A-Za-z0-9_]+)\"", block.group(1)):
            if name not in defaults:
                raise AssertionError(f"ApplyPresetShipping names unknown option {name}")
            values[name] = block.group(2) == "true"
    residue = block_re.sub("", stripped)
    residue = re.sub(r"ApplyPresetDefaults\(\);", "", residue)
    residue = re.sub(r"for\s*\(\s*auto&\s*opt\s*:\s*config\.options\s*\)", "", residue)
    build_type = re.search(r"config\.buildType\s*=\s*BuildType::([A-Za-z]+)\s*;", residue)
    if not build_type:
        raise AssertionError("ApplyPresetShipping does not select a build type")
    residue = residue.replace(build_type.group(0), "")
    if re.sub(r"[\s{}]", "", residue):
        raise AssertionError(f"ApplyPresetShipping contains unevaluated statements: {residue.strip()!r}")
    return values, build_type.group(1)


def windows_shipping_cache(presets: dict[str, Any] | None = None) -> dict[str, str]:
    presets = presets if presets is not None else inventory.extract_cmake_presets()
    return dict(inventory.resolve_configure_preset(presets, "windows-shipping")["cacheVariables"])


class SparkBuildShippingPresetTests(unittest.TestCase):
    def test_parser_sees_every_shipping_block(self) -> None:
        values, build_type = sparkbuild_shipping_values(CONFIG_CPP.read_text(encoding="utf-8"))
        self.assertEqual(build_type, "MinSizeRel")
        self.assertFalse(values["BUILD_TESTS"])
        self.assertTrue(values["STRIP_DEBUG_SYMBOLS"])

    def test_parser_rejects_unevaluated_statements(self) -> None:
        text = CONFIG_CPP.read_text(encoding="utf-8").replace(
            "config.buildType = BuildType::MinSizeRel;",
            "config.buildType = BuildType::MinSizeRel;\n        config.generator = Generator::Ninja;",
            1,
        )
        with self.assertRaisesRegex(AssertionError, "unevaluated"):
            sparkbuild_shipping_values(text)

    def test_sparkbuild_shipping_matches_windows_shipping_preset(self) -> None:
        values, build_type = sparkbuild_shipping_values(CONFIG_CPP.read_text(encoding="utf-8"))
        cache = windows_shipping_cache()
        self.assertEqual(cache.get("CMAKE_BUILD_TYPE"), build_type)
        mismatches = {
            name: (cache[name], "ON" if value else "OFF")
            for name, value in sorted(values.items())
            if name in cache and cache[name].upper() != ("ON" if value else "OFF")
        }
        self.assertEqual(
            mismatches,
            {},
            "SparkBuild's Shipping preset disagrees with CMakePresets.json windows-shipping "
            "(preset value, SparkBuild value)",
        )


class ShippingPresetToggleTests(unittest.TestCase):
    def test_repository_windows_shipping_preset_is_clean(self) -> None:
        self.assertEqual(
            check_parity.check_shipping_preset_options(inventory.extract_cmake_presets()), []
        )

    def test_development_toggles_are_blocking(self) -> None:
        for name, bad in (
            ("ENABLE_CONSOLE_IN_SHIPPING", "ON"),
            ("ENABLE_DEVCOMMANDS_IN_SHIPPING", "ON"),
            ("ENABLE_PROFILING", "ON"),
            ("BUILD_TESTS", "ON"),
            ("CMAKE_BUILD_TYPE", "Release"),
        ):
            with self.subTest(option=name):
                presets = inventory.extract_cmake_presets()
                shipping = next(
                    entry for entry in presets["configurePresets"] if entry["name"] == "windows-shipping"
                )
                shipping["cacheVariables"][name] = bad
                findings = check_parity.check_shipping_preset_options(presets)
                self.assertEqual([item.category for item in findings], ["shipping-preset-option"])
                self.assertEqual(findings[0].severity, "error")
                self.assertIn(name, findings[0].message)

    def test_absent_development_toggle_is_blocking(self) -> None:
        # An absent value falls back to the root option() default (BUILD_TESTS
        # and ENABLE_PROFILING default ON), so it must be explicit.
        presets = inventory.extract_cmake_presets()
        shipping = next(
            entry for entry in presets["configurePresets"] if entry["name"] == "windows-shipping"
        )
        del shipping["cacheVariables"]["BUILD_TESTS"]
        findings = check_parity.check_shipping_preset_options(copy.deepcopy(presets))
        self.assertEqual(len(findings), 1)
        self.assertIn("BUILD_TESTS=OFF", findings[0].message)


def _strip_cmake_comments(text: str) -> str:
    return "\n".join(re.sub(r"(^|\s)#.*$", r"\1", line) for line in text.splitlines())


def _cmake_calls(text: str, command: str) -> list[str]:
    calls = []
    for match in re.finditer(r"\b" + re.escape(command) + r"\s*\(", text):
        depth = 1
        index = match.end()
        while depth and index < len(text):
            depth += {"(": 1, ")": -1}.get(text[index], 0)
            index += 1
        calls.append(text[match.end() : index - 1])
    return calls


def _msvc_repro_block(text: str) -> str:
    match = re.search(
        r'^if\(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"\)\n(.*?)^elseif\(', text, re.MULTILINE | re.DOTALL
    )
    if not match or "/Brepro" not in match.group(1):
        raise AssertionError(f"{REPRO_MODULE} has no MSVC reproducible-output block")
    return match.group(1)


class ReproducibleOutputFlagTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = _strip_cmake_comments(ROOT_CMAKE.read_text(encoding="utf-8"))
        self.text = _strip_cmake_comments(REPRO_MODULE.read_text(encoding="utf-8"))

    def test_root_applies_module_before_any_target(self) -> None:
        # Directory compile/link options only reach targets created after them.
        include_at = self.root.find(REPRO_INCLUDE)
        self.assertGreater(include_at, 0, "root CMakeLists.txt does not include SparkReproducibleBuild.cmake")
        first_target = re.search(
            r"^\s*(add_library|add_executable|add_subdirectory|FetchContent_MakeAvailable)\s*\(",
            self.root,
            re.MULTILINE,
        )
        self.assertIsNotNone(first_target)
        self.assertLess(include_at, first_target.start())
        self.assertGreater(include_at, self.root.find("/favor:AMD64"))

    def test_msvc_objects_images_and_archives_drop_timestamps(self) -> None:
        block = _msvc_repro_block(self.text)
        compile_args = " ".join(_cmake_calls(block, "add_compile_options"))
        link_args = " ".join(_cmake_calls(block, "add_link_options"))
        self.assertIn("$<$<NOT:$<CONFIG:Debug>>:/Brepro>", compile_args)
        self.assertIn("$<$<NOT:$<CONFIG:Debug>>:/Brepro>", link_args)
        self.assertRegex(block, r"foreach\(config [^)]*MINSIZEREL[^)]*\)")
        self.assertIn('string(APPEND CMAKE_STATIC_LINKER_FLAGS_${config} " /Brepro")', block)

    def test_msvc_file_macro_is_source_root_relative(self) -> None:
        block = _msvc_repro_block(self.text)
        self.assertIn('file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" SPARK_REPRO_SOURCE_ROOT)', block)
        self.assertIn(
            '"$<$<NOT:$<CONFIG:Debug>>:/d1trimfile:${SPARK_REPRO_SOURCE_ROOT}>"',
            " ".join(_cmake_calls(block, "add_compile_options")),
        )

    def test_gnu_clang_file_macro_is_source_root_relative(self) -> None:
        self.assertIn(
            '"$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=>"',
            " ".join(_cmake_calls(self.text, "add_compile_options")),
        )

    def test_gnu_clang_build_root_is_mapped_after_the_source_root(self) -> None:
        # The DWARF comp_dir is each target's binary directory. GCC and Clang
        # apply the last matching map, so the build-root map must follow the
        # source-root map to win for a build tree inside the source tree.
        source_map = '"$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=>"'
        build_map = '"$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=.>"'
        compile_args = " ".join(_cmake_calls(self.text, "add_compile_options"))
        self.assertIn(build_map, compile_args)
        self.assertLess(compile_args.index(source_map), compile_args.index(build_map))
        # GCC LTO compiles the LTRANS units at link time, so the link repeats both maps.
        gcc_lto = re.search(
            r'if\(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND ENABLE_LTO\)(.*?)\n    endif\(\)', self.text, re.S
        )
        self.assertIsNotNone(gcc_lto, "GCC LTO link-time prefix maps are missing")
        link_args = " ".join(_cmake_calls(gcc_lto.group(1), "add_link_options"))
        self.assertLess(link_args.index(source_map), link_args.index(build_map))
        # Without a seed GCC names LTO IR sections from the clock and pid.
        self.assertIn(
            'string(APPEND CMAKE_${_spark_repro_lang}_COMPILE_OBJECT " -frandom-seed=<OBJECT>")',
            gcc_lto.group(1),
        )

    def test_shipping_never_links_incrementally(self) -> None:
        # /INCREMENTAL pads images and defeats /Brepro; it must stay Debug-only.
        for call in _cmake_calls(self.root + self.text, "add_link_options"):
            for token in re.findall(r"\S*/INCREMENTAL\S*", call):
                self.assertEqual(token, "$<$<CONFIG:Debug>:/INCREMENTAL>")


SHIPPING_PROFILE_DEFINE = "$<$<CONFIG:MinSizeRel>:SPARK_BUILD_SHIPPING>"
SHIPPING_GATE_DEFINE = "$<$<CONFIG:MinSizeRel>:SPARK_SHIPPING=1>"
DEBUG_HOOK_HEADER = REPO_ROOT / "SparkEngine" / "Source" / "Utils" / "DebugHookManager.h"
CMAKE_DEFINITION_COMMANDS = ("list", "target_compile_definitions", "add_compile_definitions")


def _first_party_cmake_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--", "CMakeLists.txt", "*/CMakeLists.txt", "*.cmake"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return [
        REPO_ROOT / name
        for name in result.stdout.splitlines()
        if not name.startswith(("ThirdParty/", "build/")) and (REPO_ROOT / name).is_file()
    ]


def shipping_definition_gaps(files: dict[str, str]) -> list[str]:
    """Return every definition call that marks MinSizeRel as Shipping without SPARK_SHIPPING.

    ``files`` maps a display name to CMake text. A call names the Shipping
    profile when it contains SPARK_BUILD_SHIPPING under a MinSizeRel generator
    expression; it must then also carry SPARK_SHIPPING=1 for MinSizeRel.
    """

    gaps = []
    for name, text in files.items():
        stripped = _strip_cmake_comments(text)
        for command in CMAKE_DEFINITION_COMMANDS:
            for call in _cmake_calls(stripped, command):
                compact = re.sub(r"\s+", "", call)
                if SHIPPING_PROFILE_DEFINE in compact and SHIPPING_GATE_DEFINE not in compact:
                    gaps.append(f"{name}: {command}({call.strip()[:120]}...)")
    return gaps


class ShippingDefinitionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = ROOT_CMAKE.read_text(encoding="utf-8")

    def test_first_party_shipping_profiles_define_spark_shipping(self) -> None:
        files = {
            str(path.relative_to(REPO_ROOT)): path.read_text(encoding="utf-8")
            for path in _first_party_cmake_files()
        }
        self.assertIn("CMakeLists.txt", files)
        self.assertEqual(shipping_definition_gaps(files), [])

    def test_root_feature_definitions_carry_both_shipping_defines(self) -> None:
        stripped = _strip_cmake_comments(self.root)
        profile_calls = [
            re.sub(r"\s+", "", call)
            for call in _cmake_calls(stripped, "list")
            if "FEATURE_DEFINITIONS" in call and "SPARK_BUILD_SHIPPING" in call
        ]
        self.assertEqual(len(profile_calls), 1, "expected one per-configuration FEATURE_DEFINITIONS block")
        self.assertIn(SHIPPING_PROFILE_DEFINE, profile_calls[0])
        self.assertIn(SHIPPING_GATE_DEFINE, profile_calls[0])
        # PUBLIC so the executables, game modules and tests that link
        # SparkEngineLib compile the same hook gates as the library.
        applied = [
            re.sub(r"\s+", " ", call).strip() for call in _cmake_calls(stripped, "target_compile_definitions")
        ]
        self.assertIn("SparkEngineLib PUBLIC ${FEATURE_DEFINITIONS}", applied)

    def test_gap_detector_flags_profile_without_gate(self) -> None:
        broken = self.root.replace(SHIPPING_GATE_DEFINE, "", 1)
        self.assertNotEqual(broken, self.root)
        gaps = shipping_definition_gaps({"CMakeLists.txt": broken})
        self.assertEqual(len(gaps), 1)
        self.assertIn("FEATURE_DEFINITIONS", gaps[0])

    def test_debug_hook_header_rejects_profile_without_gate(self) -> None:
        text = DEBUG_HOOK_HEADER.read_text(encoding="utf-8")
        guard = text.find("#if defined(SPARK_BUILD_SHIPPING) && !defined(SPARK_SHIPPING)")
        self.assertGreater(guard, 0, "DebugHookManager.h lost its SPARK_BUILD_SHIPPING/SPARK_SHIPPING guard")
        self.assertIn("#error", text[guard : text.find("#endif", guard)])
        self.assertLess(guard, text.find("#define SPARK_DEBUG_HOOKS_ENABLED 0"))


FIRST_PARTY_ROOTS = (
    "SparkEngine",
    "SparkEditor",
    "SparkBuild",
    "SparkLauncher",
    "SparkSDK",
    "SparkCrashReporter",
    "SparkInstaller",
    "GameModules",
    "Tools",
)


SOURCE_SUFFIXES = ("h", "hpp", "cpp", "c", "inl", "mm")


def _git_grep(pattern: str, roots: list[str]) -> subprocess.CompletedProcess[str]:
    # Pathspecs are OR-ed, so each one must carry both the root and the suffix;
    # a bare "**/*.h" would pull ThirdParty into the scan.
    pathspecs = [f":(glob){root}/**/*.{suffix}" for root in roots for suffix in SOURCE_SUFFIXES]
    return subprocess.run(
        ["git", "grep", "-n", "-I", "-E", pattern, "--", *pathspecs],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


class BuildClockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.roots = [root for root in FIRST_PARTY_ROOTS if (REPO_ROOT / root).is_dir()]
        self.assertTrue(self.roots, "no first-party source roots found; the scan would be vacuous")

    def test_scan_scope_selects_first_party_sources_only(self) -> None:
        # Guards the negative scan below: wrong pathspecs would select nothing
        # and report "no build clock" for every tree.
        result = _git_grep(r"__FILE__", self.roots)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SparkEngine/Source/Utils/Assert.h:", result.stdout)
        self.assertNotIn("ThirdParty/", result.stdout)

    def test_first_party_sources_do_not_embed_build_clock(self) -> None:
        result = _git_grep(r"__(DATE|TIME|TIMESTAMP)__", self.roots)
        # git grep: 0 = matches, 1 = no matches, anything else = the scan failed.
        self.assertIn(result.returncode, (0, 1), result.stderr)
        self.assertEqual(result.stdout.strip(), "", "build-clock macros break reproducible Shipping outputs")


if __name__ == "__main__":
    unittest.main()
