#!/usr/bin/env python3
"""SDK-240: the SDK ABI surface is pinned by a checker, not by a self-comparing literal.

Before SDK-240, Spark/IEngineContext.h "pinned" its vtable with
``static_assert(EngineContextVirtualCount == 90 && SPARK_SDK_VERSION == 4)``:
the count was a literal compared with itself, so appending a virtual still
compiled and an old host would accept a module that calls off the end of its
vtable. SparkSDK/Tools/sdk_abi_surface.py extracts the real virtual-slot order
and ABI struct layouts from the headers and compares them with the committed
golden SparkSDK/ABI/sdk-abi-surface.json.

Every mutation below runs against a scratch copy of the SDK headers and golden,
never the working tree.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SDK_ROOT = REPO_ROOT / "SparkSDK"
CHECKER = SDK_ROOT / "Tools" / "sdk_abi_surface.py"
GOLDEN = SDK_ROOT / "ABI" / "sdk-abi-surface.json"
INCLUDE = SDK_ROOT / "Include" / "Spark"


def current_sdk_version(include_dir: Path) -> int:
    text = (include_dir / "Version.h").read_text(encoding="utf-8")
    match = re.search(r"^#define\s+SPARK_SDK_VERSION\s+(\d+)", text, re.MULTILINE)
    assert match, "Version.h has no SPARK_SDK_VERSION"
    return int(match.group(1))


class ScratchSdk:
    """A disposable copy of SparkSDK/Include/Spark plus the golden."""

    def __init__(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="sdk240-")
        root = Path(self._tmp.name)
        self.include = root / "Include" / "Spark"
        shutil.copytree(INCLUDE, self.include)
        self.golden = root / "sdk-abi-surface.json"
        shutil.copyfile(GOLDEN, self.golden)

    def close(self) -> None:
        self._tmp.cleanup()

    def run(self, mode: str = "check") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CHECKER), mode, "--sdk-include", str(self.include.parent), "--golden",
             str(self.golden)],
            capture_output=True, text=True, timeout=60, check=False)

    def edit(self, header: str, old: str, new: str, count: int = 1) -> None:
        path = self.include / header
        text = path.read_text(encoding="utf-8")
        if old not in text:
            raise AssertionError(f"{old!r} not found in {header}")
        path.write_text(text.replace(old, new, count), encoding="utf-8")

    def bump_sdk_version(self, note: bool = True) -> int:
        version = current_sdk_version(self.include)
        new_version = version + 1
        path = self.include / "Version.h"
        text = path.read_text(encoding="utf-8")
        text = re.sub(r"^#define\s+SPARK_SDK_VERSION\s+\d+", f"#define SPARK_SDK_VERSION {new_version}", text,
                      count=1, flags=re.MULTILINE)
        if note:
            text = text.replace(f"#define SPARK_SDK_VERSION {new_version}",
                                f"// v{new_version}: SDK-240 test fixture change.\n"
                                f"#define SPARK_SDK_VERSION {new_version}", 1)
        path.write_text(text, encoding="utf-8")
        return new_version

    def append_engine_context_virtual(self) -> None:
        self.edit("IEngineContext.h",
                  "        virtual const ComponentSerializerRegistry* GetComponentSerializers() const { return nullptr; }\n",
                  "        virtual const ComponentSerializerRegistry* GetComponentSerializers() const { return nullptr; }\n"
                  "        virtual void* GetSdk240Probe() { return nullptr; }\n")

    def set_engine_context_count(self, count: int) -> None:
        path = self.include / "IEngineContext.h"
        text = path.read_text(encoding="utf-8")
        text, replaced = re.subn(r"EngineContextVirtualCount = \d+;", f"EngineContextVirtualCount = {count};", text)
        if replaced != 1:
            raise AssertionError("EngineContextVirtualCount definition not found")
        path.write_text(text, encoding="utf-8")


class SdkAbiSurfaceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.assertTrue(CHECKER.is_file(), f"missing ABI checker {CHECKER}")
        self.assertTrue(GOLDEN.is_file(), f"missing committed golden {GOLDEN}")
        self.sdk = ScratchSdk()
        self.addCleanup(self.sdk.close)

    def assertFails(self, result: subprocess.CompletedProcess[str], needle: str) -> None:
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, f"checker accepted the change:\n{output}")
        self.assertIn(needle, output)

    def test_committed_tree_matches_golden(self) -> None:
        result = subprocess.run([sys.executable, str(CHECKER), "check"], capture_output=True, text=True, timeout=60,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_golden_records_real_engine_context_vtable(self) -> None:
        golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
        self.assertEqual(golden["sdk_version"], current_sdk_version(INCLUDE))
        slots = golden["surface"]["interfaces"]["Spark::IEngineContext"]["virtuals"]
        # Declaration order is vtable order; the destructor is declared first.
        self.assertTrue(slots[0].startswith("~IEngineContext"), slots[0])
        self.assertEqual(slots[1], "GraphicsEngine* GetGraphics()")
        self.assertEqual(slots[-1], "const ComponentSerializerRegistry* GetComponentSerializers() const")
        header = (INCLUDE / "IEngineContext.h").read_text(encoding="utf-8")
        pinned = int(re.search(r"EngineContextVirtualCount = (\d+);", header).group(1))
        self.assertEqual(len(slots), pinned)
        for name in ("Spark::IModule", "Spark::ILogger", "Spark::INetworkService"):
            self.assertIn(name, golden["surface"]["interfaces"])
        info = golden["surface"]["structs"]["Spark::ModuleInfo"]
        self.assertEqual([field["name"] for field in info["fields"]],
                         ["name", "version", "sdkVersion", "loadOrder", "dependencies", "dependencyCount", "kind"])
        self.assertEqual(golden["surface"]["structs"]["SparkModuleCompatibilityDescriptor"]["lp64_size"], 64)

    def test_appended_virtual_without_version_bump_fails(self) -> None:
        # The exact change the old literal-vs-literal static_assert let through.
        self.sdk.append_engine_context_virtual()
        self.sdk.set_engine_context_count(91)
        self.assertFails(self.sdk.run(), "without a SPARK_SDK_VERSION bump")

    def test_update_refuses_to_repin_an_unbumped_abi_change(self) -> None:
        self.sdk.append_engine_context_virtual()
        self.sdk.set_engine_context_count(91)
        self.assertFails(self.sdk.run("update"), "without a SPARK_SDK_VERSION bump")

    def test_reordered_virtuals_fail(self) -> None:
        self.sdk.edit("IModule.h", "virtual void OnPause() {}", "virtual void OnPauseTmp() {}")
        self.sdk.edit("IModule.h", "virtual void OnResume() {}", "virtual void OnPause() {}")
        self.sdk.edit("IModule.h", "virtual void OnPauseTmp() {}", "virtual void OnResume() {}")
        self.assertFails(self.sdk.run(), "Spark::IModule")

    def test_struct_layout_change_fails(self) -> None:
        self.sdk.edit("IModule.h", "        int loadOrder = 1000;", "        int64_t loadOrder = 1000;")
        self.assertFails(self.sdk.run(), "Spark::ModuleInfo")

    def test_bump_that_was_not_repinned_fails(self) -> None:
        self.sdk.append_engine_context_virtual()
        self.sdk.set_engine_context_count(91)
        self.sdk.bump_sdk_version()
        self.assertFails(self.sdk.run(), "not re-pinned")

    def test_bump_without_abi_change_that_was_not_repinned_fails(self) -> None:
        self.sdk.bump_sdk_version()
        self.assertFails(self.sdk.run(), "not re-pinned")

    def test_repinned_bump_passes(self) -> None:
        self.sdk.append_engine_context_virtual()
        self.sdk.set_engine_context_count(91)
        new_version = self.sdk.bump_sdk_version()
        update = self.sdk.run("update")
        self.assertEqual(update.returncode, 0, update.stdout + update.stderr)
        check = self.sdk.run()
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        golden = json.loads(self.sdk.golden.read_text(encoding="utf-8"))
        self.assertEqual(golden["sdk_version"], new_version)
        self.assertEqual(len(golden["surface"]["interfaces"]["Spark::IEngineContext"]["virtuals"]), 91)

    def test_bump_without_migration_note_fails(self) -> None:
        self.sdk.append_engine_context_virtual()
        self.sdk.set_engine_context_count(91)
        new_version = self.sdk.bump_sdk_version(note=False)
        self.assertFails(self.sdk.run("update"), f"// v{new_version}:")
        self.assertFails(self.sdk.run(), f"// v{new_version}:")

    def test_stale_virtual_count_fails(self) -> None:
        self.sdk.set_engine_context_count(89)
        self.assertFails(self.sdk.run(), "EngineContextVirtualCount")

    def test_stale_count_after_repinned_bump_fails(self) -> None:
        self.sdk.append_engine_context_virtual()
        self.sdk.bump_sdk_version()
        self.assertFails(self.sdk.run("update"), "EngineContextVirtualCount")

    def test_descriptor_abi_macro_change_fails(self) -> None:
        self.sdk.edit("ModuleABI.h", "#define SPARK_MODULE_RUNTIME_ABI_VERSION 1u",
                      "#define SPARK_MODULE_RUNTIME_ABI_VERSION 2u")
        self.assertFails(self.sdk.run(), "SPARK_MODULE_RUNTIME_ABI_VERSION")

    def test_comment_and_whitespace_edits_are_not_abi_changes(self) -> None:
        self.sdk.edit("IEngineContext.h", "/** @brief Get the input manager */",
                      "/** @brief Get the input manager (reworded) */\n\n")
        self.sdk.edit("IModule.h", "virtual void OnRender() {}", "virtual   void   OnRender()   {}")
        result = self.sdk.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
