#!/usr/bin/env python3
"""Compiler-free checks of the Shipping policy and post-project CMake gate."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import windows_shipping_toolchain as policy_tool

INSTANCE = "C:/Program Files/Microsoft Visual Studio/2022/Enterprise"
BIN = INSTANCE + "/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64"
SELECTOR = "v143,host=x64,version=14.44.35207"
MEASURED = {
    "CMAKE_SYSTEM_NAME": "Windows", "CMAKE_BUILD_TYPE": "MinSizeRel",
    "CMAKE_GENERATOR": "Visual Studio 17 2022", "CMAKE_GENERATOR_PLATFORM": "x64",
    "CMAKE_GENERATOR_TOOLSET": SELECTOR, "CMAKE_GENERATOR_INSTANCE": INSTANCE,
    "CMAKE_VS_PLATFORM_TOOLSET": "v143", "CMAKE_VS_PLATFORM_TOOLSET_VERSION": "14.44.35207",
    "CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE": "x64",
    "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION": "10.0.26100.0",
    "CMAKE_C_COMPILER_ID": "MSVC", "CMAKE_C_COMPILER_VERSION": "19.44.35229.0",
    "CMAKE_C_COMPILER_ARCHITECTURE_ID": "x64", "CMAKE_C_COMPILER": BIN + "/cl.exe",
    "CMAKE_CXX_COMPILER_ID": "MSVC", "CMAKE_CXX_COMPILER_VERSION": "19.44.35229.0",
    "CMAKE_CXX_COMPILER_ARCHITECTURE_ID": "x64", "CMAKE_CXX_COMPILER": BIN + "/cl.exe",
    "CMAKE_LINKER": BIN + "/link.exe", "CMAKE_AR": BIN + "/lib.exe",
}


class PolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.policy = policy_tool.load_policy(ROOT)

    def test_exact_input_lock_and_preset_agree(self) -> None:
        self.assertEqual(self.policy["compilerVersion"], "19.44.35229.0")
        self.assertEqual(self.policy["msvcToolsVersion"], "14.44.35207")
        self.assertEqual(self.policy["windowsSdkVersion"], "10.0.26100.0")
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))["configurePresets"]
        shipping = next(item for item in presets if item["name"] == "windows-shipping")
        self.assertEqual(shipping["toolset"], policy_tool.toolset_selector(self.policy))
        self.assertEqual(shipping["cacheVariables"]["CMAKE_SYSTEM_VERSION"], self.policy["windowsSdkVersion"])
        self.assertEqual(shipping["cacheVariables"]["SPARK_ENFORCE_WINDOWS_SHIPPING_TOOLCHAIN"], "ON")
        release = next(item for item in presets if item["name"] == "windows-release")
        self.assertEqual(release["toolset"], "v143")
        self.assertNotIn("SPARK_ENFORCE_WINDOWS_SHIPPING_TOOLCHAIN", release["cacheVariables"])

    def test_closed_schema_rejects_missing_extra_invalid_and_wrong_scope(self) -> None:
        for key, value in (("extra", "x"), ("compilerVersion", 19), ("compilerVersion", "19.44"),
                           ("configuration", "Release"), ("schemaVersion", "unknown")):
            changed = dict(self.policy, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(policy_tool.ShippingToolchainError):
                policy_tool.parse_policy(json.dumps(changed).encode())
        for key in self.policy:
            changed = dict(self.policy)
            changed.pop(key)
            with self.subTest(missing=key), self.assertRaises(policy_tool.ShippingToolchainError):
                policy_tool.parse_policy(json.dumps(changed).encode())

    def test_duplicate_and_malformed_json_are_rejected(self) -> None:
        raw = json.dumps(self.policy)
        for value in (b"{", b"[]", raw.replace('"compilerVersion":', '"compilerVersion": "x", "compilerVersion":').encode()):
            with self.subTest(value=value), self.assertRaises(policy_tool.ShippingToolchainError):
                policy_tool.parse_policy(value)

    def test_comparator_accepts_exact_identities_and_rejects_each_component(self) -> None:
        compiler = {"id": "MSVC", "version": "19.44.35229.0", "architecture": "x64", "path": BIN + "/cl.exe"}
        values = dict(configuration="MinSizeRel", generator="Visual Studio 17 2022", architecture="x64",
                      toolset=SELECTOR, compilers={"C": compiler, "CXX": dict(compiler)},
                      windows_sdk="10.0.26100.0", linker=BIN + "/link.exe", archiver=BIN + "/lib.exe",
                      instance=INSTANCE, require_c=True)
        self.assertEqual(policy_tool.compare_toolchain(self.policy, **values), [])
        for field, wrong in (("configuration", "Release"), ("generator", "Visual Studio 18 2026"),
                             ("architecture", "Win32"), ("toolset", "v143"), ("windows_sdk", "10.0.22621.0"),
                             ("linker", BIN.replace("35207", "35208") + "/link.exe"),
                             ("archiver", BIN.replace("35207", "35208") + "/lib.exe"),
                             ("instance", "C:/other"), ("instance", "/root-relative")):
            with self.subTest(field=field):
                self.assertTrue(policy_tool.compare_toolchain(self.policy, **dict(values, **{field: wrong})))
        for language in ("C", "CXX"):
            for field, wrong in (("version", "19.44.35228.0"), ("id", "Clang"), ("architecture", "x86"),
                                 ("path", BIN.replace("35207", "35208") + "/cl.exe")):
                changed = copy.deepcopy(values)
                changed["compilers"][language][field] = wrong
                with self.subTest(language=language, field=field):
                    self.assertTrue(policy_tool.compare_toolchain(self.policy, **changed))


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for configure-gate fixtures")
class CMakeGateTests(unittest.TestCase):
    def run_gate(self, overrides: dict[str, str | None] | None = None, *, enabled: bool = True,
                 policy: str | None = "committed") -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            module = root / "SparkWindowsShippingToolchain.cmake"
            shutil.copyfile(ROOT / "cmake" / module.name, module)
            if policy == "committed":
                shutil.copyfile(ROOT / policy_tool.POLICY_PATH, root / "WindowsShippingToolchain.json")
            elif policy is not None:
                (root / "WindowsShippingToolchain.json").write_text(policy, encoding="utf-8")
            values = dict(MEASURED, **(overrides or {}))
            lines = ["cmake_minimum_required(VERSION 3.25)"]
            lines.extend(f"set({key} [==[{value}]==])" for key, value in values.items() if value is not None)
            lines.append(f"set(SPARK_ENFORCE_WINDOWS_SHIPPING_TOOLCHAIN {'ON' if enabled else 'OFF'})")
            lines.append(f'include("{module.as_posix()}")')
            lines.append(f'if(NOT CMAKE_GENERATOR_TOOLSET STREQUAL "{values["CMAKE_GENERATOR_TOOLSET"]}")')
            lines.append('message(FATAL_ERROR "module rewrote generator toolset")')
            lines.append("endif()")
            script = root / "check.cmake"
            script.write_text("\n".join(lines), encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=30)

    def test_exact_match_passes_without_rewriting_generator_inputs(self) -> None:
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_each_measured_component_mismatch_fails(self) -> None:
        for key, wrong in (("CMAKE_SYSTEM_NAME", "Linux"), ("CMAKE_BUILD_TYPE", "Release"),
                           ("CMAKE_GENERATOR", "Visual Studio 18 2026"), ("CMAKE_GENERATOR_PLATFORM", "Win32"),
                           ("CMAKE_GENERATOR_TOOLSET", "v143"), ("CMAKE_VS_PLATFORM_TOOLSET", "v145"),
                           ("CMAKE_VS_PLATFORM_TOOLSET_VERSION", "14.44.35208"),
                           ("CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE", "x86"),
                           ("CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION", "10.0.22621.0"),
                           ("CMAKE_C_COMPILER_VERSION", "19.44.35228.0"),
                           ("CMAKE_CXX_COMPILER_VERSION", "19.44.35228.0"),
                           ("CMAKE_C_COMPILER_ID", "Clang"), ("CMAKE_CXX_COMPILER_ID", "Clang"),
                           ("CMAKE_C_COMPILER_ARCHITECTURE_ID", "x86"), ("CMAKE_CXX_COMPILER_ARCHITECTURE_ID", "x86"),
                           ("CMAKE_C_COMPILER", BIN.replace("35207", "35208") + "/cl.exe"),
                           ("CMAKE_CXX_COMPILER", BIN.replace("35207", "35208") + "/cl.exe"),
                           ("CMAKE_LINKER", BIN.replace("35207", "35208") + "/link.exe"),
                           ("CMAKE_AR", BIN.replace("35207", "35208") + "/lib.exe")):
            with self.subTest(key=key):
                result = self.run_gate({key: wrong})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(key, result.stderr)

    def test_missing_measured_identity_fails(self) -> None:
        for key in MEASURED:
            if key == "CMAKE_GENERATOR_TOOLSET":
                continue
            with self.subTest(key=key):
                self.assertNotEqual(self.run_gate({key: None}).returncode, 0)

    def test_gate_off_leaves_developer_release_compiler_accepted_without_policy(self) -> None:
        result = self.run_gate({"CMAKE_BUILD_TYPE": "Release", "CMAKE_GENERATOR_TOOLSET": "v143",
                                "CMAKE_C_COMPILER_VERSION": "19.44.35228.0",
                                "CMAKE_CXX_COMPILER_VERSION": "19.44.35228.0"}, enabled=False, policy=None)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_malformed_duplicate_and_open_schema_policy_fail(self) -> None:
        policy = (ROOT / policy_tool.POLICY_PATH).read_text(encoding="utf-8")
        changed = json.loads(policy)
        changed["extra"] = "x"
        for raw in (None, "{", "[]", json.dumps(changed),
                    policy.replace('"compilerVersion":', '"compilerVersion": "x", "compilerVersion":')):
            with self.subTest(raw=raw):
                result = self.run_gate(policy=raw)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("policy", result.stderr)


if __name__ == "__main__":
    unittest.main()
