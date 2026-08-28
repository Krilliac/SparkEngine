#!/usr/bin/env python3
"""Contract tests for the readiness release profiles and their cross references.

These implement the two test selectors that `RDY-000` declares in
`docs/readiness/work-items/00-truth-ci-release.json`:

* ``site-data-contract`` — the shape and fail-closed rules of the release-profile
  section of `docs/site/readiness.json`.
* ``readiness-cross-references`` — every capability, gate, work item, path, and
  public surface a profile names must resolve, and the generated artefacts must
  stay in step with the contract.

Each negative test mutates a deep copy of the live contract and asserts the
validator rejects it, so removing a guard turns the corresponding test red.
"""

from __future__ import annotations

import copy
import re
import sys
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

from common import SiteDataError, load_contract  # noqa: E402
import render_handoff  # noqa: E402
import validate as site_data_validate  # noqa: E402


HANDOFF_PATH = REPO_ROOT / "docs" / "readiness" / "ENGINE_READINESS_HANDOFF.md"
GENERATOR_PATH = REPO_ROOT / "tools" / "site-data" / "generate.py"


class ContractTestCase(unittest.TestCase):
    """Shared loading and mutation helpers for the contract test selectors."""

    contract: dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = load_contract()

    def setUp(self) -> None:
        self.mutable = copy.deepcopy(self.contract)

    @staticmethod
    def profile_of(contract: dict[str, Any]) -> dict[str, Any]:
        return contract["readiness"]["releaseProfiles"][0]

    def assert_rejected(self, contract: dict[str, Any], fragment: str) -> None:
        with self.assertRaises(SiteDataError) as raised:
            site_data_validate.Validator(contract).validate()
        self.assertIn(fragment, str(raised.exception))


class SiteDataContractTests(ContractTestCase):
    """Test selector: ``site-data-contract``."""

    def test_repository_contract_validates(self) -> None:
        site_data_validate.Validator(copy.deepcopy(self.contract)).validate()

    def test_at_least_one_release_profile_is_declared(self) -> None:
        self.mutable["readiness"]["releaseProfiles"] = []
        self.assert_rejected(self.mutable, "at least one release profile must be declared")

    def test_profile_declares_a_windows_msvc_d3d11_nullrhi_cpp_slice(self) -> None:
        profile = self.profile_of(self.mutable)
        values = {dimension["id"]: dimension["value"] for dimension in profile["scope"]}
        self.assertIn("Windows 11", values["host"])
        self.assertIn("v143", values["toolchain"])
        self.assertIn("Direct3D 11", values["renderer"])
        self.assertIn("NullRHI", values["headless"])
        self.assertIn("C++", values["gameplay"])
        self.assertIn("single-player", values["product"])
        self.assertIn("installed", values["product"].lower())

    def test_profile_declares_a_first_party_game_that_is_in_profile(self) -> None:
        profile = self.profile_of(self.mutable)
        self.assertTrue(profile["firstPartyGameCapabilityIds"])
        for capability_id in profile["firstPartyGameCapabilityIds"]:
            self.assertIn(capability_id, profile["includedCapabilityIds"])

    def test_profile_cannot_omit_its_first_party_game(self) -> None:
        self.profile_of(self.mutable)["firstPartyGameCapabilityIds"] = []
        self.assert_rejected(self.mutable, "must declare at least one first-party game capability")

    def test_first_party_game_must_be_an_included_capability(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["includedCapabilityIds"].remove("modules.fps")
        profile["boundaries"]["experimentalCapabilityIds"].append("modules.fps")
        self.assert_rejected(self.mutable, "first-party game modules.fps must be an included capability")

    def test_first_party_game_must_be_named_by_a_scope_dimension(self) -> None:
        profile = self.profile_of(self.mutable)
        for dimension in profile["scope"]:
            dimension["capabilityIds"] = [
                value for value in dimension["capabilityIds"] if value != "modules.fps"
            ] or ["platform.windows"]
        self.assert_rejected(self.mutable, "is not named by any scope dimension")

    def test_profile_cannot_exclude_the_first_party_game_module_gate(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["requiredGateIds"].remove("G13")
        profile["excludedGates"].append({"gateId": "G13", "reason": "pretend the profile ships no module"})
        self.assert_rejected(self.mutable, "first-party game modules.fps requires gates the profile does not")

    def test_profile_requires_the_module_gate_and_its_blockers(self) -> None:
        profile = self.profile_of(self.mutable)
        self.assertIn("G13", profile["requiredGateIds"])
        self.assertNotIn("G13", [entry["gateId"] for entry in profile["excludedGates"]])
        gate = next(gate for gate in self.mutable["readiness"]["gates"] if gate["id"] == "G13")
        for work_id in gate["blockingWorkItemIds"]:
            self.assertIn(work_id, profile["blockingWorkItemIds"])

    def test_first_party_game_is_supported_scope_but_not_certified(self) -> None:
        profile = self.profile_of(self.mutable)
        capability = next(
            item for item in self.mutable["readiness"]["capabilities"] if item["id"] == "modules.fps"
        )
        # In scope and intended to be supported, but release stays blocked and the
        # profile can never outrank it.
        self.assertEqual(capability["support"], "supported")
        self.assertEqual(capability["release"], "blocked")
        self.assertEqual(profile["state"], "blocked")

    def test_toolchain_scope_does_not_claim_a_pinned_or_certified_toolchain(self) -> None:
        profile = self.profile_of(self.mutable)
        toolchain = next(dimension for dimension in profile["scope"] if dimension["id"] == "toolchain")
        self.assertIn("not pinned", toolchain["value"])
        self.assertTrue(
            any("no exact msvc compiler build" in value.lower() for value in profile["limitations"]),
            "the missing exact toolchain pin must be recorded as a profile limitation",
        )
        for anchor in ("BLD-100", "PLT-200"):
            self.assertTrue(
                any(anchor in value for value in profile["limitations"]),
                f"the toolchain-pin limitation must name ledger item {anchor}",
            )
            self.assertIn(anchor, profile["blockingWorkItemIds"])

    def test_limitations_do_not_hardcode_a_gate_count(self) -> None:
        # A limitation that spells out "none of its fifteen required gates" goes
        # stale the moment a gate is added to the profile. Keep the count derived.
        counts = re.compile(
            r"\b(\d+|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|"
            r"thirteen|fourteen|fifteen|sixteen|seventeen|eighteen)\b",
            re.IGNORECASE,
        )
        for limitation in self.profile_of(self.mutable)["limitations"]:
            if "required gate" in limitation.lower():
                with self.subTest(limitation=limitation):
                    self.assertIsNone(
                        counts.search(limitation),
                        "gate counts must be derived from requiredGateIds, not written into prose",
                    )

    def test_every_capability_is_classified_exactly_once(self) -> None:
        profile = self.profile_of(self.mutable)
        boundaries = profile["boundaries"]
        classified = [
            *profile["includedCapabilityIds"],
            *boundaries["experimentalCapabilityIds"],
            *boundaries["unsupportedCapabilityIds"],
        ]
        declared = [capability["id"] for capability in self.mutable["readiness"]["capabilities"]]
        self.assertCountEqual(classified, declared)

    def test_unclassified_capability_is_rejected(self) -> None:
        self.profile_of(self.mutable)["boundaries"]["experimentalCapabilityIds"].pop()
        self.assert_rejected(self.mutable, "unclassified capabilities")

    def test_double_classified_capability_is_rejected(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["boundaries"]["experimentalCapabilityIds"].append(profile["includedCapabilityIds"][0])
        self.assert_rejected(self.mutable, "classified more than once")

    def test_classification_must_agree_with_capability_support_state(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["boundaries"]["experimentalCapabilityIds"].remove("platform.linux")
        profile["includedCapabilityIds"].append("platform.linux")
        self.assert_rejected(self.mutable, "contradicts its profile classification")

    def test_every_gate_is_required_or_explicitly_excluded(self) -> None:
        self.profile_of(self.mutable)["requiredGateIds"].remove("G17")
        self.assert_rejected(self.mutable, "gates neither required nor explicitly excluded")

    def test_gate_exclusion_requires_a_reason(self) -> None:
        self.profile_of(self.mutable)["excludedGates"][0]["reason"] = ""
        self.assert_rejected(self.mutable, "reason is required")

    def test_profile_cannot_exclude_a_gate_its_capabilities_require(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["requiredGateIds"].remove("G09")
        profile["excludedGates"].append({"gateId": "G09", "reason": "pretend the renderer needs no parity gate"})
        self.assert_rejected(self.mutable, "included capabilities need excluded gates")

    def test_blocking_work_must_cover_required_gate_blockers(self) -> None:
        self.profile_of(self.mutable)["blockingWorkItemIds"].remove("SEC-100")
        self.assert_rejected(self.mutable, "blocking work items omit required work")

    def test_profile_state_cannot_exceed_weakest_included_capability(self) -> None:
        self.profile_of(self.mutable)["state"] = "ready"
        self.assert_rejected(self.mutable, "cannot exceed the weakest included capability release state")

    def test_ready_profile_requires_passing_gates_and_finished_work(self) -> None:
        profile = self.profile_of(self.mutable)
        profile["state"] = "ready"
        for capability in self.mutable["readiness"]["capabilities"]:
            if capability["id"] in profile["includedCapabilityIds"]:
                capability["release"] = "ready"
        self.assert_rejected(self.mutable, "ready profile has non-passing gates")

    def test_scope_dimension_requires_existing_evidence(self) -> None:
        self.profile_of(self.mutable)["scope"][0]["evidence"][0]["path"] = "docs/does-not-exist.md"
        self.assert_rejected(self.mutable, "referenced path does not exist")

    def test_scope_cannot_rest_on_an_out_of_profile_capability(self) -> None:
        self.profile_of(self.mutable)["scope"][0]["capabilityIds"] = ["platform.linux"]
        self.assert_rejected(self.mutable, "is not included in this profile")

    def test_profile_requires_an_owner(self) -> None:
        self.profile_of(self.mutable)["owner"] = ""
        self.assert_rejected(self.mutable, "owner is required")

    def test_global_ready_requires_every_profile_ready(self) -> None:
        self.mutable["readiness"]["globalRelease"]["state"] = "ready"
        self.assert_rejected(self.mutable, "global ready requires every declared release profile to be ready")


class ReadinessCrossReferenceTests(ContractTestCase):
    """Test selector: ``readiness-cross-references``."""

    def test_unknown_capability_reference_is_rejected(self) -> None:
        self.profile_of(self.mutable)["includedCapabilityIds"].append("platform.imaginary")
        self.assert_rejected(self.mutable, "unknown capability platform.imaginary")

    def test_unknown_gate_reference_is_rejected(self) -> None:
        self.profile_of(self.mutable)["requiredGateIds"].append("G99")
        self.assert_rejected(self.mutable, "unknown required gate G99")

    def test_unknown_work_item_reference_is_rejected(self) -> None:
        self.profile_of(self.mutable)["blockingWorkItemIds"].append("ZZZ-999")
        self.assert_rejected(self.mutable, "unknown blocking work item ZZZ-999")

    def test_public_surface_must_reference_the_profile(self) -> None:
        self.profile_of(self.mutable)["publicClaimSurfaces"].append("LICENSE")
        self.assert_rejected(self.mutable, "does not reference release profile")

    def test_declared_public_surfaces_name_the_profile_today(self) -> None:
        profile = self.profile_of(self.mutable)
        for surface in profile["publicClaimSurfaces"]:
            with self.subTest(surface=surface):
                text = (REPO_ROOT / surface).read_text(encoding="utf-8", errors="replace")
                self.assertIn(profile["id"], text)

    def test_website_release_profile_id_must_resolve(self) -> None:
        self.mutable["content"]["home"]["status"]["releaseProfileId"] = "no-such-profile"
        self.assert_rejected(self.mutable, "unknown release profile")

    def test_primary_public_group_cannot_claim_an_out_of_profile_capability(self) -> None:
        groups = self.mutable["content"]["home"]["status"]["groups"]
        primary = next(group for group in groups if group["tone"] == "primary")
        primary["capabilityIds"].append("platform.linux")
        self.assert_rejected(self.mutable, "primary group claims capabilities outside")

    def test_public_group_cannot_demote_an_in_profile_capability(self) -> None:
        groups = self.mutable["content"]["home"]["status"]["groups"]
        other = next(group for group in groups if group["tone"] != "primary")
        other["capabilityIds"].append("rendering.d3d11")
        self.assert_rejected(self.mutable, "demotes capabilities included in")

    def test_generated_handoff_is_current(self) -> None:
        self.assertEqual(
            HANDOFF_PATH.read_text(encoding="utf-8"),
            render_handoff.render_handoff(self.contract),
        )

    def test_handoff_rendering_is_deterministic(self) -> None:
        self.assertEqual(
            render_handoff.render_handoff(self.contract),
            render_handoff.render_handoff(load_contract()),
        )

    def test_handoff_publishes_the_declared_profile(self) -> None:
        rendered = render_handoff.render_handoff(self.contract)
        profile = self.profile_of(self.mutable)
        self.assertIn(f"## Declared release profile — `{profile['id']}`", rendered)
        for gate in profile["requiredGateIds"]:
            self.assertIn(f"`{gate}`", rendered)
        for exclusion in profile["excludedGates"]:
            self.assertIn(exclusion["reason"], rendered)

    def test_generator_publishes_every_readiness_section(self) -> None:
        # The generator fans readiness out through an explicit allow-list, so a new
        # contract section is silently dropped from the published bundle unless it
        # is named there. Missing output is the failure mode, not a hard error.
        source = GENERATOR_PATH.read_text(encoding="utf-8")
        for section in self.contract["readiness"]:
            if section == "schemaVersion":
                continue
            with self.subTest(section=section):
                self.assertIn(f'"{section}"', source)

    def test_declared_test_selectors_are_implemented(self) -> None:
        implemented = {
            "site-data-contract": SiteDataContractTests,
            "readiness-cross-references": ReadinessCrossReferenceTests,
        }
        work_item = next(item for item in self.contract["workItems"] if item["id"] == "RDY-000")
        for selector in work_item["testSelectors"]:
            with self.subTest(selector=selector):
                self.assertIn(selector, implemented)


if __name__ == "__main__":
    unittest.main()
