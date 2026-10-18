#!/usr/bin/env python3
"""REL-100 executable version contract for every stable-v1 product.

Each non-test executable in the stable-v1 build-product contract
(docs/site/readiness.json, releaseProfiles[stable-v1].buildProducts) must
answer ``--version`` with its exact, product-specific line built from the
single authoritative ``SPARK_ENGINE_VERSION`` in the root CMakeLists.txt,
exit 0, write nothing to stderr, and do so without a display.

Run without ``--bin-dir`` only the static coverage check executes: the table
below must name exactly the stable-v1 executables, so adding a product to the
contract without a version contract fails here.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
READINESS = ROOT / "docs" / "site" / "readiness.json"
VERSION_DECLARATION = re.compile(
    r'^set\(SPARK_ENGINE_VERSION "(?P<version>[0-9]+\.[0-9]+\.[0-9]+)" CACHE STRING',
    re.MULTILINE,
)
STABLE_PROFILE_ID = "stable-v1"
# The shipped products are built by this lane; the installed-SDK consumer and
# validation lanes produce test executables, not stable-v1 deliverables.
SHIPPING_BUILD_PROFILE = "windows-shipping"

# Exact stdout line each product prints for --version. {version} is the
# authoritative engine version; {platform} is the host name the product embeds.
PRODUCT_VERSION_FORMATS = {
    "SparkEngine": "SparkEngine {version}",
    "SparkEditor": "SparkEditor {version}",
    "SparkConsole": "SparkConsole {version}",
    "SparkShaderCompiler": "SparkShaderCompiler {version}",
    "SparkCrashReporter": "SparkCrashReporter {version}",
    "SparkCooker": "SparkCooker {version}",
    "SparkAutomation": "SparkAutomation {version}",
    "SparkLauncher": "SparkLauncher {version}",
    "SparkBuild": "SparkBuild v{version} ({platform})",
    "SparkInstaller": "SparkInstaller {version} ({platform})",
}
EXECUTABLE_SUFFIX = ".exe" if sys.platform == "win32" else ""
BIN_DIR: Path | None = None
EXPECTED_VERSION: str | None = None


def host_platform_name() -> str:
    if sys.platform == "win32":
        return "Windows"
    if sys.platform == "darwin":
        return "macOS"
    return "Linux"


def read_engine_version() -> str:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    matches = VERSION_DECLARATION.findall(cmake)
    if len(matches) != 1:
        raise AssertionError(
            f"expected exactly one authoritative SPARK_ENGINE_VERSION declaration, found {len(matches)}"
        )
    return matches[0]


def stable_shipping_executables() -> set[str]:
    readiness = json.loads(READINESS.read_text(encoding="utf-8"))
    profiles = [p for p in readiness["releaseProfiles"] if p["id"] == STABLE_PROFILE_ID]
    if len(profiles) != 1:
        raise AssertionError(f"expected exactly one '{STABLE_PROFILE_ID}' release profile")
    return {
        product["target"]
        for product in profiles[0]["buildProducts"]
        if product["kind"] == "executable" and product["buildProfile"] == SHIPPING_BUILD_PROFILE
    }


def display_free_environment() -> dict[str, str]:
    # Version introspection must not need a display server; strip every
    # variable a GUI product could use to reach one.
    env = dict(os.environ)
    for name in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(name, None)
    return env


class StableProductContractCoverageTests(unittest.TestCase):
    def test_table_names_exactly_the_stable_v1_shipping_executables(self) -> None:
        contract = stable_shipping_executables()
        self.assertTrue(contract, "the stable-v1 profile declares no shipping executables")
        self.assertEqual(set(PRODUCT_VERSION_FORMATS), contract)


class StableProductExecutableVersionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if BIN_DIR is None:
            raise unittest.SkipTest("requires --bin-dir when run as an executable contract")
        if EXPECTED_VERSION is None:
            raise AssertionError("the executable contract did not resolve the expected version")
        cls.bin_dir = BIN_DIR
        cls.version = EXPECTED_VERSION
        cls.env = display_free_environment()

    def test_every_product_reports_the_authoritative_engine_version(self) -> None:
        for product, line_format in sorted(PRODUCT_VERSION_FORMATS.items()):
            with self.subTest(product=product):
                executable = self.bin_dir / f"{product}{EXECUTABLE_SUFFIX}"
                self.assertTrue(executable.is_file(), f"{executable} was not built")
                result = subprocess.run(
                    [str(executable), "--version"],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    env=self.env,
                    stdin=subprocess.DEVNULL,
                    timeout=30,
                    check=False,
                )
                expected = line_format.format(version=self.version, platform=host_platform_name())
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, f"{expected}\n")
                self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--bin-dir", type=Path)
    # Test-only override proving the contract rejects a version mismatch.
    parser.add_argument("--expected-version")
    harness_args, unittest_args = parser.parse_known_args()
    BIN_DIR = harness_args.bin_dir
    EXPECTED_VERSION = harness_args.expected_version or read_engine_version()
    # unittest must not see the harness-only options.
    sys.argv[1:] = unittest_args
    unittest.main()
