#!/usr/bin/env python3
"""RDY-010: module discovery tests must exercise the production ModuleManager.

``Tests/TestModuleDiscovery.cpp`` used to declare its own ``DiscoveredModule``
struct plus ``GenerateManifest``/``FilterLoaded`` helpers and assert on them.
Its six ``ModuleDiscovery_*`` tests therefore passed inside SparkTests (the
JUnit producer bound to the stable-v1 module evidence) while being unable to
detect any regression in the shipped discovery code. RDY-010 requires that
no copied model or tautological test can satisfy a release-profile gate.

These checks pin the replacement. They are static checks over the C++ source:
they do not prove that the tests compile or pass, which only a SparkTests build
and run can establish.
"""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DISCOVERY_TEST = REPO_ROOT / "Tests" / "TestModuleDiscovery.cpp"
DISCOVERY_TEST_RELATIVE = "Tests/TestModuleDiscovery.cpp"
CENSUS_SCRIPT = REPO_ROOT / "Tools" / "test_source_census.py"
TESTS_CMAKE = REPO_ROOT / "Tests" / "CMakeLists.txt"


def _load_census():
    spec = importlib.util.spec_from_file_location("spark_test_source_census", CENSUS_SCRIPT)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot import {CENSUS_SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _strip_comments(text: str) -> str:
    """Remove // and /* */ comments so prose cannot satisfy a code check."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


class ModuleDiscoveryUsesProductionSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = DISCOVERY_TEST.read_text(encoding="utf-8")
        cls.code = _strip_comments(cls.text)
        cls.census = _load_census()

    def test_census_classifies_discovery_tests_as_production_source(self) -> None:
        kind = self.census.classify(self.text, REPO_ROOT, DISCOVERY_TEST_RELATIVE)
        self.assertEqual(
            kind,
            "production-source",
            "TestModuleDiscovery.cpp includes no production header; its tests "
            "assert on a test-local copy of module discovery",
        )

    def test_includes_the_real_module_manager_header(self) -> None:
        self.assertRegex(self.code, r'#include\s+"Core/ModuleManager\.h"')

    def test_does_not_redeclare_production_types_or_copy_helpers(self) -> None:
        # A local struct shadows ::DiscoveredModule; the helpers were copies of
        # editor/ModuleManager behaviour that no shipped code ever calls.
        self.assertNotRegex(self.code, r"\bstruct\s+DiscoveredModule\b")
        self.assertNotRegex(self.code, r"\bnamespace\s+TestModuleDiscovery\b")
        for helper in ("GenerateManifest", "FilterLoaded"):
            self.assertNotRegex(self.code, rf"\b{helper}\s*\(", helper)

    def test_drives_production_discovery_and_load_entry_points(self) -> None:
        for call in (
            r"\.DiscoverModules\s*\(",
            r"ModuleManager::DiscoverModuleCandidates\s*\(",
            r"\.GetLoadedModuleInfo\s*\(",
            r"\.LoadModule\s*\(\s*",
            r"\.LoadModulesFromManifest\s*\(",
        ):
            self.assertRegex(self.code, call)
        # The loaded-module assertions must use the real ABI fixture DLL, not a
        # fabricated file that could never be mapped.
        self.assertIn("SPARK_TEST_COMPATIBLE_MODULE_PATH", self.code)
        self.assertIn('"Spark Compatible ABI Fixture"', self.code)

    def test_every_test_asserts_something(self) -> None:
        bodies = re.split(r"\bTEST\s*\(", self.code)[1:]
        self.assertGreaterEqual(len(bodies), 6, "discovery coverage shrank")
        for body in bodies:
            name = body.split(")", 1)[0].strip()
            self.assertTrue(name.startswith("ModuleDiscovery_"), name)
            self.assertRegex(body, r"\b(EXPECT|ASSERT)_[A-Z_]+\s*\(", name)
            self.assertNotRegex(body, r"EXPECT_TRUE\s*\(\s*true\s*\)", name)

    def test_file_is_registered_in_spark_tests(self) -> None:
        cmake = TESTS_CMAKE.read_text(encoding="utf-8")
        self.assertRegex(cmake, r"(?m)^\s*TestModuleDiscovery\.cpp\s*$")


if __name__ == "__main__":
    unittest.main()
