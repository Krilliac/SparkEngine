#!/usr/bin/env python3
"""Exercise the Shipping closure driver with synthetic PE files and CMake install rules.

No compiler, C++ build or CTest run is needed. These fixtures test orchestration
and rejection paths; they are not measurements of a real Shipping package.
"""

from __future__ import annotations

import copy
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from test_pe_import_closure import make_pe


REPO_ROOT = Path(__file__).resolve().parents[2]
DRIVER = REPO_ROOT / "Tests/PackageSmoke/VerifyWindowsPackageClosure.cmake"
ROWS = ("win11-x64-msvc143-d3d11", "win11-x64-msvc143-nullrhi")
sys.path.insert(0, str(REPO_ROOT / "Tools/platform-cert"))
import pe_imports  # noqa: E402


class TestShippingAuthorityChecks(unittest.TestCase):
    """Check CLI rejection logic in memory, independently of host temp-directory support."""

    def setUp(self) -> None:
        self.plan = json.loads((REPO_ROOT / f"docs/certification/plans/{ROWS[0]}.json").read_text(encoding="utf-8"))
        self.authority = pe_imports.load_authority(REPO_ROOT / "docs/certification/dependency-authority.json")
        self.graph = {
            "firstPartyImages": [name.casefold() for name in self.plan["firstPartyImages"]],
            "images": [{"path": name} for name in self.plan["firstPartyImages"]],
            "dependencies": [{"name": dep["name"], "source": dep["source"], "resolution": "platform",
                              "importedBy": ["SparkEngine.exe"]} for dep in self.plan["dependencyClosure"]],
        }

    def check(self) -> tuple[int, str]:
        output = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
        self.addCleanup(output.close)
        errors = io.StringIO()
        with patch.object(pe_imports, "load_authority", return_value=self.authority), \
             patch.object(pe_imports.safe_fs, "read_bounded", return_value=b"{}"), \
             patch.object(pe_imports, "load_json_bytes", return_value=self.plan), \
             patch.object(pe_imports, "walk_package", return_value=self.graph), \
             patch("sys.stdout", output), patch("sys.stderr", errors):
            result = pe_imports.main(["--package-root", "unused", "--plan", "unused"])
        return result, errors.getvalue()

    def test_valid_declaration_passes(self) -> None:
        result, errors = self.check()
        self.assertEqual(result, 0, errors)

    def test_wrong_crt_version_is_rejected(self) -> None:
        self.plan["dependencyClosure"][0]["version"] = "bogus"
        result, errors = self.check()
        self.assertEqual(result, 1, errors)
        self.assertIn("version", errors)

    def test_duplicate_dependency_is_rejected(self) -> None:
        duplicate = copy.deepcopy(self.plan["dependencyClosure"][0])
        duplicate["name"] = duplicate["name"].upper()
        self.plan["dependencyClosure"].append(duplicate)
        result, errors = self.check()
        self.assertEqual(result, 1, errors)
        self.assertIn("duplicate", errors.lower())

    def test_missing_required_dependency_is_rejected_even_without_an_import(self) -> None:
        self.plan["dependencyClosure"] = [entry for entry in self.plan["dependencyClosure"]
                                          if entry["name"] != "vcruntime140_1.dll"]
        self.graph["dependencies"] = [entry for entry in self.graph["dependencies"]
                                      if entry["name"] != "vcruntime140_1.dll"]
        result, errors = self.check()
        self.assertEqual(result, 1, errors)
        self.assertIn("dependencyClosure is missing 'vcruntime140_1.dll'", errors)

    def test_unimported_os_images_are_rejected(self) -> None:
        for name in ("kernel32.dll", "d3d11.dll", "plugins/d3d11.dll"):
            with self.subTest(name=name):
                self.graph["images"].append({"path": name})
                try:
                    result, errors = self.check()
                    self.assertEqual(result, 1, errors)
                    self.assertIn(f"shipped image '{name}'", errors)
                finally:
                    self.graph["images"].pop()


class TestWindowsPackageClosure(unittest.TestCase):
    def setUp(self) -> None:
        self.cmake = shutil.which("cmake")
        self.assertIsNotNone(self.cmake, "CMake is required to exercise the install driver")
        self.temp = tempfile.TemporaryDirectory(prefix="plt200 closure ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.runtime = self.build / "runtime"
        self.redist = self.build / "redist"
        self.runtime.mkdir()
        self.redist.mkdir()
        self.runs = self.root / "runs"
        plan = json.loads((REPO_ROOT / f"docs/certification/plans/{ROWS[0]}.json").read_text(encoding="utf-8"))
        self.imports = [entry["name"] for entry in plan["dependencyClosure"]]
        for name in plan["firstPartyImages"]:
            (self.runtime / name).write_bytes(make_pe())
        (self.runtime / "SparkEngine.exe").write_bytes(make_pe(tuple(self.imports)))
        for entry in plan["dependencyClosure"]:
            if entry["source"] == "vcredist":
                (self.redist / entry["name"]).write_bytes(make_pe())
        self.install_script = self.build / "cmake_install.cmake"
        self.install_script.write_text(
            'if(CMAKE_INSTALL_COMPONENT STREQUAL "runtime" OR CMAKE_INSTALL_COMPONENT STREQUAL "redist")\n'
            '    file(COPY "${CMAKE_CURRENT_LIST_DIR}/${CMAKE_INSTALL_COMPONENT}/"\n'
            '        DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")\n'
            'endif()\n',
            encoding="utf-8",
        )

    def run_driver(self, config: str = "MinSizeRel") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [self.cmake, f"-DSPARK_ENGINE_BUILD_DIR={self.build}", f"-DSPARK_SOURCE_ROOT={REPO_ROOT}",
             f"-DSPARK_CONFIG={config}", f"-DSPARK_TEST_ROOT={self.runs}",
             f"-DSPARK_PYTHON_EXECUTABLE={sys.executable}", "-P", str(DRIVER)],
            capture_output=True, text=True, check=False, timeout=60,
        )

    def test_missing_app_local_crt_fails_even_when_the_authority_names_it(self) -> None:
        (self.redist / "msvcp140.dll").unlink()
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("msvcp140.dll", result.stderr)
        # CMake word-wraps FATAL_ERROR text, so compare whitespace-normalized output.
        self.assertIn("not in the package", " ".join(result.stderr.split()))

    def test_missing_delay_imported_crt_also_fails(self) -> None:
        imports = tuple(name for name in self.imports if name != "msvcp140.dll")
        (self.runtime / "SparkEngine.exe").write_bytes(make_pe(imports, ("msvcp140.dll",)))
        (self.redist / "msvcp140.dll").unlink()
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("msvcp140.dll (delay-import)", " ".join(result.stderr.split()))

    def test_success_retains_both_hashed_graphs_and_removes_only_the_stage(self) -> None:
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        runs = list(self.runs.glob("run-*"))
        self.assertEqual(len(runs), 1)
        for row in ROWS:
            graph = json.loads((runs[0] / f"{row}.import-graph.json").read_text(encoding="utf-8"))
            engine = next(image for image in graph["images"] if image["path"] == "SparkEngine.exe")
            self.assertEqual(len(engine["sha256"]), 64)
            self.assertGreater(engine["sizeBytes"], 0)
        self.assertTrue((runs[0] / "package-imports.log").is_file())
        self.assertFalse((runs[0] / "install").exists())

    def test_previous_stage_cannot_supply_a_removed_crt(self) -> None:
        first = self.run_driver()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        (self.redist / "msvcp140.dll").unlink()
        second = self.run_driver()
        self.assertNotEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertIn("msvcp140.dll", second.stderr)

    def test_missing_first_party_image_fails(self) -> None:
        (self.runtime / "SparkEditor.exe").unlink()
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("sparkeditor.exe", result.stderr)

    def test_a_new_import_fails_the_declared_graph(self) -> None:
        (self.runtime / "SparkEngine.exe").write_bytes(make_pe(tuple(self.imports) + ("version.dll",)))
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("version.dll", result.stderr)

    def test_install_failure_is_fatal_and_retains_its_log(self) -> None:
        self.install_script.write_text('message(FATAL_ERROR "fixture install failed")\n', encoding="utf-8")
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("fixture install failed", result.stderr)
        logs = list(self.runs.glob("run-*/install-runtime.log"))
        self.assertEqual(len(logs), 1)
        self.assertIn("fixture install failed", logs[0].read_text(encoding="utf-8"))

    def test_non_shipping_configuration_is_refused(self) -> None:
        result = self.run_driver("Release")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MinSizeRel", result.stderr)


if __name__ == "__main__":
    unittest.main()
