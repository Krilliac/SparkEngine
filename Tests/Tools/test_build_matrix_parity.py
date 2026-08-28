#!/usr/bin/env python3
"""Adversarial tests for CI-120: build-matrix inventory and configuration parity.

Proves the inventory is deterministic, catches omissions, duplicates, renames,
and configuration-surface drift between CMake, SparkBuild, and CI workflows.

All negative-path tests operate on synthetic data or deep copies — nothing here
mutates tracked files.
"""

from __future__ import annotations

import copy
import json
import re
import sys
import textwrap
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Tools" / "buildmatrix"))

import inventory  # noqa: E402
import check_parity  # noqa: E402


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_cmake_option(name: str, desc: str = "test", default: str = "ON") -> dict[str, Any]:
    return {"name": name, "description": desc, "default": default}


def _make_sb_option(name: str, default: bool = True, category: str = "Core") -> dict[str, Any]:
    return {
        "name": name,
        "displayName": name,
        "description": "test",
        "default": default,
        "category": category,
    }


# ===========================================================================
# 1. Deterministic inventory output
# ===========================================================================

class TestInventoryDeterminism(unittest.TestCase):
    """Two inventory runs produce identical JSON."""

    def test_two_runs_identical(self) -> None:
        a = json.dumps(inventory.build_inventory(), indent=2, sort_keys=True)
        b = json.dumps(inventory.build_inventory(), indent=2, sort_keys=True)
        self.assertEqual(a, b, "Inventory output is non-deterministic")


# ===========================================================================
# 2. Inventory completeness (smoke)
# ===========================================================================

class TestInventoryCompleteness(unittest.TestCase):
    """Inventory must extract a reasonable number of items."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.inv = inventory.build_inventory()

    def test_cmake_options_extracted(self) -> None:
        opts = self.inv["cmakeOptions"]
        self.assertGreater(len(opts), 20, "Expected >20 CMake options")
        names = {o["name"] for o in opts}
        for required in ("ENABLE_GRAPHICS", "BUILD_TESTS", "SPARK_STRICT_DEPS"):
            self.assertIn(required, names)

    def test_presets_extracted(self) -> None:
        presets = self.inv["cmakePresets"]["configurePresets"]
        self.assertGreater(len(presets), 5)
        names = {p["name"] for p in presets}
        self.assertIn("windows-shipping", names)
        self.assertIn("windows-release", names)

    def test_targets_extracted(self) -> None:
        targets = self.inv["cmakeTargets"]
        self.assertGreater(len(targets), 10)
        names = {t["target"] for t in targets}
        self.assertIn("SparkEngine", names)
        self.assertIn("SparkEditor", names)

    def test_sparkbuild_options_extracted(self) -> None:
        opts = self.inv["sparkBuildOptions"]
        self.assertGreater(len(opts), 10)

    def test_stable_v1_products_declared(self) -> None:
        products = self.inv["stableV1Products"]
        self.assertGreater(len(products), 10)
        names = {p["target"] for p in products}
        self.assertIn("SparkEngine", names)
        self.assertIn("SparkEditor", names)


# ===========================================================================
# 3. Omission detection — synthetic
# ===========================================================================

class TestOmissionDetection(unittest.TestCase):
    """Removing an option from one surface is caught as a discrepancy."""

    def test_cmake_option_missing_from_sparkbuild(self) -> None:
        cmake = [_make_cmake_option("ENABLE_FOO"), _make_cmake_option("ENABLE_BAR")]
        sb = [_make_sb_option("ENABLE_FOO")]
        findings = check_parity.check_sparkbuild_vs_cmake(cmake, sb)
        msgs = [f.message for f in findings]
        self.assertTrue(
            any("ENABLE_BAR" in m for m in msgs),
            "Should detect ENABLE_BAR missing from SparkBuild",
        )

    def test_sparkbuild_orphan_detected(self) -> None:
        cmake = [_make_cmake_option("ENABLE_FOO")]
        sb = [_make_sb_option("ENABLE_FOO"), _make_sb_option("ENABLE_GHOST")]
        findings = check_parity.check_sparkbuild_vs_cmake(cmake, sb)
        orphans = [f for f in findings if f.category == "sparkbuild-orphan"]
        self.assertTrue(
            any("ENABLE_GHOST" in f.message for f in orphans),
            "Should detect ENABLE_GHOST as a SparkBuild orphan",
        )


# ===========================================================================
# 4. Duplicate detection — synthetic
# ===========================================================================

class TestDuplicateDetection(unittest.TestCase):
    """Duplicate option declarations are caught."""

    def test_duplicate_cmake_option(self) -> None:
        opts = [
            _make_cmake_option("ENABLE_DUP"),
            _make_cmake_option("ENABLE_DUP"),
            _make_cmake_option("ENABLE_UNIQUE"),
        ]
        findings = check_parity.check_duplicate_options(opts)
        self.assertEqual(len(findings), 1)
        self.assertIn("ENABLE_DUP", findings[0].message)

    def test_no_false_duplicate(self) -> None:
        opts = [_make_cmake_option("A"), _make_cmake_option("B")]
        findings = check_parity.check_duplicate_options(opts)
        self.assertEqual(len(findings), 0)


# ===========================================================================
# 5. Renamed-target detection — synthetic
# ===========================================================================

class TestRenamedTargetDetection(unittest.TestCase):
    """A stable-v1 product whose CMake target was renamed/removed is caught."""

    def test_missing_stable_v1_target(self) -> None:
        targets = [
            {"target": "SparkEngine", "kind": "executable", "file": "CMakeLists.txt"},
        ]
        products = [
            {"target": "SparkEngine", "kind": "executable", "profile": "required"},
            {"target": "SparkEditorRenamed", "kind": "executable", "profile": "required"},
        ]
        findings = check_parity.check_stable_v1_targets(targets, products)
        self.assertEqual(len(findings), 1)
        self.assertIn("SparkEditorRenamed", findings[0].message)

    def test_all_targets_present(self) -> None:
        targets = [
            {"target": "A", "kind": "executable", "file": "x.txt"},
            {"target": "B", "kind": "executable", "file": "x.txt"},
        ]
        products = [
            {"target": "A", "kind": "executable", "profile": "required"},
            {"target": "B", "kind": "executable", "profile": "required"},
        ]
        findings = check_parity.check_stable_v1_targets(targets, products)
        self.assertEqual(len(findings), 0)


# ===========================================================================
# 6. Default-mismatch detection — synthetic
# ===========================================================================

class TestDefaultMismatch(unittest.TestCase):
    """Options with different defaults across surfaces are caught."""

    def test_mismatched_default(self) -> None:
        cmake = [_make_cmake_option("ENABLE_X", default="ON")]
        sb = [_make_sb_option("ENABLE_X", default=False)]
        findings = check_parity.check_sparkbuild_defaults(cmake, sb)
        self.assertEqual(len(findings), 1)
        self.assertIn("ENABLE_X", findings[0].message)

    def test_matching_defaults_clean(self) -> None:
        cmake = [_make_cmake_option("ENABLE_X", default="ON")]
        sb = [_make_sb_option("ENABLE_X", default=True)]
        findings = check_parity.check_sparkbuild_defaults(cmake, sb)
        self.assertEqual(len(findings), 0)


# ===========================================================================
# 7. Workflow preset parity — synthetic
# ===========================================================================

class TestWorkflowPresetParity(unittest.TestCase):
    """CI workflow --preset refs must match CMakePresets.json."""

    def test_phantom_preset_detected(self) -> None:
        presets = {
            "configurePresets": [{"name": "windows-release", "hidden": False}],
            "buildPresets": [],
            "testPresets": [],
        }
        refs = ["windows-release", "nonexistent-preset"]
        findings = check_parity.check_preset_workflow_parity(presets, refs)
        self.assertEqual(len(findings), 1)
        self.assertIn("nonexistent-preset", findings[0].message)

    def test_valid_refs_pass(self) -> None:
        presets = {
            "configurePresets": [
                {"name": "a", "hidden": False},
                {"name": "b", "hidden": False},
            ],
            "buildPresets": [],
            "testPresets": [],
        }
        refs = ["a", "b"]
        findings = check_parity.check_preset_workflow_parity(presets, refs)
        self.assertEqual(len(findings), 0)


# ===========================================================================
# 8. Shipping preset validation — synthetic
# ===========================================================================

class TestShippingPresetValidation(unittest.TestCase):
    """The windows-shipping preset must enforce strict deps and no native arch."""

    def test_missing_strict_deps(self) -> None:
        presets = {
            "configurePresets": [{
                "name": "windows-shipping",
                "hidden": False,
                "cacheVariables": {"SPARK_NATIVE_ARCH": "OFF"},
            }],
        }
        findings = check_parity.check_shipping_preset_options(presets, [])
        self.assertTrue(any("SPARK_STRICT_DEPS" in f.message for f in findings))

    def test_native_arch_on_rejected(self) -> None:
        presets = {
            "configurePresets": [{
                "name": "windows-shipping",
                "hidden": False,
                "cacheVariables": {"SPARK_STRICT_DEPS": "ON", "SPARK_NATIVE_ARCH": "ON"},
            }],
        }
        findings = check_parity.check_shipping_preset_options(presets, [])
        self.assertTrue(any("SPARK_NATIVE_ARCH" in f.message for f in findings))

    def test_correct_shipping_passes(self) -> None:
        presets = {
            "configurePresets": [{
                "name": "windows-shipping",
                "hidden": False,
                "cacheVariables": {"SPARK_STRICT_DEPS": "ON", "SPARK_NATIVE_ARCH": "OFF"},
            }],
        }
        findings = check_parity.check_shipping_preset_options(presets, [])
        self.assertEqual(len(findings), 0)


# ===========================================================================
# 9. Orphan configure preset detection — synthetic
# ===========================================================================

class TestOrphanConfigurePreset(unittest.TestCase):
    def test_orphan_detected(self) -> None:
        presets = {
            "configurePresets": [
                {"name": "used", "hidden": False},
                {"name": "orphan", "hidden": False},
            ],
            "buildPresets": [{"name": "used", "configurePreset": "used"}],
        }
        findings = check_parity.check_preset_binary_dirs(presets)
        self.assertTrue(any("orphan" in f.message for f in findings))

    def test_hidden_ignored(self) -> None:
        presets = {
            "configurePresets": [{"name": "base", "hidden": True}],
            "buildPresets": [],
        }
        findings = check_parity.check_preset_binary_dirs(presets)
        self.assertEqual(len(findings), 0)


# ===========================================================================
# 10. Live repository — stable-v1 product targets exist in CMake tree
# ===========================================================================

class TestLiveStableV1Coverage(unittest.TestCase):
    """Every declared stable-v1 product must have a CMake target in the tree."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.targets = inventory.extract_cmake_targets()
        cls.target_names = {t["target"] for t in cls.targets}

    def test_every_product_has_target(self) -> None:
        missing = []
        for product in inventory.STABLE_V1_PRODUCTS:
            if product["target"] not in self.target_names:
                missing.append(product["target"])
        self.assertEqual(
            missing, [],
            f"stable-v1 products without CMake targets: {missing}",
        )


# ===========================================================================
# 11. Live repository — workflow preset refs are valid
# ===========================================================================

class TestLiveWorkflowPresets(unittest.TestCase):
    """CI workflow --preset references must exist in CMakePresets.json."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.presets = inventory.extract_cmake_presets()
        cls.workflow_refs = inventory.extract_workflow_presets()

    def test_no_phantom_presets(self) -> None:
        known = {p["name"] for p in self.presets["configurePresets"]}
        phantoms = [r for r in self.workflow_refs if r not in known]
        self.assertEqual(
            phantoms, [],
            f"CI workflow references presets not in CMakePresets.json: {phantoms}",
        )


# ===========================================================================
# 12. Live repository — windows-shipping preset enforces strict config
# ===========================================================================

class TestLiveShippingPreset(unittest.TestCase):
    """The windows-shipping preset must have correct strict options."""

    @classmethod
    def setUpClass(cls) -> None:
        presets = inventory.extract_cmake_presets()
        cls.shipping = None
        for p in presets["configurePresets"]:
            if p["name"] == "windows-shipping":
                cls.shipping = p
                break

    def test_shipping_preset_exists(self) -> None:
        self.assertIsNotNone(self.shipping, "windows-shipping preset not found")

    def test_strict_deps_on(self) -> None:
        if self.shipping is None:
            self.skipTest("no shipping preset")
        cache = self.shipping.get("cacheVariables", {})
        self.assertEqual(cache.get("SPARK_STRICT_DEPS"), "ON")

    def test_native_arch_off(self) -> None:
        if self.shipping is None:
            self.skipTest("no shipping preset")
        cache = self.shipping.get("cacheVariables", {})
        self.assertEqual(cache.get("SPARK_NATIVE_ARCH"), "OFF")

    def test_editor_off_in_shipping(self) -> None:
        if self.shipping is None:
            self.skipTest("no shipping preset")
        cache = self.shipping.get("cacheVariables", {})
        self.assertEqual(cache.get("ENABLE_EDITOR"), "OFF")

    def test_tests_off_in_shipping(self) -> None:
        if self.shipping is None:
            self.skipTest("no shipping preset")
        cache = self.shipping.get("cacheVariables", {})
        self.assertEqual(cache.get("BUILD_TESTS"), "OFF")


# ===========================================================================
# 13. Live repository — no duplicate CMake options
# ===========================================================================

class TestLiveNoDuplicateOptions(unittest.TestCase):
    def test_no_duplicates(self) -> None:
        opts = inventory.extract_cmake_options()
        names = [o["name"] for o in opts]
        dupes = [n for n in names if names.count(n) > 1]
        self.assertEqual(
            list(set(dupes)), [],
            f"Duplicate CMake options: {set(dupes)}",
        )


# ===========================================================================
# 14. Regex extraction fidelity
# ===========================================================================

class TestRegexExtraction(unittest.TestCase):
    """Verify the option regexes handle edge cases."""

    def test_cmake_option_regex(self) -> None:
        text = textwrap.dedent('''\
            option(ENABLE_FOO "Some feature" ON)
            option(ENABLE_BAR "Another one" OFF)
            option(ENABLE_META "Platform-conditional" ${APPLE})
            # option(COMMENTED_OUT "nope" ON)
        ''')
        import tempfile
        tmp = Path(tempfile.mktemp(suffix=".txt"))
        try:
            tmp.write_text(text)
            opts = inventory.extract_cmake_options(tmp)
            names = {o["name"] for o in opts}
            self.assertIn("ENABLE_FOO", names)
            self.assertIn("ENABLE_BAR", names)
            self.assertIn("ENABLE_META", names)
            self.assertNotIn("COMMENTED_OUT", names)
        finally:
            tmp.unlink(missing_ok=True)

    def test_sparkbuild_option_regex(self) -> None:
        text = textwrap.dedent('''\
            config.options.push_back({"ENABLE_ALPHA", "Alpha",
                                      "Alpha feature", true, true,
                                      OptionCategory::Core});
            config.options.push_back({"ENABLE_BETA", "Beta", "Beta feature", false, false, OptionCategory::Graphics});
        ''')
        import tempfile
        tmp = Path(tempfile.mktemp(suffix=".cpp"))
        try:
            tmp.write_text(text)
            opts = inventory.extract_sparkbuild_options(tmp)
            names = {o["name"] for o in opts}
            self.assertIn("ENABLE_ALPHA", names)
            self.assertIn("ENABLE_BETA", names)
        finally:
            tmp.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
