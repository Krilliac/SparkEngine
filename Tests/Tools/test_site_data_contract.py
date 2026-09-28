#!/usr/bin/env python3
"""Fail-closed tests for release-profile scope, dependencies, and publication.

All negative cases operate on deep-copied contract data or pure strings.  This
suite must never edit a tracked repository file while it is running.
"""

from __future__ import annotations

import ast
import copy
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import assets as site_data_assets  # noqa: E402
import common as site_data_common  # noqa: E402
from common import SiteDataError, load_contract  # noqa: E402
import contract_selectors  # noqa: E402
import exact_evidence  # noqa: E402
import generate as site_data_generate  # noqa: E402
import render_handoff  # noqa: E402
import validate as site_data_validate  # noqa: E402


HANDOFF_PATH = REPO_ROOT / "docs" / "readiness" / "ENGINE_READINESS_HANDOFF.md"
GENERATOR_PATH = REPO_ROOT / "tools" / "site-data" / "generate.py"
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "site-data.yml"
TEST_PATH = Path(__file__).resolve()


class ContractTestCase(unittest.TestCase):
    """Load once and provide mutation helpers shared by the frozen cases."""

    contract: dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_contract()

    def setUp(self) -> None:
        self.mutable = copy.deepcopy(self.contract)

    @staticmethod
    def profile_of(contract: dict[str, Any]) -> dict[str, Any]:
        return contract["readiness"]["releaseProfiles"][0]

    @staticmethod
    def items_of(contract: dict[str, Any]) -> dict[str, dict[str, Any]]:
        return {item["id"]: item for item in contract["workItems"]}

    @staticmethod
    def gates_of(contract: dict[str, Any]) -> dict[str, dict[str, Any]]:
        return {gate["id"]: gate for gate in contract["readiness"]["gates"]}

    @staticmethod
    def capabilities_of(contract: dict[str, Any]) -> dict[str, dict[str, Any]]:
        return {
            capability["id"]: capability
            for capability in contract["readiness"]["capabilities"]
        }

    def assert_rejected(self, contract: dict[str, Any], fragment: str) -> None:
        with self.assertRaises(SiteDataError) as raised:
            site_data_validate.Validator(contract).validate()
        self.assertIn(fragment, str(raised.exception))

    def promote_ready(self, contract: dict[str, Any]) -> None:
        """Create a valid ready mutation without closing excluded work or gates."""
        profile = self.profile_of(contract)
        profile["state"] = "ready"
        profile["owner"] = "release-engineering"
        profile["signOffEvidence"] = [
            {"label": "Test-only release sign-off", "path": "README.md"}
        ]
        included = set(profile["includedCapabilityIds"])
        for capability in contract["readiness"]["capabilities"]:
            if capability["id"] in included:
                capability["release"] = "ready"
        required = set(profile["requiredGateIds"])
        for gate in contract["readiness"]["gates"]:
            if gate["id"] in required:
                gate["state"] = "passing"
        declared = set(profile["blockingWorkItemIds"])
        finalizers = set(profile["publicationFinalization"]["workItemIds"])
        for item in contract["workItems"]:
            if item["id"] in declared:
                item["status"] = "done"
                reference = PUBLICATION_CI_REFERENCE if item["id"] in finalizers else EXACT_CI_REFERENCE
                for entry in item["acceptanceStatus"]:
                    entry["state"] = "evidenced"
                    entry["evidence"] = ["README.md", reference]
                if item["id"] == "GOV-400":
                    contract["content"]["legal"]["policyGaps"] = []
        contract["readiness"]["execution"]["firstUnblockedWorkItemId"] = None
        contract["readiness"]["globalRelease"]["state"] = "ready"
        # REL-192: N-1 work (now done above) presupposes a published predecessor.
        contract["readiness"]["predecessorRelease"]["state"] = "published"

    @staticmethod
    def item_text(item: dict[str, Any], *fields: str) -> str:
        values: list[str] = []
        for field in fields:
            value = item.get(field, [])
            values.extend(value if isinstance(value, list) else [str(value)])
        return " ".join(values).lower()


EXACT_CI_REFERENCE = "ci:build.yml/1@" + "0" * 40
PUBLICATION_CI_REFERENCE = "ci:release.yml/1@" + "0" * 40

# ENG-220 certifies only the D3D11 path; each experimental backend's parity is
# owned by its own rendering work item (backend name, accepted title words).
BACKEND_PARITY_OWNERS = {
    "RHI-220": ("metal", ("parity",)),
    "RHI-225": ("d3d12", ("parity",)),
    "RHI-230": ("vulkan", ("parity",)),
    "RHI-240": ("opengl", ("visual", "parity")),
}
ENG_220_PARITY_EXCLUSION = "Certifying D3D12, Vulkan, OpenGL, or Metal parity"


def backend_parity_owner_errors(contract: dict[str, Any]) -> list[str]:
    """Return every way experimental backend parity has lost its RHI owner."""
    items = {item["id"]: item for item in contract["workItems"]}
    errors: list[str] = []
    for owner_id, (backend, title_words) in BACKEND_PARITY_OWNERS.items():
        owner = items.get(owner_id)
        if owner is None:
            errors.append(f"{owner_id}: {backend} parity owner is missing")
            continue
        title = str(owner.get("title", "")).lower()
        if backend not in title or not any(word in title for word in title_words):
            errors.append(f"{owner_id}: title no longer names {backend} parity")
        if owner.get("profileApplicability", {}).get("stable-v1") != "outside":
            errors.append(f"{owner_id}: {backend} parity must stay outside stable-v1")
        if owner.get("area") != "rendering":
            errors.append(f"{owner_id}: {backend} parity owner must be a rendering item")
        if "ENG-220" not in owner.get("dependencies", []):
            errors.append(f"{owner_id}: must depend on ENG-220")
    engine = items.get("ENG-220")
    if engine is None:
        errors.append("ENG-220: missing")
        return errors
    if ENG_220_PARITY_EXCLUSION not in engine.get("outOfScope", []):
        errors.append("ENG-220: outOfScope must exclude experimental backend parity")
    missing_parallel = sorted(set(BACKEND_PARITY_OWNERS) - set(engine.get("parallelWith", [])))
    if missing_parallel:
        errors.append(f"ENG-220: parallelWith is missing {missing_parallel}")
    return errors


class AcceptanceStatusTests(ContractTestCase):
    """acceptanceStatus tracks every criterion and never outruns the item status."""

    def entries(self, item_id: str) -> list[dict[str, Any]]:
        return self.items_of(self.mutable)[item_id]["acceptanceStatus"]

    def test_every_work_item_tracks_every_criterion(self) -> None:
        for item in self.contract["workItems"]:
            with self.subTest(item=item["id"]):
                self.assertEqual(len(item["acceptanceStatus"]), len(item["acceptanceCriteria"]))
                for criterion, entry in zip(item["acceptanceCriteria"], item["acceptanceStatus"]):
                    self.assertEqual(entry["criterionDigest"], site_data_common.criterion_digest(criterion))

    def test_length_mismatch_is_rejected(self) -> None:
        self.entries("CI-100").pop()
        self.assert_rejected(self.mutable, "workItems.CI-100.acceptanceStatus")

    def test_reworded_criterion_requires_reassessment(self) -> None:
        item = self.items_of(self.mutable)["CI-100"]
        item["acceptanceCriteria"][0] += " (reworded)"
        self.assert_rejected(self.mutable, "criterionDigest does not match acceptanceCriteria[0]")

    def test_progress_requires_real_repository_evidence(self) -> None:
        entry = self.entries("CI-100")[0]
        cases = (
            ([], "must cite the committed test or check"),
            ([EXACT_CI_REFERENCE], "must cite the committed test or check"),
            (["Tests/DoesNotExist.cpp"], "referenced path does not exist"),
        )
        for evidence, fragment in cases:
            with self.subTest(evidence=evidence):
                mutated = copy.deepcopy(self.mutable)
                target = self.items_of(mutated)["CI-100"]
                target["status"] = "in-progress"
                target["acceptanceStatus"][0] = {**entry, "state": "implemented", "evidence": evidence}
                self.assert_rejected(mutated, fragment)

    def test_evidenced_requires_a_well_formed_exact_commit_ci_reference(self) -> None:
        for evidence, fragment in (
            (["README.md"], "must cite the exact-commit CI run"),
            (["README.md", "ci:build.yml/latest@main"], "CI evidence must look like"),
        ):
            with self.subTest(evidence=evidence):
                mutated = copy.deepcopy(self.mutable)
                target = self.items_of(mutated)["CI-100"]
                target["status"] = "in-progress"
                target["acceptanceStatus"][0].update(state="evidenced", evidence=evidence)
                self.assert_rejected(mutated, fragment)

    def test_open_item_cannot_record_progress(self) -> None:
        target = self.items_of(self.mutable)["CI-100"]
        target["status"] = "open"
        target["acceptanceStatus"][0].update(state="implemented", evidence=["README.md"])
        self.assert_rejected(self.mutable, "an open work item has no implemented or evidenced criteria")

    def test_done_item_needs_every_criterion_evidenced(self) -> None:
        target = self.items_of(self.mutable)["CI-100"]
        target["status"] = "done"
        for entry in target["acceptanceStatus"]:
            entry.update(state="evidenced", evidence=["README.md", EXACT_CI_REFERENCE])
        target["acceptanceStatus"][-1].update(state="implemented", evidence=["README.md"])
        self.assert_rejected(self.mutable, "a done work item must have every acceptance criterion evidenced")

    def test_fully_evidenced_done_item_passes_the_acceptance_rules(self) -> None:
        target = self.items_of(self.mutable)["CI-100"]
        target["status"] = "done"
        for entry in target["acceptanceStatus"]:
            entry.update(state="evidenced", evidence=["README.md", EXACT_CI_REFERENCE])
        validator = site_data_validate.Validator(self.mutable)
        validator.validate_acceptance_status(target, "workItems.CI-100")
        self.assertEqual(validator.errors, [])


class ReleaseProfileShapeTests(ContractTestCase):
    """Frozen case 1: the one intended stable product shape is exact."""

    def test_repository_contract_validates(self) -> None:
        # Strict: the legacy-contract waiver is retired, so this must pass with
        # no downgrade of unresolvable CI job, test selector or path references.
        site_data_validate.Validator(copy.deepcopy(self.contract)).validate()

    def test_stable_v1_shape_is_exact(self) -> None:
        profile = self.profile_of(self.mutable)
        values = {dimension["id"]: dimension["value"] for dimension in profile["scope"]}
        self.assertEqual(profile["supportedHosts"], ["Windows 11 x64"])
        self.assertIn("Windows 11 x64 only", values["host"])
        self.assertIn("MSVC v143", values["toolchain"])
        self.assertIn("Direct3D 11", values["renderer"])
        self.assertIn("NullRHI on Windows 11 x64", values["headless"])
        self.assertIn("renders nothing", values["headless"])
        self.assertIn("not software rendering", values["headless"])
        self.assertIn("C++", values["gameplay"])
        self.assertIn("single-player", values["firstPartyGame"])
        self.assertIn("public SDK", values["firstPartyGame"])
        self.assertIn("installed", values["product"])
        self.assertEqual(profile["firstPartyGameCapabilityIds"], ["modules.fps"])

    def test_toolchain_line_is_honestly_unpinned(self) -> None:
        profile = self.profile_of(self.mutable)
        toolchain = next(value for value in profile["scope"] if value["id"] == "toolchain")
        self.assertIn("not pinned", toolchain["value"])
        limitations = " ".join(profile["limitations"]).lower()
        self.assertIn("no exact msvc compiler build", limitations)
        self.assertIn("windows sdk version", limitations)

    def test_optional_lan_is_not_a_product_requirement(self) -> None:
        profile = self.profile_of(self.mutable)
        first_party = next(value for value in profile["scope"] if value["id"] == "firstPartyGame")
        self.assertIn("optional", first_party["value"].lower())
        self.assertIn("not required", first_party["value"].lower())
        self.assertNotIn("networking.multiplayer", profile["includedCapabilityIds"])

    def test_installed_consumer_has_canonical_source_and_build_directories(self) -> None:
        profile = self.profile_of(self.mutable)
        consumer = next(
            item for item in profile["buildConfigurations"]
            if item["purpose"] == "installed-sdk-consumer"
        )
        self.assertEqual(consumer["sourceDirectory"], "Tests/PackageSmoke")
        self.assertEqual(consumer["buildDirectory"], "build/installed-sdk-consumer")

    def test_installed_consumer_without_safe_directories_is_rejected(self) -> None:
        missing = copy.deepcopy(self.mutable)
        missing_consumer = next(
            item for item in self.profile_of(missing)["buildConfigurations"]
            if item["purpose"] == "installed-sdk-consumer"
        )
        missing_consumer.pop("sourceDirectory")
        self.assert_rejected(missing, "sourceDirectory must be a safe relative path")

        unsafe = copy.deepcopy(self.mutable)
        unsafe_consumer = next(
            item for item in self.profile_of(unsafe)["buildConfigurations"]
            if item["purpose"] == "installed-sdk-consumer"
        )
        unsafe_consumer["buildDirectory"] = "../outside"
        self.assert_rejected(unsafe, "buildDirectory must be a safe relative path")


class ClassificationCoverageTests(ContractTestCase):
    """Frozen case 2: every capability and gate is classified exactly once."""

    def test_every_capability_is_classified_exactly_once(self) -> None:
        profile = self.profile_of(self.mutable)
        classified = [
            *profile["includedCapabilityIds"],
            *profile["boundaries"]["experimentalCapabilityIds"],
            *profile["boundaries"]["unsupportedCapabilityIds"],
        ]
        declared = [value["id"] for value in self.mutable["readiness"]["capabilities"]]
        self.assertCountEqual(classified, declared)
        self.assertEqual(len(classified), len(set(classified)))

    def test_missing_and_duplicate_capability_classification_fail_closed(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["boundaries"]["experimentalCapabilityIds"].pop()
        self.assert_rejected(self.mutable, "unclassified capabilities")

        duplicate = copy.deepcopy(self.contract)
        profile = self.profile_of(duplicate)
        profile["boundaries"]["experimentalCapabilityIds"].append(
            profile["includedCapabilityIds"][0]
        )
        self.assert_rejected(duplicate, "classified more than once")

    def test_every_gate_is_required_or_excluded_with_a_reason(self) -> None:
        profile = self.profile_of(self.mutable)
        classified = [
            *profile["requiredGateIds"],
            *(entry["gateId"] for entry in profile["excludedGates"]),
        ]
        declared = [value["id"] for value in self.mutable["readiness"]["gates"]]
        self.assertCountEqual(classified, declared)
        self.assertTrue(all(entry["reason"] for entry in profile["excludedGates"]))

        profile["requiredGateIds"].remove("G17")
        self.assert_rejected(self.mutable, "gates neither required nor explicitly excluded")

    def test_capability_support_must_match_its_classification(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["boundaries"]["experimentalCapabilityIds"].remove("platform.linux")
        profile["includedCapabilityIds"].append("platform.linux")
        self.assert_rejected(self.mutable, "contradicts its profile classification")


class WorkItemApplicabilityTests(ContractTestCase):
    """Frozen case 3: applicability is explicit, complete, and not ID-inferred."""

    def test_every_work_item_classifies_every_profile(self) -> None:
        profile_ids = {
            profile["id"] for profile in self.mutable["readiness"]["releaseProfiles"]
        }
        for item in self.mutable["workItems"]:
            with self.subTest(work_item=item["id"]):
                self.assertEqual(set(item["profileApplicability"]), profile_ids)
                self.assertLessEqual(
                    set(item["profileApplicability"].values()),
                    {"required", "shared", "outside"},
                )

    def test_profile_blockers_exactly_equal_required_and_shared_items(self) -> None:
        profile = self.profile_of(self.mutable)
        applicable = {
            item["id"]
            for item in self.mutable["workItems"]
            if item["profileApplicability"][profile["id"]] in {"required", "shared"}
        }
        self.assertEqual(set(profile["blockingWorkItemIds"]), applicable)

    def test_named_broad_items_have_explicit_profile_scope(self) -> None:
        expected = {
            "ENG-220": "required",
            "HEAD-220": "required",
            "RDY-010": "required",
            "RDY-020": "required",
            "MOD-290": "shared",
            "BLD-100": "required",
            "CI-120": "required",
            "REL-100": "required",
            "REL-190": "shared",
            "REL-200": "shared",
            "INST-130": "required",
            "SEC-120": "required",
            "PERF-100": "required",
        }
        items = self.items_of(self.mutable)
        for work_id, applicability in expected.items():
            with self.subTest(work_item=work_id):
                self.assertEqual(
                    items[work_id]["profileApplicability"]["stable-v1"],
                    applicability,
                )

    def test_missing_metadata_and_outside_declaration_fail_closed(self) -> None:
        del self.items_of(self.mutable)["MOD-310"]["profileApplicability"]["stable-v1"]
        self.assert_rejected(self.mutable, "must classify every release profile exactly once")

        leaked = copy.deepcopy(self.contract)
        self.profile_of(leaked)["blockingWorkItemIds"].append("NET-100")
        self.assert_rejected(leaked, "explicitly outside this profile")

    def test_split_experimental_work_stays_open_owned_and_scheduled(self) -> None:
        items = self.items_of(self.mutable)
        capabilities = self.capabilities_of(self.mutable)
        gates = self.gates_of(self.mutable)
        execution_ids = {
            value
            for wave in self.mutable["readiness"]["execution"]["waves"]
            for value in wave["workItemIds"]
        }
        anchors = {
            "RDY-015": capabilities["modules.prototypes"]["blockingWorkItemIds"],
            "MOD-295": capabilities["modules.prototypes"]["blockingWorkItemIds"],
            "MOD-315": capabilities["networking.multiplayer"]["blockingWorkItemIds"],
        }
        for work_id, anchor in anchors.items():
            with self.subTest(work_item=work_id):
                self.assertEqual(items[work_id]["profileApplicability"]["stable-v1"], "outside")
                self.assertNotEqual(items[work_id]["status"], "done")
                self.assertIn(work_id, anchor)
                self.assertIn(work_id, execution_ids)
        self.assertIn("MOD-315", gates["G12"]["blockingWorkItemIds"])

    def test_all_outside_work_remains_open_and_anchored(self) -> None:
        profile = self.profile_of(self.mutable)
        items = self.items_of(self.mutable)
        anchors: dict[str, set[str]] = {}
        for capability in self.mutable["readiness"]["capabilities"]:
            for work_id in capability["blockingWorkItemIds"]:
                anchors.setdefault(work_id, set()).add(capability["id"])
        for gate in self.mutable["readiness"]["gates"]:
            for work_id in gate["blockingWorkItemIds"]:
                anchors.setdefault(work_id, set()).add(gate["id"])
        outside = {
            work_id
            for work_id, item in items.items()
            if item["profileApplicability"][profile["id"]] == "outside"
        }
        for work_id in outside:
            with self.subTest(work_item=work_id):
                self.assertNotEqual(items[work_id]["status"], "done")
                self.assertTrue(anchors.get(work_id), f"{work_id} has lost all ownership anchors")
                self.assertNotIn(work_id, profile["blockingWorkItemIds"])

    def test_executable_ctest_work_item_commands_fail_closed(self) -> None:
        hostile_forms = (
            "if ctest --output-on-failure; then echo ok; fi",
            "env CTEST_OUTPUT_ON_FAILURE=1 ctest --output-on-failure",
            "(ctest --output-on-failure)",
            "! ctest --output-on-failure",
            "time ctest --output-on-failure",
            '"C:/Program Files/CMake/bin/ctest.exe" --output-on-failure',
        )
        for command in hostile_forms:
            with self.subTest(command=command):
                self.assertEqual(
                    len(site_data_validate.executable_ctest_segments(command)),
                    1,
                )

        rdy_010 = self.items_of(self.mutable)["RDY-010"]
        command_index = next(
            index
            for index, command in enumerate(rdy_010["commands"])
            if command.startswith("ctest ")
        )
        original_command = rdy_010["commands"][command_index]
        before_flag, separator, after_flag = original_command.partition(
            " --no-tests=error"
        )
        self.assertTrue(separator, "fixture must contain the fail-on-empty flag")
        rdy_010["commands"][command_index] = before_flag + after_flag
        self.assert_rejected(
            self.mutable,
            "executable CTest commands must include --no-tests=error",
        )

        discovery = copy.deepcopy(self.contract)
        self.items_of(discovery)["RDY-010"]["commands"].append(
            "ctest --show-only=json-v1"
        )
        site_data_validate.Validator(discovery, allow_legacy_contract=True).validate()

        masked = copy.deepcopy(self.contract)
        self.items_of(masked)["RDY-010"]["commands"].append(
            "ctest --show-only=json-v1 && ctest -R ModuleProfileLifecycle"
        )
        self.assert_rejected(
            masked,
            "executable CTest commands must include --no-tests=error",
        )

        for smuggled_command in (
            "ctest --no-tests=error\nctest -R MissingFlag",
            "echo --no-tests=error $(ctest -R MissingFlag)",
        ):
            with self.subTest(smuggled_command=smuggled_command):
                smuggled = copy.deepcopy(self.contract)
                self.items_of(smuggled)["RDY-010"]["commands"].append(
                    smuggled_command
                )
                self.assert_rejected(
                    smuggled,
                    "executable CTest commands must include --no-tests=error",
                )


class WorkItemPresetResolutionTests(ContractTestCase):
    """Work-item commands must name presets and build trees CMakePresets.json defines."""

    def preset_errors(self, identifier: str, commands: list[str]) -> list[str]:
        validator = site_data_validate.Validator(self.mutable)
        uses: dict[str, set[str]] = {}
        validator.validate_work_item_presets(identifier, commands, f"workItems.{identifier}", uses)
        return validator.errors

    def test_live_work_item_commands_resolve(self) -> None:
        for item in self.contract["workItems"]:
            with self.subTest(work_item=item["id"]):
                self.assertEqual(self.preset_errors(item["id"], item["commands"]), [])

    def test_ctest_against_tests_off_preset_is_rejected(self) -> None:
        for command in (
            "ctest --test-dir build/windows-shipping -L profile-package --output-on-failure --no-tests=error",
            "ctest --test-dir build/linux-shipping -L unit --output-on-failure --no-tests=error",
            "ctest --test-dir=build/minimal/Tests --output-on-failure --no-tests=error",
            '"C:/Program Files/CMake/bin/ctest.exe" --test-dir build/windows-shipping --no-tests=error',
            "ctest --show-only=json-v1 && ctest --test-dir build/linux-shipping --no-tests=error",
        ):
            with self.subTest(command=command):
                errors = self.preset_errors("RDY-020", [command])
                self.assertEqual(len(errors), 1, errors)
                self.assertIn("sets BUILD_TESTS=OFF", errors[0])

    def test_tests_off_preset_is_excused_only_by_same_item_configure(self) -> None:
        run = "ctest --test-dir build/windows-shipping -C MinSizeRel -L module-profile --no-tests=error"
        self.assertEqual(
            self.preset_errors("PLT-200", ["cmake --preset windows-shipping -DBUILD_TESTS=ON", run]), []
        )
        self.assertEqual(
            self.preset_errors("PLT-200", ["cmake --preset windows-shipping -D BUILD_TESTS:BOOL=TRUE", run]), []
        )
        for configure in (
            "cmake --preset windows-shipping",
            "cmake --preset windows-shipping -DBUILD_TESTS=OFF",
            "cmake --preset windows-release -DBUILD_TESTS=ON",
        ):
            with self.subTest(configure=configure):
                errors = self.preset_errors("PLT-200", [configure, run])
                self.assertTrue(any("sets BUILD_TESTS=OFF" in error for error in errors), errors)

        rdy_010 = self.items_of(self.mutable)["RDY-010"]
        rdy_010["commands"] = [
            " ".join(token for token in command.split(" ") if token != "-DBUILD_TESTS=ON")
            for command in rdy_010["commands"]
        ]
        self.assert_rejected(self.mutable, "sets BUILD_TESTS=OFF")

    def test_unknown_presets_and_build_trees_are_rejected(self) -> None:
        cases = (
            ("cmake --preset linux-asan", "--preset 'linux-asan' names no configure preset"),
            ("cmake --build --preset linux-asan", "--preset 'linux-asan' names no build preset"),
            ("ctest --preset linux-gcc-release --no-tests=error", "--preset 'linux-gcc-release' names no test preset"),
            ("ctest --test-dir build/linux-asan -L lifecycle --no-tests=error", "build tree 'build/linux-asan'"),
            ("cmake --build build/linux-tsan", "build tree 'build/linux-tsan'"),
            ("cmake --install build/nope --prefix /tmp/x", "build tree 'build/nope'"),
            ("cmake -LAH -N build/nope", "build tree 'build/nope'"),
            ("cmake --build ./build/nope/sub", "build tree 'build/nope'"),
        )
        for command, fragment in cases:
            with self.subTest(command=command):
                errors = self.preset_errors("LIFE-200", [command])
                self.assertEqual(len(errors), 1, errors)
                self.assertIn(fragment, errors[0])

    def test_existing_presets_resolve_through_inheritance(self) -> None:
        self.assertEqual(
            self.preset_errors(
                "LIFE-200",
                [
                    "ctest --test-dir build/ci-linux-asan -L lifecycle --output-on-failure --no-tests=error",
                    "ctest --preset default --no-tests=error",
                    "cmake --build --preset windows-shipping --config MinSizeRel",
                    "cmake --install build/linux-shipping --prefix /tmp/spark-install",
                    "ctest --test-dir /tmp/spark-consumer --no-tests=error",
                ],
            ),
            [],
        )
        index = contract_selectors.cmake_preset_index()
        self.assertTrue(index.builds_tests("ci-linux-asan"))
        self.assertFalse(index.builds_tests("windows-shipping"))

    def test_inherited_tests_off_is_resolved(self) -> None:
        presets = {
            "configurePresets": [
                {"name": "base", "hidden": True, "binaryDir": "${sourceDir}/build/${presetName}",
                 "cacheVariables": {"BUILD_TESTS": "ON"}},
                {"name": "off", "hidden": True, "cacheVariables": {"BUILD_TESTS": {"type": "BOOL", "value": "OFF"}}},
                {"name": "child", "inherits": ["off", "base"]},
                {"name": "reset", "inherits": "child", "cacheVariables": {"BUILD_TESTS": None}},
                {"name": "plain", "inherits": "base"},
            ],
            "testPresets": [{"name": "child-tests", "configurePreset": "child"}],
        }
        index = contract_selectors.CMakePresetIndex(presets)
        self.assertEqual(index.binary_dirs["build/child"], "child")
        self.assertNotIn("build/base", index.binary_dirs)
        self.assertFalse(index.builds_tests("child"), "earlier inherits entry must win")
        self.assertTrue(index.builds_tests("reset"), "null unsets back to the option default")
        self.assertTrue(index.builds_tests("plain"))
        self.assertEqual(
            index.configure_for(contract_selectors.PresetReference("ctest", "test", "child-tests")), "child"
        )

    def test_planned_preset_is_owner_scoped_and_prunes_itself(self) -> None:
        # A fictitious preset: the mechanism must not depend on a live planned entry.
        planned = {"macos-notarized": "PLT-220"}
        with mock.patch.object(site_data_validate, "PLANNED_CMAKE_PRESETS", planned):
            self.assertEqual(
                self.preset_errors(
                    "PLT-220", ["cmake --preset macos-notarized", "cmake --build build/macos-notarized"]
                ),
                [],
            )
            for identifier, command in (
                ("RHI-220", "cmake --preset macos-notarized"),
                ("PLT-220", "ctest --test-dir build/macos-notarized -L metal --no-tests=error"),
            ):
                with self.subTest(identifier=identifier, command=command):
                    self.assertEqual(len(self.preset_errors(identifier, [command])), 1)

        items = self.items_of(self.mutable)
        uses = {"macos-notarized": {"PLT-220"}}
        cases = (
            ({"linux-gcc-release": "PLT-220"}, "preset now exists in CMakePresets.json"),
            ({"macos-notarized": "NOPE-000"}, "owner NOPE-000 is not a work item"),
            ({"macos-notarized": "RHI-220"}, "owner RHI-220 no longer references this preset"),
        )
        for entries, fragment in cases:
            with self.subTest(planned=entries):
                validator = site_data_validate.Validator(self.mutable)
                with mock.patch.object(site_data_validate, "PLANNED_CMAKE_PRESETS", entries):
                    validator.validate_planned_presets(items, uses)
                self.assertTrue(any(fragment in error for error in validator.errors), validator.errors)

        done = copy.deepcopy(items)
        done["PLT-220"]["status"] = "done"
        validator = site_data_validate.Validator(self.mutable)
        with mock.patch.object(site_data_validate, "PLANNED_CMAKE_PRESETS", planned):
            validator.validate_planned_presets(done, uses)
        self.assertTrue(any("owner PLT-220 is done" in error for error in validator.errors), validator.errors)

        validator = site_data_validate.Validator(self.mutable)
        with mock.patch.object(site_data_validate, "PLANNED_CMAKE_PRESETS", planned):
            validator.validate_planned_presets(items, uses)
        self.assertEqual(validator.errors, [])

    def test_macos_shipping_preset_resolves_for_its_owner(self) -> None:
        # PLT-220 promoted macos-shipping from a planned entry to a real Darwin preset.
        self.assertNotIn("macos-shipping", site_data_validate.PLANNED_CMAKE_PRESETS)
        index = contract_selectors.cmake_preset_index()
        self.assertTrue(index.exists("configure", "macos-shipping"))
        self.assertTrue(index.exists("build", "macos-shipping"))
        self.assertFalse(index.builds_tests("macos-shipping"))
        self.assertEqual(
            self.preset_errors(
                "RHI-220", ["cmake --preset macos-shipping", "cmake --build build/macos-shipping"]
            ),
            [],
        )

    def shipping_errors(self, contract: dict[str, Any], extra_presets: tuple[str, ...] = ()) -> list[str]:
        presets = {*contract_selectors.cmake_preset_index().names["configure"], *extra_presets}
        return site_data_validate.experimental_shipping_preset_errors(contract, presets)

    def assert_shipping_error(self, errors: list[str], fragment: str) -> None:
        self.assertTrue(any(fragment in error for error in errors), errors)

    def test_experimental_shipping_presets_stay_platform_owned(self) -> None:
        self.assertEqual(self.shipping_errors(self.mutable), [])

        self.assert_shipping_error(
            self.shipping_errors(self.mutable, ("android-shipping",)),
            "experimentalShipping.android-shipping: Shipping preset is outside stable-v1",
        )

        promoted = copy.deepcopy(self.contract)
        self.profile_of(promoted)["buildConfigurations"].append(
            {"id": "linux-shipping", "preset": "linux-shipping", "configuration": "Release", "purpose": "shipping"}
        )
        self.assert_shipping_error(
            self.shipping_errors(promoted), "experimentalShipping.linux-shipping: experimental Shipping preset is in"
        )

        unowned = copy.deepcopy(self.contract)
        platform = self.items_of(unowned)["PLT-210"]
        platform["commands"] = [command for command in platform["commands"] if "--preset linux-shipping" not in command]
        self.assert_shipping_error(
            self.shipping_errors(unowned), "owner PLT-210 must configure the preset in its own commands"
        )

        required = copy.deepcopy(self.contract)
        self.items_of(required)["PLT-220"]["profileApplicability"]["stable-v1"] = "required"
        self.assert_shipping_error(self.shipping_errors(required), "owner PLT-220 must not be required by stable-v1")

        stranger = copy.deepcopy(self.contract)
        self.items_of(stranger)["HEAD-220"]["commands"].append("cmake --preset macos-shipping")
        self.assert_shipping_error(
            self.shipping_errors(stranger), "HEAD-220 configures an experimental Shipping preset owned by PLT-220"
        )

        # The rule runs inside work-item validation, not only as a helper.
        validator = site_data_validate.Validator(promoted)
        validator.validate_work_items()
        self.assert_shipping_error(validator.errors, "experimental Shipping preset is in stable-v1 build scope")


class LegalContractConsistencyTests(ContractTestCase):
    """GOV-400 legal data stays explicit while its policy work is open."""

    @staticmethod
    def license_of(contract: dict[str, Any]) -> dict[str, Any]:
        return contract["content"]["legal"]["license"]

    def test_current_license_declaration_cannot_be_relabelled(self) -> None:
        mutations = (
            ("name", "MIT", "content.legal.license.name"),
            ("kind", "OSI-approved open-source license", "content.legal.license.kind"),
            ("osiApproved", True, "content.legal.license.osiApproved"),
        )
        for field, value, location in mutations:
            with self.subTest(field=field):
                mutated = copy.deepcopy(self.contract)
                self.license_of(mutated)[field] = value
                self.assert_rejected(mutated, location)

    def test_open_gov_400_requires_non_empty_unique_string_policy_gaps(self) -> None:
        for policy_gaps in ([], ["same gap", "same gap"], ["valid gap", 7], [""]):
            with self.subTest(policy_gaps=policy_gaps):
                mutated = copy.deepcopy(self.contract)
                mutated["content"]["legal"]["policyGaps"] = policy_gaps
                self.assert_rejected(mutated, "content.legal.policyGaps")

        missing = copy.deepcopy(self.contract)
        missing["content"]["legal"].pop("policyGaps")
        self.assert_rejected(missing, "content.legal.policyGaps")

    def test_done_gov_400_rejects_remaining_policy_gaps(self) -> None:
        mutated = copy.deepcopy(self.contract)
        self.items_of(mutated)["GOV-400"]["status"] = "done"
        self.assert_rejected(mutated, "GOV-400 cannot be done while policyGaps remain")


class LegalPublicWordingTests(ContractTestCase):
    """Public legal wording cannot outrun the reviewed license declaration."""

    def test_non_osi_license_accepts_reviewed_source_available_project_wording(self) -> None:
        validator = site_data_validate.Validator(copy.deepcopy(self.contract))
        validator.validate(legal=True)

        errors = site_data_validate.legal_public_wording_errors(
            self.contract["content"]["legal"]["license"],
            {"README.md": "A C++23 open-source game engine."},
        )
        self.assertEqual(1, len(errors))
        self.assertIn("unreviewed open-source wording", errors[0])
        self.assertIn("README.md", errors[0])

    def test_negated_wording_is_allowed_for_explaining_the_distinction(self) -> None:
        errors = site_data_validate.legal_public_wording_errors(
            {"osiApproved": False},
            {"legal.md": "This project is not open-source under an OSI-approved license."},
        )
        self.assertEqual([], errors)

    def test_osi_approved_declaration_does_not_apply_the_custom_license_guard(self) -> None:
        errors = site_data_validate.legal_public_wording_errors(
            {"osiApproved": True},
            {"README.md": "A C++23 open-source game engine."},
        )
        self.assertEqual([], errors)


class OnlineServiceBoundaryTests(unittest.TestCase):
    """OD-08 / NET-110: public surfaces never claim hosted online services."""

    def claims(self, text: str) -> list[str]:
        return site_data_validate.hosted_online_service_claim_errors({"page.md": text})

    def test_hosted_service_claims_are_rejected(self) -> None:
        for text in (
            "SparkEngine provides hosted matchmaking for every game.",
            "Built-in leaderboards and managed cloud saves out of the box.",
            "| Hosted identity | Included |",
            "The engine operates a billing service for store purchases.",
            "SparkEngine offers an entitlement backend.",
            "Turnkey online services let you ship multiplayer today.",
        ):
            with self.subTest(text=text):
                errors = self.claims(text)
                self.assertEqual(1, len(errors), errors)
                self.assertIn("page.md:1: claims hosted online services", errors[0])
                self.assertIn("OD-08", errors[0])

    def test_negated_boundary_statements_are_allowed(self) -> None:
        for text in (
            "The engine ships no hosted online services.",
            "SparkEngine does not provide hosted matchmaking, fleet, or billing.",
            "Identity, matchmaking, fleet, entitlement and billing services are out of engine scope.",
            "| Hosted leaderboards | Not provided; the product owns them |",
            "SparkDaemon is not a managed fleet.",
        ):
            with self.subTest(text=text):
                self.assertEqual([], self.claims(text))

    def test_negation_in_another_sentence_does_not_excuse_a_claim(self) -> None:
        errors = self.claims("Accounts are not stored in plaintext. SparkEngine hosts an identity service.")
        self.assertEqual(1, len(errors), errors)

    def test_engine_interfaces_are_not_service_claims(self) -> None:
        text = (
            "IOnlinePlatform is an integration point. NullOnlinePlatform keeps cloud-save slots in memory. "
            "SparkGateway authenticates admission credentials issued by the product."
        )
        self.assertEqual([], self.claims(text))

    def test_non_text_surface_is_rejected(self) -> None:
        errors = site_data_validate.hosted_online_service_claim_errors({"page.md": None})  # type: ignore[dict-item]
        self.assertEqual(["page.md: online-service wording source must be text"], errors)

    def test_repository_surfaces_carry_no_hosted_service_claim(self) -> None:
        surfaces: dict[str, str] = {}
        for surface in sorted(
            site_data_validate.REQUIRED_GLOBAL_PUBLIC_CLAIM_SURFACES
            | site_data_validate.ONLINE_SERVICE_BOUNDARY_SURFACES
        ):
            path = site_data_validate.REPO_ROOT / surface
            if path.is_file():
                surfaces[surface] = path.read_text(encoding="utf-8", errors="replace")
        for surface in site_data_validate.ONLINE_SERVICE_BOUNDARY_SURFACES:
            self.assertIn(surface, surfaces)
        self.assertEqual([], site_data_validate.hosted_online_service_claim_errors(surfaces))

    # -- Specification content contract (NET-110 criterion 1) -----------------

    SPEC_TEXT = (REPO_ROOT / "docs" / "specs" / "online-services.md").read_text(encoding="utf-8")

    def spec_errors(self, text: str, source_paths: list[str] | None = None) -> list[str]:
        # An empty source list keeps the adapter scan out of cases about the text.
        return site_data_validate.online_service_spec_contract_errors(
            text, REPO_ROOT, [] if source_paths is None else source_paths
        )

    def mutated_spec(self, old: str, new: str) -> str:
        start = self.SPEC_TEXT.find(old)
        self.assertGreaterEqual(start, 0, old)
        return self.SPEC_TEXT[:start] + new + self.SPEC_TEXT[start + len(old):]

    @staticmethod
    def create_temporary_file(root: Path, relative: str, content: str) -> None:
        """Create a new file under a temporary root; mode "x" can never overwrite an existing file."""
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8") as stream:
            stream.write(content)

    def test_spec_contract_passes_on_the_live_repository(self) -> None:
        self.assertEqual(
            [], site_data_validate.online_service_spec_contract_errors(self.SPEC_TEXT, REPO_ROOT)
        )

    def test_spec_diagram_boundary_missing_from_table_is_rejected(self) -> None:
        row = next(line for line in self.SPEC_TEXT.splitlines() if line.startswith("| B9 |"))
        errors = self.spec_errors(self.mutated_spec(row + "\n", ""))
        self.assertIn(
            "docs/specs/online-services.md: diagram boundary B9 has no row in the trust-boundary table", errors
        )

    def test_spec_table_boundary_missing_from_diagram_is_rejected(self) -> None:
        errors = self.spec_errors(self.mutated_spec('Match -- "B9: placement" --> Gateway', "Match --> Gateway"))
        self.assertIn(
            "docs/specs/online-services.md: trust boundary B9 is not drawn on the deployment diagram", errors
        )

    def test_spec_boundary_with_empty_trust_owner_is_rejected(self) -> None:
        errors = self.spec_errors(
            self.mutated_spec("| Device → product | Product identity service |", "| Device → product |  |")
        )
        self.assertIn(
            "docs/specs/online-services.md: trust boundary B2 must name who is trusted and the enforcing mechanism",
            errors,
        )

    def test_spec_non_contiguous_boundary_ids_are_rejected(self) -> None:
        errors = self.spec_errors(self.mutated_spec("| B7 | Supervisor", "| B17 | Supervisor"))
        self.assertTrue(any("must run contiguously from B1" in error for error in errors), errors)

    def test_spec_without_mermaid_diagram_is_rejected(self) -> None:
        errors = self.spec_errors(self.mutated_spec("```mermaid", "```text"))
        self.assertIn("docs/specs/online-services.md: section 3 must contain a ```mermaid deployment diagram", errors)

    def test_spec_stale_interface_path_is_rejected(self) -> None:
        errors = self.spec_errors(
            self.mutated_spec("`SparkEngine/Source/Utils/PasswordHash.h`", "`SparkEngine/Source/Utils/Removed.h`")
        )
        self.assertIn(
            "docs/specs/online-services.md: engine-interface source 'SparkEngine/Source/Utils/Removed.h' "
            "does not exist",
            errors,
        )

    def test_spec_interface_symbol_missing_from_its_source_is_rejected(self) -> None:
        errors = self.spec_errors(self.mutated_spec("`Spark::PasswordHash`", "`Spark::PasswordVault`"))
        self.assertIn(
            "docs/specs/online-services.md: engine interface 'PasswordVault' is not found in "
            "'SparkEngine/Source/Utils/PasswordHash.h'",
            errors,
        )

    def test_spec_production_adapter_label_is_rejected(self) -> None:
        errors = self.spec_errors(
            self.mutated_spec("| `SteamPlatform` | B1 | **stub** |", "| `SteamPlatform` | B1 | **production** |")
        )
        self.assertTrue(any("adapter SteamPlatform has label 'production'" in error for error in errors), errors)

    def test_spec_unregistered_adapter_class_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.create_temporary_file(
                root,
                "Engine/LanMatchmaker.h",
                "// class Mentioned : public ITransport in a comment is not a declaration\n"
                "namespace Spark\n{\n    class LanMatchmaker final\n        : public IOnlinePlatform\n    {\n    };\n}\n",
            )
            errors = site_data_validate.online_service_spec_contract_errors(
                self.SPEC_TEXT, root, ["Engine/LanMatchmaker.h"]
            )
        adapter_errors = [error for error in errors if "adapter register" in error]
        self.assertEqual(
            [
                "docs/specs/online-services.md: LanMatchmaker (Engine/LanMatchmaker.h) implements IOnlinePlatform "
                "but is not in the section 6 adapter register"
            ],
            adapter_errors,
        )

    # -- Local stores are never production infrastructure (NET-110 criterion 4) --

    def local_store_claims(self, text: str) -> list[str]:
        return site_data_validate.local_store_production_claim_errors({"page.md": text})

    def test_local_store_production_claims_are_rejected(self) -> None:
        for text in (
            "TFDatabase is a production-grade backend.",
            "The MMOFPS persistence layer is battle-tested.",
            "SparkGateway is production-ready infrastructure for your launch.",
            "Use the JSON store as a production database.",
            "| TFDatabase | Production-ready JSON store |",
        ):
            with self.subTest(text=text):
                errors = self.local_store_claims(text)
                self.assertEqual(1, len(errors), errors)
                self.assertIn("page.md:1: markets", errors[0])
                self.assertIn("NET-110", errors[0])

    def test_negated_or_unrelated_production_wording_is_allowed(self) -> None:
        for text in (
            "TFDatabase is not production infrastructure.",
            "| TFDatabase | Local JSON store; never a production database |",
            "SparkDaemon is a single-host reference, not a production-grade fleet.",
            "Star Citizen's replication layer is the production-grade example.",
            "TFDatabase keeps accounts in a JSON file.",
        ):
            with self.subTest(text=text):
                self.assertEqual([], self.local_store_claims(text))

    def test_local_store_claim_in_another_sentence_is_still_rejected(self) -> None:
        errors = self.local_store_claims("Passwords are never stored in plaintext. TFDatabase is production-grade.")
        self.assertEqual(1, len(errors), errors)

    def test_local_store_surfaces_cover_module_descriptions_and_skip_quoted_criteria(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = {
                "GameModules/Demo/module.json": json.dumps(
                    {"name": "Demo", "description": "Battle-tested MMO persistence for live games"}
                ),
                "GameModules/Demo/README.md": "Demo module.\n",
                "docs/site/readiness.json": json.dumps({"rows": [{"text": "AsyncDatabase is enterprise-grade"}]}),
                "docs/readiness/work-items/10-net.json": "No local JSON/demo service is production-ready infrastructure",
                "docs/readiness/ENGINE_READINESS_HANDOFF.md": "TFDatabase is production-grade.\n",
                "SparkEngine/Source/Notes.md": "TFDatabase is production-grade.\n",
            }
            for relative, content in files.items():
                self.create_temporary_file(root, relative, content)
            surfaces = site_data_validate.local_store_claim_surfaces(root, files)
        self.assertEqual(
            {"GameModules/Demo/module.json#description", "GameModules/Demo/README.md", "docs/site/readiness.json"},
            set(surfaces),
        )
        errors = site_data_validate.local_store_production_claim_errors(surfaces)
        self.assertEqual(2, len(errors), errors)
        self.assertTrue(errors[0].startswith("GameModules/Demo/module.json#description:1: markets"), errors)
        self.assertTrue(errors[1].startswith("docs/site/readiness.json:1: markets 'AsyncDatabase'"), errors)

    def test_adapter_reporting_itself_as_production_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.create_temporary_file(
                root,
                "Engine/Adapter.h",
                "class ProductionSteam final : public IOnlinePlatform\n{\n"
                "    std::string GetPlatformName() const override { return \"Steam (Production)\"; }\n"
                "    std::string GetLastError() const override { return \"stub\"; }\n};\n",
            )
            errors = site_data_validate.adapter_name_production_errors(root, ["Engine/Adapter.h"])
        self.assertEqual(1, len(errors), errors)
        self.assertTrue(errors[0].startswith("Engine/Adapter.h:3: adapter reports 'Steam (Production)'"), errors)

    def test_repository_markets_no_local_store_or_adapter_as_production(self) -> None:
        surfaces = site_data_validate.local_store_claim_surfaces(REPO_ROOT)
        self.assertIn("README.md", surfaces)
        self.assertIn("docs/specs/online-services.md", surfaces)
        self.assertNotIn("docs/readiness/ENGINE_READINESS_HANDOFF.md", surfaces)
        self.assertEqual([], site_data_validate.local_store_production_claim_errors(surfaces))
        self.assertEqual([], site_data_validate.adapter_name_production_errors(REPO_ROOT))


class InstallerPlatformOwnershipTests(ContractTestCase):
    """INST-130: experimental platform installers stay owned by their platform work."""

    def installer_product(self, contract: dict[str, Any]) -> dict[str, Any]:
        return next(
            product
            for product in self.profile_of(contract)["buildProducts"]
            if product["target"] == "SparkInstaller"
        )

    def test_repository_contract_keeps_installer_ownership(self) -> None:
        self.assertEqual([], site_data_validate.installer_platform_ownership_errors(self.contract))

    def test_required_non_windows_installer_build_is_rejected(self) -> None:
        product = self.installer_product(self.mutable)
        product["buildProfile"] = "linux-gcc-release"
        errors = site_data_validate.installer_platform_ownership_errors(self.mutable)
        self.assertTrue(
            any("non-Windows installer build 'linux-gcc-release' cannot be required" in e for e in errors),
            errors,
        )
        self.assertTrue(any("not platform.windows" in e for e in errors), errors)
        self.assert_rejected(self.mutable, "installerPlatformOwnership")

    def test_non_windows_installer_package_owned_by_windows_is_rejected(self) -> None:
        self.profile_of(self.mutable)["buildProducts"].append(
            {
                "target": "SparkInstaller-macOS",
                "kind": "package",
                "buildProfile": "macos-release",
                "applicability": "outside",
                "capabilityIds": ["platform.windows"],
                "requiredOptions": {},
            }
        )
        errors = site_data_validate.installer_platform_ownership_errors(self.mutable)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("must be owned by platform.linux or platform.macos", errors[0])

    def test_experimental_platform_owned_installer_is_accepted(self) -> None:
        self.profile_of(self.mutable)["buildProducts"].append(
            {
                "target": "SparkInstaller-Linux",
                "kind": "installer",
                "buildProfile": "linux-gcc-release",
                "applicability": "outside",
                "capabilityIds": ["platform.linux"],
                "requiredOptions": {},
            }
        )
        self.assertEqual([], site_data_validate.installer_platform_ownership_errors(self.mutable))

    def test_stable_installer_work_naming_macos_installer_is_rejected(self) -> None:
        item = self.items_of(self.mutable)["INST-130"]
        item["implementationScope"].append("Certify the notarized macOS installer package")
        errors = site_data_validate.installer_platform_ownership_errors(self.mutable)
        self.assertTrue(
            any(e.startswith("workItems.INST-130.implementationScope[") and "PLT-*" in e for e in errors),
            errors,
        )
        self.assert_rejected(self.mutable, "stable-v1 installer work names a non-Windows installer")

    def test_predecessor_only_installer_work_is_not_stable_scope(self) -> None:
        item = self.items_of(self.mutable)["INST-132"]
        self.assertEqual("outside", item["profileApplicability"]["stable-v1"])
        item["implementationScope"].append("Linux bootstrap notes")
        self.assertEqual([], site_data_validate.installer_platform_ownership_errors(self.mutable))

    def test_platform_capability_without_plt_work_is_rejected(self) -> None:
        capability = self.capabilities_of(self.mutable)["platform.macos"]
        capability["blockingWorkItemIds"] = [
            item for item in capability["blockingWorkItemIds"] if not item.startswith("PLT-")
        ]
        errors = site_data_validate.installer_platform_ownership_errors(self.mutable)
        self.assertEqual(
            ["capabilities.platform.macos: owns an experimental installer but lists no PLT-* blocking work item"],
            errors,
        )

    def test_stable_profile_including_linux_installer_owner_is_rejected(self) -> None:
        self.profile_of(self.mutable)["includedCapabilityIds"].append("platform.linux")
        errors = site_data_validate.installer_platform_ownership_errors(self.mutable)
        self.assertIn(
            "releaseProfiles.stable-v1.includedCapabilityIds: platform.linux owns an experimental "
            "installer and cannot be part of stable-v1",
            errors,
        )


class TransitiveDependencyTests(ContractTestCase):
    """Frozen case 4: profile dependency closure is transitive and diagnostic."""

    def test_current_profile_dependency_closure_never_enters_outside_work(self) -> None:
        profile = self.profile_of(self.mutable)
        items = self.items_of(self.mutable)
        stack = [(work_id, [work_id]) for work_id in profile["blockingWorkItemIds"]]
        while stack:
            current, path = stack.pop()
            for dependency in items[current]["dependencies"]:
                dependency_path = [*path, dependency]
                with self.subTest(path=" -> ".join(dependency_path)):
                    self.assertNotEqual(
                        items[dependency]["profileApplicability"][profile["id"]],
                        "outside",
                    )
                if dependency not in path:
                    stack.append((dependency, dependency_path))

    def test_mod_310_to_net_100_reports_the_full_dependency_path(self) -> None:
        self.items_of(self.mutable)["MOD-310"]["dependencies"].append("NET-100")
        self.assert_rejected(self.mutable, "MOD-310 -> NET-100")

    def test_ready_rejects_an_unfinished_transitive_dependency(self) -> None:
        self.promote_ready(self.mutable)
        self.items_of(self.mutable)["RDY-020"]["status"] = "open"
        self.assert_rejected(self.mutable, "unfinished transitive dependencies")

    def close_blocker_over_open_dependency(self) -> None:
        """DOC-410 done while its dependency RDY-000 is still open."""
        items = self.items_of(self.mutable)
        self.assertIn("RDY-000", items["DOC-410"]["dependencies"])
        items["DOC-410"]["status"] = "done"
        items["RDY-000"]["status"] = "open"

    def validation_errors(self) -> str:
        try:
            site_data_validate.Validator(self.mutable).validate()
        except SiteDataError as error:
            return str(error)
        return ""

    def test_ready_capability_rejects_an_unfinished_transitive_dependency(self) -> None:
        self.close_blocker_over_open_dependency()
        capability = self.capabilities_of(self.mutable)["platform.console"]
        capability["release"] = "ready"
        capability["requiredGateIds"] = []
        capability["blockingWorkItemIds"] = ["DOC-410"]
        self.assert_rejected(
            self.mutable,
            "capabilities.platform.console: ready capability has unfinished "
            "transitive dependencies: DOC-410 -> RDY-000",
        )
        # Control: once the dependency closes, the diagnostic disappears.
        self.items_of(self.mutable)["RDY-000"]["status"] = "done"
        self.assertNotIn("transitive dependencies", self.validation_errors())

    def test_passing_gate_rejects_an_unfinished_transitive_dependency(self) -> None:
        self.close_blocker_over_open_dependency()
        gate = self.gates_of(self.mutable)["G00"]
        gate["state"] = "passing"
        gate["blockingWorkItemIds"] = ["DOC-410"]
        self.assert_rejected(
            self.mutable,
            "gates.G00: passing gate has unfinished transitive dependencies: DOC-410 -> RDY-000",
        )
        self.items_of(self.mutable)["RDY-000"]["status"] = "done"
        self.assertNotIn("transitive dependencies", self.validation_errors())


class ReadyPromotionTests(ContractTestCase):
    """Frozen case 5: ready derives from profiles, not every ledger gate."""

    def test_excluded_gates_and_work_may_remain_open_when_ready(self) -> None:
        self.promote_ready(self.mutable)
        gates = self.gates_of(self.mutable)
        items = self.items_of(self.mutable)
        self.assertEqual(gates["G11"]["state"], "blocked")
        self.assertEqual(gates["G12"]["state"], "blocked")
        self.assertNotEqual(items["MOD-315"]["status"], "done")
        self.assertNotEqual(items["NET-100"]["status"], "done")
        site_data_validate.Validator(self.mutable, allow_legacy_contract=True).validate(require_ready=True)

    def test_a_required_gate_still_blocks_ready(self) -> None:
        self.promote_ready(self.mutable)
        self.gates_of(self.mutable)["G09"]["state"] = "blocked"
        self.assert_rejected(self.mutable, "ready profile has non-passing gates")

    def test_global_ready_requires_every_declared_profile_ready(self) -> None:
        self.mutable["readiness"]["globalRelease"]["state"] = "ready"
        self.assert_rejected(
            self.mutable,
            "global ready requires every declared release profile to be ready",
        )

    def test_global_state_cannot_lag_when_every_profile_is_ready(self) -> None:
        self.promote_ready(self.mutable)
        self.mutable["readiness"]["globalRelease"]["state"] = "blocked"
        self.assert_rejected(
            self.mutable,
            "global release must be ready when every declared release profile is ready",
        )

    def test_ready_requires_assigned_owner_and_signoff_evidence(self) -> None:
        self.promote_ready(self.mutable)
        self.profile_of(self.mutable)["owner"] = "unassigned"
        self.assert_rejected(self.mutable, "ready profile requires an assigned owner")

        unsigned = copy.deepcopy(self.contract)
        self.promote_ready(unsigned)
        self.profile_of(unsigned)["signOffEvidence"] = []
        self.assert_rejected(unsigned, "ready profile requires sign-off evidence")

    def test_global_ready_requires_same_commit_publication_evidence(self) -> None:
        self.assertEqual(site_data_validate.publication_workflows(), {"release.yml"})
        self.assertEqual(site_data_validate.publication_evidence_errors(self.mutable), [])
        self.promote_ready(self.mutable)
        self.assertEqual(site_data_validate.publication_evidence_errors(self.mutable), [])

        def finalizer(contract: dict[str, Any]) -> dict[str, Any]:
            return self.items_of(contract)["REL-200"]

        def build_only(contract: dict[str, Any]) -> None:
            for entry in finalizer(contract)["acceptanceStatus"]:
                entry["evidence"] = ["README.md", EXACT_CI_REFERENCE]

        def split_commits(contract: dict[str, Any]) -> None:
            finalizer(contract)["acceptanceStatus"][-1]["evidence"] = [
                "README.md",
                "ci:release.yml/2@" + "1" * 40,
            ]

        def no_publication_job(contract: dict[str, Any]) -> None:
            finalizer(contract)["requiredCiJobs"].remove("verify-stable-publication")

        def gate_not_passing(contract: dict[str, Any]) -> None:
            self.gates_of(contract)["G17"]["state"] = "blocked"

        cases = {
            "build-workflow-only": (build_only, "must be evidenced by a ci:<release.yml>/<run>@<sha>"),
            "two-commits": (split_commits, "publication evidence cites different commits"),
            "no-publication-job": (no_publication_job, "requiredCiJobs must include verify-stable-publication"),
            "g17-not-passing": (gate_not_passing, "finalization gate G17 must be passing with evidence"),
        }
        for name, (mutate, fragment) in cases.items():
            with self.subTest(mutation=name):
                contract = copy.deepcopy(self.mutable)
                mutate(contract)
                errors = site_data_validate.publication_evidence_errors(contract)
                self.assertTrue(any(fragment in error for error in errors), errors)
                if name == "build-workflow-only":
                    # The full validator runs the rule, so --require-ready cannot pass without it.
                    self.assert_rejected(contract, fragment)

    def test_ready_bundle_deploy_runs_the_require_ready_gate(self) -> None:
        try:
            import yaml  # noqa: PLC0415
        except ImportError:
            self.skipTest("PyYAML is not installed")
        workflow = yaml.safe_load(
            (REPO_ROOT / ".github" / "workflows" / "site-data-publish.yml").read_text(encoding="utf-8")
        )
        deploy = next(
            step["run"]
            for step in workflow["jobs"]["site-data-publish"]["steps"]
            if 'git init "$PUBLISH_REPO"' in step.get("run", "")
        )
        gate = deploy.index('validate.py" --require-ready')
        self.assertLess(gate, deploy.index('git init "$PUBLISH_REPO"'))
        self.assertIn('["globalRelease"]["state"] != "ready"', deploy[:gate])
        self.assertLess(deploy.index("--require-exact-evidence"), gate)


class ScopeNarrowingTests(ContractTestCase):
    """Frozen case 6: broad work is split or bounded to stable-v1."""

    def test_fps_singleplayer_and_optional_multiplayer_are_split(self) -> None:
        items = self.items_of(self.mutable)
        stable = items["MOD-310"]
        experimental = items["MOD-315"]
        self.assertNotIn("NET-100", stable["dependencies"])
        self.assertFalse(any("multiplayer" in value.lower() for value in stable["testSelectors"]))
        self.assertIn("No multiplayer result is required for stable-v1", stable["acceptanceCriteria"])
        self.assertIn("LAN or public multiplayer", stable["outOfScope"])
        self.assertEqual(experimental["dependencies"], ["MOD-310", "NET-100"])
        self.assertEqual(experimental["profileApplicability"]["stable-v1"], "outside")

    def test_engine_headless_and_readiness_work_are_profile_bounded(self) -> None:
        items = self.items_of(self.mutable)
        eng = self.item_text(items["ENG-220"], "implementationScope", "acceptanceCriteria", "commands")
        self.assertIn("d3d11", eng)
        self.assertIn("canonical", eng)
        for breadth in ("d3d12", "vulkan", "opengl", "metal"):
            self.assertNotIn(breadth, eng)

        head = self.item_text(items["HEAD-220"], "implementationScope", "acceptanceCriteria", "commands")
        for required in ("windows 11", "nullrhi", "fps", "asset", "save", "shutdown", "soak"):
            self.assertIn(required, head)
        self.assertNotIn("linux", head)

        lifecycle = self.item_text(items["RDY-010"], "implementationScope", "acceptanceCriteria")
        self.assertIn("in-profile", lifecycle)
        self.assertIn("rdy-015", lifecycle)
        manifests = self.item_text(items["RDY-020"], "implementationScope", "acceptanceCriteria")
        self.assertIn("per-module", manifests)
        self.assertIn("in-profile", manifests)

    def test_experimental_backend_parity_is_owned_by_rhi_items(self) -> None:
        self.assertEqual(backend_parity_owner_errors(self.mutable), [])

        def drift(mutate: Any, fragment: str) -> None:
            contract = copy.deepcopy(self.mutable)
            mutate(self.items_of(contract), contract)
            errors = backend_parity_owner_errors(contract)
            self.assertTrue(any(fragment in error for error in errors), errors)

        cases = {
            "deleted-owner": (
                lambda items, contract: contract["workItems"].remove(items["RHI-230"]),
                "RHI-230: vulkan parity owner is missing",
            ),
            "owner-promoted-into-stable": (
                lambda items, _: items["RHI-225"]["profileApplicability"].__setitem__("stable-v1", "required"),
                "RHI-225: d3d12 parity must stay outside stable-v1",
            ),
            "owner-detached-from-eng-220": (
                lambda items, _: items["RHI-240"]["dependencies"].remove("ENG-220"),
                "RHI-240: must depend on ENG-220",
            ),
            "eng-220-exclusion-dropped": (
                lambda items, _: items["ENG-220"].__setitem__("outOfScope", []),
                "ENG-220: outOfScope must exclude experimental backend parity",
            ),
        }
        for name, (mutate, fragment) in cases.items():
            with self.subTest(drift=name):
                drift(mutate, fragment)

    def test_shared_manifest_kit_and_prototype_helpers_are_split(self) -> None:
        items = self.items_of(self.mutable)
        shared = self.item_text(items["MOD-290"], "implementationScope", "acceptanceCriteria")
        prototype = self.item_text(items["MOD-295"], "implementationScope", "acceptanceCriteria")
        self.assertIn("manifest", shared)
        self.assertIn("public-sdk", shared)
        self.assertNotIn("controller, ecs", shared)
        self.assertIn("controller", prototype)
        self.assertIn("prototype", prototype)
        self.assertEqual(items["MOD-290"]["profileApplicability"]["stable-v1"], "shared")

    def test_build_release_and_installer_work_is_windows_stable_only(self) -> None:
        items = self.items_of(self.mutable)
        for work_id in ("BLD-100", "CI-120", "REL-100", "INST-130"):
            with self.subTest(work_item=work_id):
                text = self.item_text(
                    items[work_id],
                    "implementationScope",
                    "acceptanceCriteria",
                    "commands",
                    "requiredCiJobs",
                )
                self.assertIn("windows", text)
                self.assertNotIn("linux", text)
        linux = self.item_text(items["PLT-210"], "implementationScope", "commands")
        self.assertIn("linux", linux)
        self.assertEqual(items["PLT-210"]["profileApplicability"]["stable-v1"], "outside")
        self.assertEqual(items["CI-110"]["profileApplicability"]["stable-v1"], "shared")

    def test_security_performance_and_rehearsal_are_profile_bounded(self) -> None:
        items = self.items_of(self.mutable)
        security = self.item_text(items["SEC-120"], "implementationScope", "testSelectors")
        self.assertNotIn("packet", security)
        self.assertNotIn("script", security)
        self.assertIn("packet", self.item_text(items["NET-100"], "implementationScope"))
        self.assertIn("script", self.item_text(items["ENG-200"], "implementationScope"))

        performance = self.item_text(
            items["PERF-100"], "implementationScope", "acceptanceCriteria", "commands"
        )
        for required in ("windows", "d3d11", "editor", "fps", "nullrhi"):
            self.assertIn(required, performance)
        self.assertNotIn("linux", performance)

        rehearsal = self.item_text(items["REL-190"], "implementationScope", "acceptanceCriteria")
        self.assertIn("requiredgateids", rehearsal)
        for excluded in ("protocol", "backup", "incident"):
            self.assertNotIn(excluded, rehearsal)
        operations = self.item_text(items["OPS-110"], "implementationScope", "acceptanceCriteria")
        for owned in ("backup", "incident", "telemetry"):
            self.assertIn(owned, operations)

    def test_production_operations_stay_owned_by_g12_and_ops_110(self) -> None:
        self.assertEqual(site_data_validate.operations_boundary_errors(self.mutable), [])

        def scope_backup(items: dict[str, Any], _: dict[str, Any]) -> None:
            items["HEAD-220"]["implementationScope"].append("Automate backup and restore")

        def select_server_soak(items: dict[str, Any], _: dict[str, Any]) -> None:
            items["HEAD-220"]["testSelectors"].append("Server_Soak")

        def drop_ops_from_g12(_: dict[str, Any], contract: dict[str, Any]) -> None:
            self.gates_of(contract)["G12"]["blockingWorkItemIds"].remove("OPS-110")

        def add_headless_to_g12(_: dict[str, Any], contract: dict[str, Any]) -> None:
            self.gates_of(contract)["G12"]["blockingWorkItemIds"].append("HEAD-220")

        def drop_incident_scope(items: dict[str, Any], _: dict[str, Any]) -> None:
            operations = items["OPS-110"]
            for field in ("implementationScope", "acceptanceCriteria"):
                operations[field] = [value for value in operations[field] if "incident" not in value.lower()]

        cases = {
            "headless-scope-backup": (scope_backup, "names production operations ('backup')"),
            "headless-server-selector": (select_server_soak, "names production operations ('server_')"),
            "g12-loses-ops-110": (drop_ops_from_g12, "G12.blockingWorkItemIds: must list OPS-110"),
            "g12-gains-head-220": (add_headless_to_g12, "G12.blockingWorkItemIds: must not list HEAD-220"),
            "ops-110-loses-incidents": (drop_incident_scope, "OPS-110: scope and criteria must own incident"),
        }
        for name, (mutate, fragment) in cases.items():
            with self.subTest(mutation=name):
                contract = copy.deepcopy(self.contract)
                mutate(self.items_of(contract), contract)
                errors = site_data_validate.operations_boundary_errors(contract)
                self.assertTrue(any(fragment in error for error in errors), errors)
                if name == "headless-scope-backup":
                    # The rule runs inside the OD-08 boundary check, not only as a helper.
                    validator = site_data_validate.Validator(contract)
                    validator.validate_online_service_boundary()
                    self.assertTrue(any(fragment in error for error in validator.errors), validator.errors)


class WindowsRowEvidenceTests(ContractTestCase):
    """CI-110: Linux sanitizer evidence never promotes the Windows 11 stable-v1 row by itself."""

    ASAN = {"type": "workflow", "path": ".github/workflows/build.yml", "job": "build-linux-asan", "label": "ASan"}
    WINDOWS = {
        "type": "workflow",
        "path": ".github/workflows/build.yml",
        "job": "build-windows-vs2022",
        "label": "Windows",
    }

    def errors(self, contract: dict[str, Any]) -> list[str]:
        return site_data_validate.windows_row_evidence_errors(contract)

    def assert_error(self, contract: dict[str, Any], fragment: str) -> None:
        errors = self.errors(contract)
        self.assertTrue(any(fragment in error for error in errors), errors)

    def test_live_contract_cites_windows_jobs_for_the_windows_row(self) -> None:
        self.assertEqual(self.errors(self.mutable), [])

    def test_runner_and_sanitizer_classification_come_from_the_workflow(self) -> None:
        jobs = contract_selectors.workflow_jobs(REPO_ROOT / ".github" / "workflows" / "build.yml")
        for sanitizer in ("build-linux-asan", "build-linux-tsan", "build-linux-msan"):
            with self.subTest(job=sanitizer):
                self.assertTrue(jobs[sanitizer].sanitizer)
                self.assertFalse(jobs[sanitizer].windows)
        self.assertTrue(jobs["build-windows-vs2022"].windows)
        self.assertFalse(jobs["build-windows-vs2022"].sanitizer)

    def test_sanitizer_job_replacing_windows_evidence_is_rejected(self) -> None:
        windows = self.capabilities_of(self.mutable)["platform.windows"]
        windows["evidence"] = [self.ASAN if entry.get("job") else entry for entry in windows["evidence"]]
        self.assert_error(self.mutable, "stable-v1.platform.windows: promoted Windows row cites no workflow job")
        self.assert_error(self.mutable, "Linux sanitizer evidence (.github/workflows/build.yml#build-linux-asan)")
        self.assert_rejected(self.mutable, "cannot promote a Windows row by itself")

    def test_sanitizer_job_alongside_windows_evidence_is_accepted(self) -> None:
        self.capabilities_of(self.mutable)["platform.windows"]["evidence"].append(self.ASAN)
        self.assertEqual(self.errors(self.mutable), [])

    def test_passing_required_gate_with_only_sanitizer_evidence_is_rejected(self) -> None:
        gate = self.gates_of(self.mutable)["G02"]
        gate["state"] = "passing"
        gate["evidence"] = [self.ASAN]
        self.assert_error(self.mutable, "stable-v1.G02: Linux sanitizer evidence")
        gate["evidence"] = [self.ASAN, self.WINDOWS]
        self.assertEqual(self.errors(self.mutable), [])

    def test_windows_certification_work_cannot_be_evidenced_by_sanitizer_reports(self) -> None:
        entry = self.items_of(self.mutable)["PLT-200"]["acceptanceStatus"][0]
        entry["state"] = "evidenced"
        entry["evidence"] = [".github/scripts/verify-sanitizer-evidence.py", EXACT_CI_REFERENCE]
        self.assert_error(self.mutable, "PLT-200.acceptanceStatus[0]: sanitizer-derived artifacts alone")
        entry["evidence"] = ["Tools/platform-cert/validate_certification.py", EXACT_CI_REFERENCE]
        self.assertEqual(self.errors(self.mutable), [])

    def test_linux_host_certification_record_is_rejected_for_a_windows_row(self) -> None:
        sys.path.insert(0, str(REPO_ROOT / "Tools" / "platform-cert"))
        try:
            import validate_certification  # noqa: PLC0415
        finally:
            sys.path.pop(0)
        row = {"os": {"family": "Windows", "version": "11"}, "arch": "x64"}
        evidence = {"host": {"os": {"family": "Linux", "version": "24.04"}, "arch": "x64", "compiler": {}}}
        errors = validate_certification._cross_validate_host(row, evidence)
        self.assertIn("OS family mismatch: row='Windows' host='Linux'", errors)


class WebsitePrimaryGroupTests(ContractTestCase):
    """Frozen case 7: one primary group exactly equals profile capabilities."""

    def groups(self, contract: dict[str, Any]) -> list[dict[str, Any]]:
        return contract["content"]["home"]["status"]["groups"]

    def test_primary_group_is_unique_and_exact(self) -> None:
        profile = self.profile_of(self.mutable)
        primary = [value for value in self.groups(self.mutable) if value["tone"] == "primary"]
        self.assertEqual(len(primary), 1)
        self.assertEqual(set(primary[0]["capabilityIds"]), set(profile["includedCapabilityIds"]))
        self.assertLessEqual(
            {"scope.singleplayer", "editor.authoring", "modules.fps"},
            set(primary[0]["capabilityIds"]),
        )

    def test_primary_omission_and_second_primary_fail_closed(self) -> None:
        primary = next(value for value in self.groups(self.mutable) if value["tone"] == "primary")
        primary["capabilityIds"].remove("modules.fps")
        self.assert_rejected(self.mutable, "primary group must exactly equal")

        duplicate = copy.deepcopy(self.contract)
        next(value for value in self.groups(duplicate) if value["tone"] != "primary")["tone"] = "primary"
        self.assert_rejected(duplicate, "exactly one primary group is required")

    def test_capability_cannot_appear_in_multiple_groups(self) -> None:
        groups = self.groups(self.mutable)
        nonprimary = [value for value in groups if value["tone"] != "primary"]
        capability = nonprimary[0]["capabilityIds"][0]
        nonprimary[1]["capabilityIds"].append(capability)
        self.assert_rejected(self.mutable, "capabilities appear in more than one public group")


class PublicClaimInvariantTests(ContractTestCase):
    """Frozen case 8: public surfaces and boundary guards are mandatory."""

    def test_mandatory_public_surfaces_cannot_be_removed(self) -> None:
        profile = self.profile_of(self.mutable)
        expected = {
            ".github/copilot-instructions.md",
            ".github/prompts/build-test.prompt.md",
            ".github/prompts/copilot-instructions.md",
            "README.md",
            "CHANGELOG.md",
            "SECURITY.md",
            "SparkInstaller/README.md",
            "Templates/EmptyProject/README.md",
            "Templates/FPSStarter/README.md",
            "Templates/MultiplayerArena/README.md",
            "Templates/ThirdPersonStarter/README.md",
            "docs/README.md",
            "docs/guides/External-Services-and-Orchestration.md",
            "docs/plans/FEATURE_ROADMAP.md",
            "docs/site/content.json",
            "docs/status/PROJECT_STATUS.md",
            "docs/tooling/README.md",
            "wiki/Build-Guide.md",
            "wiki/Changelog.md",
            "wiki/Documentation.md",
            "wiki/Docs.md",
            "wiki/API.md",
            "wiki/Examples.md",
            "wiki/Guides.md",
            "wiki/Home.md",
            "wiki/Reference.md",
            "wiki/Roadmap.md",
            "wiki/Samples.md",
            "wiki/Tutorials.md",
            "wiki/Wiki.md",
            "wiki/advanced/Build-System-and-CMake-Modules.md",
            "wiki/advanced/Codebase-Health.md",
            "wiki/advanced/Codebase-Statistics.md",
            "wiki/advanced/Gameplay-Systems-Status.md",
            "wiki/advanced/SparkGame-Module-Status.md",
            "wiki/advanced/Testing.md",
            "wiki/gameplay-tools/Asset-Pipeline.md",
            "wiki/gameplay-tools/Project-Templates.md",
            "wiki/gameplay-tools/SparkEditor.md",
            "wiki/getting-started/Architecture-Overview.md",
            "wiki/getting-started/FAQ.md",
            "wiki/getting-started/Game-Modules.md",
            "wiki/getting-started/Getting-Started.md",
            "wiki/getting-started/Migration-Guide.md",
            "wiki/getting-started/Making-Your-First-Game.md",
            "wiki/getting-started/Making-Your-First-Multiplayer-Game.md",
            "wiki/getting-started/Quick-Start-Tutorial.md",
            "wiki/getting-started/Creating-a-Game-Module.md",
            "wiki/gameplay-tools/Game-Packaging.md",
            "wiki/gameplay-tools/SparkConsole.md",
            "wiki/graphics/D3D11-Backend.md",
            "wiki/graphics/D3D12-Backend.md",
            "wiki/graphics/Metal-Backend.md",
            "wiki/graphics/OpenGL-Backend.md",
            "wiki/graphics/RHI-Abstraction-Layer.md",
            "wiki/graphics/Vulkan-Backend.md",
            "wiki/getting-started/Editor-Walkthrough.md",
            "wiki/platform/System-Requirements.md",
            "wiki/platform/Cross-Compilation-Wine-Testing.md",
            "wiki/subsystems/Collaborative-Editing.md",
            "wiki/subsystems/Dedicated-Server.md",
            "wiki/subsystems/Animation.md",
            "wiki/subsystems/Rendering-and-Graphics.md",
            "wiki/subsystems/Scene-Management.md",
            "wiki/subsystems/Scripting-with-AngelScript.md",
            "wiki/subsystems/Tween-System.md",
            "GameModules/README.md",
        }
        self.assertEqual(set(profile["publicClaimSurfaces"]), expected)
        profile["publicClaimSurfaces"].remove("README.md")
        self.assert_rejected(self.mutable, "must exactly match the independently required")

        missing_profile_doc = copy.deepcopy(self.contract)
        self.profile_of(missing_profile_doc)["publicClaimSurfaces"].remove(
            "GameModules/README.md"
        )
        self.assert_rejected(missing_profile_doc, "and profile documentation")

        for path in tuple(profile["documentation"]):
            with self.subTest(paired_removal=path):
                paired_removal = copy.deepcopy(self.contract)
                paired_profile = self.profile_of(paired_removal)
                paired_profile["documentation"].remove(path)
                paired_profile["publicClaimSurfaces"].remove(path)
                self.assert_rejected(
                    paired_removal,
                    "profile documentation must exactly match",
                )

        undocumented_surface = copy.deepcopy(self.contract)
        self.profile_of(undocumented_surface)["documentation"].append(
            "Tests/Tools/test_site_data_contract.py"
        )
        self.assert_rejected(
            undocumented_surface,
            "profile documentation must exactly match",
        )

        duplicate_surface = copy.deepcopy(self.contract)
        self.profile_of(duplicate_surface)["publicClaimSurfaces"].append("README.md")
        self.assert_rejected(duplicate_surface, "must not contain duplicates")

    def test_public_claim_arrays_reject_mapping_and_null_shapes(self) -> None:
        for field in (
            "publicClaimSurfaces",
            "documentation",
            "forbiddenUnqualifiedClaims",
        ):
            for shape in ("mapping", "null"):
                with self.subTest(field=field, shape=shape):
                    hostile = copy.deepcopy(self.contract)
                    profile = self.profile_of(hostile)
                    owner = profile["publicClaimRules"] if field == "forbiddenUnqualifiedClaims" else profile
                    original = owner[field]
                    owner[field] = (
                        {str(value): True for value in original}
                        if shape == "mapping"
                        else None
                    )
                    self.assert_rejected(hostile, f"{field} must be an array")

        for shape in (["unexpected"], None):
            with self.subTest(field="publicClaimRules", shape=type(shape).__name__):
                hostile = copy.deepcopy(self.contract)
                self.profile_of(hostile)["publicClaimRules"] = shape
                self.assert_rejected(hostile, "publicClaimRules must be an object")

        non_string_surface = copy.deepcopy(self.contract)
        self.profile_of(non_string_surface)["publicClaimSurfaces"].append(
            {"README.md": True}
        )
        self.assert_rejected(non_string_surface, "path must be a non-empty string")

        for field in (
            "breadthTokens",
            "forbiddenInProfileCells",
            "forbiddenUnqualifiedClaims",
        ):
            with self.subTest(field=field, shape="non-string-member"):
                hostile = copy.deepcopy(self.contract)
                self.profile_of(hostile)["publicClaimRules"][field].append(
                    {"hostile": True}
                )
                self.assert_rejected(hostile, "member must be a non-empty string")

        hostile_conflict = copy.deepcopy(self.contract)
        self.profile_of(hostile_conflict)["publicClaimRules"]["conflatedTerms"][0][
            "conflictsWith"
        ].append({"hostile": True})
        self.assert_rejected(hostile_conflict, "member must be a non-empty string")

    def test_mandatory_breadth_and_nullrhi_rules_cannot_be_removed(self) -> None:
        profile = self.profile_of(self.mutable)
        expected_breadth = {
            "windows 7",
            "windows 8",
            "windows 10",
            "windows 10+",
            "windows server",
            "linux",
            "ubuntu",
            "macos",
            "mac os",
            "android",
            "ios",
            "any platform",
            "any host",
            "any compiler",
        }
        expected_cells = {
            "Any",
            "All",
            "Any platform",
            "Any host",
            "Windows",
            "Windows 10",
            "Windows 10+",
            "Headless",
        }
        expected_conflicts = {
            "llvmpipe",
            "software rendering",
            "software render",
            "software rasterization",
            "software rasterizer",
            "software renderer",
            "render in software",
        }
        rules = profile["publicClaimRules"]
        self.assertEqual({value.lower() for value in rules["breadthTokens"]}, expected_breadth)
        self.assertEqual(set(rules["forbiddenInProfileCells"]), expected_cells)
        self.assertEqual(set(rules["conflatedTerms"][0]["conflictsWith"]), expected_conflicts)

        profile["supportedHosts"] = ["Windows"]
        self.assert_rejected(
            self.mutable,
            "supportedHosts must exactly match the independently required host set",
        )

        self.mutable = copy.deepcopy(self.contract)
        profile = self.profile_of(self.mutable)
        profile["publicClaimRules"]["breadthTokens"] = []
        self.assert_rejected(self.mutable, "mandatory invariants are missing")

        missing_any_host = copy.deepcopy(self.contract)
        self.profile_of(missing_any_host)["publicClaimRules"]["breadthTokens"].remove(
            "any host"
        )
        self.assert_rejected(missing_any_host, "mandatory invariants are missing")

        for token in expected_breadth:
            with self.subTest(removed_breadth=token):
                hostile = copy.deepcopy(self.contract)
                values = self.profile_of(hostile)["publicClaimRules"]["breadthTokens"]
                values.remove(next(value for value in values if value.lower() == token))
                self.assert_rejected(hostile, "mandatory invariants are missing")

        for cell in expected_cells:
            with self.subTest(removed_forbidden_cell=cell):
                hostile = copy.deepcopy(self.contract)
                self.profile_of(hostile)["publicClaimRules"]["forbiddenInProfileCells"].remove(cell)
                self.assert_rejected(hostile, "mandatory ambiguous profile cells")

        for conflict in expected_conflicts:
            with self.subTest(removed_nullrhi_conflict=conflict):
                hostile = copy.deepcopy(self.contract)
                self.profile_of(hostile)["publicClaimRules"]["conflatedTerms"][0][
                    "conflictsWith"
                ].remove(conflict)
                self.assert_rejected(hostile, "conflict vocabulary must exactly preserve")

        unexpected = copy.deepcopy(self.contract)
        self.profile_of(unexpected)["publicClaimRules"]["breadthTokens"].append("surprise host")
        self.assert_rejected(unexpected, "unexpected values were added")

        duplicate = copy.deepcopy(self.contract)
        self.profile_of(duplicate)["publicClaimRules"]["forbiddenInProfileCells"].append("All")
        self.assert_rejected(duplicate, "mandatory ambiguous profile cells")

        no_nullrhi = copy.deepcopy(self.contract)
        self.profile_of(no_nullrhi)["publicClaimRules"]["conflatedTerms"] = []
        self.assert_rejected(no_nullrhi, "mandatory NullRHI distinction is missing")

        for claim in ("fully supported", "production-ready", "production ready"):
            with self.subTest(claim=claim):
                missing_claim = copy.deepcopy(self.contract)
                self.profile_of(missing_claim)["publicClaimRules"][
                    "forbiddenUnqualifiedClaims"
                ].remove(claim)
                self.assert_rejected(
                    missing_claim,
                    "mandatory unqualified release/support claims are missing",
                )

    def test_pure_public_claim_helper_rejects_scope_widening(self) -> None:
        profile = self.profile_of(self.mutable)
        cases = {
            "windows": "| Windows 10+ | MSVC v143 | D3D11 | In `stable-v1` |",
            "linux-profile-subject": "stable-v1 supports Linux.",
            "macos-profile-subject": "stable-v1 supports macOS.",
            "ubuntu-profile-subject": "stable-v1 supports Ubuntu 24.04.",
            "android-profile-subject": "stable-v1 supports Android.",
            "mixed-supported-hosts": "stable-v1 supports Windows 11 x64 and Linux.",
            "mixed-windows-server": (
                "stable-v1 supports Windows 11 x64 and Windows Server 2025."
            ),
            "windows-or-later": "stable-v1 supports Windows 11 x64 or later.",
            "windows-x64-plus": "stable-v1 supports Windows 11 x64+.",
            "windows-generic": "stable-v1 supports Windows.",
            "windows-generic-x64": "stable-v1 supports Windows x64.",
            "windows-eleven-no-arch": "| Windows 11 | stable-v1 target |",
            "windows-win32": "| Win32 | In stable-v1 |",
            "windows-arm64": "| Windows ARM64 | In stable-v1 |",
            "windows-unformatted": "| Windows 10+ | MSVC v143 | D3D11 | In stable-v1 |",
            "windows-profile-subject": "stable-v1 supports Windows 10+.",
            "windows-profile-object": "Windows 10+ is supported by stable-v1.",
            "windows-profile-copular": "stable-v1 is supported on Windows 10+.",
            "windows-profile-in": "Windows 10+ is supported in the stable-v1 profile.",
            "possessive-supported-hosts": "stable-v1's supported hosts include Linux.",
            "support-includes": "stable-v1 support includes macOS.",
            "profile-host-label": "stable-v1 hosts: Linux",
            "supported-host-label": "Supported hosts (stable-v1): Linux",
            "cross-clause-carry": (
                "stable-v1 supports Windows 11 x64; it also supports Linux."
            ),
            "windows-profile-soft-wrapped": "stable-v1 supports\nWindows 10+.",
            "windows-in-scope": "Windows 10+ is in scope for stable-v1.",
            "windows-format-characters": "stable-\u200bv1 supports Win\u200bdows 10+.",
            "windows-qualified-status": "| Windows 10 x64 | In stable-v1 - supported |",
            "windows-noun-status": "| Windows 10 x64 | stable-v1 target |",
            "windows-object-status": "| Windows 10+ | Supported by stable-v1 |",
            "windows-no-outer-pipes": "Windows 10+ | stable-v1 target",
            "table-profile-and-breadth-split": (
                "| stable-v1 supports Windows 11 x64 | Windows 10+ |"
            ),
            "table-predicate-and-profile-split": "| Linux | Supported | stable-v1 |",
            "table-target-and-profile-split": "| Windows 10+ | stable-v1 | target |",
            "table-header-profile-scope": (
                "| Host | Supported in stable-v1 |\n"
                "|---|---|\n"
                "| Linux | Yes |"
            ),
            "list-profile-split": (
                "stable-v1 supports the following hosts:\n- Windows 10+"
            ),
            "list-profile-split-blank": (
                "stable-v1 supports the following hosts:\n\n- Windows 10+"
            ),
            "list-profile-wrapped-item": (
                "stable-v1 supports the following hosts:\n"
                "- Windows 11 x64 and\n"
                "  Linux"
            ),
            "any-host": "| Headless | Any | NullRHI | In `stable-v1` |",
            "headless-qualified-status": "| Headless | In stable-v1 - blocked |",
            "any-qualified-status": "| Any | In stable-v1 - target |",
            "any-noun-status": "| Any host | stable-v1 target |",
            "nullrhi": "| NullRHI | headless software rendering via llvmpipe |",
            "nullrhi-format-character": "Null\u200bRHI is software rendering.",
            "nullrhi-hyphenated-rendering": "NullRHI is software-rendering.",
            "nullrhi-rasterizer": "NullRHI is a software rasterizer.",
            "nullrhi-renderer": "NullRHI is the software renderer.",
            "nullrhi-renders-in-software": "NullRHI renders in software.",
            "nullrhi-software-rendered": (
                "NullRHI produces software-rendered frames."
            ),
            "nullrhi-list-split": (
                "NullRHI provides the following headless renderer:\n"
                "- software rendering via llvmpipe"
            ),
            "nullrhi-list-split-blank": (
                "NullRHI provides the following headless renderer:\n\n"
                "- software rendering via llvmpipe"
            ),
            "nullrhi-pronoun-reversal": (
                "NullRHI is not software rendering; it actually is."
            ),
            "nullrhi-postfix-reversal": (
                "NullRHI is not software rendering; it is, actually."
            ),
            "nullrhi-repeated-term-reversal": (
                "NullRHI is not software rendering; NullRHI actually is."
            ),
            "nullrhi-dash-reversal": (
                "NullRHI is not software rendering—but it actually is."
            ),
            "nullrhi-colon-reversal": (
                "NullRHI is not software rendering: but it actually is."
            ),
            "nullrhi-parenthetical-reversal": (
                "NullRHI is not software rendering (but it actually is)."
            ),
            "nullrhi-one-reversal": (
                "NullRHI is not software rendering; it actually is one."
            ),
            "nullrhi-contradictory-distinction": (
                "NullRHI is not llvmpipe. NullRHI is not software rendering. "
                "However, NullRHI is software rendering via llvmpipe."
            ),
            "nullrhi-false-distinction": (
                "NullRHI is not certified; llvmpipe is the same software-rendering "
                "headless path."
            ),
            "fully-supported": "Linux is fully supported.",
            "production-hyphen": "SparkEngine is production-ready.",
            "production-space-uppercase": "SPARKENGINE IS PRODUCTION READY.",
            "production-nonbreaking-hyphen": "SparkEngine is production‑ready.",
            "production-en-dash": "SparkEngine is production–ready.",
            "fully-spaced": "Linux is fully   supported.",
            "fully-marked-up": "Linux is fully **supported**.",
            "fully-linked": "Linux is fully [supported](https://example.invalid).",
            "fully-reference-linked": (
                "Linux is fully [supported][status].\n\n"
                "[status]: https://example.invalid"
            ),
            "fully-shortcut-linked": (
                "Linux is fully [supported].\n\n"
                "[supported]: https://example.invalid"
            ),
            "fully-image-alt": "Linux is fully ![supported](badge.svg).",
            "fully-html": "Linux is fully <strong>supported</strong>.",
            "fully-html-break": "Linux is fully<br>supported.",
            "fully-html-comment": "Linux is fully<!-- invisible --> supported.",
            "fully-zero-width": "Linux is fully\u200bsupported.",
            "fully-soft-wrapped": "Linux is fully\nsupported.",
            "production-soft-hyphen": "SparkEngine is production\u00adready.",
            "production-markdown-escape": "SparkEngine is production\\-ready.",
            "production-variation-selector": "SparkEngine is production\ufe0f-ready.",
            "production-cgj": "SparkEngine is produc\u034ftion-ready.",
            "production-hangul-filler": "SparkEngine is produc\u3164tion-ready.",
            "production-hangul-choseong-filler": (
                "SparkEngine is produc\u115ftion-ready."
            ),
            "production-html-alt": '<img alt="SparkEngine is production-ready">',
            "production-html-alt-quoted-gt": (
                '<img alt="SparkEngine is production-ready >">'
            ),
            "production-html-alt-entity-gt": (
                '<img alt="SparkEngine is production-ready &gt;">'
            ),
            "production-html-title-quoted-gt": (
                '<abbr title="SparkEngine is production-ready >">preview</abbr>'
            ),
        }
        for name, text in cases.items():
            with self.subTest(case=name):
                self.assertTrue(site_data_validate.validate_public_claim_text(profile, name, text))

        malformed = copy.deepcopy(profile)
        malformed["publicClaimRules"] = None
        self.assertIn(
            "publicClaimRules must be an object",
            site_data_validate.validate_public_claim_text(
                malformed, "malformed", "Linux is fully supported."
            )[0],
        )

        malformed_member = copy.deepcopy(profile)
        malformed_member["publicClaimRules"]["forbiddenUnqualifiedClaims"].append(
            {"hostile": True}
        )
        self.assertTrue(
            any(
                "must be a non-empty string" in value
                for value in site_data_validate.validate_public_claim_text(
                    malformed_member, "malformed-member", "bounded text"
                )
            )
        )

        malformed_term = copy.deepcopy(profile)
        malformed_term["publicClaimRules"]["conflatedTerms"] = [None]
        self.assertTrue(
            any(
                "must be an object" in value
                for value in site_data_validate.validate_public_claim_text(
                    malformed_term, "malformed-term", "bounded text"
                )
            )
        )

        malformed_conflicts = copy.deepcopy(profile)
        malformed_conflicts["publicClaimRules"]["conflatedTerms"][0][
            "conflictsWith"
        ] = None
        self.assertTrue(
            any(
                "conflictsWith must be an array" in value
                for value in site_data_validate.validate_public_claim_text(
                    malformed_conflicts,
                    "malformed-conflicts",
                    "NullRHI software rendering",
                )
            )
        )

        for field, hostile_value, expected in (
            ("conflictsWith", None, "conflictsWith must be an array"),
            ("conflictsWith", [], "conflictsWith must not be empty"),
            ("conflictsWith", [{"hostile": True}], "must be a non-empty string"),
            ("reason", {"hostile": True}, "reason must be a non-empty string"),
        ):
            with self.subTest(helper_field=field, text="unrelated"):
                malformed_policy = copy.deepcopy(profile)
                malformed_policy["publicClaimRules"]["conflatedTerms"][0][
                    field
                ] = hostile_value
                self.assertTrue(
                    any(
                        expected in value
                        for value in site_data_validate.validate_public_claim_text(
                            malformed_policy, "malformed-policy", ""
                        )
                    )
                )

    def test_pure_public_claim_helper_accepts_bounded_profile_status(self) -> None:
        profile = self.profile_of(self.mutable)
        cases = {
            "bounded": "The `stable-v1` profile is blocked and uncertified.",
            "null-not-llvmpipe": "NullRHI is not llvmpipe.",
            "null-separate-software": "NullRHI is separate from software rendering.",
            "null-does-not-render": "NullRHI does not perform software rendering.",
            "null-does-not-render-in-software": "NullRHI does not render in software.",
            "null-cannot-render-in-software": "NullRHI cannot render in software.",
            "null-never-renders-in-software": "NullRHI never renders in software.",
            "null-contraction-render-in-software": (
                "NullRHI doesn't render in software."
            ),
            "null-performs-no-software-rendering": (
                "NullRHI performs no software rendering."
            ),
            "null-neither-nor": (
                "NullRHI neither uses llvmpipe nor performs software rendering."
            ),
            "null-coordinated-negatives": (
                "NullRHI is not llvmpipe and does not perform software rendering."
            ),
            "null-distinct-from-both": (
                "NullRHI is distinct from both llvmpipe and software rendering."
            ),
            "null-unlike": "Unlike NullRHI, llvmpipe provides software rendering.",
            "null-whereas": (
                "NullRHI rasterizes no pixels, whereas llvmpipe provides software "
                "rendering."
            ),
            "null-rather-than": (
                "llvmpipe can provide software rendering rather than NullRHI no-ops."
            ),
            "linux-macos-outside": (
                "Linux and macOS remain outside stable-v1; only Windows 11 x64 "
                "is certified."
            ),
            "linux-not-supported": "Linux is not supported by stable-v1.",
            "linux-not-currently-in": "Linux is not currently in stable-v1.",
            "linux-unsupported-in": "Linux remains unsupported in stable-v1.",
            "linux-uncertified-in": "Linux is uncertified in stable-v1.",
            "linux-table-outside": "| Linux | Outside stable-v1 |",
            "safe-list-items": (
                "- Windows 10 x64 is outside the release profile.\n"
                "- Windows 11 x64 is the stable-v1 target."
            ),
            "safe-table-cells": (
                "| Host | Classification |\n"
                "|---|---|\n"
                "| Windows 10 x64 | Outside the release profile |\n"
                "| Windows 11 x64 | stable-v1 target |"
            ),
            "safe-table-no-outer-pipes": (
                "Host | Classification\n"
                "--- | ---\n"
                "Windows 10 x64 | Outside the release profile\n"
                "Windows 11 x64 | stable-v1 target"
            ),
        }
        for name, text in cases.items():
            with self.subTest(case=name):
                self.assertEqual(
                    site_data_validate.validate_public_claim_text(profile, name, text),
                    [],
                )

    def test_profile_identifier_rejects_near_match_tokens(self) -> None:
        contains = site_data_validate.contains_release_profile_identifier
        self.assertTrue(contains("stable-v1", "The stable-v1 profile is blocked."))
        self.assertTrue(contains("stable-v1", "The [stable-v1](status.md) profile is blocked."))
        for near_match in ("stable-v1x", "xstable-v1", "stable-v1_extra"):
            with self.subTest(near_match=near_match):
                self.assertFalse(contains("stable-v1", near_match))
        self.assertFalse(contains("stable-v1", "<!-- stable-v1 -->"))
        self.assertFalse(
            contains("stable-v1", "[release profile](https://example.invalid/stable-v1)")
        )

    def test_current_public_surfaces_pass_the_pure_helper(self) -> None:
        profile = self.profile_of(self.mutable)
        for surface in profile["publicClaimSurfaces"]:
            with self.subTest(surface=surface):
                text = (REPO_ROOT / surface).read_text(encoding="utf-8", errors="replace")
                self.assertEqual(
                    site_data_validate.validate_public_claim_text(profile, surface, text),
                    [],
                )

    def test_multiplayer_quick_start_keeps_rcon_local_only(self) -> None:
        quick_start = (REPO_ROOT / "wiki" / "subsystems" / "Multiplayer-Quick-Start.md").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn("trusted local administration", quick_start)
        self.assertIn("remote RCON is unavailable", quick_start)
        self.assertIn("There is no network RCON listener", quick_start)
        self.assertNotIn('config.rconPassword = "admin123";', quick_start)

    def test_memory_integrity_docs_keep_admin_boundary_local_only(self) -> None:
        surfaces = (
            REPO_ROOT / "wiki" / "subsystems" / "Memory-Integrity.md",
            REPO_ROOT / "wiki" / "advanced" / "Memory-Integrity-System.md",
        )
        for surface in surfaces:
            with self.subTest(surface=surface):
                text = surface.read_text(encoding="utf-8", errors="replace")
                self.assertIn("local administration", text.lower())
                self.assertNotIn("Chat-to-RCON command gate", text)
                self.assertNotIn("RCON command gate", text)

    def test_negative_tests_have_no_tracked_file_mutation_calls(self) -> None:
        tree = ast.parse(TEST_PATH.read_text(encoding="utf-8"))
        forbidden: list[str] = []
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
                if node.func.attr in {"write_text", "write_bytes", "unlink", "replace"}:
                    forbidden.append(node.func.attr)
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "open":
                if len(node.args) > 1 and isinstance(node.args[1], ast.Constant):
                    if any(value in str(node.args[1].value) for value in "wa+"):
                        forbidden.append("open-for-write")
        self.assertEqual(forbidden, [])

    def test_readme_does_not_overclaim_templates_or_generic_nullrhi(self) -> None:
        readme = (REPO_ROOT / "README.md").read_text(encoding="utf-8")
        self.assertNotIn("Nine complete installed-SDK-independent templates", readme)
        self.assertIn("headless entry points now own and tick a NullRHI bridge", readme)
        self.assertIn("passing no windowed `GraphicsEngine` or `InputManager`", readme)
        self.assertIn("the FPS client module does not load through `SparkServer`", readme)
        self.assertIn("Packaged clean-host, soak, and recovery qualification remains open (`HEAD-220`)", readme)
        self.assertIn("NullRHI rasterizes no pixels", readme)

    def test_public_content_uses_profile_derived_gate_wording(self) -> None:
        content = (REPO_ROOT / "docs" / "site" / "content.json").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("every global gate", content.lower())
        self.assertNotIn("global release gates", content.lower())
        self.assertIn("every declared profile's required gates", content)


class DerivedEvidenceTests(ContractTestCase):
    """Frozen case 9: changing state cannot leave stale hand-authored facts."""

    def test_limitations_do_not_freeze_current_gate_state_or_count(self) -> None:
        limitations = self.profile_of(self.mutable)["limitations"]
        lowered = " ".join(limitations).lower()
        self.assertNotIn("no required gate is passing", lowered)
        numeric = re.compile(
            r"\b(?:\d+|one|two|three|four|five|six|seven|eight|nine|ten|eleven|"
            r"twelve|thirteen|fourteen|fifteen|sixteen|seventeen|eighteen)\b",
            re.IGNORECASE,
        )
        for limitation in limitations:
            if "required gate" in limitation.lower():
                with self.subTest(limitation=limitation):
                    self.assertIsNone(numeric.search(limitation))

    def test_required_gate_state_wording_is_derived(self) -> None:
        current = render_handoff.render_handoff(self.mutable)
        self.assertIn("Required gate states: 0 passing, 0 at risk, 16 blocked", current)
        self.gates_of(self.mutable)["G00"]["state"] = "passing"
        changed = render_handoff.render_handoff(self.mutable)
        self.assertIn("Required gate states: 1 passing, 0 at risk, 15 blocked", changed)

    def test_handoff_carries_applicability_and_signoff_state(self) -> None:
        rendered = render_handoff.render_handoff(self.mutable)
        self.assertIn("**Profile applicability:** `stable-v1`=", rendered)
        self.assertIn("Sign-off evidence: none recorded", rendered)
        self.assertNotIn("Blocking release gates:", rendered)
        self.assertIn("profile applicability determines release impact", rendered)

    def test_rdy_000_remains_in_progress_until_original_truth_debt_closes(self) -> None:
        self.assertEqual(self.items_of(self.mutable)["RDY-000"]["status"], "in-progress")
        self.assertEqual(self.profile_of(self.mutable)["state"], "blocked")


class GenerationAndCiTests(ContractTestCase):
    """Frozen case 10: generated outputs and dedicated CI stay wired."""

    def test_generated_handoff_is_current_and_deterministic(self) -> None:
        rendered = render_handoff.render_handoff(self.contract)
        self.assertEqual(HANDOFF_PATH.read_text(encoding="utf-8"), rendered)
        self.assertEqual(rendered, render_handoff.render_handoff(load_contract()))

    def test_generator_publishes_release_profiles(self) -> None:
        source = GENERATOR_PATH.read_text(encoding="utf-8")
        self.assertIn('"releaseProfiles": readiness["releaseProfiles"]', source)
        self.assertIn('"releaseProfiles": bundle["releaseProfiles"]', source)

    def test_dedicated_ci_runs_contract_and_determinism_checks(self) -> None:
        workflow = WORKFLOW_PATH.read_text(encoding="utf-8")
        # The docs-health copy moved to build.yml (DOC-410); site-data-validate keeps its own run.
        self.assertIn("python3 Tests/Tools/test_site_data_contract.py -v", workflow)
        self.assertIn("python3 tools/site-data/render_handoff.py --check", workflow)
        self.assertGreaterEqual(workflow.count("python3 tools/site-data/generate.py"), 2)
        self.assertIn("diff --recursive --brief", workflow)
        self.assertGreaterEqual(workflow.count("set -euo pipefail"), 3)

    def test_declared_rdy_000_selectors_are_implemented(self) -> None:
        implemented = {"site-data-contract", "readiness-cross-references"}
        selectors = set(self.items_of(self.mutable)["RDY-000"]["testSelectors"])
        self.assertEqual(selectors, implemented)


class ExactEvidenceManifestTests(unittest.TestCase):
    """Durable site/release provenance is complete, canonical, and replay-bound."""

    VALUES = {
        "GITHUB_REPOSITORY": "Krilliac/SparkEngine",
        "EXACT_SOURCE_COMMIT": "1" * 40,
        "EXACT_BUILD_RUN_ID": "101",
        "EXACT_BUILD_RUN_ATTEMPT": "2",
        "EXACT_BUILD_RUN_URL": "https://github.com/Krilliac/SparkEngine/actions/runs/101",
        "EXACT_BUILD_EVENT": "workflow_dispatch",
        "EXACT_BUILD_MATRIX_PRODUCER_JOB_ID": "401",
        "EXACT_BUILD_REQUIRED_GATE_JOB_ID": "402",
        "EXACT_BUILD_JOB_INVENTORY_DIGEST": "sha256:" + "5" * 64,
        "EXACT_BUILD_MATRIX_SOURCE_ARTIFACT_ID": "204",
        "EXACT_BUILD_MATRIX_SOURCE_ARTIFACT_DIGEST": "sha256:" + "6" * 64,
        "EXACT_BUILD_MATRIX_SOURCE_ARTIFACT_BYTES": "4096",
        "EXACT_BUILD_MATRIX_STATUS_ID": "201",
        "EXACT_BUILD_MATRIX_STATUS_TARGET_URL": "https://github.com/Krilliac/SparkEngine/actions/runs/202/attempts/3",
        "EXACT_BUILD_MATRIX_STATUS_CREATED_AT": "2026-08-30T01:00:02Z",
        "EXACT_BUILD_MATRIX_STATUS_UPDATED_AT": "2026-08-30T01:00:03Z",
        "EXACT_BUILD_MATRIX_VERIFIER_RUN_ID": "202",
        "EXACT_BUILD_MATRIX_VERIFIER_RUN_ATTEMPT": "3",
        "EXACT_BUILD_MATRIX_VERIFIER_RUN_URL": "https://github.com/Krilliac/SparkEngine/actions/runs/202",
        "EXACT_VERIFIER_COMMIT": "2" * 40,
        "EXACT_BUILD_MATRIX_TRUSTED_VERIFIER_JOB_ID": "403",
        "EXACT_BUILD_MATRIX_VERIFIER_JOB_INVENTORY_DIGEST": "sha256:" + "7" * 64,
        "EXACT_BUILD_MATRIX_STATUS_PUBLISH_STEP_STARTED_AT": "2026-08-30T01:00:00Z",
        "EXACT_BUILD_MATRIX_STATUS_PUBLISH_STEP_COMPLETED_AT": "2026-08-30T01:00:05Z",
        "EXACT_BUILD_MATRIX_RECEIPT_ARTIFACT_ID": "203",
        "EXACT_BUILD_MATRIX_RECEIPT_ARTIFACT_DIGEST": "sha256:" + "3" * 64,
        "EXACT_BUILD_MATRIX_RECEIPT_ARTIFACT_BYTES": "1024",
        "EXACT_CODEQL_RUN_ID": "301",
        "EXACT_CODEQL_RUN_ATTEMPT": "4",
        "EXACT_CODEQL_RUN_URL": "https://github.com/Krilliac/SparkEngine/actions/runs/301",
        "EXACT_CODEQL_ACTIONS_SOURCE_JOB_ID": "404",
        "EXACT_CODEQL_C_CPP_SOURCE_JOB_ID": "405",
        "EXACT_CODEQL_PYTHON_SOURCE_JOB_ID": "406",
        "EXACT_CODEQL_SOURCE_JOB_INVENTORY_DIGEST": "sha256:" + "8" * 64,
        "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_ID": "305",
        "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_DIGEST": "sha256:" + "9" * 64,
        "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_BYTES": "2048",
        "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_ID": "306",
        "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_DIGEST": "sha256:" + "a" * 64,
        "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_BYTES": "3072",
        "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_ID": "307",
        "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_DIGEST": "sha256:" + "b" * 64,
        "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_BYTES": "4096",
        "EXACT_CODEQL_STATUS_ID": "302",
        "EXACT_CODEQL_STATUS_TARGET_URL": "https://github.com/Krilliac/SparkEngine/actions/runs/303/attempts/5",
        "EXACT_CODEQL_STATUS_CREATED_AT": "2026-08-30T02:00:02Z",
        "EXACT_CODEQL_STATUS_UPDATED_AT": "2026-08-30T02:00:03Z",
        "EXACT_CODEQL_REPORTER_RUN_ID": "303",
        "EXACT_CODEQL_REPORTER_RUN_ATTEMPT": "5",
        "EXACT_CODEQL_REPORTER_RUN_URL": "https://github.com/Krilliac/SparkEngine/actions/runs/303",
        "EXACT_CODEQL_TRUSTED_REPORTER_JOB_ID": "407",
        "EXACT_CODEQL_REPORTER_JOB_INVENTORY_DIGEST": "sha256:" + "c" * 64,
        "EXACT_CODEQL_STATUS_PUBLISH_STEP_STARTED_AT": "2026-08-30T02:00:00Z",
        "EXACT_CODEQL_STATUS_PUBLISH_STEP_COMPLETED_AT": "2026-08-30T02:00:05Z",
        "EXACT_CODEQL_SUMMARY_ARTIFACT_ID": "304",
        "EXACT_CODEQL_SUMMARY_ARTIFACT_DIGEST": "sha256:" + "4" * 64,
        "EXACT_CODEQL_SUMMARY_ARTIFACT_BYTES": "512",
    }

    def test_manifest_round_trip_is_canonical_and_source_bound(self) -> None:
        manifest = exact_evidence.build_manifest(self.VALUES)
        self.assertEqual(exact_evidence.validate_manifest(manifest), manifest)
        self.assertEqual(manifest["sourceCommit"], self.VALUES["EXACT_SOURCE_COMMIT"])
        self.assertEqual(manifest["schemaVersion"], 2)
        self.assertEqual(manifest["build"]["event"], "workflow_dispatch")
        self.assertEqual(manifest["build"]["ci120SourceArtifact"]["id"], 204)
        self.assertEqual(manifest["ci120"]["statusId"], 201)
        self.assertEqual(manifest["ci120"]["verifierJobId"], 403)
        self.assertEqual(manifest["codeql"]["statusId"], 302)
        self.assertEqual(
            [item["language"] for item in manifest["codeql"]["sourceJobs"]],
            ["actions", "c-cpp", "python"],
        )
        self.assertEqual(
            manifest["ci120"]["receiptArtifact"]["name"],
            f"build-matrix-trusted-receipt-{'1' * 40}-101-2-3",
        )
        self.assertEqual(
            manifest["codeql"]["summaryArtifact"]["name"],
            f"codeql-trusted-summary-{'1' * 40}-301-4-5",
        )
        self.assertEqual(
            exact_evidence.canonical_bytes(manifest),
            exact_evidence.canonical_bytes(exact_evidence.build_manifest(dict(self.VALUES))),
        )

    def test_every_gate_field_is_required_and_changes_canonical_bytes(self) -> None:
        baseline = exact_evidence.canonical_bytes(
            exact_evidence.build_manifest(self.VALUES)
        )
        for field in self.VALUES:
            missing = dict(self.VALUES)
            missing.pop(field)
            with self.subTest(field=field, mode="missing"):
                with self.assertRaises(exact_evidence.ExactEvidenceError):
                    exact_evidence.build_manifest(missing)

            mutated = dict(self.VALUES)
            if field == "GITHUB_REPOSITORY":
                replacement = "Krilliac/SparkEngineFork"
                for url_field in (
                    "EXACT_BUILD_RUN_URL",
                    "EXACT_BUILD_MATRIX_VERIFIER_RUN_URL",
                    "EXACT_CODEQL_RUN_URL",
                    "EXACT_CODEQL_REPORTER_RUN_URL",
                ):
                    prefix, suffix = mutated[url_field].split(
                        "Krilliac/SparkEngine", maxsplit=1
                    )
                    mutated[url_field] = prefix + replacement + suffix
                for url_field in (
                    "EXACT_BUILD_MATRIX_STATUS_TARGET_URL",
                    "EXACT_CODEQL_STATUS_TARGET_URL",
                ):
                    prefix, suffix = mutated[url_field].split(
                        "Krilliac/SparkEngine", maxsplit=1
                    )
                    mutated[url_field] = prefix + replacement + suffix
            elif field in {"EXACT_SOURCE_COMMIT", "EXACT_VERIFIER_COMMIT"}:
                replacement = "a" * 40
            elif field.endswith("_DIGEST"):
                replacement = "sha256:" + "d" * 64
            elif field in {
                "EXACT_BUILD_RUN_URL",
                "EXACT_BUILD_MATRIX_VERIFIER_RUN_URL",
                "EXACT_CODEQL_RUN_URL",
                "EXACT_CODEQL_REPORTER_RUN_URL",
                "EXACT_BUILD_MATRIX_STATUS_TARGET_URL",
                "EXACT_CODEQL_STATUS_TARGET_URL",
            }:
                run_id_field = field.removesuffix("_URL") + "_ID"
                # URL replacements are exercised through their run IDs below;
                # a mismatched direct URL must fail rather than silently normalize.
                hostile = dict(self.VALUES)
                hostile[field] = self.VALUES[field] + "/unexpected"
                with self.subTest(field=field, mode="mismatched-url"):
                    with self.assertRaises(exact_evidence.ExactEvidenceError):
                        exact_evidence.build_manifest(hostile)
                continue
            elif field.endswith("_AT"):
                timestamp_replacements = {
                    "EXACT_BUILD_MATRIX_STATUS_CREATED_AT": "2026-08-30T01:00:01Z",
                    "EXACT_BUILD_MATRIX_STATUS_UPDATED_AT": "2026-08-30T01:00:04Z",
                    "EXACT_BUILD_MATRIX_STATUS_PUBLISH_STEP_STARTED_AT": "2026-08-30T00:59:59Z",
                    "EXACT_BUILD_MATRIX_STATUS_PUBLISH_STEP_COMPLETED_AT": "2026-08-30T01:00:06Z",
                    "EXACT_CODEQL_STATUS_CREATED_AT": "2026-08-30T02:00:01Z",
                    "EXACT_CODEQL_STATUS_UPDATED_AT": "2026-08-30T02:00:04Z",
                    "EXACT_CODEQL_STATUS_PUBLISH_STEP_STARTED_AT": "2026-08-30T01:59:59Z",
                    "EXACT_CODEQL_STATUS_PUBLISH_STEP_COMPLETED_AT": "2026-08-30T02:00:06Z",
                }
                replacement = timestamp_replacements[field]
            elif field == "EXACT_BUILD_EVENT":
                replacement = "push"
            else:
                replacement = str(int(self.VALUES[field]) + 1000)
            mutated[field] = replacement
            run_url_for_id = {
                "EXACT_BUILD_RUN_ID": "EXACT_BUILD_RUN_URL",
                "EXACT_BUILD_MATRIX_VERIFIER_RUN_ID": "EXACT_BUILD_MATRIX_VERIFIER_RUN_URL",
                "EXACT_CODEQL_RUN_ID": "EXACT_CODEQL_RUN_URL",
                "EXACT_CODEQL_REPORTER_RUN_ID": "EXACT_CODEQL_REPORTER_RUN_URL",
            }
            if field in run_url_for_id:
                mutated[run_url_for_id[field]] = (
                    f"https://github.com/{mutated['GITHUB_REPOSITORY']}/actions/runs/{replacement}"
                )
            status_target_for_id = {
                "EXACT_BUILD_MATRIX_VERIFIER_RUN_ID": "EXACT_BUILD_MATRIX_STATUS_TARGET_URL",
                "EXACT_CODEQL_REPORTER_RUN_ID": "EXACT_CODEQL_STATUS_TARGET_URL",
            }
            if field in status_target_for_id:
                attempt_field = field.removesuffix("_ID") + "_ATTEMPT"
                mutated[status_target_for_id[field]] = (
                    f"https://github.com/{mutated['GITHUB_REPOSITORY']}/actions/runs/"
                    f"{replacement}/attempts/{mutated[attempt_field]}"
                )
            status_target_for_attempt = {
                "EXACT_BUILD_MATRIX_VERIFIER_RUN_ATTEMPT": (
                    "EXACT_BUILD_MATRIX_VERIFIER_RUN_ID",
                    "EXACT_BUILD_MATRIX_STATUS_TARGET_URL",
                ),
                "EXACT_CODEQL_REPORTER_RUN_ATTEMPT": (
                    "EXACT_CODEQL_REPORTER_RUN_ID",
                    "EXACT_CODEQL_STATUS_TARGET_URL",
                ),
            }
            if field in status_target_for_attempt:
                run_id_field, target_field = status_target_for_attempt[field]
                mutated[target_field] = (
                    f"https://github.com/{mutated['GITHUB_REPOSITORY']}/actions/runs/"
                    f"{mutated[run_id_field]}/attempts/{replacement}"
                )
            with self.subTest(field=field):
                changed = exact_evidence.canonical_bytes(
                    exact_evidence.build_manifest(mutated)
                )
                self.assertNotEqual(changed, baseline)

    def test_gate_output_parser_rejects_partial_duplicate_and_unknown_fields(self) -> None:
        inverse = {value: key for key, value in exact_evidence.ENV_FROM_GATE_KEY.items()}
        lines = [
            f"{inverse[environment]}={self.VALUES[environment]}"
            for environment in exact_evidence.ENV_FROM_GATE_KEY.values()
        ]
        with tempfile.TemporaryDirectory() as raw:
            def temporary_payload(payload: str) -> Path:
                with tempfile.NamedTemporaryFile(
                    mode="w",
                    encoding="utf-8",
                    newline="\n",
                    dir=raw,
                    delete=False,
                ) as stream:
                    stream.write(payload)
                    return Path(stream.name)

            path = temporary_payload("\n".join(lines) + "\n")
            values = exact_evidence.values_from_gate_output(
                path,
                repository=self.VALUES["GITHUB_REPOSITORY"],
                source_commit=self.VALUES["EXACT_SOURCE_COMMIT"],
            )
            self.assertEqual(exact_evidence.build_manifest(values), exact_evidence.build_manifest(self.VALUES))

            hostile_payloads = (
                "\n".join(lines[:-1]) + "\n",
                "\n".join([*lines, lines[0]]) + "\n",
                "\n".join([*lines, "unknown=value"]) + "\n",
            )
            for payload in hostile_payloads:
                with self.subTest(payload=payload[-40:]):
                    path = temporary_payload(payload)
                    with self.assertRaises(exact_evidence.ExactEvidenceError):
                        exact_evidence.parse_gate_output(path)

    def test_self_contained_validation_rejects_extra_or_fabricated_fields(self) -> None:
        manifest = exact_evidence.build_manifest(self.VALUES)
        mutations = []
        extra = copy.deepcopy(manifest)
        extra["untrusted"] = True
        mutations.append(extra)
        renamed = copy.deepcopy(manifest)
        renamed["codeql"]["summaryArtifact"]["name"] = "summary.json"
        mutations.append(renamed)
        inconsistent = copy.deepcopy(manifest)
        inconsistent["codeql"]["reporterCommit"] = "a" * 40
        mutations.append(inconsistent)
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                with self.assertRaises(exact_evidence.ExactEvidenceError):
                    exact_evidence.validate_manifest(mutation)

    def test_semantic_order_identity_and_status_windows_fail_closed(self) -> None:
        manifest = exact_evidence.build_manifest(self.VALUES)
        mutations = []

        reordered_jobs = copy.deepcopy(manifest)
        reordered_jobs["codeql"]["sourceJobs"][0:2] = reversed(
            reordered_jobs["codeql"]["sourceJobs"][0:2]
        )
        mutations.append(reordered_jobs)

        reordered_artifacts = copy.deepcopy(manifest)
        reordered_artifacts["codeql"]["sourceArtifacts"][1:3] = reversed(
            reordered_artifacts["codeql"]["sourceArtifacts"][1:3]
        )
        mutations.append(reordered_artifacts)

        duplicate_artifact_id = copy.deepcopy(manifest)
        duplicate_artifact_id["codeql"]["sourceArtifacts"][1]["id"] = (
            duplicate_artifact_id["codeql"]["sourceArtifacts"][0]["id"]
        )
        mutations.append(duplicate_artifact_id)

        duplicate_build_job_id = copy.deepcopy(manifest)
        duplicate_build_job_id["build"]["requiredGateJobId"] = (
            duplicate_build_job_id["build"]["ci120ProducerJobId"]
        )
        mutations.append(duplicate_build_job_id)

        cross_workflow_job_id = copy.deepcopy(manifest)
        cross_workflow_job_id["codeql"]["reporterJobId"] = (
            cross_workflow_job_id["ci120"]["verifierJobId"]
        )
        mutations.append(cross_workflow_job_id)

        duplicate_status_id = copy.deepcopy(manifest)
        duplicate_status_id["codeql"]["statusId"] = duplicate_status_id["ci120"]["statusId"]
        mutations.append(duplicate_status_id)

        duplicate_run_id = copy.deepcopy(manifest)
        duplicate_run_id["codeql"]["reporterRunId"] = duplicate_run_id["ci120"][
            "verifierRunId"
        ]
        duplicate_run_id["codeql"]["reporterRunUrl"] = duplicate_run_id["ci120"][
            "verifierRunUrl"
        ]
        duplicate_run_id["codeql"]["statusTargetUrl"] = (
            f"{duplicate_run_id['ci120']['verifierRunUrl']}/attempts/"
            f"{duplicate_run_id['codeql']['reporterRunAttempt']}"
        )
        mutations.append(duplicate_run_id)

        outside_publish_step = copy.deepcopy(manifest)
        outside_publish_step["ci120"]["statusCreatedAt"] = "2026-08-30T00:59:58Z"
        mutations.append(outside_publish_step)

        noncanonical_timestamp = copy.deepcopy(manifest)
        noncanonical_timestamp["codeql"]["statusUpdatedAt"] = (
            "2026-08-29T21:00:03-05:00"
        )
        mutations.append(noncanonical_timestamp)

        for mutation in mutations:
            with self.subTest(mutation=mutation):
                with self.assertRaises(exact_evidence.ExactEvidenceError):
                    exact_evidence.validate_manifest(mutation)

    def test_status_target_and_timestamp_boundaries_are_exact(self) -> None:
        boundary = dict(self.VALUES)
        boundary["EXACT_BUILD_MATRIX_STATUS_CREATED_AT"] = "2026-08-30T00:59:59Z"
        boundary["EXACT_BUILD_MATRIX_STATUS_UPDATED_AT"] = "2026-08-30T01:00:06Z"
        exact_evidence.build_manifest(boundary)

        for field, replacement in (
            ("EXACT_BUILD_MATRIX_STATUS_CREATED_AT", "2026-08-30T00:59:58Z"),
            ("EXACT_BUILD_MATRIX_STATUS_UPDATED_AT", "2026-08-30T01:00:07Z"),
            ("EXACT_BUILD_MATRIX_STATUS_CREATED_AT", "2026-08-29T20:00:02-05:00"),
            (
                "EXACT_CODEQL_STATUS_TARGET_URL",
                "https://github.com/Krilliac/SparkEngine/actions/runs/303/attempts/4",
            ),
        ):
            hostile = dict(self.VALUES)
            hostile[field] = replacement
            with self.subTest(field=field, replacement=replacement):
                with self.assertRaises(exact_evidence.ExactEvidenceError):
                    exact_evidence.build_manifest(hostile)

    def test_verify_manifest_rejects_valid_in_window_timestamp_replay(self) -> None:
        replayed = dict(self.VALUES)
        replayed["EXACT_BUILD_MATRIX_STATUS_CREATED_AT"] = "2026-08-30T01:00:01Z"
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "SparkEngine-Exact-CI-Evidence.json"
            written = exact_evidence.write_manifest(path, replayed)
            self.assertEqual(exact_evidence.validate_manifest(written), written)
            with self.assertRaisesRegex(
                exact_evidence.ExactEvidenceError, "differs from the verified gate outputs"
            ):
                exact_evidence.verify_manifest(path, self.VALUES)

    def test_manifest_loader_rejects_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                newline="\n",
                suffix=".json",
                dir=raw,
                delete=False,
            ) as stream:
                stream.write('{"schemaVersion": 2, "schemaVersion": 2}\n')
                path = Path(stream.name)
            with self.assertRaisesRegex(
                exact_evidence.ExactEvidenceError, "duplicate key"
            ):
                exact_evidence.load_manifest(path)



class BuildMatrixEvidenceTests(ContractTestCase):
    """The build-matrix evidence pair must fail closed on absence and fabrication.

    Every rejection below used to pass: site-data validation never opened these
    two files, so deleted, truncated, and hand-written evidence all validated
    exactly as well as real evidence.
    """

    INVENTORY = {
        "schemaVersion": 3,
        "stableV1Products": [],
        "configuredTargetEvidence": [],
    }
    REPORT = {
        "schemaVersion": 3,
        "state": "blocked",
        "errorCount": 1,
        "warningCount": 0,
        "findings": [{"category": "c", "severity": "error", "message": "m"}],
    }

    def errors(self, inventory=None, report=None, profile=None):
        return site_data_validate.build_matrix_evidence_errors(
            copy.deepcopy(self.INVENTORY if inventory is None else inventory),
            copy.deepcopy(self.REPORT if report is None else report),
            copy.deepcopy(profile),
        )

    def assert_error(self, fragment: str, **kwargs) -> None:
        messages = " ".join(self.errors(**kwargs))
        self.assertIn(fragment, messages)

    def test_real_repository_evidence_passes(self) -> None:
        # The committed evidence pair must satisfy every rejection above.
        profile = self.profile_of(copy.deepcopy(self.contract))
        inventory = site_data_common.load_json(
            REPO_ROOT / "docs" / "readiness" / "build-matrix-inventory.json"
        )
        report = site_data_common.load_json(
            REPO_ROOT / "docs" / "readiness" / "build-matrix-parity-findings.json"
        )
        self.assertEqual(
            site_data_validate.build_matrix_evidence_errors(inventory, report, profile), []
        )

    def test_stale_schema_version_is_rejected(self) -> None:
        self.assert_error("inventory schemaVersion must be 3", inventory={**self.INVENTORY, "schemaVersion": 2})
        self.assert_error("findings schemaVersion must be 3", report={**self.REPORT, "schemaVersion": 2})

    def test_non_object_evidence_is_rejected(self) -> None:
        self.assert_error("must contain objects", inventory=[])

    def test_internal_error_state_is_not_an_accepted_report(self) -> None:
        self.assert_error(
            "state must be blocked or clean",
            report={**self.REPORT, "state": "internal-error"},
        )

    def test_declared_counts_must_match_the_findings_list(self) -> None:
        self.assert_error(
            "do not match the findings list",
            report={**self.REPORT, "errorCount": 0, "warningCount": 0},
        )

    def test_blocked_state_cannot_be_relabelled_clean(self) -> None:
        self.assert_error(
            "state must be blocked exactly when a blocking finding is present",
            report={**self.REPORT, "state": "clean", "errorCount": 0},
        )

    def test_truncated_findings_list_is_rejected(self) -> None:
        self.assert_error("findings must be a list", report={**self.REPORT, "findings": None})

    def test_malformed_finding_entry_is_rejected(self) -> None:
        self.assert_error(
            "needs a category, severity, and message",
            report={**self.REPORT, "findings": ["oops"], "errorCount": 0},
        )

    def test_product_drift_from_the_profile_is_rejected(self) -> None:
        profile = self.profile_of(copy.deepcopy(self.contract))
        self.assert_error("have drifted from the canonical stable-v1 profile", profile=profile)

    def test_missing_configuration_evidence_record_is_rejected(self) -> None:
        profile = self.profile_of(copy.deepcopy(self.contract))
        inventory = {
            "schemaVersion": 3,
            "stableV1Products": profile["buildProducts"],
            "configuredTargetEvidence": [{"profile": "windows-shipping", "status": "absent"}],
        }
        self.assert_error(
            "must name every canonical build configuration exactly once",
            inventory=inventory,
            profile=profile,
        )

    def test_clean_report_may_not_omit_a_supported_product(self) -> None:
        profile = self.profile_of(copy.deepcopy(self.contract))
        # Flip one product to `shared`: it stays supported surface, so a clean
        # report that never evidenced its profile must still be refused.
        profile["buildProducts"][0]["applicability"] = "shared"
        inventory = {
            "schemaVersion": 3,
            "stableV1Products": profile["buildProducts"],
            "configuredTargetEvidence": [
                {"profile": entry["id"], "status": "absent"} for entry in profile["buildConfigurations"]
            ],
        }
        clean = {
            "schemaVersion": 3, "state": "clean", "errorCount": 0, "warningCount": 0, "findings": [],
        }
        messages = " ".join(self.errors(inventory=inventory, report=clean, profile=profile))
        self.assertIn("clean build-matrix report omits configured evidence for supported product", messages)
        self.assertIn("(shared)", messages)

    def test_clean_report_with_full_evidence_is_accepted(self) -> None:
        profile = self.profile_of(copy.deepcopy(self.contract))
        inventory = {
            "schemaVersion": 3,
            "stableV1Products": profile["buildProducts"],
            "configuredTargetEvidence": [
                {"profile": entry["id"], "status": "available"} for entry in profile["buildConfigurations"]
            ],
        }
        clean = {
            "schemaVersion": 3, "state": "clean", "errorCount": 0, "warningCount": 0, "findings": [],
        }
        self.assertEqual(self.errors(inventory=inventory, report=clean, profile=profile), [])


class DuplicateJsonKeyTests(unittest.TestCase):
    """A repeated JSON key silently keeps the last value; refuse it.

    Exercised through the decoder hook rather than a temporary file, so this
    suite keeps its no-file-mutation guarantee.
    """

    @staticmethod
    def parse(text: str) -> Any:
        return json.loads(text, object_pairs_hook=site_data_common._reject_duplicate_keys)

    def test_duplicate_key_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate JSON key 'a'"):
            self.parse('{"a": 1, "a": 2}')

    def test_duplicate_key_nested_in_a_list_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate JSON key 'severity'"):
            self.parse('{"findings": [{"severity": "warning", "severity": "error"}]}')

    def test_distinct_keys_still_load(self) -> None:
        self.assertEqual(self.parse('{"a": 1, "b": 2}'), {"a": 1, "b": 2})
        self.assertEqual(self.parse('{"a": {"b": 1}, "c": [1, 2]}'), {"a": {"b": 1}, "c": [1, 2]})

    def test_contract_files_contain_no_duplicate_keys(self) -> None:
        for name in (
            "docs/site/readiness.json",
            "docs/readiness/build-matrix-inventory.json",
            "docs/readiness/build-matrix-parity-findings.json",
        ):
            with self.subTest(name=name):
                self.parse((REPO_ROOT / name).read_text(encoding="utf-8"))


class StrictJsonDecoderTests(unittest.TestCase):
    def test_numeric_overflow_is_rejected(self) -> None:
        with self.assertRaisesRegex(SiteDataError, "non-finite"):
            site_data_common.decode_json_bytes(b'{"value":1e999}', "probe")

    def test_lone_surrogate_is_rejected(self) -> None:
        with self.assertRaisesRegex(SiteDataError, "invalid Unicode"):
            site_data_common.decode_json_bytes(b'{"value":"\\ud800"}', "probe")


class AtomicPublicationTests(unittest.TestCase):
    def test_repeated_atomic_publication_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            output = Path(raw) / "bundle.json"

            for payload in (b"first", b"second payload", b"third"):
                site_data_common.write_bytes_atomic(output, payload)
                self.assertEqual(output.read_bytes(), payload)

    def test_directory_identity_ignores_size_changes(self) -> None:
        before = mock.Mock(st_dev=7, st_ino=11, st_size=128)
        after = mock.Mock(st_dev=7, st_ino=11, st_size=256)

        self.assertEqual(
            site_data_common._directory_identity(before),
            site_data_common._directory_identity(after),
        )
        self.assertNotEqual(
            site_data_common._identity(before), site_data_common._identity(after)
        )

    def test_directory_chain_token_ignores_unrelated_child_churn(self) -> None:
        before = mock.Mock(
            st_dev=7,
            st_ino=11,
            st_mode=0o40755,
            st_mtime_ns=100,
            st_ctime_ns=200,
            st_file_attributes=0,
        )
        after = mock.Mock(
            st_dev=7,
            st_ino=11,
            st_mode=0o40755,
            st_mtime_ns=300,
            st_ctime_ns=400,
            st_file_attributes=0,
        )
        replacement = mock.Mock(
            st_dev=7,
            st_ino=12,
            st_mode=0o40755,
            st_mtime_ns=300,
            st_ctime_ns=400,
            st_file_attributes=0,
        )

        self.assertEqual(
            site_data_common._directory_token(before),
            site_data_common._directory_token(after),
        )
        self.assertNotEqual(
            site_data_common._directory_token(before),
            site_data_common._directory_token(replacement),
        )

    def test_parent_directory_substitution_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            output = Path(raw) / "bundle.json"
            identities = iter(((7, 11), (7, 11), (7, 12)))

            with mock.patch.object(
                site_data_common, "_directory_identity", side_effect=identities
            ):
                with self.assertRaisesRegex(SiteDataError, "replaced during publication"):
                    site_data_common.write_bytes_atomic(output, b"payload")

    def test_same_content_file_substitution_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            output = directory / "bundle.json"
            original_replace = os.replace
            substituted = False

            def substituting_replace(source: Any, destination: Any) -> None:
                nonlocal substituted
                original_replace(source, destination)
                if not substituted:
                    substituted = True
                    impostor = directory / "impostor.json"
                    shutil.copyfile(destination, impostor)
                    original_replace(impostor, destination)

            with mock.patch.object(
                site_data_common.os, "replace", side_effect=substituting_replace
            ):
                with self.assertRaisesRegex(SiteDataError, "verified temporary file"):
                    site_data_common.write_bytes_atomic(output, b"payload")



class SelectorResolutionTests(ContractTestCase):
    """requiredCiJobs and testSelectors must name something that exists."""

    def selectors_of(self, item: dict[str, Any], *, legacy: bool = False) -> tuple[list[str], list[str]]:
        validator = site_data_validate.Validator(self.mutable, allow_legacy_contract=legacy)
        validator.validate_selectors(item, "workItems.PROBE")
        return validator.errors, validator.legacy

    def test_resolvable_job_and_selector_pass(self) -> None:
        resolvable_job = sorted(contract_selectors.workflow_job_ids())[0]
        resolvable_test = sorted(contract_selectors.test_selector_targets())[0]
        errors, legacy = self.selectors_of(
            {"requiredCiJobs": [resolvable_job], "testSelectors": [resolvable_test]}
        )
        self.assertEqual([], errors)
        self.assertEqual([], legacy)

    def test_ctest_filters_must_select_a_registered_label_or_test(self) -> None:
        for arguments, fragment in (
            (["-L", "no-such-label", "--no-tests=error"], "ctest -L no-such-label selects no registered label"),
            (["-R", "^NoSuchTest_X$"], "ctest -R ^NoSuchTest_X$ selects no registered test"),
            # A ${}-built name admits only its own literal parts.
            (["-R", "^TerrafrontMultiClientX$"], "selects no registered test"),
            (["-R", "(["], "is not a valid regular expression"),
        ):
            with self.subTest(arguments=arguments):
                errors = contract_selectors.ctest_filter_errors(arguments)
                self.assertEqual(1, len(errors), errors)
                self.assertIn(fragment, errors[0])
        for arguments in (
            ["-R", "^ModuleManifest_SparkGameRTS_RTSSkirmish$"],  # module.json tests.prefixes
            ["-R", "^TerrafrontMultiClient_VehicleLifecycle$"],  # NAME TerrafrontMultiClient_${_tf_name}
            ["-R", "BackupRestore"],  # foreach item "Persistence_BackupRestore_=7"
            ["--tests-regex=^Persistence_RecoveryDrill$"],
            ["-L", "module-package-run"],  # label list passed to spark_add_module_objective_test
            ["-L", "recovery-drill", "-LE", "no-such-label", "-E", "NoSuchTest"],  # exclusions need not match
            ["-R", "$TEST_NAME"],  # shell placeholder, not resolvable
        ):
            with self.subTest(arguments=arguments):
                self.assertEqual([], contract_selectors.ctest_filter_errors(arguments))

    def test_unregistered_ctest_filter_is_accepted_only_as_planned_debt(self) -> None:
        self.assertEqual(1, len(contract_selectors.ctest_filter_errors(["-L", "metal"])))
        self.assertEqual(1, len(contract_selectors.ctest_filter_errors(["-L", "metal"], ["Metal_*"])))
        self.assertEqual([], contract_selectors.ctest_filter_errors(["-L", "metal"], ["Metal_*", "metal"]))
        self.assertEqual(
            [], contract_selectors.ctest_filter_errors(["-R", "MMOIntegratedWorld"], ["MMOIntegratedWorld_*"])
        )

    def test_work_item_ctest_filters_that_select_nothing_are_rejected(self) -> None:
        # The contract before CI-110: a label no test carries and two filters
        # whose tests do not exist yet, undeclared.
        items = self.items_of(self.mutable)
        commands = items["RDY-020"]["commands"]
        index = next(i for i, command in enumerate(commands) if " -L asset " in command)
        commands[index] = " -L profile-package ".join(commands[index].split(" -L asset "))
        for identifier, selector in (("RHI-220", "metal"), ("MOD-320", "MMOIntegratedWorld_*")):
            items[identifier]["testSelectors"].remove(selector)
            items[identifier]["plannedTestSelectors"].remove(selector)
        validator = site_data_validate.Validator(self.mutable)
        validator.validate_work_items()
        ctest_errors = sorted(error for error in validator.errors if "selects no registered" in error)
        self.assertEqual(
            [
                "workItems.MOD-320.commands[0]: ctest -R MMOIntegratedWorld selects no registered test; "
                "fix it or declare it in plannedTestSelectors",
                f"workItems.RDY-020.commands[{index}]: ctest -L profile-package selects no registered label; "
                "fix it or declare it in plannedTestSelectors",
                "workItems.RHI-220.commands[0]: ctest -L metal selects no registered label; "
                "fix it or declare it in plannedTestSelectors",
            ],
            ctest_errors,
        )

    def test_product_directory_ctest_registrations_resolve(self) -> None:
        # OPS-100's crash selectors are registered in SparkCrashReporter/CMakeLists.txt,
        # not Tests/CMakeLists.txt; a resolver blind to product directories
        # reported them missing.
        for name in ("CrashManifest_PathEscape", "CrashManifest_SecretRedaction", "CrashReporter_Consent"):
            self.assertTrue(contract_selectors.resolve_test_selector(name), name)

    def test_installer_selectors_registered_only_in_sparkinstaller_resolve(self) -> None:
        # INST-130/131/132 selectors are registered in SparkInstaller/CMakeLists.txt
        # so they also run in the standalone build-installer combo.
        installer_cmake = (REPO_ROOT / "SparkInstaller" / "CMakeLists.txt").read_text(encoding="utf-8")
        tests_cmake = (REPO_ROOT / "Tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        names = ("Installer_Tamper", "Installer_AtomicUpdate", "Installer_Interrupted",
                 "Installer_Uninstall", "Installer_Upgrade", "Installer_Rollback")
        for name in names:
            self.assertIn(f"NAME {name} ", installer_cmake, name)
            self.assertNotIn(f"NAME {name} ", tests_cmake, name)
            self.assertTrue(contract_selectors.resolve_test_selector(name), name)
        self.assertFalse(contract_selectors.resolve_test_selector("Installer_AtomicUpdates"))
        self.assertFalse(contract_selectors.resolve_test_selector("Installer_Tampered"))

    def test_registration_scan_skips_vendored_build_and_hidden_trees(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw).resolve()
            registrations = {
                "SparkProduct/CMakeLists.txt": "add_test(NAME ProductOnly_Case COMMAND x)",
                "cmake/SparkPolicy.cmake": "add_test(NAME ModuleOnly_Case COMMAND x)",
                "ThirdParty/lib/CMakeLists.txt": "add_test(NAME Vendored_Case COMMAND x)",
                "build/windows-release/CMakeLists.txt": "add_test(NAME BuildTree_Case COMMAND x)",
                ".claude/worktrees/copy/CMakeLists.txt": "add_test(NAME WorktreeCopy_Case COMMAND x)",
                "Tests/CMakeLists.txt": "set(UNRELATED ON)",
            }
            for relative, text in registrations.items():
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                site_data_common.write_bytes_atomic(root / relative, text.encode("utf-8"))
            contract_selectors.reset_caches()
            try:
                with mock.patch.object(contract_selectors, "REPO_ROOT", root), mock.patch.object(
                    contract_selectors, "TEST_ROOT", root / "Tests"
                ):
                    targets = contract_selectors.test_selector_targets()
            finally:
                contract_selectors.reset_caches()
        self.assertEqual({"ProductOnly_Case", "ModuleOnly_Case"}, set(targets))

    def test_unresolvable_job_is_an_error_by_default(self) -> None:
        errors, legacy = self.selectors_of(
            {"requiredCiJobs": ["no-such-workflow-job"], "testSelectors": []}
        )
        self.assertEqual([], legacy)
        self.assertEqual(1, len(errors))
        self.assertIn("no workflow job is defined with this id", errors[0])

    def test_unresolvable_selector_is_an_error_by_default(self) -> None:
        errors, _ = self.selectors_of(
            {"requiredCiJobs": [], "testSelectors": ["NoSuchTestFamily_*"]}
        )
        self.assertEqual(1, len(errors))
        self.assertIn("no CTest test, label, or SparkTests definition matches", errors[0])

    def test_legacy_flag_downgrades_but_never_hides(self) -> None:
        errors, legacy = self.selectors_of(
            {"requiredCiJobs": ["no-such-workflow-job"], "testSelectors": []}, legacy=True
        )
        self.assertEqual([], errors)
        self.assertEqual(1, len(legacy))
        self.assertIn("no-such-workflow-job", legacy[0])

    # A synthetic id, never a real workflow job: these two cases are about a job
    # that does not exist YET, so naming a real one makes the fixture stop testing
    # what it claims the moment that job is added (as happened with asset-integrity).
    PLANNED_JOB_ID = "planned-future-gate-that-does-not-exist"

    def test_planned_debt_excuses_only_what_it_declares(self) -> None:
        errors, legacy = self.selectors_of(
            {
                "requiredCiJobs": [self.PLANNED_JOB_ID],
                "plannedCiJobs": [self.PLANNED_JOB_ID],
                "testSelectors": [],
            }
        )
        self.assertEqual([], errors)
        self.assertEqual([], legacy)

    def test_planned_entry_must_be_declared_as_required(self) -> None:
        errors, _ = self.selectors_of(
            {"requiredCiJobs": [], "plannedCiJobs": [self.PLANNED_JOB_ID], "testSelectors": []}
        )
        self.assertEqual(1, len(errors))
        self.assertIn("is not declared in requiredCiJobs", errors[0])

    def test_planned_entry_that_now_exists_must_be_promoted(self) -> None:
        existing = sorted(contract_selectors.workflow_job_ids())[0]
        errors, _ = self.selectors_of(
            {
                "requiredCiJobs": [existing],
                "plannedCiJobs": [existing],
                "testSelectors": [],
            }
        )
        self.assertEqual(1, len(errors))
        self.assertIn("must be promoted out of plannedCiJobs", errors[0])

    def test_glob_entry_point_must_match_a_real_file(self) -> None:
        validator = site_data_validate.Validator(self.mutable)
        validator.require_path("GameModules/*/probe-that-matches-nothing.json", "probe.entryPoints[0]")
        self.assertEqual(1, len(validator.errors))
        self.assertIn("path pattern matches no file", validator.errors[0])

    def test_glob_entry_point_that_resolves_is_accepted(self) -> None:
        validator = site_data_validate.Validator(self.mutable)
        validator.require_path("Templates/*/Assets/manifest.json", "probe.entryPoints[0]")
        self.assertEqual([], validator.errors)


class LegacyContractDebtTests(ContractTestCase):
    """The contract's unresolved references are a ledger that may only shrink."""

    # The ledger reached zero: every requiredCiJobs, testSelectors and glob entry
    # point in the contract now resolves, the workflows dropped
    # --allow-legacy-contract, and the flag survives only to drive the downgrade
    # path from these tests. A rise means a new unresolvable reference was
    # written. Never raise this ceiling to make a run green.
    DEBT_CEILING = 0

    def test_legacy_debt_does_not_grow(self) -> None:
        validator = site_data_validate.Validator(self.mutable, allow_legacy_contract=True)
        validator.validate()
        self.assertEqual([], validator.errors)
        self.assertLessEqual(len(validator.legacy), self.DEBT_CEILING)

    def test_every_downgraded_entry_names_its_reference_class(self) -> None:
        validator = site_data_validate.Validator(self.mutable, allow_legacy_contract=True)
        validator.validate()
        recognized = (
            "no workflow job is defined with this id",
            "no CTest test, label, or SparkTests definition matches",
            "path pattern matches no file",
        )
        unrecognized = [
            entry for entry in validator.legacy if not any(reason in entry for reason in recognized)
        ]
        self.assertEqual([], unrecognized)

    def test_strict_default_accepts_the_current_contract(self) -> None:
        """The debt is paid: no waiver, no downgrade, no error."""
        validator = site_data_validate.Validator(self.mutable)
        validator.validate()
        self.assertEqual([], validator.errors)
        self.assertEqual([], validator.legacy)

    # RED proof for the pair above: strict validation still has to REFUSE an
    # unresolvable reference. Without these, "strict passes" would also be true
    # of a validator that stopped resolving anything at all.
    def test_strict_default_refuses_an_unresolvable_required_ci_job(self) -> None:
        item = self.mutable["workItems"][0]
        item["requiredCiJobs"] = ["no-such-workflow-job"]
        item.pop("plannedCiJobs", None)
        self.assert_rejected(self.mutable, "no workflow job is defined with this id")

    def test_strict_default_refuses_an_unresolvable_test_selector(self) -> None:
        item = self.mutable["workItems"][0]
        item["testSelectors"] = ["NoSuchTestFamily_*"]
        item.pop("plannedTestSelectors", None)
        self.assert_rejected(
            self.mutable, "no CTest test, label, or SparkTests definition matches"
        )


class MissingReferencedPathTests(ContractTestCase):
    """Every entryPoints/documentationUpdates and docs-catalog path must exist, for open work too.

    A planned output is referenced only after it lands; not-yet-written tests and
    CI jobs are declared in plannedTestSelectors/plannedCiJobs instead.
    """

    # Retired from the old future-path allowlist, so the pre-retirement validator
    # excused it on an unfinished item. Asserted missing so a later file of that
    # name cannot turn these rejection cases into silent passes.
    RETIRED_PLANNED_PATH = "docs/operations/server-runbook.md"
    MISSING_MESSAGE = f"referenced path does not exist: {RETIRED_PLANNED_PATH}"

    def setUp(self) -> None:
        super().setUp()
        self.assertFalse((REPO_ROOT / self.RETIRED_PLANNED_PATH).exists())

    def open_item(self) -> dict[str, Any]:
        item = self.items_of(self.mutable)["OPS-110"]
        self.assertNotEqual("done", item["status"])
        return item

    def work_item_errors(self) -> list[str]:
        validator = site_data_validate.Validator(self.mutable)
        validator.validate_work_items()
        return validator.errors

    def docs_catalog_errors(self) -> list[str]:
        validator = site_data_validate.Validator(self.mutable)
        validator.validate_docs_catalog()
        return validator.errors

    def assert_one_missing(self, errors: list[str], location: str) -> None:
        matching = [error for error in errors if self.MISSING_MESSAGE in error]
        self.assertEqual(1, len(matching), errors)
        self.assertIn(location, matching[0])

    def test_missing_entry_point_on_an_open_item_is_rejected(self) -> None:
        item = self.open_item()
        item["entryPoints"].append(self.RETIRED_PLANNED_PATH)
        index = len(item["entryPoints"]) - 1
        self.assert_one_missing(self.work_item_errors(), f"workItems.OPS-110.entryPoints[{index}]")

    def test_missing_documentation_update_on_an_open_item_is_rejected(self) -> None:
        item = self.open_item()
        item["documentationUpdates"].append(self.RETIRED_PLANNED_PATH)
        index = len(item["documentationUpdates"]) - 1
        self.assert_one_missing(self.work_item_errors(), f"workItems.OPS-110.documentationUpdates[{index}]")

    def test_missing_docs_catalog_featured_path_is_rejected(self) -> None:
        featured = self.mutable["docsCatalog"].setdefault("featuredSourcePaths", [])
        featured.append(self.RETIRED_PLANNED_PATH)
        self.assert_one_missing(
            self.docs_catalog_errors(), f"docsCatalog.featuredSourcePaths[{len(featured) - 1}]"
        )

    def test_existing_path_on_an_open_item_is_accepted(self) -> None:
        self.open_item()["documentationUpdates"].append("wiki/advanced/Server-Operations-Runbook.md")
        errors = self.work_item_errors()
        self.assertFalse(
            any("Server-Operations-Runbook.md" in error for error in errors),
            errors,
        )


class ReadyAndPassingEvidenceTests(ContractTestCase):
    """Direct blockers and required gates stop promotion; promotion needs evidence.

    These drive validate_readiness directly: validate() always runs it, and the
    full pipeline would add seconds per case to a suite CI bounds at five minutes.
    """

    def readiness_errors(self) -> str:
        validator = site_data_validate.Validator(self.mutable)
        validator.validate_readiness({item["id"] for item in self.mutable["workItems"]})
        return "\n".join(validator.errors)

    def ready_console(self) -> dict[str, Any]:
        """platform.console made ready with a passing gate and evidence, all else unchanged."""
        capability = self.capabilities_of(self.mutable)["platform.console"]
        gate = self.gates_of(self.mutable)["G00"]
        gate["state"] = "passing"
        gate["blockingWorkItemIds"] = []
        gate["evidence"] = [{"type": "document", "label": "Test-only gate evidence", "path": "README.md"}]
        capability["release"] = "ready"
        capability["requiredGateIds"] = ["G00"]
        capability["blockingWorkItemIds"] = []
        capability["evidence"] = [
            {"type": "document", "label": "Test-only capability evidence", "path": "README.md"}
        ]
        return capability

    def test_control_ready_capability_with_gate_and_evidence_is_accepted(self) -> None:
        self.ready_console()
        self.assertEqual("", self.readiness_errors())

    def test_ready_capability_rejects_an_open_direct_blocker(self) -> None:
        capability = self.ready_console()
        self.assertNotEqual("done", self.items_of(self.mutable)["RDY-000"]["status"])
        capability["blockingWorkItemIds"] = ["RDY-000"]
        self.assertIn(
            "capabilities.platform.console: ready capability has unfinished blockers: ['RDY-000']",
            self.readiness_errors(),
        )

    def test_ready_capability_rejects_a_non_passing_required_gate(self) -> None:
        capability = self.ready_console()
        self.assertNotEqual("passing", self.gates_of(self.mutable)["G09"]["state"])
        capability["requiredGateIds"] = ["G00", "G09"]
        self.assertIn(
            "capabilities.platform.console: ready capability has non-passing gates: ['G09']",
            self.readiness_errors(),
        )

    def test_passing_gate_rejects_an_open_direct_blocker(self) -> None:
        self.ready_console()
        self.gates_of(self.mutable)["G00"]["blockingWorkItemIds"] = ["RDY-000"]
        self.assertIn("gates.G00: passing gate has unfinished blockers: ['RDY-000']", self.readiness_errors())

    def test_ready_capability_requires_a_required_gate(self) -> None:
        self.ready_console()["requiredGateIds"] = []
        self.assertIn(
            "capabilities.platform.console: ready capability must name at least one required gate",
            self.readiness_errors(),
        )

    def test_ready_capability_requires_evidence(self) -> None:
        self.ready_console()["evidence"] = []
        self.assertIn(
            "capabilities.platform.console: ready capability requires evidence", self.readiness_errors()
        )

    def test_passing_gate_requires_evidence(self) -> None:
        self.ready_console()
        self.gates_of(self.mutable)["G00"]["evidence"] = []
        self.assertIn("gates.G00: passing gate requires evidence", self.readiness_errors())


class LiveContractTests(ContractTestCase):
    """The checked-in contract passes the strict validator, which runs every cross-reference check.

    Each full validate() costs seconds and CI bounds this suite at five minutes,
    so the wiring of the cross-reference checks shares one hostile run.
    """

    def test_live_contract_validates_strictly(self) -> None:
        validator = site_data_validate.Validator(self.mutable)
        validator.validate()
        self.assertEqual([], validator.legacy)

    def test_validate_runs_the_referenced_path_and_prose_reference_checks(self) -> None:
        missing = MissingReferencedPathTests.RETIRED_PLANNED_PATH
        item = self.items_of(self.mutable)["OPS-110"]
        self.assertNotEqual("done", item["status"])
        item["entryPoints"].append(missing)
        index = len(item["entryPoints"]) - 1
        self.gates_of(self.mutable)["G00"]["summary"] += " Tracked by DOC-999."
        with self.assertRaises(SiteDataError) as raised:
            site_data_validate.Validator(self.mutable).validate()
        message = str(raised.exception)
        self.assertIn(
            f"workItems.OPS-110.entryPoints[{index}]: referenced path does not exist: {missing}", message
        )
        self.assertIn("gates.G00.summary: names unknown work item DOC-999", message)


class ProseCrossReferenceTests(ContractTestCase):
    """Work-item and gate IDs named in free text must resolve to declared records."""

    def prose_errors(self) -> list[str]:
        validator = site_data_validate.Validator(self.mutable)
        item_ids = {item["id"] for item in self.mutable["workItems"]}
        gate_ids = {gate["id"] for gate in self.mutable["readiness"]["gates"]}
        validator.validate_prose_references(item_ids, gate_ids)
        return validator.errors

    def test_live_contract_prose_references_all_resolve(self) -> None:
        self.assertEqual([], self.prose_errors())

    def test_unknown_gate_in_readiness_changes_is_rejected(self) -> None:
        self.items_of(self.mutable)["RDY-000"]["readinessChanges"].append("Moves G99 to passing.")
        errors = self.prose_errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn("workItems.RDY-000.readinessChanges", errors[0])
        self.assertIn("unknown gate G99", errors[0])

    def test_unknown_work_item_in_capability_limitations_is_rejected(self) -> None:
        capability = self.capabilities_of(self.mutable)["platform.console"]
        capability["limitations"].append("Waits on RDY-999 before any claim.")
        errors = self.prose_errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn("capabilities.platform.console.limitations", errors[0])
        self.assertIn("unknown work item RDY-999", errors[0])

    def test_unknown_work_item_in_gate_summary_is_rejected(self) -> None:
        self.gates_of(self.mutable)["G00"]["summary"] += " Tracked by DOC-999."
        errors = self.prose_errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn("gates.G00.summary: names unknown work item DOC-999", errors[0])

    def test_hash_names_and_unprefixed_tokens_are_not_references(self) -> None:
        item = self.items_of(self.mutable)["RDY-000"]
        item["risks"].append("SHA-256 digests, UTF-8 text, and X-100 are not work-item IDs; nor is G100.")
        self.assertEqual([], self.prose_errors())


class PublicNumericClaimTests(ContractTestCase):
    """Hand-written counts on public surfaces must resolve to a metric or a reviewed entry."""

    SURFACE = "README.md"

    @staticmethod
    def claim_errors(
        texts: dict[str, str],
        entries: Any,
        metrics: dict[str, int | float] | None = None,
        managed: dict[str, tuple[re.Pattern[str], ...]] | None = None,
    ) -> list[str]:
        return site_data_validate.public_numeric_claim_errors(
            texts,
            entries,
            lambda: metrics or {},
            managed or {},
        )

    @staticmethod
    def entry(text: str, classification: str = "historical", **extra: str) -> dict[str, str]:
        return {
            "surface": PublicNumericClaimTests.SURFACE,
            "text": text,
            "classification": classification,
            "owner": "unassigned",
            **extra,
        }

    def test_current_public_surfaces_have_no_unclaimed_numbers(self) -> None:
        validator = site_data_validate.Validator(self.mutable)
        validator.validate_public_numeric_claims()
        self.assertEqual(validator.errors, [])

    def test_unclaimed_number_is_rejected(self) -> None:
        errors = self.claim_errors({self.SURFACE: "The editor ships 12 panels.\n"}, [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("README.md:1", errors[0])
        self.assertIn("unclaimed numeric claim '12 panels'", errors[0])

    def test_wrapped_and_qualified_claims_are_detected(self) -> None:
        errors = self.claim_errors(
            {self.SURFACE: "Covers 50+ other\nsubsystems and ~1,326 lines and 2,504/2,509 tests.\n"},
            [],
        )
        joined = "\n".join(errors)
        self.assertIn("'50+ other\\nsubsystems'", joined)
        self.assertIn("'~1,326 lines'", joined)
        self.assertIn("'2,504/2,509 tests'", joined)
        self.assertIn("README.md:1", joined)
        self.assertIn("README.md:2", joined)

    def test_product_versions_are_not_counts(self) -> None:
        text = "Win32 + DirectX 11 ImGui backends; a DirectX 12 backend; version 1 files are rejected.\n"
        self.assertEqual(self.claim_errors({self.SURFACE: text}, []), [])

    def test_stale_metric_value_is_rejected(self) -> None:
        text = "- 65 node palette entries\n"
        entry = self.entry("65 node palette entries", "metric", metricId="visualScript.nodes")
        errors = self.claim_errors({self.SURFACE: text}, [entry], {"visualScript.nodes": 64})
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("claims 65 but visualScript.nodes is 64", errors[0])
        self.assertEqual(
            self.claim_errors(
                {self.SURFACE: "- 64 node palette entries\n"},
                [self.entry("64 node palette entries", "metric", metricId="visualScript.nodes")],
                {"visualScript.nodes": 64},
            ),
            [],
        )

    def test_lower_bound_metric_claim_tolerates_growth_only(self) -> None:
        metrics = {"visualScript.nodes": 64}
        accepted = self.entry("60+ nodes", "metric", metricId="visualScript.nodes")
        self.assertEqual(self.claim_errors({self.SURFACE: "60+ nodes\n"}, [accepted], metrics), [])
        rejected = self.entry("70+ nodes", "metric", metricId="visualScript.nodes")
        errors = self.claim_errors({self.SURFACE: "70+ nodes\n"}, [rejected], metrics)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("claims at least 70 but visualScript.nodes is 64", errors[0])

    def test_metric_claims_must_be_exact_single_and_measurable(self) -> None:
        metrics = {"visualScript.nodes": 64}
        cases = [
            ("~64 nodes", "visualScript.nodes", "approximate"),
            ("64 nodes and 64 files", "visualScript.nodes", "exactly one numeric claim"),
            ("64 nodes", "tests.executed", "not measurable from the source tree"),
            ("64 nodes", "made.up", "unknown metric"),
        ]
        for text, metric_id, fragment in cases:
            with self.subTest(text=text, metric=metric_id):
                entry = self.entry(text, "metric", metricId=metric_id)
                errors = self.claim_errors({self.SURFACE: text + "\n"}, [entry], metrics)
                self.assertTrue(any(fragment in error for error in errors), errors)

    def test_claim_inside_auto_block_is_ignored(self) -> None:
        text = "Intro.\n<!-- AUTO:stats -->\n| Tests | 9,999 tests |\n<!-- /AUTO:stats -->\nOutro.\n"
        self.assertEqual(self.claim_errors({self.SURFACE: text}, []), [])
        outside = text + "Also 7 panels.\n"
        errors = self.claim_errors({self.SURFACE: outside}, [])
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("README.md:6", errors[0])

    def test_unterminated_auto_block_is_rejected(self) -> None:
        errors = self.claim_errors({self.SURFACE: "<!-- AUTO:stats -->\n| 9 tests |\n"}, [])
        self.assertTrue(any("unterminated AUTO block 'stats'" in error for error in errors), errors)

    def test_managed_line_is_exempt_only_on_its_managed_surface(self) -> None:
        managed = site_data_validate.managed_numeric_claim_patterns()
        text = "Tests: 7,564 test definitions across 631 files.\n"
        self.assertEqual(self.claim_errors({self.SURFACE: text}, [], managed=managed), [])
        errors = self.claim_errors({"wiki/Home.md": text}, [], managed=managed)
        self.assertEqual(len(errors), 2, errors)

    def test_managed_pattern_is_exempt_only_on_the_file_its_call_rewrites(self) -> None:
        managed = site_data_validate.managed_numeric_claim_patterns()
        text = "SparkEditor has 12 specialized panels today.\n"
        self.assertEqual(self.claim_errors({"README.md": text}, [], managed=managed), [])
        # The badge script rewrites FAQ.md, but only for the `*Panel.h` pattern.
        errors = self.claim_errors({"wiki/getting-started/FAQ.md": text}, [], managed=managed)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("'12 specialized panels'", errors[0])
        self.assertEqual(
            self.claim_errors({"wiki/getting-started/FAQ.md": "Ships 59 `*Panel.h` headers.\n"}, [], managed=managed),
            [],
        )
        self.assertEqual(len(managed["wiki/getting-started/FAQ.md"]), 1)

    def test_unresolvable_badge_script_target_fails_closed(self) -> None:
        bindings = {"readme": ("$PROJECT_ROOT/README.md",)}
        self.assertEqual(
            site_data_validate._resolve_managed_claim_target("$readme", bindings),
            ["README.md"],
        )
        for target in ("$unknown", "$PROJECT_ROOT/../outside.md", "/etc/$readme", "docs/$PROJECT_ROOT/x.md"):
            with self.subTest(target=target):
                with self.assertRaises(site_data_validate.SiteDataError):
                    site_data_validate._resolve_managed_claim_target(target, bindings)

    def test_entry_does_not_cover_qualified_or_overlapping_page_claims(self) -> None:
        metrics = {"visualScript.nodes": 64}
        entry = self.entry("64 nodes", "metric", metricId="visualScript.nodes")
        self.assertEqual(self.claim_errors({self.SURFACE: "Has 64 nodes.\n"}, [entry], metrics), [])
        for text in ("Has ~64 nodes.\n", "Has 1/64 nodes.\n", "Has #64 nodes\n", "Has 1+64 nodes\n"):
            with self.subTest(text=text):
                errors = self.claim_errors({self.SURFACE: text}, [entry], metrics)
                self.assertTrue(any("does not occur in README.md" in error for error in errors), errors)
        # A managed pattern that matches only part of a claim does not exempt the rest.
        partial = {self.SURFACE: (re.compile(r"12 specialized panels"),)}
        errors = self.claim_errors({self.SURFACE: "~12 specialized panels\n"}, [], managed=partial)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("'~12 specialized panels'", errors[0])

    def test_badge_script_patterns_are_parsed(self) -> None:
        managed = site_data_validate.managed_numeric_claim_patterns()
        self.assertIn("README.md", managed)
        self.assertIn(".github/copilot-instructions.md", managed)
        self.assertIn(".github/prompts/build-test.prompt.md", managed)
        readme = "\n".join(pattern.pattern for pattern in managed["README.md"])
        self.assertIn("panel", readme)
        self.assertTrue(
            any(
                pattern.search("7,564 test definitions across 631 files")
                for pattern in managed[".github/copilot-instructions.md"]
            )
        )

    def test_entries_must_match_surface_text_and_claim_something(self) -> None:
        text = "- 59 panels shipped in 0.1\n"
        cases = [
            (self.entry("61 panels"), "does not occur in README.md"),
            (self.entry("shipped in"), "contains no numeric claim"),
            (self.entry("59 panels", "rumor"), "classification must be one of"),
            (self.entry("59 panels", "metric"), "metricId is required"),
            (self.entry("59 panels", "static-fact"), "evidencePath is required"),
            (self.entry("59 panels", metricId="editor.panels"), "unexpected field(s) ['metricId']"),
            ({**self.entry("59 panels"), "owner": ""}, "owner must be a non-empty string"),
            ({**self.entry("59 panels"), "surface": "wiki/Not-A-Surface.md"}, "not a governed public claim surface"),
        ]
        for entry, fragment in cases:
            with self.subTest(fragment=fragment):
                errors = self.claim_errors({self.SURFACE: text}, [entry])
                self.assertTrue(any(fragment in error for error in errors), errors)
        duplicate = self.claim_errors({self.SURFACE: text}, [self.entry("59 panels"), self.entry("59 panels")])
        self.assertTrue(any("duplicate entry" in error for error in duplicate), duplicate)
        self.assertTrue(
            any("must be an array" in error for error in self.claim_errors({self.SURFACE: text}, {"not": "a list"}))
        )

    def test_entry_text_is_whitespace_insensitive_across_wrapped_lines(self) -> None:
        text = "- Dear ImGui editor with 59\n  panels and collaborative editing\n"
        self.assertEqual(self.claim_errors({self.SURFACE: text}, [self.entry("59 panels")]), [])

    def test_removing_a_contract_entry_rejects_the_live_contract(self) -> None:
        entries = self.mutable["readiness"]["publicNumericClaims"]
        removed = next(entry for entry in entries if entry["classification"] == "metric")
        entries.remove(removed)
        self.assert_rejected(self.mutable, "unclaimed numeric claim")

    def test_live_static_facts_cite_existing_evidence(self) -> None:
        entry = next(
            entry
            for entry in self.mutable["readiness"]["publicNumericClaims"]
            if entry["classification"] == "static-fact"
        )
        entry["evidencePath"] = "SparkEngine/Source/DoesNotExist.h"
        self.assert_rejected(self.mutable, "referenced path does not exist: SparkEngine/Source/DoesNotExist.h")


class NoHardcodedClaimsTests(ContractTestCase):
    """DOC-400: site contract copy names bundle metrics, never mutable literals."""

    CONTENT = "docs/site/content.json"
    CATALOG = "docs/site/docs-catalog.json"

    @staticmethod
    def errors_for(texts: list[str]) -> list[str]:
        return site_data_validate.hardcoded_site_claim_errors({"docs/site/content.json": {"copy": texts}})

    def test_live_site_contract_has_no_hardcoded_claims(self) -> None:
        documents = {
            self.CONTENT: self.contract["content"],
            self.CATALOG: self.contract["docsCatalog"],
        }
        self.assertEqual(site_data_validate.hardcoded_site_claim_errors(documents), [])

    def test_validator_scans_both_site_contract_files(self) -> None:
        self.assertEqual(
            set(site_data_validate.HARDCODED_CLAIM_SURFACES),
            {self.CONTENT, self.CATALOG},
        )

    def test_injected_test_count_is_rejected(self) -> None:
        overview = self.mutable["content"]["home"]["overview"]
        overview["copy"] = f"{overview['copy']} It ships 7329 tests."
        self.assert_rejected(self.mutable, "content.json.home.overview.copy: hardcoded count claim '7329 tests'")

    def test_injected_full_commit_sha_is_rejected(self) -> None:
        sha = "3f9c2d1e4b5a69788796a5b4c3d2e1f0a9b8c7d6"
        self.mutable["content"]["links"]["actions"] = f"https://github.com/Krilliac/SparkEngine/commit/{sha}"
        self.assert_rejected(self.mutable, f"hardcoded commit SHA '{sha}'")

    def test_injected_catalog_literal_is_rejected(self) -> None:
        section = self.mutable["docsCatalog"]["sections"][0]
        section["description"] = f"{section['description']} Covers 2,509 files."
        self.assert_rejected(self.mutable, "docs-catalog.json.sections[0].description: hardcoded count claim")

    def test_a_reviewed_public_numeric_entry_is_not_a_waiver(self) -> None:
        overview = self.mutable["content"]["home"]["overview"]
        overview["copy"] = f"{overview['copy']} It has 64 panels."
        self.mutable["readiness"]["publicNumericClaims"].append(
            {
                "surface": self.CONTENT,
                "text": "64 panels",
                "classification": "historical",
                "owner": "docs",
            }
        )
        self.assert_rejected(self.mutable, "hardcoded count claim '64 panels'")

    def test_each_mutable_claim_kind_is_rejected(self) -> None:
        cases = {
            "0123abc": "commit SHA",
            "see https://github.com/o/r/actions/runs/1234567890 for proof": "CI run ID",
            "run 17654321098 passed": "CI run ID",
            "SparkEngine 1.4.0 is out": "version string",
            "1.0.0-rc.1": "version string",
            "tagged v2.1": "version string",
            "12 source files": "count claim",
            "~3,000 lines": "count claim",
        }
        for text, kind in cases.items():
            with self.subTest(text=text):
                errors = self.errors_for([text])
                self.assertEqual(len(errors), 1, errors)
                self.assertIn(f"hardcoded {kind}", errors[0])

    def test_stable_facts_are_not_mistaken_for_claims(self) -> None:
        stable = [
            "CMake 3.25+ \u00b7 C++23 \u00b7 recursive submodules",
            "D3D11 client on Windows 11 x64",
            'cmake -G "Visual Studio 17 2022"',
            "stable-v1",
            "listen on 127.0.0.1 port 7777",
            "latest.json under 32 KiB",
            "a defaced facade",
            "https://discord.gg/cuJv5uWA5V",
            "Spark Open License 1.0",
            "\u00a9 2024\u20132026 Krilliac.",
        ]
        self.assertEqual(self.errors_for(stable), [])


class PublishedMetricTests(unittest.TestCase):
    """One definition of the public source and test counts, bound to the commit."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.metrics = {
            row["id"]: row
            for row in site_data_generate.collect_metrics(1, site_data_generate.module_statistics())
        }

    def test_counts_match_the_readme_generator(self) -> None:
        inventory = site_data_generate.codebase_metrics_module().collect()
        self.assertEqual(inventory["test_definitions"], self.metrics["tests.definitions"]["value"])
        self.assertEqual(inventory["test_files"], self.metrics["tests.files"]["value"])
        self.assertEqual(inventory["total_lines"], self.metrics["code.totalLines"]["value"])
        self.assertEqual(inventory["file_count"], self.metrics["code.files"]["value"])

    def test_test_metric_names_the_harness_the_repository_uses(self) -> None:
        label = self.metrics["tests.definitions"]["label"]
        self.assertNotIn("GoogleTest", label)
        self.assertIn("SparkTests", label)


if __name__ == "__main__":
    unittest.main()
