#!/usr/bin/env python3
"""Fail-closed tests for release-profile scope, dependencies, and publication.

All negative cases operate on deep-copied contract data or pure strings.  This
suite must never edit a tracked repository file while it is running.
"""

from __future__ import annotations

import ast
import copy
import json
import re
import sys
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import common as site_data_common  # noqa: E402
from common import SiteDataError, load_contract  # noqa: E402
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
        site_data_validate.Validator(self.mutable).validate(require_ready=True)

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
        self.assertEqual(
            set(profile["publicClaimSurfaces"]),
            {"README.md", "docs/README.md", "docs/site/content.json"},
        )
        profile["publicClaimSurfaces"].remove("README.md")
        self.assert_rejected(self.mutable, "must exactly match the independently required")

    def test_mandatory_breadth_and_nullrhi_rules_cannot_be_removed(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["publicClaimRules"]["breadthTokens"] = []
        self.assert_rejected(self.mutable, "mandatory invariants are missing")

        missing_any_host = copy.deepcopy(self.contract)
        self.profile_of(missing_any_host)["publicClaimRules"]["breadthTokens"].remove(
            "any host"
        )
        self.assert_rejected(missing_any_host, "mandatory invariants are missing")

        no_nullrhi = copy.deepcopy(self.contract)
        self.profile_of(no_nullrhi)["publicClaimRules"]["conflatedTerms"] = []
        self.assert_rejected(no_nullrhi, "mandatory NullRHI distinction is missing")

    def test_pure_public_claim_helper_rejects_scope_widening(self) -> None:
        profile = self.profile_of(self.mutable)
        cases = {
            "windows": "| Windows 10+ | MSVC v143 | D3D11 | In `stable-v1` |",
            "any-host": "| Headless | Any | NullRHI | In `stable-v1` |",
            "nullrhi": "| NullRHI | headless software rendering via llvmpipe |",
        }
        for name, text in cases.items():
            with self.subTest(case=name):
                self.assertTrue(site_data_validate.validate_public_claim_text(profile, name, text))

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
        self.assertIn("The stable-v1 headless row uses", readme)
        self.assertIn("no pixels are rasterized", readme)

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
        self.assertIn("python3 Tests/Tools/test_site_data_contract.py -v", workflow)
        self.assertIn("python3 tools/site-data/render_handoff.py --check", workflow)
        self.assertGreaterEqual(workflow.count("python3 tools/site-data/generate.py"), 2)
        self.assertIn("diff --recursive --brief", workflow)
        self.assertGreaterEqual(workflow.count("set -euo pipefail"), 3)

    def test_declared_rdy_000_selectors_are_implemented(self) -> None:
        implemented = {"site-data-contract", "readiness-cross-references"}
        selectors = set(self.items_of(self.mutable)["RDY-000"]["testSelectors"])
        self.assertEqual(selectors, implemented)



class BuildMatrixEvidenceTests(ContractTestCase):
    """The CI-120 evidence pair must fail closed on absence and fabrication.

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
            REPO_ROOT / "docs" / "readiness" / "ci120-build-matrix-inventory.json"
        )
        report = site_data_common.load_json(
            REPO_ROOT / "docs" / "readiness" / "ci120-parity-findings.json"
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
        self.assertIn("clean CI-120 report omits configured evidence for supported product", messages)
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
            "docs/readiness/ci120-build-matrix-inventory.json",
            "docs/readiness/ci120-parity-findings.json",
        ):
            with self.subTest(name=name):
                self.parse((REPO_ROOT / name).read_text(encoding="utf-8"))

if __name__ == "__main__":
    unittest.main()
