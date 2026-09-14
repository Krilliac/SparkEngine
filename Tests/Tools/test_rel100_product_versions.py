#!/usr/bin/env python3
"""REL-100 executable version contracts for shipped pipeline tools."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
VERSION_DECLARATION = re.compile(
    r'^set\(SPARK_ENGINE_VERSION "(?P<version>[0-9]+\.[0-9]+\.[0-9]+)" CACHE STRING',
    re.MULTILINE,
)
PRODUCTS = (
    "SparkShaderCompiler",
    "SparkCooker",
    "SparkAutomation",
    "SparkCrashReporter",
)
BIN_DIR: Path | None = None
ENGINE_VERSION: str | None = None


def read_engine_version() -> str:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = VERSION_DECLARATION.search(cmake)
    if match is None:
        raise AssertionError("the authoritative SPARK_ENGINE_VERSION declaration is missing")
    return match.group("version")


class ShippedProductVersionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if BIN_DIR is None:
            raise unittest.SkipTest("requires --bin-dir when run as an executable contract")
        cls.bin_dir = BIN_DIR
        if ENGINE_VERSION is None:
            raise AssertionError("the executable contract did not resolve the engine version")
        cls.version = ENGINE_VERSION

    def test_pipeline_tools_report_the_authoritative_engine_version(self) -> None:
        for product in PRODUCTS:
            with self.subTest(product=product):
                executable = self.bin_dir / f"{product}.exe"
                result = subprocess.run(
                    [str(executable), "--version"],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=30,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, f"{product} {self.version}\n")
                self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--bin-dir", type=Path, required=True)
    harness_args, unittest_args = parser.parse_known_args()
    BIN_DIR = harness_args.bin_dir
    ENGINE_VERSION = read_engine_version()
    # unittest must not see the harness-only option.
    sys.argv[1:] = unittest_args
    unittest.main()
