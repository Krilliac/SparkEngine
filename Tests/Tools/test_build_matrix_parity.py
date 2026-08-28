#!/usr/bin/env python3
"""Adversarial CI-120 tests using only synthetic strings and temporary copies."""

from __future__ import annotations

import copy
from contextlib import contextmanager
import hashlib
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
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_ROOT = REPO_ROOT / "Tools" / "buildmatrix"
TEST_TEMP_ROOT = Path(os.environ.get("CI120_TEST_TMPDIR", tempfile.gettempdir()))
sys.path.insert(0, str(TOOLS_ROOT))

import check_parity  # noqa: E402
import inventory  # noqa: E402


def synthetic_ci_context(commit: str = "0" * 40) -> dict[str, str]:
    return {
        "provider": "github-actions",
        "repository": "Krilliac/SparkEngine",
        "sourceCommit": commit,
        "runId": "120",
        "runAttempt": "1",
        "workflowRef": "Krilliac/SparkEngine/.github/workflows/build.yml@refs/heads/Working",
        "job": "build-windows-shipping",
        "runnerOs": "Windows",
    }


def synthetic_ci_environment(commit: str = "0" * 40) -> dict[str, str]:
    context = synthetic_ci_context(commit)
    return {
        "GITHUB_ACTIONS": "true",
        "GITHUB_REPOSITORY": context["repository"],
        "GITHUB_SHA": context["sourceCommit"],
        "GITHUB_RUN_ID": context["runId"],
        "GITHUB_RUN_ATTEMPT": context["runAttempt"],
        "GITHUB_WORKFLOW_REF": context["workflowRef"],
        "GITHUB_JOB": context["job"],
        "RUNNER_OS": context["runnerOs"],
    }


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

    def test_live_configures_are_expanded_per_matrix_leg_with_owners(self) -> None:
        configs = self.data["workflowCmakeConfigs"]
        self.assertEqual(len(configs), 20)
        self.assertEqual(
            sorted({entry["job"] for entry in configs}),
            [
                "build-installer",
                "build-linux-asan",
                "build-linux-clang",
                "build-linux-gcc",
                "build-linux-mingw-wine",
                "build-linux-msan",
                "build-linux-tsan",
                "build-macos",
                "build-windows-vs2022",
                "build-windows-vs2026",
                "clang-tidy",
                "coverage",
            ],
        )
        # A matrix lane contributes one record per combination, so narrowing
        # `config: [Debug, Release]` to `[Debug]` removes a record outright.
        windows = [entry for entry in configs if entry["job"] == "build-windows-vs2022"]
        self.assertEqual(len(windows), 2)
        self.assertEqual(
            sorted(entry["matrix"]["config"] for entry in windows), ["Debug", "Release"]
        )
        self.assertTrue(all(entry["runnerOs"] == "windows" for entry in windows))
        self.assertTrue(windows[0]["fresh"])
        self.assertEqual(windows[0]["generator"], "Visual Studio 17 2022")
        self.assertEqual(windows[0]["architecture"], "x64")
        self.assertEqual(windows[0]["toolset"], "v143")
        # Step names are read structurally, not by a fixed-indentation regex.
        installer = next(entry for entry in configs if entry["job"] == "build-installer")
        self.assertEqual(
            installer["step"], "Configure SparkBuild + SparkInstaller (standalone, no engine)"
        )
        self.assertEqual(installer["sourceDir"], "build-installer")
        self.assertEqual(installer["buildDir"], "build-installer/build")
        self.assertEqual(
            next(entry for entry in configs if entry["job"] == "coverage")["step"], "Configure"
        )

    def test_live_workflow_binds_builds_tests_and_gating(self) -> None:
        workflow = self.data["workflow"]
        self.assertEqual(workflow["pathFilteredEvents"], [])
        self.assertEqual(workflow["unresolvedInvocations"], [])
        summary = workflow["summary"]
        self.assertEqual(summary["jobCount"], len(workflow["jobs"]))
        self.assertEqual(
            summary["matrixLegCount"],
            sum(max(1, len(job["matrixCombinations"])) for job in workflow["jobs"]),
        )
        self.assertEqual(summary["ci120InvocationCount"], len(workflow["ci120Invocations"]))
        self.assertGreater(summary["buildCount"], 0)
        self.assertGreater(summary["testCount"], 0)
        self.assertIn("windows:Release", summary["builtOsConfigurationPairs"])
        self.assertIn("windows:Debug", summary["builtOsConfigurationPairs"])
        gating = {job["id"]: job["gating"] for job in workflow["jobs"]}
        self.assertEqual(gating["build-windows-vs2022"], "blocking")
        self.assertEqual(gating["build-linux-msan"], "advisory")
        self.assertEqual(gating["build-linux-mingw-wine"], "conditional")

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
        self.assertEqual(checked_inventory["schemaVersion"], 3)
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


WORKFLOW_ENVELOPE = """name: synthetic
on:
  push:
    branches: [ Working ]
jobs:
"""


def workflow_document(jobs_body: str, runner: str = "ubuntu-24.04") -> str:
    """Wrap synthetic job bodies in a complete, valid workflow document.

    The analyzer refuses a document with no trigger block and cannot resolve a
    run block's shell without a runner, because both are exactly the shapes a
    weakened workflow would present.
    """
    lines: list[str] = []
    for line in textwrap.dedent(jobs_body).splitlines():
        if not line.strip():
            continue
        lines.append("  " + line)
        if line and not line[0].isspace() and line.rstrip().endswith(":"):
            lines.append(f"    runs-on: {runner}")
    return WORKFLOW_ENVELOPE + "\n".join(lines) + "\n"


class WorkflowParserTests(unittest.TestCase):
    def parse(self, text: str, runner: str = "ubuntu-24.04") -> list[dict[str, Any]]:
        return inventory.extract_workflow_cmake_configs_text(workflow_document(text, runner))

    def test_multiline_fresh_source_binary_preset_and_folded_forms(self) -> None:
        data = self.parse(
            textwrap.dedent(
                """\
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
        self.assertEqual([entry["commandIndex"] for entry in data], [0, 1])
        without_position = [
            {key: value for key, value in entry.items() if key != "commandIndex"}
            for entry in data
        ]
        self.assertEqual(without_position[0], without_position[1])

    def test_unparsed_configure_looking_command_fails_closed(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "unparsed"):
            self.parse(
                textwrap.dedent(
                    """\
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
        build_directory = Path("C:/synthetic-build")
        evidence = inventory.parse_codemodel_targets(
            "windows-shipping",
            {
                "configurations": [
                    {
                        "name": "MinSizeRel",
                        "targets": [
                            {
                                "name": "SparkEngine",
                                "id": "SparkEngine::@synthetic",
                                "jsonFile": "target.json",
                            }
                        ],
                    }
                ]
            },
            {
                "target.json": {
                    "name": "SparkEngine",
                    "id": "SparkEngine::@synthetic",
                    "type": "EXECUTABLE",
                    "nameOnDisk": "SparkEngine.exe",
                    "artifacts": [{"path": "bin/MinSizeRel/SparkEngine.exe"}],
                }
            },
            build_directory,
        )
        self.assertEqual(evidence["status"], "available")
        self.assertEqual(
            evidence["targets"],
            [
                {
                    "target": "SparkEngine",
                    "id": "SparkEngine::@synthetic",
                    "kind": "executable",
                    "configuration": "MinSizeRel",
                    "artifactState": "declared-not-built",
                    "nameOnDisk": "SparkEngine.exe",
                    "artifacts": [
                        (build_directory / "bin/MinSizeRel/SparkEngine.exe").as_posix()
                    ],
                }
            ],
        )

    def test_windows_product_name_must_match_cmake_target_type(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "inconsistent with STATIC_LIBRARY"):
            inventory.parse_codemodel_targets(
                "windows-shipping",
                {
                    "configurations": [
                        {
                            "name": "MinSizeRel",
                            "targets": [
                                {
                                    "name": "SparkEngine",
                                    "id": "SparkEngine::@synthetic",
                                    "jsonFile": "target.json",
                                }
                            ],
                        }
                    ]
                },
                {
                    "target.json": {
                        "name": "SparkEngine",
                        "id": "SparkEngine::@synthetic",
                        "type": "STATIC_LIBRARY",
                        "nameOnDisk": "SparkEngine.exe",
                        "artifacts": [{"path": "bin/MinSizeRel/SparkEngine.exe"}],
                    }
                },
                Path("C:/synthetic-build"),
            )

    def test_one_codemodel_id_cannot_identify_two_targets(self) -> None:
        shared_id = "shared::@synthetic"
        with self.assertRaisesRegex(inventory.InventoryError, "identifies multiple targets"):
            inventory.parse_codemodel_targets(
                "windows-shipping",
                {
                    "configurations": [
                        {
                            "name": "MinSizeRel",
                            "targets": [
                                {"name": "One", "id": shared_id, "jsonFile": "one.json"},
                                {"name": "Two", "id": shared_id, "jsonFile": "two.json"},
                            ],
                        }
                    ]
                },
                {
                    "one.json": {
                        "name": "One",
                        "id": shared_id,
                        "type": "EXECUTABLE",
                        "nameOnDisk": "One.exe",
                        "artifacts": [{"path": "bin/MinSizeRel/One.exe"}],
                    },
                    "two.json": {
                        "name": "Two",
                        "id": shared_id,
                        "type": "EXECUTABLE",
                        "nameOnDisk": "Two.exe",
                        "artifacts": [{"path": "bin/MinSizeRel/Two.exe"}],
                    },
                },
                Path("C:/synthetic-build"),
            )

    def test_absent_codemodel_is_explicit_and_blocking(self) -> None:
        missing = TEST_TEMP_ROOT / f".ci120-nonexistent-{uuid.uuid4().hex}"
        evidence = inventory.extract_codemodel_targets(missing, "windows-shipping")
        self.assertEqual(evidence["profile"], "windows-shipping")
        self.assertEqual(evidence["status"], "absent")
        self.assertEqual(evidence["targets"], [])
        self.assertIn("evidenceDirectory", evidence)
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



# ===========================================================================
# Adversarial regressions: one test per confirmed false green.
#
# Each of these failed to be detected before the CI-120 repair. They exist to
# make the specific weakening visible, so re-introducing it turns a test red
# rather than producing identical "clean" evidence.
# ===========================================================================


BS = chr(92)
BACKTICK = chr(96)


def workflow_record(text: str) -> dict[str, Any]:
    return inventory.extract_workflow_record_text(text, "synthetic/build.yml")


LIVE_WORKFLOW = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")


def workflow_fingerprint(text: str) -> str:
    return json.dumps(workflow_record(text), sort_keys=True)


class WorkflowWeakeningTests(unittest.TestCase):
    """A weakened workflow must not produce the evidence of a strong one."""

    def assert_weakening_is_visible(self, mutated: str, label: str) -> dict[str, Any]:
        self.assertNotEqual(mutated, LIVE_WORKFLOW, f"{label}: mutation did not apply")
        self.assertNotEqual(
            workflow_fingerprint(mutated),
            workflow_fingerprint(LIVE_WORKFLOW),
            f"{label}: weakened workflow produced identical evidence",
        )
        return workflow_record(mutated)

    def test_ubuntu_only_runner_is_visible(self) -> None:
        mutated = LIVE_WORKFLOW.replace("    runs-on: windows-2022", "    runs-on: ubuntu-24.04", 1)
        record = self.assert_weakening_is_visible(mutated, "windows -> ubuntu")
        job = next(item for item in record["jobs"] if item["id"] == "build-windows-vs2022")
        self.assertEqual(job["runnerOsClasses"], ["linux"])

    def test_debug_only_matrix_loses_a_leg(self) -> None:
        mutated = LIVE_WORKFLOW.replace("        config: [Debug, Release]", "        config: [Debug]", 1)
        record = self.assert_weakening_is_visible(mutated, "Debug+Release -> Debug")
        self.assertLess(
            record["summary"]["matrixLegCount"],
            workflow_record(LIVE_WORKFLOW)["summary"]["matrixLegCount"],
        )
        narrowed = next(item for item in record["jobs"] if item["id"] == "build-windows-vs2022")
        self.assertEqual([entry["config"] for entry in narrowed["matrixCombinations"]], ["Debug"])
        self.assertEqual(
            len([item for item in record["buildInvocations"] if item["job"] == "build-windows-vs2022"]), 1
        )

    def test_if_false_job_stops_being_blocking(self) -> None:
        mutated = LIVE_WORKFLOW.replace(
            "  build-windows-vs2022:\n", "  build-windows-vs2022:\n    if: false\n", 1
        )
        record = self.assert_weakening_is_visible(mutated, "if: false")
        job = next(item for item in record["jobs"] if item["id"] == "build-windows-vs2022")
        self.assertIs(job["if"], False)
        self.assertEqual(job["gating"], "conditional")

    def test_continue_on_error_job_becomes_advisory(self) -> None:
        mutated = LIVE_WORKFLOW.replace(
            "  build-windows-vs2022:\n", "  build-windows-vs2022:\n    continue-on-error: true\n", 1
        )
        record = self.assert_weakening_is_visible(mutated, "continue-on-error")
        job = next(item for item in record["jobs"] if item["id"] == "build-windows-vs2022")
        self.assertEqual(job["gating"], "advisory")

    def test_added_path_filter_is_visible_and_blocking(self) -> None:
        mutated = LIVE_WORKFLOW.replace(
            "  push:\n    branches:", "  push:\n    paths-ignore: [ '**' ]\n    branches:", 1
        )
        record = self.assert_weakening_is_visible(mutated, "paths-ignore")
        self.assertEqual(record["pathFilteredEvents"], ["push"])
        data = copy.deepcopy(inventory.build_inventory())
        data["workflow"] = record
        findings = check_parity.check_workflow_semantics(data)
        filtered = [item for item in findings if item.category == "workflow-path-filter"]
        self.assertEqual(len(filtered), 1)
        self.assertEqual(filtered[0].severity, "error")

    def test_live_workflow_has_no_path_filter(self) -> None:
        # The blocking coverage above is only meaningful while the real
        # workflow is genuinely unfiltered.
        self.assertEqual(workflow_record(LIVE_WORKFLOW)["pathFilteredEvents"], [])

    def test_building_one_target_differs_from_building_all(self) -> None:
        mutated = LIVE_WORKFLOW.replace(
            "cmake --build build --config ${{ matrix.config }} --parallel",
            "cmake --build build --config ${{ matrix.config }} --target SparkTests --parallel",
            1,
        )
        record = self.assert_weakening_is_visible(mutated, "build one target")
        self.assertLess(
            record["summary"]["buildAllTargetCount"],
            workflow_record(LIVE_WORKFLOW)["summary"]["buildAllTargetCount"],
        )

    def test_default_build_records_all_targets_explicitly(self) -> None:
        record = workflow_record(
            workflow_document(
                """
                lane:
                  steps:
                  - name: Build
                    run: cmake --build build --config Release --parallel
                """
            )
        )
        build = record["buildInvocations"][0]
        self.assertEqual(build["targets"], ["all"])
        self.assertTrue(build["buildsAllTargets"])

    def test_skipped_advisory_build_cannot_satisfy_a_profile(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data["profile"]["buildConfigurations"] = [
            entry
            for entry in data["profile"]["buildConfigurations"]
            if entry["id"] == "windows-shipping"
        ]
        data["profile"]["buildProducts"] = [
            entry
            for entry in data["profile"]["buildProducts"]
            if entry["buildProfile"] == "windows-shipping"
        ]
        data["workflow"] = inventory.workflow_tool.build_workflow_record(
            """name: probe
on: push
jobs:
  ship:
    runs-on: windows-latest
    steps:
      - name: Configure
        run: cmake --preset windows-shipping
      - name: Never build
        if: false
        continue-on-error: true
        run: cmake --build build/windows-shipping --config MinSizeRel
""",
            "probe.yml",
        )
        categories = finding_categories(check_parity.check_workflow_semantics(data))
        self.assertIn("workflow-configuration-not-built", categories)

    def test_ci120_producer_without_oidc_permission_is_blocking(self) -> None:
        mutated = LIVE_WORKFLOW.replace("      id-token: write\n", "", 1)
        record = self.assert_weakening_is_visible(mutated, "CI-120 OIDC permission removed")
        data = copy.deepcopy(inventory.build_inventory())
        data["workflow"] = record
        categories = finding_categories(check_parity.check_ci120_producer_chain(data))
        self.assertIn("ci120-producer-oidc-permission-missing", categories)

    def test_unresolved_matrix_cannot_satisfy_a_profile(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data["profile"]["buildConfigurations"] = [
            entry
            for entry in data["profile"]["buildConfigurations"]
            if entry["id"] == "windows-shipping"
        ]
        data["profile"]["buildProducts"] = [
            entry
            for entry in data["profile"]["buildProducts"]
            if entry["buildProfile"] == "windows-shipping"
        ]
        data["workflow"] = inventory.workflow_tool.build_workflow_record(
            """name: probe
on:
  push:
    branches: [Working]
jobs:
  ship:
    runs-on: windows-latest
    strategy:
      matrix: ${{ fromJSON(needs.plan.outputs.matrix) }}
    steps:
      - name: Configure
        run: cmake --preset windows-shipping
      - name: Build
        run: cmake --build build/windows-shipping --config MinSizeRel
""",
            "probe.yml",
        )
        categories = finding_categories(check_parity.check_workflow_semantics(data))
        self.assertIn("workflow-matrix-unresolved", categories)
        self.assertIn("workflow-configuration-not-built", categories)

    def test_manual_only_workflow_cannot_be_a_required_gate(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data["workflow"] = inventory.workflow_tool.build_workflow_record(
            """name: probe
on: workflow_dispatch
jobs:
  ship:
    runs-on: windows-latest
    steps:
      - name: Configure
        run: cmake --preset windows-shipping
      - name: Build
        run: cmake --build build/windows-shipping --config MinSizeRel
""",
            "probe.yml",
        )
        categories = finding_categories(check_parity.check_workflow_semantics(data))
        self.assertIn("workflow-working-branch-unreachable", categories)

    def test_closed_only_pull_request_trigger_cannot_be_a_required_gate(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data["workflow"] = inventory.workflow_tool.build_workflow_record(
            """name: probe
on:
  pull_request:
    branches: [Working]
    types: [closed]
jobs:
  ship:
    runs-on: windows-latest
    steps:
      - name: Configure
        run: cmake --preset windows-shipping
      - name: Build
        run: cmake --build build/windows-shipping --config MinSizeRel
""",
            "probe.yml",
        )
        categories = finding_categories(check_parity.check_workflow_semantics(data))
        self.assertIn("workflow-working-branch-unreachable", categories)


class ShellFormTests(unittest.TestCase):
    """Supported shell forms must be parsed, and confused forms must not pass."""

    def test_powershell_backtick_continuation_is_joined(self) -> None:
        script = (
            "cmake -B build " + BACKTICK + "\n  -S . " + BACKTICK + "\n  -DENABLE_EDITOR=OFF -DBUILD_TESTS=OFF"
        )
        joined = inventory.workflow_tool.logical_shell_lines(script, "pwsh")
        self.assertEqual(len(joined), 1)
        commands, unresolved = inventory.workflow_tool.parse_commands(
            script, "pwsh", {}, {}, {"job": "win", "step": "Configure"}
        )
        self.assertEqual(unresolved, [])
        self.assertEqual(commands[0]["options"], {"ENABLE_EDITOR": "OFF", "BUILD_TESTS": "OFF"})

    def test_bash_continuation_is_joined(self) -> None:
        script = "cmake -B build " + BS + "\n  -S . " + BS + "\n  -DENABLE_EDITOR=ON"
        commands, _ = inventory.workflow_tool.parse_commands(
            script, "bash", {}, {}, {"job": "nix", "step": "Configure"}
        )
        self.assertEqual(commands[0]["options"], {"ENABLE_EDITOR": "ON"})

    def test_powershell_block_parsed_as_bash_loses_arguments(self) -> None:
        # Guards the reason shell resolution matters: reading a pwsh block with
        # bash rules silently truncates the command at its first line.
        script = "cmake -B build " + BACKTICK + "\n  -S . " + BACKTICK + "\n  -DENABLE_EDITOR=OFF"
        as_pwsh = inventory.workflow_tool.logical_shell_lines(script, "pwsh")
        as_bash = inventory.workflow_tool.logical_shell_lines(script, "bash")
        self.assertEqual(len(as_pwsh), 1)
        self.assertEqual(len(as_bash), 3)

    def test_windows_runner_defaults_to_powershell(self) -> None:
        self.assertEqual(inventory.workflow_tool.effective_shell("", "", ["windows"]), "pwsh")
        self.assertEqual(inventory.workflow_tool.effective_shell("", "", ["linux"]), "bash")
        self.assertEqual(inventory.workflow_tool.effective_shell("bash", "", ["windows"]), "bash")

    def test_dangling_continuation_fails_closed(self) -> None:
        with self.assertRaises(inventory.workflow_tool.WorkflowError):
            inventory.workflow_tool.logical_shell_lines("cmake -B build " + BS, "bash")


class EnvIndirectionTests(unittest.TestCase):
    """Safe env indirection resolves; anything else is an explicit unknown."""

    def parse(self, command: str, env: dict[str, Any], shell: str = "bash"):
        return inventory.workflow_tool.parse_commands(
            command, shell, env, {}, {"job": "j", "step": "s"}
        )

    def test_workflow_env_command_word_is_resolved(self) -> None:
        commands, unresolved = self.parse("$CMAKE_CMD -B build -S . -DBUILD_TESTS=ON", {"CMAKE_CMD": "cmake"})
        self.assertEqual(unresolved, [])
        self.assertEqual(commands[0]["kind"], "configure")
        self.assertEqual(commands[0]["commandProvenance"], "env")

    def test_powershell_env_command_word_is_resolved(self) -> None:
        commands, unresolved = self.parse(
            "& $env:CMAKE -B build -S . -DBUILD_TESTS=ON", {"CMAKE": "cmake"}, "pwsh"
        )
        self.assertEqual(unresolved, [])
        self.assertEqual(commands[0]["kind"], "configure")

    def test_github_env_expression_argument_is_resolved(self) -> None:
        commands, unresolved = self.parse('cmake -S . -B "${{ env.build }}" -G Ninja', {"build": "out/dir"})
        self.assertEqual(unresolved, [])
        self.assertEqual(commands[0]["buildDir"], "out/dir")

    def test_unresolvable_command_word_is_reported_not_dropped(self) -> None:
        commands, unresolved = self.parse("$CMAKE_CMD -B build -S . -DBUILD_TESTS=ON", {})
        self.assertEqual(commands, [])
        self.assertEqual(len(unresolved), 1)
        self.assertIn("cannot resolve", unresolved[0]["reason"])

    def test_unresolvable_invocation_is_blocking(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data["workflow"]["unresolvedInvocations"] = [
            {"job": "j", "step": "s", "reason": "command word is a variable", "command": "$X -B b -S ."}
        ]
        findings = check_parity.check_workflow_semantics(data)
        matching = [item for item in findings if item.category == "workflow-unresolved-invocation"]
        self.assertEqual(len(matching), 1)
        self.assertEqual(matching[0].severity, "error")

    def test_disguised_cmake_invocation_still_fails_closed(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "unparsed"):
            inventory.extract_workflow_cmake_configs_text(
                workflow_document(
                    """
                    bad:
                      steps:
                      - name: Hidden
                        run: echo cmake -B build -S .
                    """
                )
            )


def write_codemodel_reply(
    root: Path,
    *,
    source_dir: str,
    build_dir: str,
    generator: str = "Visual Studio 17 2022",
    architecture: str = "x64",
    toolset: str = "v143",
    configuration: str = "MinSizeRel",
    targets: tuple[tuple[str, str], ...] = (("SparkEngine", "EXECUTABLE"),),
    cache: dict[str, str] | None = None,
) -> Path:
    """Write a real CMake File API reply tree, not a hand-built dict."""
    reply = root / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True, exist_ok=True)
    cmake_producer = root / "cmake-producer.exe"
    cmake_producer.write_bytes(b"synthetic-cmake-producer")
    target_refs = []
    for index, (name, kind) in enumerate(targets):
        target_file = f"target-{index}.json"
        target_id = f"{name}::@synthetic-{index}"
        suffix = {
            "EXECUTABLE": ".exe",
            "STATIC_LIBRARY": ".lib",
            "SHARED_LIBRARY": ".dll",
            "MODULE_LIBRARY": ".dll",
        }.get(kind, "")
        name_on_disk = f"{name}{suffix}"
        target_document: dict[str, Any] = {"name": name, "id": target_id, "type": kind}
        if suffix:
            target_document.update(
                {
                    "nameOnDisk": name_on_disk,
                    "artifacts": [{"path": f"bin/{configuration}/{name_on_disk}"}],
                }
            )
        write_json(reply / target_file, target_document)
        target_refs.append({"name": name, "id": target_id, "jsonFile": target_file})
    write_json(
        reply / "codemodel-v2.json",
        {
            "paths": {"source": source_dir, "build": build_dir},
            "configurations": [{"name": configuration, "targets": target_refs}],
        },
    )
    entries = {
        "CMAKE_GENERATOR": generator,
        "CMAKE_GENERATOR_PLATFORM": architecture,
        "CMAKE_GENERATOR_TOOLSET": toolset,
        "CMAKE_HOME_DIRECTORY": source_dir,
    }
    entries.update(cache or {})
    write_json(
        reply / "cache-v2.json",
        {"entries": [{"name": name, "value": value} for name, value in sorted(entries.items())]},
    )
    write_json(
        reply / "index-0001.json",
        {
            "cmake": {
                "version": {"string": "9.9.9"},
                "paths": {"cmake": cmake_producer.as_posix()},
                "generator": {
                    "name": generator,
                    "platform": architecture,
                    "multiConfig": True,
                },
            },
            "objects": [
                {"kind": "codemodel", "version": {"major": 2}, "jsonFile": "codemodel-v2.json"},
                {"kind": "cache", "version": {"major": 2}, "jsonFile": "cache-v2.json"},
            ],
            "reply": {},
        },
    )
    return root


def write_synthetic_transaction_provenance(
    root: Path,
    profile: str,
    *,
    repository_root: str,
    commit: str = "0" * 40,
) -> None:
    """Complete a synthetic fixture with the v3 client mirror and transaction record."""
    run_id = "1" * 32
    client_name = inventory._CAPTURE_CLIENT_PREFIX + run_id
    query = inventory._capture_query(profile, run_id)
    reply = root / ".cmake" / "api" / "v1" / "reply"
    index_path = reply / "index-0001.json"
    index = json.loads(index_path.read_text(encoding="utf-8"))
    objects = {item["kind"]: item for item in index["objects"]}
    index["reply"] = {
        client_name: {
            "query.json": {
                "client": query["client"],
                "requests": query["requests"],
                "responses": [objects[request["kind"]] for request in query["requests"]],
            }
        }
    }
    write_json(index_path, index)
    evidence, snapshot, records = inventory._extract_reply_core(
        root, profile, client_name=client_name, query=query
    )
    snapshot.close()
    repository = {
        "root": repository_root,
        "commit": commit,
        "clean": True,
        "untrackedPolicy": "all-nonignored",
        "statusSha256": hashlib.sha256(b"").hexdigest(),
    }
    executable = evidence["cmakeProducer"]["executable"]
    configuration = evidence["configurations"][0]
    record = {
        "schemaVersion": inventory._PROVENANCE_SCHEMA,
        "producer": inventory._PROVENANCE_PRODUCER,
        "profile": profile,
        "evidenceDirectory": evidence["evidenceDirectory"],
        "transaction": {
            "runId": run_id,
            "queryClient": client_name,
            "querySha256": hashlib.sha256(
                (json.dumps(query, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
            ).hexdigest(),
            "query": query,
            "profile": profile,
            "preset": profile,
            "configuration": configuration,
            "sourceDirectory": evidence["sourceDirectory"],
            "buildDirectory": evidence["buildDirectory"],
            "configure": {
                "executable": executable,
                "executableIdentity": inventory._executable_identity(Path(executable)),
                "version": evidence["cmakeProducer"]["version"],
                "argv": [executable, "--preset", profile],
                "cwd": repository_root,
                "exitCode": 0,
            },
            "repositoryBefore": repository,
            "repositoryAfter": copy.deepcopy(repository),
        },
        "ci": synthetic_ci_context(commit),
        "observed": {
            "sourceDirectory": evidence["sourceDirectory"],
            "buildDirectory": evidence["buildDirectory"],
            "preset": profile,
            "configuration": configuration,
            "generator": evidence["generator"],
            "architecture": evidence["architecture"],
            "toolset": evidence["toolset"],
            "cacheVariables": evidence["cacheVariables"],
            "cmakeProducer": evidence["cmakeProducer"],
        },
        "artifacts": {"state": "declared-not-built", "build": None, "targets": []},
        "reply": {
            "index": evidence["replyIndex"],
            "files": records,
            "digest": inventory._reply_records_digest(records),
        },
    }
    record["identity"] = inventory._ci120_oidc_identity(record)
    provenance_path = inventory._provenance_path(root, profile)
    provenance_path.parent.mkdir(parents=True, exist_ok=True)
    write_json(provenance_path, record)


class CodemodelProvenanceTests(unittest.TestCase):
    """Configured evidence must be bound to the tree and configuration it claims."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = inventory.build_inventory()
        preset = inventory.resolve_configure_preset(cls.data["cmakePresets"], "windows-shipping")
        cls.shipping_cache = {
            name: str(value) for name, value in preset["cacheVariables"].items()
        }

    def bound_data(self, evidence: dict[str, Any], *, commit: str = "", clean: bool = True) -> dict[str, Any]:
        data = copy.deepcopy(self.data)
        data["repository"] = {
            "root": Path(REPO_ROOT).as_posix(),
            "commit": commit or "0" * 40,
            "clean": clean,
        }
        config = next(
            item for item in data["profile"]["buildConfigurations"]
            if item["id"] == evidence["profile"]
        )
        if config.get("preset") and evidence.get("evidenceDirectory"):
            preset = next(
                item for item in data["cmakePresets"]["configurePresets"]
                if item["name"] == config["preset"]
            )
            preset["binaryDir"] = evidence["evidenceDirectory"]
        data["configuredTargetEvidence"] = [
            evidence if item["profile"] == evidence["profile"] else item
            for item in data["configuredTargetEvidence"]
        ]
        return data

    def shipping_evidence(self, directory: Path, **kwargs: Any) -> dict[str, Any]:
        capture = bool(kwargs.pop("capture", True))
        defaults: dict[str, Any] = {
            "source_dir": Path(REPO_ROOT).as_posix(),
            "build_dir": directory.as_posix(),
            "cache": self.shipping_cache,
        }
        defaults.update(kwargs)
        write_codemodel_reply(directory, **defaults)
        if capture:
            write_synthetic_transaction_provenance(
                directory,
                "windows-shipping",
                repository_root=Path(REPO_ROOT).as_posix(),
            )
        with (
            mock.patch.dict(os.environ, synthetic_ci_environment(), clear=False),
            mock.patch.object(inventory, "_verify_ci120_oidc_identity"),
        ):
            return inventory.extract_codemodel_targets(directory, "windows-shipping", "0" * 40)

    def categories(self, data: dict[str, Any]) -> set[str]:
        return {item.category for item in check_parity.check_codemodel_provenance(data)}

    def test_real_reply_tree_is_parsed_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw))
        self.assertEqual(evidence["status"], "available")
        self.assertEqual(evidence["generator"], "Visual Studio 17 2022")
        self.assertEqual(evidence["toolset"], "v143")
        self.assertEqual(evidence["configurations"], ["MinSizeRel"])
        self.assertEqual(evidence["producerProvenance"]["state"], "verified")
        self.assertEqual(evidence["targets"], [
            {
                "target": "SparkEngine",
                "id": "SparkEngine::@synthetic-0",
                "kind": "executable",
                "configuration": "MinSizeRel",
                "artifactState": "declared-not-built",
                "nameOnDisk": "SparkEngine.exe",
                "artifacts": [(Path(evidence["buildDirectory"]) / "bin/MinSizeRel/SparkEngine.exe").as_posix()],
            }
        ])
        self.assertNotIn("codemodel-source-mismatch", self.categories(self.bound_data(evidence)))

    def test_capture_owns_query_configure_and_new_matching_index(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            trust_root = Path(raw)
            source = trust_root / "source"
            build = source / "build" / "windows-shipping"
            source.mkdir()
            executable = trust_root / "cmake.exe"
            executable.write_bytes(b"synthetic-cmake")
            repository = {
                "root": source.as_posix(),
                "commit": "0" * 40,
                "clean": True,
                "untrackedPolicy": "all-nonignored",
                "statusSha256": hashlib.sha256(b"").hexdigest(),
            }
            config = {
                "id": "windows-shipping",
                "preset": "windows-shipping",
                "configuration": "MinSizeRel",
            }
            configure_calls: list[list[str]] = []
            build_calls: list[list[str]] = []

            def fake_run(arguments: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
                if arguments[1:] == ["--version"]:
                    return subprocess.CompletedProcess(arguments, 0, "cmake version 9.9.9\n", "")
                if arguments[1:2] == ["--build"]:
                    build_calls.append(list(arguments))
                    artifact = build / "bin" / "MinSizeRel" / "SparkEngine.exe"
                    artifact.parent.mkdir(parents=True, exist_ok=True)
                    artifact.write_bytes(b"built-spark-engine")
                    return subprocess.CompletedProcess(arguments, 0, "", "")
                configure_calls.append(list(arguments))
                write_codemodel_reply(
                    build,
                    source_dir=source.as_posix(),
                    build_dir=build.as_posix(),
                    cache=self.shipping_cache,
                )
                query_path = next(
                    (build / ".cmake" / "api" / "v1" / "query").glob("client-*/query.json")
                )
                query = json.loads(query_path.read_text(encoding="utf-8"))
                client_name = query_path.parent.name
                index_path = build / ".cmake" / "api" / "v1" / "reply" / "index-0001.json"
                index = json.loads(index_path.read_text(encoding="utf-8"))
                index["cmake"]["paths"]["cmake"] = executable.as_posix()
                objects = {item["kind"]: item for item in index["objects"]}
                index["reply"] = {
                    client_name: {
                        "query.json": {
                            "client": query["client"],
                            "requests": query["requests"],
                            "responses": [objects[item["kind"]] for item in query["requests"]],
                        }
                    }
                }
                write_json(index_path, index)
                return subprocess.CompletedProcess(arguments, 0, "", "")

            with (
                mock.patch.object(inventory, "REPO_ROOT", source),
                mock.patch.object(
                    inventory,
                    "_capture_plan",
                    return_value=(
                        config,
                        source,
                        build,
                        [executable.as_posix(), "--preset", "windows-shipping"],
                    ),
                ),
                mock.patch.object(inventory, "_repository_provenance", return_value=repository),
                mock.patch.object(inventory, "_capture_material_errors", return_value=[]),
                mock.patch.object(inventory.subprocess, "run", side_effect=fake_run),
                mock.patch.dict(os.environ, synthetic_ci_environment(), clear=False),
                mock.patch.object(inventory, "_verify_ci120_oidc_identity"),
            ):
                record_path = inventory.capture_codemodel_transaction(
                    build, "windows-shipping", cmake_executable=executable, build=True
                )
                evidence = inventory.extract_codemodel_targets(build, "windows-shipping")

            self.assertTrue(record_path.is_file())
            self.assertEqual(
                configure_calls,
                [[executable.as_posix(), "--preset", "windows-shipping"]],
            )
            self.assertEqual(
                build_calls,
                [[
                    executable.as_posix(), "--build", build.as_posix(), "--config", "MinSizeRel", "--parallel"
                ]],
            )
            self.assertEqual(evidence["status"], "available")
            self.assertEqual(evidence["producerProvenance"]["state"], "verified")
            self.assertEqual(evidence["producerProvenance"]["artifactState"], "verified-post-build")
            self.assertEqual(evidence["targets"][0]["artifactState"], "verified-post-build")
            self.assertFalse(
                any((build / ".cmake" / "api" / "v1" / "query").glob("client-*/query.json"))
            )

    def test_capture_rejects_cmake_replacement_during_version_query(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            trust_root = Path(raw)
            source = trust_root / "source"
            build = source / "build" / "windows-shipping"
            source.mkdir()
            executable = trust_root / "cmake.exe"
            executable.write_bytes(b"first-cmake")
            repository = {
                "root": source.as_posix(),
                "commit": "0" * 40,
                "clean": True,
                "untrackedPolicy": "all-nonignored",
                "statusSha256": hashlib.sha256(b"").hexdigest(),
            }
            config = {
                "id": "windows-shipping",
                "preset": "windows-shipping",
                "configuration": "MinSizeRel",
            }

            def fake_run(arguments: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
                if arguments[1:] == ["--version"]:
                    executable.write_bytes(b"other-cmake")
                    return subprocess.CompletedProcess(arguments, 0, "cmake version 9.9.9\n", "")
                raise AssertionError("configure must not run after executable replacement")

            with (
                mock.patch.object(inventory, "REPO_ROOT", source),
                mock.patch.object(
                    inventory,
                    "_capture_plan",
                    return_value=(
                        config,
                        source,
                        build,
                        [executable.as_posix(), "--preset", "windows-shipping"],
                    ),
                ),
                mock.patch.object(inventory, "_repository_provenance", return_value=repository),
                mock.patch.object(inventory.subprocess, "run", side_effect=fake_run),
                mock.patch.dict(os.environ, synthetic_ci_environment(), clear=False),
            ):
                with self.assertRaisesRegex(inventory.InventoryError, "changed while querying"):
                    inventory.capture_codemodel_transaction(
                        build, "windows-shipping", cmake_executable=executable
                    )

    def test_reply_without_cache_object_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            reply = Path(raw) / ".cmake" / "api" / "v1" / "reply"
            reply.mkdir(parents=True)
            write_json(reply / "codemodel-v2.json", {"paths": {}, "configurations": []})
            write_json(
                reply / "index-0001.json",
                {"objects": [{"kind": "codemodel", "version": {"major": 2}, "jsonFile": "codemodel-v2.json"}]},
            )
            evidence = inventory.extract_codemodel_targets(Path(raw), "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("cache-v2", evidence["rejection"])

    def test_foreign_source_tree_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw), source_dir="/some/other/checkout")
        self.assertIn("codemodel-source-mismatch", self.categories(self.bound_data(evidence)))

    def test_arbitrary_build_directory_relabelled_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(
                Path(raw), build_dir="/tmp/some-other-build", capture=False
            )
        self.assertIn("codemodel-build-dir-mismatch", self.categories(self.bound_data(evidence)))

    def test_wrong_generator_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw), generator="Ninja")
        self.assertIn("codemodel-generator-mismatch", self.categories(self.bound_data(evidence)))

    def test_cache_that_contradicts_the_preset_is_rejected(self) -> None:
        cache = dict(self.shipping_cache)
        cache["SPARK_STRICT_DEPS"] = "OFF"
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw), cache=cache)
        self.assertIn("codemodel-cache-mismatch", self.categories(self.bound_data(evidence)))

    def test_wrong_configuration_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw), configuration="Debug")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("preset/configuration", evidence["rejection"])
        self.assertIn(
            "codemodel-provenance-unavailable",
            self.categories(self.bound_data(evidence)),
        )

    def test_fabricated_target_with_no_declaration_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(
                Path(raw), targets=(("SparkEngine", "EXECUTABLE"), ("TotallyInvented", "EXECUTABLE"))
            )
        findings = check_parity.check_codemodel_provenance(self.bound_data(evidence))
        invented = [item for item in findings if item.category == "configured-target-undeclared"]
        self.assertEqual(len(invented), 1)
        self.assertIn("TotallyInvented", invented[0].message)
        self.assertEqual(invented[0].severity, "error")

    def test_caller_asserted_commit_cannot_replace_producer_provenance(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            write_codemodel_reply(
                Path(raw),
                source_dir=Path(REPO_ROOT).as_posix(),
                build_dir=Path(raw).as_posix(),
                cache=self.shipping_cache,
            )
            evidence = inventory.extract_codemodel_targets(
                Path(raw), "windows-shipping", "0" * 40
            )
        self.assertNotIn("assertedSourceCommit", evidence)
        self.assertEqual(evidence["producerProvenance"]["state"], "missing")
        self.assertIn("codemodel-provenance-missing", self.categories(self.bound_data(evidence)))

    def test_commit_mismatch_is_blocking(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw))
        self.assertIn("codemodel-commit-mismatch", self.categories(self.bound_data(evidence, commit="a" * 40)))

    def test_oidc_binding_rejects_post_capture_record_change(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = Path(raw)
            write_codemodel_reply(
                root,
                source_dir=Path(REPO_ROOT).as_posix(),
                build_dir=root.as_posix(),
                cache=self.shipping_cache,
            )
            write_synthetic_transaction_provenance(
                root, "windows-shipping", repository_root=Path(REPO_ROOT).as_posix()
            )
            provenance = inventory._provenance_path(root, "windows-shipping")
            record = json.loads(provenance.read_text(encoding="utf-8"))
            record["observed"]["toolset"] = "forged-v143"
            write_json(provenance, record)
            with (
                mock.patch.dict(os.environ, synthetic_ci_environment(), clear=False),
                mock.patch.object(inventory, "_request_ci120_oidc_token", return_value="synthetic"),
                mock.patch.object(inventory, "_verify_rs256_oidc_token"),
            ):
                evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("does not bind", evidence["rejection"])

    def test_spoofed_github_environment_without_oidc_capability_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = Path(raw)
            write_codemodel_reply(
                root,
                source_dir=Path(REPO_ROOT).as_posix(),
                build_dir=root.as_posix(),
                cache=self.shipping_cache,
            )
            write_synthetic_transaction_provenance(
                root, "windows-shipping", repository_root=Path(REPO_ROOT).as_posix()
            )
            with mock.patch.dict(os.environ, synthetic_ci_environment(), clear=True):
                evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("OIDC request capability", evidence["rejection"])

    def test_dirty_worktree_evidence_is_blocking(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw))
        self.assertIn("codemodel-worktree-dirty", self.categories(self.bound_data(evidence, clean=False)))

    def test_evidence_without_repository_provenance_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            evidence = self.shipping_evidence(Path(raw))
        data = self.bound_data(evidence)
        data.pop("repository")
        with self.assertRaisesRegex(inventory.InventoryError, "repository provenance"):
            check_parity.run_all_checks(data)


class CodemodelReplyBoundaryTests(unittest.TestCase):
    """Hostile reply trees must be bounded, contained, stable, and provenance-bound."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = inventory.build_inventory()
        preset = inventory.resolve_configure_preset(cls.data["cmakePresets"], "windows-shipping")
        cls.shipping_cache = {
            name: str(value) for name, value in preset["cacheVariables"].items()
        }

    def write_complete(self, root: Path) -> Path:
        return write_codemodel_reply(
            root,
            source_dir=Path(REPO_ROOT).as_posix(),
            build_dir=root.as_posix(),
            cache=self.shipping_cache,
        )

    def test_index_jsonfile_cannot_traverse_outside_reply(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            reply = root / ".cmake" / "api" / "v1" / "reply"
            write_json(
                reply / "index-0001.json",
                {
                    "objects": [
                        {
                            "kind": "codemodel",
                            "version": {"major": 2},
                            "jsonFile": "../../../../codemodel-v2.json",
                        },
                        {"kind": "cache", "version": {"major": 2}, "jsonFile": "cache-v2.json"},
                    ]
                },
            )
            evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("plain filename", evidence["rejection"])

    def test_target_jsonfile_cannot_traverse_outside_reply(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            reply = root / ".cmake" / "api" / "v1" / "reply"
            write_json(
                reply / "codemodel-v2.json",
                {
                    "paths": {"source": Path(REPO_ROOT).as_posix(), "build": root.as_posix()},
                    "configurations": [
                        {
                            "name": "MinSizeRel",
                            "targets": [
                                {"name": "SparkEngine", "jsonFile": "../../../../target-0.json"}
                            ],
                        }
                    ],
                },
            )
            evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("plain filename", evidence["rejection"])

    def test_handwritten_reply_with_caller_sha_reports_every_missing_material_class(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            reply = root / ".cmake" / "api" / "v1" / "reply"
            write_json(
                reply / "codemodel-v2.json",
                {
                    "paths": {},
                    "configurations": [
                        {
                            "name": "MinSizeRel",
                            "targets": [{"name": "SparkEngine", "jsonFile": "target-0.json"}],
                        }
                    ],
                },
            )
            write_json(reply / "cache-v2.json", {"entries": []})
            evidence = inventory.extract_codemodel_targets(
                root, "windows-shipping", "0" * 40
            )
        data = copy.deepcopy(self.data)
        data["repository"] = {
            "root": Path(REPO_ROOT).as_posix(),
            "commit": "0" * 40,
            "clean": True,
        }
        data["configuredTargetEvidence"] = [evidence]
        categories = finding_categories(check_parity.check_codemodel_provenance(data))
        self.assertEqual(evidence["status"], "invalid")
        self.assertNotIn("assertedSourceCommit", evidence)
        self.assertIn("no valid id", evidence["rejection"])
        self.assertEqual(categories, {"codemodel-provenance-unavailable"})

    def test_reply_change_after_capture_invalidates_provenance(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            write_synthetic_transaction_provenance(
                root, "windows-shipping", repository_root=Path(REPO_ROOT).as_posix()
            )
            reply = root / ".cmake" / "api" / "v1" / "reply"
            write_json(reply / "target-0.json", {"name": "SparkEngine", "type": "STATIC_LIBRARY"})
            evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertRegex(evidence["rejection"], "content changed|provenance|target")

    def test_posthoc_capture_is_explicitly_forbidden(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "post-hoc provenance capture is forbidden"):
            inventory.capture_codemodel_provenance(Path("unused"), "windows-shipping")

    def test_hardlinked_reply_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            reply = root / ".cmake" / "api" / "v1" / "reply"
            target = reply / "target-0.json"
            outside = root / "outside-target.json"
            target.replace(outside)
            try:
                os.link(outside, target)
            except OSError as error:
                self.skipTest(f"hard links unavailable: {error}")
            evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("hard links", evidence["rejection"])

    def test_symlink_or_reparse_reply_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            reply = root / ".cmake" / "api" / "v1" / "reply"
            target = reply / "target-0.json"
            outside = root / "outside-target.json"
            target.replace(outside)
            try:
                target.symlink_to(outside)
            except OSError as error:
                self.skipTest(f"symlinks unavailable: {error}")
            evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertRegex(evidence["rejection"], "symlink|reparse")

    def test_non_regular_reply_entry_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            target = root / ".cmake" / "api" / "v1" / "reply" / "target-0.json"
            target.unlink()
            target.mkdir()
            evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertIn("not a regular file", evidence["rejection"])

    def test_reply_count_and_file_size_bounds_are_enforced(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            with mock.patch.object(inventory, "_MAX_REPLY_FILES", 3):
                count_evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
            with mock.patch.object(inventory, "_MAX_INDEX_BYTES", 8):
                size_evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(count_evidence["status"], "invalid")
        self.assertIn("above", count_evidence["rejection"])
        self.assertEqual(size_evidence["status"], "invalid")
        self.assertIn("above", size_evidence["rejection"])

    def test_json_depth_and_shape_bounds_are_enforced(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            reply = root / ".cmake" / "api" / "v1" / "reply"
            nested: Any = None
            for _ in range(inventory._MAX_JSON_DEPTH + 2):
                nested = [nested]
            write_json(reply / "cache-v2.json", {"entries": [], "nested": nested})
            depth_evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
            write_json(reply / "index-0001.json", {"objects": {}})
            shape_evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(depth_evidence["status"], "invalid")
        self.assertIn("JSON-depth", depth_evidence["rejection"])
        self.assertEqual(shape_evidence["status"], "invalid")
        self.assertIn("JSON array", shape_evidence["rejection"])

    def test_json_numeric_overflow_and_lone_surrogate_are_rejected(self) -> None:
        with self.assertRaisesRegex(inventory.ReplyValidationError, "non-finite"):
            inventory._decode_bounded_json(b'{"value":1e999}', "probe")
        with self.assertRaisesRegex(inventory.ReplyValidationError, "invalid Unicode"):
            inventory._decode_bounded_json(b'{"value":"\\ud800"}', "probe")

    def test_reply_directory_is_enumerated_once(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            with mock.patch.object(inventory.os, "scandir", wraps=inventory.os.scandir) as scandir:
                evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "available")
        self.assertEqual(scandir.call_count, 1)

    def test_file_replacement_between_snapshot_and_open_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = self.write_complete(Path(raw))
            original_open = inventory.os.open
            replaced = False

            def swapping_open(path: Any, flags: int, *args: Any, **kwargs: Any) -> int:
                nonlocal replaced
                candidate = Path(path)
                if candidate.name == "target-0.json" and not replaced:
                    replaced = True
                    candidate.write_text(
                        json.dumps({"name": "SparkEngine", "type": "STATIC_LIBRARY"}),
                        encoding="utf-8",
                    )
                return original_open(path, flags, *args, **kwargs)

            with mock.patch.object(inventory.os, "open", side_effect=swapping_open):
                evidence = inventory.extract_codemodel_targets(root, "windows-shipping")
        self.assertEqual(evidence["status"], "invalid")
        self.assertRegex(evidence["rejection"], "replaced|changed")

    def test_same_size_reply_rewrite_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            directory = Path(raw)
            path = directory / "x.json"
            path.write_bytes(b'{"v":"AAAA"}')
            metadata = os.lstat(path)
            snapshot = inventory._ReplySnapshot(
                directory,
                directory,
                inventory._directory_stat_token(os.lstat(directory)),
                {
                    "x.json": inventory._ReplyFile(
                        path,
                        inventory._stat_token(metadata),
                        metadata.st_size,
                    )
                },
                {"x.json"},
            )
            path.write_bytes(b'{"v":"BBBB"}')
            with self.assertRaisesRegex(inventory.ReplyValidationError, "changed"):
                snapshot.read_json("x.json", 100, "probe")

    def test_random_atomic_temp_ignores_predictable_hardlink(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            directory = Path(raw)
            baseline = directory / "baseline.json"
            output = directory / "report.json"
            predictable = directory / "report.json.tmp"
            baseline.write_bytes(b"old")
            try:
                os.link(baseline, predictable)
            except OSError as error:
                self.skipTest(f"hard links unavailable: {error}")
            inventory._write_atomic(output, b"new")
            self.assertEqual(baseline.read_bytes(), b"old")
            self.assertEqual(output.read_bytes(), b"new")
            self.assertFalse(os.path.samefile(baseline, output))

    def test_atomic_publication_rejects_same_content_file_substitution(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            directory = Path(raw)
            output = directory / "report.json"
            original_replace = inventory.os.replace
            substituted = False

            def substituting_replace(source: Any, destination: Any) -> None:
                nonlocal substituted
                original_replace(source, destination)
                if not substituted:
                    substituted = True
                    impostor = directory / "impostor.json"
                    impostor.write_bytes(b"new")
                    original_replace(impostor, destination)

            with mock.patch.object(inventory.os, "replace", side_effect=substituting_replace):
                with self.assertRaisesRegex(inventory.InventoryError, "verified temporary file"):
                    inventory._write_atomic(output, b"new")

    @unittest.skipUnless(os.name == "nt", "junction probe is Windows-specific")
    def test_ancestor_junction_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(dir=TEST_TEMP_ROOT) as raw:
            root = Path(raw)
            outside = root / "outside"
            (outside / "build").mkdir(parents=True)
            junction = root / "trusted-root"
            completed = subprocess.run(
                ["cmd", "/c", "mklink", "/J", str(junction), str(outside)],
                capture_output=True,
                text=True,
                check=False,
            )
            if completed.returncode != 0:
                self.skipTest(completed.stderr.strip() or completed.stdout.strip())
            try:
                with self.assertRaisesRegex(inventory.ReplyValidationError, "reparse"):
                    inventory._validate_directory_chain(junction / "build", "probe")
            finally:
                subprocess.run(
                    ["cmd", "/c", "rmdir", str(junction)],
                    capture_output=True,
                    check=True,
                )


class PresetSemanticsTests(unittest.TestCase):
    """CMake's own inheritance and linkage rules, not a convenient approximation."""

    def presets(self, **overrides: Any) -> dict[str, Any]:
        base = {
            "presetsVersion": 6,
            "configurePresets": [
                {"name": "hidden-base", "hidden": True, "binaryDir": "${sourceDir}/build/${presetName}",
                 "cacheVariables": {"BUILD_TESTS": "ON"}},
                {"name": "first", "hidden": True, "generator": "Ninja",
                 "cacheVariables": {"SHARED": "FROM_FIRST", "ONLY_FIRST": "1"}},
                {"name": "second", "hidden": True, "generator": "Visual Studio 17 2022",
                 "cacheVariables": {"SHARED": "FROM_SECOND", "ONLY_SECOND": "1"}},
                {"name": "child", "inherits": ["first", "second", "hidden-base"],
                 "binaryDir": "${sourceDir}/build/${presetName}"},
            ],
            "buildPresets": [{"name": "child", "configurePreset": "child", "hidden": False}],
            "testPresets": [],
            "packagePresets": [],
        }
        base.update(overrides)
        return base

    def test_earlier_parent_wins_per_cmake_semantics(self) -> None:
        resolved = inventory.resolve_configure_preset(self.presets(), "child")
        # cmake-presets(7): "the earlier preset in the inherits array will be preferred".
        self.assertEqual(resolved["cacheVariables"]["SHARED"], "FROM_FIRST")
        self.assertEqual(resolved["generator"], "Ninja")
        self.assertEqual(resolved["cacheVariables"]["ONLY_SECOND"], "1")
        self.assertEqual(resolved["cacheVariables"]["BUILD_TESTS"], "ON")

    def test_non_inherited_fields_are_not_inherited(self) -> None:
        resolved = inventory.resolve_configure_preset(self.presets(), "child")
        self.assertFalse(resolved["hidden"])
        self.assertEqual(resolved["name"], "child")

    def test_string_inherits_is_one_element_list(self) -> None:
        presets = self.presets(
            configurePresets=[
                {"name": "base", "hidden": True, "binaryDir": "b", "cacheVariables": {"A": "1"}},
                {"name": "leaf", "inherits": "base", "binaryDir": "b2"},
            ]
        )
        resolved = inventory.resolve_configure_preset(presets, "leaf")
        self.assertEqual(resolved["cacheVariables"]["A"], "1")

    def test_null_cache_variable_unsets_an_inherited_value(self) -> None:
        presets = self.presets(
            configurePresets=[
                {"name": "base", "hidden": True, "binaryDir": "b", "cacheVariables": {"A": "1"}},
                {"name": "leaf", "inherits": "base", "binaryDir": "b2", "cacheVariables": {"A": None}},
            ]
        )
        self.assertNotIn("A", inventory.resolve_configure_preset(presets, "leaf")["cacheVariables"])

    def test_inheritance_cycle_fails_closed(self) -> None:
        presets = self.presets(
            configurePresets=[
                {"name": "a", "inherits": "b", "binaryDir": "x"},
                {"name": "b", "inherits": "a", "binaryDir": "y"},
            ]
        )
        with self.assertRaisesRegex(inventory.InventoryError, "cycle"):
            inventory.resolve_configure_preset(presets, "a")

    def test_binary_dir_is_inventoried_and_preset_scoped(self) -> None:
        presets = inventory.extract_cmake_presets()
        base = next(item for item in presets["configurePresets"] if item["name"] == "default")
        self.assertEqual(base["binaryDir"], "${sourceDir}/build/${presetName}")
        shipping = inventory.resolve_configure_preset(presets, "windows-shipping")
        # ${presetName} expands in the inheriting preset's context, so twenty
        # presets sharing one inherited string are twenty distinct directories.
        self.assertEqual(shipping["resolvedBinaryDir"], "${sourceDir}/build/windows-shipping")
        release = inventory.resolve_configure_preset(presets, "windows-release")
        self.assertNotEqual(shipping["resolvedBinaryDir"], release["resolvedBinaryDir"])

    def test_live_presets_have_no_binary_dir_collision(self) -> None:
        findings = check_parity.check_preset_binary_dirs(inventory.extract_cmake_presets())
        self.assertEqual(
            [item for item in findings if item.category == "preset-binary-dir-collision"], []
        )

    def test_colliding_binary_dirs_are_blocking(self) -> None:
        presets = self.presets(
            configurePresets=[
                {"name": "one", "binaryDir": "${sourceDir}/build/shared"},
                {"name": "two", "binaryDir": "${sourceDir}/build/shared"},
            ],
            buildPresets=[
                {"name": "one", "configurePreset": "one"},
                {"name": "two", "configurePreset": "two"},
            ],
        )
        findings = check_parity.check_preset_binary_dirs(presets)
        collisions = [item for item in findings if item.category == "preset-binary-dir-collision"]
        self.assertEqual(len(collisions), 1)
        self.assertEqual(collisions[0].severity, "error")

    def test_preset_without_binary_dir_is_blocking(self) -> None:
        presets = self.presets(
            configurePresets=[{"name": "one", "generator": "Ninja"}],
            buildPresets=[{"name": "one", "configurePreset": "one"}],
        )
        findings = check_parity.check_preset_binary_dirs(presets)
        self.assertIn("preset-missing-binary-dir", {item.category for item in findings})

    def test_build_preset_naming_a_phantom_configure_is_blocking(self) -> None:
        presets = self.presets(
            buildPresets=[{"name": "ghost", "configurePreset": "does-not-exist", "hidden": False}]
        )
        findings = check_parity.check_dependent_preset_linkage(presets)
        self.assertIn("dependent-preset-phantom-configure", {item.category for item in findings})
        self.assertTrue(all(item.severity == "error" for item in findings))

    def test_build_preset_naming_a_hidden_configure_is_blocking(self) -> None:
        presets = self.presets(
            buildPresets=[{"name": "hid", "configurePreset": "hidden-base", "hidden": False}]
        )
        findings = check_parity.check_dependent_preset_linkage(presets)
        self.assertIn("dependent-preset-hidden-configure", {item.category for item in findings})

    def test_live_dependent_presets_all_link(self) -> None:
        self.assertEqual(
            check_parity.check_dependent_preset_linkage(inventory.extract_cmake_presets()), []
        )


class OptionActivationTests(unittest.TestCase):
    """A branch the context never takes is not this context's configuration surface."""

    def effective(self, text: str) -> list[dict[str, Any]]:
        declarations = inventory.extract_cmake_options_text(text, "synthetic/CMakeLists.txt")
        return inventory.effective_cmake_options(declarations)

    def test_lone_option_in_inactive_branch_is_not_a_windows_blocker(self) -> None:
        effective = self.effective(
            textwrap.dedent(
                """\
                if(MSVC)
                  add_compile_options(/W4)
                elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                  option(ENABLE_LTO "Link-time optimization" ON)
                endif()
                """
            )
        )
        self.assertEqual(len(effective), 1)
        self.assertEqual(effective[0]["name"], "ENABLE_LTO")
        self.assertEqual(effective[0]["status"], "platform-inactive")
        self.assertEqual(effective[0]["activeDeclarationCount"], 0)
        # It must not reach the SparkBuild comparison at all.
        findings = check_parity.check_sparkbuild_vs_cmake(check_parity._active_options(effective), [], [])
        self.assertEqual(findings, [])

    def test_live_enable_lto_is_platform_inactive(self) -> None:
        options = {item["name"]: item for item in inventory.build_inventory()["cmakeOptions"]}
        self.assertEqual(options["ENABLE_LTO"]["status"], "platform-inactive")
        self.assertEqual(options["ENABLE_LTO"]["declarationCount"], 1)

    def test_active_branch_option_is_still_compared(self) -> None:
        effective = self.effective(
            textwrap.dedent(
                """\
                if(WIN32)
                  option(ENABLE_D3D11 "Direct3D 11" ON)
                endif()
                """
            )
        )
        self.assertEqual(effective[0]["status"], "active")
        findings = check_parity.check_sparkbuild_vs_cmake(check_parity._active_options(effective), [], [])
        self.assertEqual([item.category for item in findings], ["cmake-only"])

    def test_undecidable_condition_is_blocking_not_assumed(self) -> None:
        effective = self.effective(
            textwrap.dedent(
                """\
                if(SOME_PROBED_FEATURE_WE_CANNOT_KNOW)
                  option(ENABLE_MYSTERY "Mystery" ON)
                endif()
                """
            )
        )
        self.assertEqual(effective[0]["status"], "indeterminate")
        findings = check_parity.check_option_resolution(effective)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].category, "option-condition-indeterminate")
        self.assertEqual(findings[0].severity, "error")

    def test_kleene_logic_does_not_collapse_unknowns(self) -> None:
        self.assertIs(inventory._evaluate_condition("UNKNOWN_VAR AND UNIX"), False)
        self.assertIsNone(inventory._evaluate_condition("UNKNOWN_VAR AND WIN32"))
        self.assertIs(inventory._evaluate_condition("UNKNOWN_VAR OR WIN32"), True)
        self.assertIsNone(inventory._evaluate_condition("UNKNOWN_VAR OR UNIX"))
        self.assertIs(inventory._evaluate_condition('CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"'), True)
        self.assertIs(inventory._evaluate_condition("NOT (UNIX OR APPLE)"), True)

    def test_unparseable_condition_raises_rather_than_guessing(self) -> None:
        for expression in ("(WIN32", "NOT", "", "WIN32 STREQUAL"):
            with self.subTest(expression=expression):
                with self.assertRaises(inventory.InventoryError):
                    inventory._evaluate_condition(expression)


class TargetInventoryTests(unittest.TestCase):
    """Module targets are inventoried; templates and unknowns are not invented."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.declarations = inventory.extract_cmake_targets()
        cls.targets = {item["target"]: item for item in inventory.aggregate_cmake_targets(cls.declarations)}

    def test_cmake_module_target_is_inventoried(self) -> None:
        # imgui is declared only in cmake/BuildImGui.cmake and is linked by four
        # executables; a CMakeLists-only scan cannot see it at all.
        self.assertIn("imgui", self.targets)
        entry = self.targets["imgui"]
        self.assertTrue(entry["buildable"])
        self.assertEqual(entry["kind"], "static_library")
        self.assertTrue(all("BuildImGui.cmake" in item["file"] for item in entry["declarations"]))

    def test_component_library_targets_are_inventoried_but_not_claimed_buildable(self) -> None:
        for name in ("SparkCore", "SparkUtils", "SparkECS", "SparkGraphics", "SparkPhysics",
                     "SparkAudio", "SparkAI", "SparkAnimation", "SparkNetworking"):
            self.assertIn(name, self.targets, f"{name} missing from the target inventory")
            entry = self.targets[name]
            self.assertEqual(entry["origins"], ["function-template"])
            self.assertFalse(
                entry["buildable"],
                f"{name} is declared inside an uninvoked function and must not read as a build product",
            )

    def test_wrapper_created_targets_are_resolved(self) -> None:
        for name in ("SparkValidPluginFixture", "SparkForwardMinorPluginFixture"):
            self.assertIn(name, self.targets)
            self.assertTrue(self.targets[name]["buildable"])
            self.assertIn("wrapper-call", self.targets[name]["origins"])

    def test_imported_target_is_not_a_build_product(self) -> None:
        self.assertIn("OpenGL::GL", self.targets)
        self.assertFalse(self.targets["OpenGL::GL"]["buildable"])
        self.assertEqual(self.targets["OpenGL::GL"]["kind"], "imported")

    def test_unresolvable_target_name_is_reported_not_dropped(self) -> None:
        unresolved = [item for item in self.declarations if not item.get("resolved", True)]
        self.assertTrue(unresolved, "an unresolvable target name must be recorded, not skipped")
        findings = check_parity.check_target_declaration_resolution(self.declarations)
        self.assertIn("target-name-unresolved", {item.category for item in findings})

    def test_a_module_only_product_would_be_found(self) -> None:
        # Proves the widened scan is load-bearing: the same declaration seen
        # only through a .cmake module still satisfies a stable-v1 product.
        products = [{"target": "imgui", "kind": "static_library", "buildProfile": "windows-shipping",
                     "applicability": "required"}]
        aggregated = list(self.targets.values())
        self.assertEqual(check_parity.check_stable_v1_targets(aggregated, products), [])


class SharedProductBlockingTests(unittest.TestCase):
    """A shared product is supported surface: missing it may never read clean."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = inventory.build_inventory()

    def test_shared_product_severity_is_error(self) -> None:
        self.assertEqual(check_parity._product_severity({"applicability": "shared"}), "error")
        self.assertEqual(check_parity._product_severity({"applicability": "required"}), "error")

    def test_missing_shared_product_blocks_and_report_is_not_clean(self) -> None:
        products = [
            {"target": "AbsentSharedProduct", "kind": "executable",
             "buildProfile": "windows-shipping", "applicability": "shared"}
        ]
        findings = check_parity.check_stable_v1_targets([], products)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].category, "missing-target")
        self.assertEqual(findings[0].severity, "error")
        report = {
            "errorCount": sum(1 for item in findings if item.severity == "error"),
            "warningCount": sum(1 for item in findings if item.severity == "warning"),
        }
        self.assertGreater(report["errorCount"], 0, "a missing shared product must not read as clean")

    def test_shared_only_profile_missing_evidence_still_blocks(self) -> None:
        data = copy.deepcopy(self.data)
        for product in data["profile"]["buildProducts"]:
            product["applicability"] = "shared"
        data["stableV1Products"] = data["profile"]["buildProducts"]
        findings = check_parity.check_configured_targets(data)
        absent = [item for item in findings if item.category == "configured-evidence-absent"]
        self.assertTrue(absent)
        self.assertTrue(
            all(item.severity == "error" for item in absent),
            "absent evidence for an all-shared profile must stay blocking",
        )

    def test_contract_states_the_shared_product_rule(self) -> None:
        readiness = json.loads((REPO_ROOT / "docs" / "site" / "readiness.json").read_text(encoding="utf-8"))
        rules = " ".join(readiness["statusPromotionRules"]).lower()
        self.assertIn("required and shared products are both blocking", rules)


class SchemaContractTests(unittest.TestCase):
    """The inventory shape the checker depends on is itself checked."""

    def test_schema_version_two_is_rejected(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data["schemaVersion"] = 2
        with self.assertRaisesRegex(inventory.InventoryError, "schemaVersion must be 3"):
            check_parity.run_all_checks(data)

    def test_missing_workflow_record_is_rejected(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data.pop("workflow")
        with self.assertRaisesRegex(inventory.InventoryError, "required fields"):
            check_parity.run_all_checks(data)

    def test_truncated_workflow_record_is_rejected(self) -> None:
        data = copy.deepcopy(inventory.build_inventory())
        data["workflow"].pop("buildInvocations")
        with self.assertRaisesRegex(inventory.InventoryError, "workflow record lacks fields"):
            check_parity.run_all_checks(data)

    def test_duplicate_yaml_key_is_rejected(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "duplicate YAML key"):
            workflow_record(
                workflow_document(
                    """
                    lane:
                      steps:
                      - name: One
                        run: cmake -B a -S .
                      steps:
                      - name: Two
                        run: cmake -B b -S .
                    """
                )
            )

    def test_yaml_anchor_is_refused(self) -> None:
        with self.assertRaises(inventory.InventoryError):
            workflow_record(
                "name: x\non:\n  push:\n    branches: [ Working ]\njobs:\n  a:\n    runs-on: &anchor ubuntu-24.04\n"
            )

    def test_oversized_workflow_document_is_refused(self) -> None:
        oversized = "a: 1\n" + "# pad\n" * 900000
        with self.assertRaisesRegex(inventory.workflow_tool.WorkflowError, "byte bound"):
            inventory.workflow_tool.parse_workflow_yaml(oversized)

    def test_deeply_nested_flow_collection_is_refused(self) -> None:
        document = "name: x\non: [push]\njobs:\n  a:\n    runs-on: " + "[" * 40 + "]" * 40 + "\n"
        with self.assertRaisesRegex(inventory.workflow_tool.WorkflowError, "nested deeper"):
            inventory.workflow_tool.parse_workflow_yaml(document)

    def test_duplicate_key_in_a_flow_mapping_is_refused(self) -> None:
        document = "name: x\non: [push]\njobs:\n  a:\n    runs-on: { os: a, os: b }\n"
        with self.assertRaisesRegex(inventory.workflow_tool.WorkflowError, "duplicate YAML key"):
            inventory.workflow_tool.parse_workflow_yaml(document)

    def test_tab_indentation_is_refused(self) -> None:
        with self.assertRaisesRegex(inventory.workflow_tool.WorkflowError, "tab in YAML indentation"):
            inventory.workflow_tool.parse_workflow_yaml("jobs:\n\ta: 1\n")

    def test_cache_reply_entry_bound_is_enforced(self) -> None:
        oversized = {"entries": [{"name": f"SPARK_{index}", "value": "1"} for index in range(5000)]}
        with self.assertRaisesRegex(inventory.InventoryError, "above the"):
            inventory._bound_cache_entries(oversized, "windows-shipping")


class OrchestrationTests(unittest.TestCase):
    """run_all_checks must actually run every check, not merely define it.

    A check that exists but is never called from the orchestrator is the same
    as no check at all, and unit-testing the function directly cannot see that.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = inventory.build_inventory()

    def categories_for(self, data: dict[str, Any]) -> set[str]:
        return {item.category for item in check_parity.run_all_checks(data)}

    def test_dependent_preset_linkage_runs_in_the_report(self) -> None:
        data = copy.deepcopy(self.data)
        data["cmakePresets"]["buildPresets"].append(
            {"name": "ghost", "configurePreset": "does-not-exist", "hidden": False}
        )
        self.assertIn("dependent-preset-phantom-configure", self.categories_for(data))

    def test_codemodel_provenance_runs_in_the_report(self) -> None:
        data = copy.deepcopy(self.data)
        data["repository"] = {"root": Path(REPO_ROOT).as_posix(), "commit": "b" * 40, "clean": True}
        data["configuredTargetEvidence"] = [
            {
                "profile": "windows-shipping",
                "status": "available",
                "evidenceDirectory": "/somewhere/else",
                "sourceDirectory": "/some/other/checkout",
                "buildDirectory": "/somewhere/else",
                "generator": "Ninja",
                "architecture": "",
                "toolset": "",
                "cacheVariables": {"SPARK_STRICT_DEPS": "OFF"},
                "configurations": ["Debug"],
                "replyDigest": "f" * 64,
                "producerProvenance": {"state": "missing"},
                "targets": [{"target": "TotallyInvented", "kind": "executable", "configuration": "Debug"}],
            }
            if item["profile"] == "windows-shipping"
            else item
            for item in data["configuredTargetEvidence"]
        ]
        categories = self.categories_for(data)
        for expected in (
            "codemodel-source-mismatch",
            "codemodel-build-dir-mismatch",
            "codemodel-generator-mismatch",
            "codemodel-cache-mismatch",
            "codemodel-configuration-missing",
            "codemodel-provenance-missing",
            "configured-target-undeclared",
        ):
            self.assertIn(expected, categories, f"{expected} is defined but never reaches the report")

    def test_workflow_semantics_run_in_the_report(self) -> None:
        categories = self.categories_for(copy.deepcopy(self.data))
        self.assertIn("workflow-configuration-not-built", categories)
        self.assertIn("workflow-shipping-runner-os", categories)

    def test_option_resolution_and_target_resolution_run_in_the_report(self) -> None:
        categories = self.categories_for(copy.deepcopy(self.data))
        self.assertIn("target-name-unresolved", categories)

    def test_report_state_is_derived_from_error_severity_only(self) -> None:
        report = check_parity.build_report(copy.deepcopy(self.data))
        self.assertEqual(report["errorCount"] + report["warningCount"], len(report["findings"]))
        self.assertEqual(report["state"], "blocked" if report["errorCount"] else "clean")

if __name__ == "__main__":
    unittest.main(verbosity=2)
