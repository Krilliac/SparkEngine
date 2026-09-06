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
        for item in contract["workItems"]:
            if item["id"] in declared:
                item["status"] = "done"
        contract["readiness"]["execution"]["firstUnblockedWorkItemId"] = None
        contract["readiness"]["globalRelease"]["state"] = "ready"

    @staticmethod
    def item_text(item: dict[str, Any], *fields: str) -> str:
        values: list[str] = []
        for field in fields:
            value = item.get(field, [])
            values.extend(value if isinstance(value, list) else [str(value)])
        return " ".join(values).lower()


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


class ReadyPromotionTests(ContractTestCase):
    """Frozen case 5: ready derives from profiles, not every ledger gate."""

    def test_excluded_gates_and_work_may_remain_open_when_ready(self) -> None:
        self.promote_ready(self.mutable)
        gates = self.gates_of(self.mutable)
        items = self.items_of(self.mutable)
        self.assertEqual(gates["G11"]["state"], "blocked")
        self.assertEqual(gates["G12"]["state"], "blocked")
        self.assertEqual(items["MOD-315"]["status"], "open")
        self.assertEqual(items["NET-100"]["status"], "open")
        site_data_validate.Validator(self.mutable, allow_legacy_contract=True).validate(
            require_ready=True
        )

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
        sanitizer = self.item_text(items["CI-110"], "implementationScope", "acceptanceCriteria")
        self.assertIn("linux sanitizer", sanitizer)
        self.assertIn("shared", sanitizer)
        self.assertIn("without treating", sanitizer)
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

        rehearsal = self.item_text(items["REL-200"], "implementationScope", "acceptanceCriteria")
        self.assertIn("requiredgateids", rehearsal)
        for excluded in ("protocol", "backup", "incident"):
            self.assertNotIn(excluded, rehearsal)
        operations = self.item_text(items["OPS-110"], "implementationScope", "acceptanceCriteria")
        for owned in ("backup", "incident", "telemetry"):
            self.assertIn(owned, operations)


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
        self.assertIn("contract targets the no-render `NullRHIDevice` path", readme)
        self.assertIn("do not instantiate it (`HEAD-220` remains open)", readme)
        self.assertIn("NullRHI itself rasterizes no pixels", readme)

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
        self.assertIn(
            "timeout 5m python3 Tests/Tools/test_site_data_contract.py -v", workflow
        )
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
        validator.require_path("GameModules/*/module.json", "probe.entryPoints[0]", allow_future=True)
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
