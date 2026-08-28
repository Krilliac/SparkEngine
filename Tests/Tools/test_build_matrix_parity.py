#!/usr/bin/env python3
"""Adversarial CI-120 tests using only synthetic strings and temporary copies."""

from __future__ import annotations

import copy
from contextlib import contextmanager
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
import uuid
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_ROOT = REPO_ROOT / "Tools" / "buildmatrix"
TEST_TEMP_ROOT = Path(os.environ.get("CI120_TEST_TMPDIR", tempfile.gettempdir()))
sys.path.insert(0, str(TOOLS_ROOT))

import check_parity  # noqa: E402
import inventory  # noqa: E402


def finding_categories(findings: list[check_parity.Finding]) -> set[str]:
    return {finding.category for finding in findings}


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


@contextmanager
def temporary_paths(*names: str):
    """Yield unique temporary files directly in a writable root (no tracked mutation)."""
    token = uuid.uuid4().hex
    paths = [TEST_TEMP_ROOT / f".ci120-{token}-{name}" for name in names]
    try:
        yield paths
    finally:
        for path in paths:
            path.unlink(missing_ok=True)


class RepositoryInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.data = inventory.build_inventory()

    def test_inventory_is_byte_deterministic(self) -> None:
        self.assertEqual(
            inventory.render_inventory(inventory.build_inventory()),
            inventory.render_inventory(inventory.build_inventory()),
        )

    def test_canonical_products_are_profile_owned_and_service_free(self) -> None:
        products = self.data["profile"]["buildProducts"]
        targets = {product["target"] for product in products}
        self.assertIn("SparkGameFPS", targets)
        self.assertIn("spark_package_smoke", targets)
        self.assertIn("spark_package_module_header_smoke", targets)
        self.assertTrue(
            targets.isdisjoint(
                {"SparkDaemon", "SparkCollabServer", "SparkOrchestrator", "SparkServer", "SparkGateway"}
            )
        )
        self.assertTrue(all(product["applicability"] in {"required", "shared"} for product in products))

    def test_canonical_capability_union_is_exact(self) -> None:
        profile = self.data["profile"]
        actual = {
            capability
            for product in profile["buildProducts"]
            for capability in product["capabilityIds"]
        }
        self.assertEqual(actual, set(profile["includedCapabilityIds"]))
        self.assertEqual(
            {entry["id"]: entry["configuration"] for entry in profile["buildConfigurations"]},
            {
                "installed-sdk-consumer": "Release",
                "windows-shipping": "MinSizeRel",
                "windows-validation": "Release",
            },
        )

    def test_all_twelve_live_configures_and_owners_are_extracted(self) -> None:
        configs = self.data["workflowCmakeConfigs"]
        self.assertEqual(len(configs), 12)
        self.assertEqual(
            [entry["job"] for entry in configs],
            [
                "build-linux-asan",
                "build-linux-tsan",
                "build-linux-msan",
                "build-windows-vs2022",
                "build-windows-vs2026",
                "build-linux-gcc",
                "build-linux-clang",
                "build-linux-mingw-wine",
                "build-macos",
                "coverage",
                "clang-tidy",
                "build-installer",
            ],
        )
        self.assertEqual(configs[-1]["sourceDir"], "build-installer")
        self.assertEqual(configs[-1]["buildDir"], "build-installer/build")
        windows = next(entry for entry in configs if entry["job"] == "build-windows-vs2022")
        self.assertTrue(windows["fresh"])
        self.assertEqual(windows["generator"], "Visual Studio 17 2022")
        self.assertEqual(windows["architecture"], "x64")
        self.assertEqual(windows["toolset"], "v143")

    def test_current_debt_is_blocking_not_baseline_masked(self) -> None:
        report = check_parity.build_report(copy.deepcopy(self.data))
        self.assertEqual(report["state"], "blocked")
        self.assertGreater(report["errorCount"], 0)
        self.assertIn("configured-evidence-absent", {item["category"] for item in report["findings"]})

    def test_shipping_preset_currently_disables_declared_editor(self) -> None:
        findings = check_parity.check_product_preset_activation(copy.deepcopy(self.data))
        editor = [finding for finding in findings if "SparkEditor" in finding.message]
        self.assertEqual(len(editor), 1)
        self.assertEqual(editor[0].severity, "error")

    def test_exact_checked_in_artifacts_are_valid_and_current(self) -> None:
        inventory_path = REPO_ROOT / "docs" / "readiness" / "ci120-build-matrix-inventory.json"
        report_path = REPO_ROOT / "docs" / "readiness" / "ci120-parity-findings.json"
        checked_inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
        checked_report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(inventory_path.read_bytes(), inventory.render_inventory(self.data))
        self.assertEqual(report_path.read_bytes(), check_parity.render_report(check_parity.build_report(self.data)))
        self.assertEqual(checked_inventory["schemaVersion"], 2)
        self.assertEqual(checked_report["state"], "blocked")

    def test_analysis_does_not_mutate_tracked_files(self) -> None:
        command = ["git", "status", "--short", "--untracked-files=no"]
        before = subprocess.run(command, cwd=REPO_ROOT, check=True, capture_output=True).stdout
        inventory.build_inventory()
        check_parity.build_report(inventory.build_inventory())
        after = subprocess.run(command, cwd=REPO_ROOT, check=True, capture_output=True).stdout
        self.assertEqual(before, after)


class ProfileMutationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.readiness = json.loads((REPO_ROOT / "docs" / "site" / "readiness.json").read_text(encoding="utf-8"))
        self.profile = next(profile for profile in self.readiness["releaseProfiles"] if profile["id"] == "stable-v1")

    def load_temp(self) -> dict[str, Any]:
        return inventory.load_stable_profile_data(copy.deepcopy(self.readiness))

    def test_missing_fps_product_is_rejected(self) -> None:
        self.profile["buildProducts"] = [
            product for product in self.profile["buildProducts"] if product["target"] != "SparkGameFPS"
        ]
        with self.assertRaisesRegex(inventory.InventoryError, "SparkGameFPS"):
            self.load_temp()

    def test_extra_nonprofile_service_is_rejected(self) -> None:
        self.profile["buildProducts"].append(
            {
                "target": "SparkServer",
                "kind": "executable",
                "buildProfile": "windows-shipping",
                "applicability": "required",
                "capabilityIds": ["services.production"],
                "requiredOptions": {"ENABLE_SERVER_PROCESSES": "ON"},
            }
        )
        with self.assertRaisesRegex(inventory.InventoryError, "out-of-profile|outside|exactly equal"):
            self.load_temp()

    def test_target_kind_change_is_blocking(self) -> None:
        data = inventory.build_inventory()
        mutated = copy.deepcopy(data)
        product = next(item for item in mutated["profile"]["buildProducts"] if item["target"] == "SparkGameFPS")
        product["kind"] = "executable"
        mutated["stableV1Products"] = copy.deepcopy(mutated["profile"]["buildProducts"])
        findings = check_parity.check_stable_v1_targets(
            mutated["cmakeTargets"], mutated["profile"]["buildProducts"]
        )
        self.assertIn("target-kind-mismatch", finding_categories(findings))

    def test_applicable_option_mismatch_blocks_unless_explicitly_outside(self) -> None:
        cmake = [{"name": "ONLY_ROOT", "default": True}]
        required = check_parity.check_sparkbuild_vs_cmake(cmake, [])
        outside = check_parity.check_sparkbuild_vs_cmake(
            cmake,
            [],
            [{"name": "ONLY_ROOT", "applicability": "outside", "reason": "experimental backend"}],
        )
        self.assertEqual(required[0].severity, "error")
        self.assertEqual(outside[0].severity, "warning")


class CMakeParserTests(unittest.TestCase):
    def parse_options(self, text: str) -> list[dict[str, Any]]:
        return inventory.extract_cmake_options_text(text, "synthetic/CMakeLists.txt")

    def test_case_omitted_default_and_locations_are_preserved(self) -> None:
        declarations = self.parse_options(
            'OpTiOn(MixedCase "mixed command" on)\noption(NO_DEFAULT "omitted")\n'
        )
        by_name = {entry["name"]: entry for entry in declarations}
        self.assertEqual(by_name["MixedCase"]["default"], "ON")
        self.assertEqual(by_name["MixedCase"]["line"], 1)
        self.assertEqual(by_name["NO_DEFAULT"]["default"], "OFF")
        self.assertEqual(by_name["NO_DEFAULT"]["line"], 2)
        self.assertTrue(all(entry["file"].endswith("CMakeLists.txt") for entry in declarations))

    def test_mutually_exclusive_conditionals_are_not_duplicates(self) -> None:
        declarations = self.parse_options(
            textwrap.dedent(
                """\
                if(WIN32)
                  option(PLATFORM_FEATURE "Windows" ON)
                else()
                  option(PLATFORM_FEATURE "Other" OFF)
                endif()
                """
            )
        )
        self.assertEqual(len(declarations), 2)
        self.assertEqual(check_parity.check_duplicate_options(declarations), [])
        effective = inventory.effective_cmake_options(declarations)
        self.assertEqual(effective[0]["activeDeclarationCount"], 1)
        self.assertIs(effective[0]["default"], True)

    def test_overlapping_duplicate_declarations_are_blocking(self) -> None:
        declarations = self.parse_options(
            'option(DUP "first" ON)\noption(DUP "second" OFF)\n'
        )
        findings = check_parity.check_duplicate_options(declarations)
        self.assertEqual(finding_categories(findings), {"duplicate-option"})
        self.assertIn(":1", findings[0].detail)
        self.assertIn(":2", findings[0].detail)


class WorkflowParserTests(unittest.TestCase):
    def parse(self, text: str) -> list[dict[str, Any]]:
        return inventory.extract_workflow_cmake_configs_text(text)

    def test_multiline_fresh_source_binary_preset_and_folded_forms(self) -> None:
        data = self.parse(
            textwrap.dedent(
                """\
                name: parser-test
                jobs:
                  multiline:
                    steps:
                    - name: Backslash
                      run: |
                        cmake -B build \\
                          -S . \\
                          -DENABLE_EDITOR=ON
                  fresh:
                    steps:
                    - name: Fresh
                      run: cmake --fresh -S source -Bout -G Ninja
                  preset:
                    steps:
                    - name: Preset
                      run: cmake --preset=windows-shipping
                  folded:
                    steps:
                    - name: Folded
                      run: >-
                        cmake -S package
                        -B package-build
                        -DSPARK_EXPECTED_ENGINE_VERSION=1.0
                """
            )
        )
        self.assertEqual(len(data), 4)
        self.assertEqual([entry["job"] for entry in data], ["multiline", "fresh", "preset", "folded"])
        self.assertEqual(data[0]["options"], {"ENABLE_EDITOR": "ON"})
        self.assertEqual(data[1]["buildDir"], "out")
        self.assertEqual(data[2]["preset"], "windows-shipping")
        self.assertEqual(data[3]["sourceDir"], "package")

    def test_quoted_inline_run_and_separated_cache_assignment(self) -> None:
        data = self.parse(
            textwrap.dedent(
                """\
                jobs:
                  quoted:
                    steps:
                    - name: Quoted
                      run: "cmake -S . -B build -D ENABLE_EDITOR=ON"
                """
            )
        )
        self.assertEqual(len(data), 1)
        self.assertEqual(data[0]["options"], {"ENABLE_EDITOR": "ON"})

    def test_duplicate_configures_are_preserved_not_deduplicated(self) -> None:
        data = self.parse(
            textwrap.dedent(
                """\
                jobs:
                  duplicate:
                    steps:
                    - name: Both
                      run: |
                        cmake -B build -S .
                        cmake -B build -S .
                """
            )
        )
        self.assertEqual(len(data), 2)
        self.assertEqual(data[0]["job"], "duplicate")
        self.assertEqual(data[0], data[1])

    def test_unparsed_configure_looking_command_fails_closed(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "unparsed"):
            self.parse(
                textwrap.dedent(
                    """\
                    jobs:
                      bad:
                        steps:
                        - name: Hidden
                          run: echo cmake -B build -S .
                    """
                )
            )


class PresetAndCodemodelTests(unittest.TestCase):
    def test_inherited_shipping_values_are_resolved(self) -> None:
        presets = {
            "configurePresets": [
                {"name": "base", "hidden": True, "cacheVariables": {"SPARK_STRICT_DEPS": "ON"}},
                {
                    "name": "windows-shipping",
                    "inherits": "base",
                    "cacheVariables": {"SPARK_NATIVE_ARCH": "OFF"},
                },
            ],
            "buildPresets": [],
            "testPresets": [],
        }
        resolved = inventory.resolve_configure_preset(presets, "windows-shipping")
        self.assertEqual(
            resolved["cacheVariables"],
            {"SPARK_STRICT_DEPS": "ON", "SPARK_NATIVE_ARCH": "OFF"},
        )
        self.assertEqual(check_parity.check_shipping_preset_options(presets), [])

    def test_missing_shipping_preset_is_blocking(self) -> None:
        findings = check_parity.check_shipping_preset_options(
            {"configurePresets": [], "buildPresets": [], "testPresets": []}
        )
        self.assertEqual(finding_categories(findings), {"missing-shipping-preset"})
        self.assertEqual(findings[0].severity, "error")

    def test_every_canonical_preset_must_resolve(self) -> None:
        data = inventory.build_inventory()
        mutated = copy.deepcopy(data)
        mutated["cmakePresets"]["configurePresets"] = [
            preset
            for preset in mutated["cmakePresets"]["configurePresets"]
            if preset["name"] != "windows-release"
        ]
        findings = check_parity.check_profile_presets(mutated)
        self.assertTrue(any("windows-validation" in finding.message for finding in findings))

    def test_codemodel_available_evidence_extracts_exact_kind(self) -> None:
        evidence = inventory.parse_codemodel_targets(
            "windows-shipping",
            {
                "configurations": [
                    {
                        "name": "MinSizeRel",
                        "targets": [{"name": "SparkEngine", "jsonFile": "target.json"}],
                    }
                ]
            },
            {"target.json": {"type": "EXECUTABLE"}},
        )
        self.assertEqual(evidence["status"], "available")
        self.assertEqual(
            evidence["targets"],
            [{"target": "SparkEngine", "kind": "executable", "configuration": "MinSizeRel"}],
        )

    def test_absent_codemodel_is_explicit_and_blocking(self) -> None:
        missing = TEST_TEMP_ROOT / f".ci120-nonexistent-{uuid.uuid4().hex}"
        evidence = inventory.extract_codemodel_targets(missing, "windows-shipping")
        self.assertEqual(evidence, {"profile": "windows-shipping", "status": "absent", "targets": []})
        data = inventory.build_inventory()
        findings = check_parity.check_configured_targets(data)
        self.assertIn("configured-evidence-absent", finding_categories(findings))

    def test_wrong_codemodel_configuration_cannot_satisfy_profile(self) -> None:
        data = inventory.build_inventory()
        mutated = copy.deepcopy(data)
        evidence = next(
            entry for entry in mutated["configuredTargetEvidence"] if entry["profile"] == "windows-validation"
        )
        evidence.update(
            {
                "status": "available",
                "targets": [
                    {"target": "SparkTests", "kind": "executable", "configuration": "Debug"}
                ],
            }
        )
        findings = check_parity.check_configured_targets(mutated)
        missing = [finding for finding in findings if "SparkTests" in finding.message]
        self.assertEqual(len(missing), 1)
        self.assertIn("Release", missing[0].message)


class CommandLineContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.data = inventory.build_inventory()
        cls.report = check_parity.build_report(cls.data)

    def invoke(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOLS_ROOT / "check_parity.py"), *arguments],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def invoke_inventory(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(TOOLS_ROOT / "inventory.py"), *arguments],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_exact_reviewed_baseline_stays_red_with_exit_one(self) -> None:
        with temporary_paths("inventory.json", "baseline.json") as paths:
            inv_path, baseline_path = paths
            write_json(inv_path, self.data)
            baseline_path.write_bytes(check_parity.render_report(self.report))
            result = self.invoke("--inventory", str(inv_path), "--baseline", str(baseline_path))
        self.assertEqual(result.returncode, check_parity.EXIT_FINDINGS)
        self.assertEqual(json.loads(result.stdout)["state"], "blocked")
        self.assertIn("REVIEWED FINDINGS", result.stderr)

    def test_new_drift_and_malformed_baseline_use_exit_two(self) -> None:
        with temporary_paths("inventory.json", "drift.json", "malformed.json") as paths:
            inv_path, drift_path, malformed_path = paths
            write_json(inv_path, self.data)
            drift = copy.deepcopy(self.report)
            drift["warningCount"] += 1
            drift_path.write_bytes(check_parity.render_report(drift))
            malformed_path.write_text("{", encoding="utf-8")
            drift_result = self.invoke("--inventory", str(inv_path), "--baseline", str(drift_path))
            malformed_result = self.invoke("--inventory", str(inv_path), "--baseline", str(malformed_path))
        self.assertEqual(drift_result.returncode, check_parity.EXIT_BASELINE_DRIFT)
        self.assertEqual(malformed_result.returncode, check_parity.EXIT_BASELINE_DRIFT)
        json.loads(drift_result.stdout)
        json.loads(malformed_result.stdout)

    def test_tool_input_crash_uses_exit_three_and_json_stdout(self) -> None:
        with temporary_paths("bad-inventory.json") as paths:
            bad = paths[0]
            bad.write_text("{}\n", encoding="utf-8")
            result = self.invoke("--inventory", str(bad))
        self.assertEqual(result.returncode, check_parity.EXIT_INTERNAL)
        self.assertEqual(json.loads(result.stdout)["state"], "internal-error")
        self.assertIn("INTERNAL ERROR", result.stderr)

    def test_output_cannot_alias_an_authoritative_input(self) -> None:
        with temporary_paths("inventory.json") as paths:
            inv_path = paths[0]
            original = inventory.render_inventory(self.data)
            inv_path.write_bytes(original)
            result = self.invoke("--inventory", str(inv_path), "--output", str(inv_path))
            after = inv_path.read_bytes()
        self.assertEqual(result.returncode, check_parity.EXIT_INTERNAL)
        self.assertEqual(after, original)
        self.assertEqual(json.loads(result.stdout)["state"], "internal-error")

    def test_inventory_output_cannot_overwrite_its_checked_baseline(self) -> None:
        with temporary_paths("inventory.json") as paths:
            path = paths[0]
            original = b"reviewed baseline sentinel\n"
            path.write_bytes(original)
            result = self.invoke_inventory("--output", str(path), "--check", str(path))
            after = path.read_bytes()
        self.assertEqual(result.returncode, check_parity.EXIT_INTERNAL)
        self.assertEqual(after, original)
        self.assertEqual(json.loads(result.stdout)["state"], "internal-error")


class WorkflowEnforcementTests(unittest.TestCase):
    def test_workflow_compares_uploads_and_enforces_without_or_echo(self) -> None:
        text = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        self.assertNotIn("check_parity.py || echo", text)
        self.assertIn("--baseline docs/readiness/ci120-parity-findings.json", text)
        self.assertIn("ci120-build-matrix-inventory.json", text)
        self.assertIn("actions/upload-artifact", text)
        self.assertIn("Enforce reviewed CI-120 findings", text)

    def test_ci120_work_item_remains_in_progress_and_blocking(self) -> None:
        data = json.loads(
            (REPO_ROOT / "docs" / "readiness" / "work-items" / "00-truth-ci-release.json").read_text(
                encoding="utf-8"
            )
        )
        item = next(entry for entry in data["workItems"] if entry["id"] == "CI-120")
        self.assertEqual(item["status"], "in-progress")
        self.assertTrue(item["blocking"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
