#!/usr/bin/env python3
"""Fail-closed validation for SparkEngine's site/readiness source of truth."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any, Iterable

from common import METRIC_IDS, REPO_ROOT, SCHEMA_VERSION, SiteDataError, load_contract, load_json


IMPLEMENTATION_STATES = {"absent", "stub", "partial", "functional", "complete"}
VERIFICATION_STATES = {"none", "unit-tested", "integration-tested", "system-tested"}
SUPPORT_STATES = {"unsupported", "experimental", "supported", "primary"}
RELEASE_STATES = {"blocked", "candidate", "ready"}
RELEASE_RANK = {"blocked": 0, "candidate": 1, "ready": 2}
GATE_STATES = {"blocked", "at-risk", "passing", "not-evaluated"}
WORK_STATES = {"open", "in-progress", "blocked", "done"}
PRIORITIES = {"P0", "P1", "P2", "P3"}
PROFILE_APPLICABILITY_STATES = {"required", "shared", "outside"}
# Schema version of the CI-120 build-matrix inventory and findings artifacts.
BUILD_MATRIX_SCHEMA_VERSION = 3
BUILD_PRODUCT_KINDS = {
    "executable", "static_library", "shared_library", "module_library",
    "object_library", "interface_library", "unknown_library",
}
BUILD_PRODUCT_APPLICABILITY = {"required", "shared"}
BUILD_CONFIGURATION_PURPOSES = {"shipping", "validation", "installed-sdk-consumer"}
UNASSIGNED_OWNERS = {"", "unassigned", "none", "tbd", "todo"}
REQUIRED_PUBLIC_CLAIM_SURFACES = {
    "README.md",
    "docs/README.md",
    "docs/site/content.json",
}
REQUIRED_BREADTH_TOKENS = {
    "windows 10", "windows 10+", "any platform", "any host", "any compiler"
}
REQUIRED_FORBIDDEN_PROFILE_CELLS = {
    "Any", "Any platform", "Any host", "Windows", "Windows 10", "Windows 10+", "Headless"
}
REQUIRED_NULLRHI_CONFLICTS = {"llvmpipe", "software rendering", "software render"}

# Deliberate outputs of unfinished work items. A missing path not listed here is
# a contract error, not a soft warning.
FUTURE_ACCEPTANCE_PATHS = {
    "GameModules/*/module.json",
    "GameModules/SparkGame/README.md",
    "GameModules/SparkGameARPG/README.md",
    "GameModules/SparkGameFPS/README.md",
    "GameModules/SparkGameFPS/Source/Multiplayer",
    "GameModules/SparkGameMMO/README.md",
    "GameModules/SparkGameOpenWorld/README.md",
    "GameModules/SparkGamePlatformer/README.md",
    "GameModules/SparkGameRPG/README.md",
    "GameModules/SparkGameRTS/README.md",
    "GameModules/SparkGameRTS/Source/AI",
    "GameModules/SparkGameRTS/Source/Fog",
    "GameModules/SparkGameRacing/README.md",
    "GameModules/SparkGameVisualScript/README.md",
    "SparkEditor/Source/Commands",
    "SparkEngine/Source/Platform",
    "SparkSDK/README.md",
    "THIRD_PARTY_NOTICES",
    "Tests/Benchmarks",
    "Tests/Fixtures/Compatibility",
    "Tests/Fuzz",
    "Tests/ModuleKit",
    "ThirdParty/README.md",
    "Tools/spark-cli/README.md",
    "docs/operations/server-runbook.md",
    "docs/specs/online-services.md",
    "docs/specs/persistence.md",
    "docs/specs/telemetry.md",
    "wiki/advanced/Crash-Reporting.md",
    "wiki/gameplay-tools/Visual-Scripting.md",
    "wiki/getting-started/Building-from-Source.md",
    "wiki/subsystems/Scripting.md",
}
GENERATED_PATHS = {"docs/readiness/ENGINE_READINESS_HANDOFF.md"}

WORK_ITEM_REQUIRED_KEYS = {
    "id", "title", "priority", "status", "blocking", "wave", "area", "owner",
    "profileApplicability",
    "rationale", "dependencies", "parallelWith", "sourceContext", "entryPoints",
    "implementationScope", "acceptanceCriteria", "commands", "testSelectors",
    "requiredCiJobs", "performanceBudgets", "documentationUpdates", "readinessChanges",
    "websiteImpact", "risks", "outOfScope", "definitionOfDone",
}
WORK_ITEM_LIST_KEYS = {
    "dependencies", "parallelWith", "sourceContext", "entryPoints", "implementationScope",
    "acceptanceCriteria", "commands", "testSelectors", "requiredCiJobs",
    "performanceBudgets", "documentationUpdates", "readinessChanges", "websiteImpact",
    "risks", "outOfScope", "definitionOfDone",
}


def build_matrix_evidence_errors(
    inventory: Any, report: Any, profile: dict[str, Any] | None
) -> list[str]:
    """Every way the CI-120 evidence pair can fail to mean what it claims.

    Kept free of file access so each rejection has a direct test; the caller
    supplies the parsed documents.
    """
    errors: list[str] = []
    if not isinstance(inventory, dict) or not isinstance(report, dict):
        return ["CI-120 evidence files must contain objects"]

    if inventory.get("schemaVersion") != BUILD_MATRIX_SCHEMA_VERSION:
        errors.append(f"inventory schemaVersion must be {BUILD_MATRIX_SCHEMA_VERSION}")
    if report.get("schemaVersion") != BUILD_MATRIX_SCHEMA_VERSION:
        errors.append(f"findings schemaVersion must be {BUILD_MATRIX_SCHEMA_VERSION}")
    if report.get("state") not in {"blocked", "clean"}:
        errors.append(f"findings state must be blocked or clean, got {report.get('state')!r}")

    findings = report.get("findings")
    if not isinstance(findings, list):
        return errors + ["findings must be a list"]

    counted = {"error": 0, "warning": 0}
    for index, item in enumerate(findings):
        if not isinstance(item, dict) or item.get("severity") not in counted:
            errors.append(f"findings[{index}] needs a category, severity, and message")
            continue
        counted[item["severity"]] += 1
        for key in ("category", "message"):
            if not item.get(key):
                errors.append(f"findings[{index}] lacks {key}")
    if counted["error"] + counted["warning"] != len(findings):
        errors.append(
            f"finding severities do not reconcile: {counted['error']} error + "
            f"{counted['warning']} warning != {len(findings)}"
        )
    if report.get("errorCount") != counted["error"] or report.get("warningCount") != counted["warning"]:
        errors.append(
            f"declared counts {report.get('errorCount')}/{report.get('warningCount')} do not match "
            f"the findings list {counted['error']}/{counted['warning']}"
        )
    if report.get("state") != ("blocked" if counted["error"] else "clean"):
        errors.append("state must be blocked exactly when a blocking finding is present")

    if profile is None:
        return errors

    def by_profile_and_target(products: Any) -> list[Any]:
        if not isinstance(products, list):
            return []
        return sorted(
            products, key=lambda item: (str(item.get("buildProfile")), str(item.get("target")))
        )

    if by_profile_and_target(inventory.get("stableV1Products")) != by_profile_and_target(
        profile.get("buildProducts")
    ):
        errors.append("inventory build products have drifted from the canonical stable-v1 profile")

    evidence = inventory.get("configuredTargetEvidence")
    if not isinstance(evidence, list):
        return errors + ["configuredTargetEvidence must be a list"]
    declared = sorted(str(entry.get("id")) for entry in profile.get("buildConfigurations", []))
    if sorted(str(entry.get("profile")) for entry in evidence) != declared:
        errors.append("configured evidence must name every canonical build configuration exactly once")

    # A clean report is a claim that every supported product is evidenced, and
    # required and shared products are both supported surface.
    if report.get("state") == "clean":
        available = {
            str(entry.get("profile")) for entry in evidence if entry.get("status") == "available"
        }
        for product in profile.get("buildProducts", []):
            if product.get("applicability") == "outside":
                continue
            if product.get("buildProfile") not in available:
                errors.append(
                    f"clean CI-120 report omits configured evidence for supported product "
                    f"{product.get('target')!r} ({product.get('applicability')})"
                )
    return errors


def validate_public_claim_text(
    profile: dict[str, Any],
    surface_location: str,
    text: str,
) -> list[str]:
    """Return deterministic public-claim violations without touching the filesystem."""
    violations: list[str] = []
    identifier = str(profile.get("id", ""))
    rules = profile.get("publicClaimRules") or {}
    breadth_tokens = [str(value).lower() for value in rules.get("breadthTokens", [])]
    forbidden_cells = [str(value) for value in rules.get("forbiddenInProfileCells", [])]
    marker = re.compile(rf"\bin\s+`{re.escape(identifier)}`", re.IGNORECASE)
    for number, line in enumerate(text.splitlines(), start=1):
        lowered = line.lower()
        if marker.search(line):
            widened = [token for token in breadth_tokens if token in lowered]
            if widened:
                violations.append(
                    f"{surface_location}:{number}: claims {widened} inside the profile"
                )
            cells = {value.strip().lower() for value in line.split("|") if value.strip()}
            vague = sorted(value for value in forbidden_cells if value.lower() in cells)
            if vague:
                violations.append(
                    f"{surface_location}:{number}: marks unscoped column {vague} as inside the profile"
                )
        for entry in rules.get("conflatedTerms", []):
            term = str(entry.get("term", ""))
            if not term or term.lower() not in lowered:
                continue
            clashes = [
                str(value)
                for value in entry.get("conflictsWith", [])
                if str(value).lower() in lowered
            ]
            if clashes:
                violations.append(
                    f"{surface_location}:{number}: conflates {term!r} with {clashes}: "
                    f"{entry.get('reason')}"
                )
    return violations


class Validator:
    def __init__(self, contract: dict[str, Any]) -> None:
        self.contract = contract
        self.errors: list[str] = []

    def error(self, location: str, message: str) -> None:
        self.errors.append(f"{location}: {message}")

    def require(self, condition: bool, location: str, message: str) -> None:
        if not condition:
            self.error(location, message)

    def unique_ids(self, records: Iterable[dict[str, Any]], location: str) -> set[str]:
        values = [record.get("id") for record in records]
        for index, value in enumerate(values):
            if not isinstance(value, str) or not value:
                self.error(f"{location}[{index}]", "id must be a non-empty string")
        for value, count in Counter(values).items():
            if value and count > 1:
                self.error(location, f"duplicate id {value!r}")
        return {value for value in values if isinstance(value, str) and value}

    @staticmethod
    def is_future_path(value: str) -> bool:
        return value in GENERATED_PATHS or any(
            fnmatch.fnmatchcase(value, pattern) for pattern in FUTURE_ACCEPTANCE_PATHS
        )

    def require_path(self, value: Any, location: str, *, allow_future: bool = False) -> None:
        if not isinstance(value, str) or not value:
            self.error(location, "path must be a non-empty string")
            return
        path = Path(value)
        if path.is_absolute() or ".." in path.parts or "\\" in value:
            self.error(location, f"invalid repository-relative path {value!r}")
            return
        if (REPO_ROOT / path).exists():
            return
        if allow_future and self.is_future_path(value):
            return
        self.error(location, f"referenced path does not exist: {value}")

    def validate_schema_versions(self) -> None:
        for name in ("content", "readiness", "docsCatalog"):
            self.require(
                self.contract[name].get("schemaVersion") == SCHEMA_VERSION,
                name,
                f"schemaVersion must be {SCHEMA_VERSION}",
            )

    def validate_work_items(self) -> set[str]:
        items = self.contract["workItems"]
        item_ids = self.unique_ids(items, "workItems")
        by_id = {item.get("id"): item for item in items}
        for item in items:
            identifier = item.get("id", "?")
            location = f"workItems.{identifier}"
            missing = WORK_ITEM_REQUIRED_KEYS.difference(item)
            self.require(not missing, location, f"missing fields: {', '.join(sorted(missing))}")
            self.require(item.get("priority") in PRIORITIES, location, "invalid priority")
            self.require(item.get("status") in WORK_STATES, location, "invalid status")
            self.require(isinstance(item.get("blocking"), bool), location, "blocking must be boolean")
            self.require(isinstance(item.get("wave"), int), location, "wave must be an integer")
            self.require(bool(item.get("owner")), location, "owner is required")
            applicability = item.get("profileApplicability")
            self.require(
                isinstance(applicability, dict),
                location,
                "profileApplicability must be an object keyed by release profile",
            )
            if isinstance(applicability, dict):
                for profile_id, value in applicability.items():
                    self.require(
                        isinstance(profile_id, str) and bool(profile_id),
                        location,
                        "profileApplicability keys must be non-empty profile IDs",
                    )
                    self.require(
                        value in PROFILE_APPLICABILITY_STATES,
                        location,
                        f"invalid profile applicability {value!r}",
                    )
            for key in WORK_ITEM_LIST_KEYS:
                self.require(isinstance(item.get(key), list), location, f"{key} must be an array")
            for dependency in item.get("dependencies", []):
                self.require(dependency in item_ids, location, f"unknown dependency {dependency}")
                self.require(dependency != identifier, location, "cannot depend on itself")
            for parallel in item.get("parallelWith", []):
                self.require(parallel in item_ids, location, f"unknown parallelWith item {parallel}")
                self.require(parallel != identifier, location, "cannot be parallel with itself")
            for index, source_path in enumerate(item.get("sourceContext", [])):
                self.require_path(source_path, f"{location}.sourceContext[{index}]")
            for key in ("entryPoints", "documentationUpdates"):
                for index, target_path in enumerate(item.get(key, [])):
                    self.require_path(target_path, f"{location}.{key}[{index}]", allow_future=True)

        visiting: set[str] = set()
        visited: set[str] = set()

        def visit(identifier: str, chain: list[str]) -> None:
            if identifier in visited:
                return
            if identifier in visiting:
                self.error("workItems.dependencies", f"cycle: {' -> '.join([*chain, identifier])}")
                return
            visiting.add(identifier)
            for dependency in by_id.get(identifier, {}).get("dependencies", []):
                if dependency in item_ids:
                    visit(dependency, [*chain, identifier])
            visiting.remove(identifier)
            visited.add(identifier)

        for identifier in sorted(item_ids):
            visit(identifier, [])
        return item_ids

    def validate_readiness(self, item_ids: set[str]) -> tuple[set[str], set[str]]:
        readiness = self.contract["readiness"]
        capabilities = readiness.get("capabilities", [])
        gates = readiness.get("gates", [])
        capability_ids = self.unique_ids(capabilities, "readiness.capabilities")
        gate_ids = self.unique_ids(gates, "readiness.gates")
        gate_by_id = {gate.get("id"): gate for gate in gates}
        item_by_id = {item.get("id"): item for item in self.contract["workItems"]}

        for capability in capabilities:
            identifier = capability.get("id", "?")
            location = f"capabilities.{identifier}"
            self.require(capability.get("implementation") in IMPLEMENTATION_STATES, location, "invalid implementation state")
            self.require(capability.get("verification") in VERIFICATION_STATES, location, "invalid verification state")
            self.require(capability.get("support") in SUPPORT_STATES, location, "invalid support state")
            self.require(capability.get("release") in RELEASE_STATES, location, "invalid release state")
            self.require(bool(capability.get("owner")), location, "owner is required")
            for gate_id in capability.get("requiredGateIds", []):
                self.require(gate_id in gate_ids, location, f"unknown required gate {gate_id}")
            for work_id in capability.get("blockingWorkItemIds", []):
                self.require(work_id in item_ids, location, f"unknown blocking work item {work_id}")
            for index, evidence in enumerate(capability.get("evidence", [])):
                if evidence.get("path"):
                    self.require_path(evidence["path"], f"{location}.evidence[{index}]")
            for index, path in enumerate(capability.get("documentation", [])):
                self.require_path(path, f"{location}.documentation[{index}]")
            for metric_id in capability.get("website", {}).get("proofMetricIds", []):
                self.require(metric_id in METRIC_IDS, location, f"unknown metric {metric_id}")
            if capability.get("release") == "ready":
                unfinished = [
                    work_id for work_id in capability.get("blockingWorkItemIds", [])
                    if work_id in item_by_id and item_by_id[work_id].get("status") != "done"
                ]
                nonpassing = [
                    gate_id for gate_id in capability.get("requiredGateIds", [])
                    if gate_id in gate_by_id and gate_by_id[gate_id].get("state") != "passing"
                ]
                self.require(not unfinished, location, f"ready capability has unfinished blockers: {unfinished}")
                self.require(not nonpassing, location, f"ready capability has non-passing gates: {nonpassing}")

        for gate in gates:
            identifier = gate.get("id", "?")
            location = f"gates.{identifier}"
            self.require(gate.get("state") in GATE_STATES, location, "invalid gate state")
            self.require(isinstance(gate.get("blocking"), bool), location, "blocking must be boolean")
            for work_id in gate.get("blockingWorkItemIds", []):
                self.require(work_id in item_ids, location, f"unknown blocking work item {work_id}")
            for index, evidence in enumerate(gate.get("evidence", [])):
                if evidence.get("path"):
                    self.require_path(evidence["path"], f"{location}.evidence[{index}]")
            if gate.get("state") == "passing":
                unfinished = [
                    work_id for work_id in gate.get("blockingWorkItemIds", [])
                    if work_id in item_by_id and item_by_id[work_id].get("status") != "done"
                ]
                self.require(not unfinished, location, f"passing gate has unfinished blockers: {unfinished}")

        release = readiness.get("globalRelease", {})
        self.require(release.get("state") in RELEASE_STATES, "globalRelease.state", "invalid release state")
        return capability_ids, gate_ids

    def validate_release_profiles(
        self, item_ids: set[str], capability_ids: set[str], gate_ids: set[str]
    ) -> set[str]:
        readiness = self.contract["readiness"]
        profiles = readiness.get("releaseProfiles", [])
        self.require(bool(profiles), "releaseProfiles", "at least one release profile must be declared")
        profile_ids = self.unique_ids(profiles, "releaseProfiles")
        capability_by_id = {capability.get("id"): capability for capability in readiness.get("capabilities", [])}
        gate_by_id = {gate.get("id"): gate for gate in readiness.get("gates", [])}
        item_by_id = {item.get("id"): item for item in self.contract["workItems"]}
        for work_id, item in item_by_id.items():
            applicability = item.get("profileApplicability")
            if not isinstance(applicability, dict):
                continue
            keys = set(applicability)
            self.require(
                keys == profile_ids,
                f"workItems.{work_id}.profileApplicability",
                f"must classify every release profile exactly once; expected={sorted(profile_ids)} actual={sorted(keys)}",
            )

        for profile in profiles:
            identifier = profile.get("id", "?")
            location = f"releaseProfiles.{identifier}"
            self.require(profile.get("state") in RELEASE_STATES, location, "invalid release state")
            self.require(bool(profile.get("owner")), location, "owner is required")
            self.require(bool(profile.get("summary")), location, "summary is required")

            # Every capability must be explicitly inside the profile, explicitly
            # experimental, or explicitly unsupported. Silence is not a boundary.
            included = list(profile.get("includedCapabilityIds", []))
            boundaries = profile.get("boundaries") or {}
            experimental = list(boundaries.get("experimentalCapabilityIds", []))
            unsupported = list(boundaries.get("unsupportedCapabilityIds", []))
            classified = [*included, *experimental, *unsupported]
            for capability_id in classified:
                self.require(capability_id in capability_ids, location, f"unknown capability {capability_id}")
            self.require(len(classified) == len(set(classified)), location, "a capability is classified more than once")
            unclassified = sorted(capability_ids.difference(classified))
            self.require(not unclassified, location, f"unclassified capabilities: {unclassified}")

            for capability_id, allowed in (
                (included, {"primary", "supported"}),
                (experimental, {"experimental"}),
                (unsupported, {"unsupported"}),
            ):
                for value in capability_id:
                    capability = capability_by_id.get(value)
                    if capability is None:
                        continue
                    self.require(
                        capability.get("support") in allowed,
                        location,
                        f"{value} support {capability.get('support')!r} contradicts its profile classification",
                    )

            # A release profile that ships no first-party game proves nothing about
            # the product shape it claims, so the declaration is mandatory and must
            # resolve to an in-profile capability that a scope dimension names.
            first_party = list(profile.get("firstPartyGameCapabilityIds", []))
            self.require(
                bool(first_party),
                location,
                "must declare at least one first-party game capability",
            )
            for value in first_party:
                self.require(
                    value in included,
                    location,
                    f"first-party game {value} must be an included capability",
                )

            scope = profile.get("scope", [])
            self.unique_ids(scope, f"{location}.scope")
            scoped_capabilities = {
                value for dimension in scope for value in dimension.get("capabilityIds", [])
            }
            for value in first_party:
                self.require(
                    value in scoped_capabilities,
                    location,
                    f"first-party game {value} is not named by any scope dimension",
                )

            # CI-120 consumes this profile declaration directly.  The product
            # inventory is not permitted to reconstruct stable scope from
            # target names, directory names, or a second hard-coded list.
            build_configurations = profile.get("buildConfigurations", [])
            build_configuration_ids = self.unique_ids(
                build_configurations, f"{location}.buildConfigurations"
            )
            purposes = [entry.get("purpose") for entry in build_configurations]
            self.require(
                set(purposes) == BUILD_CONFIGURATION_PURPOSES
                and len(purposes) == len(BUILD_CONFIGURATION_PURPOSES),
                f"{location}.buildConfigurations",
                "must declare shipping, validation, and installed-sdk-consumer exactly once",
            )
            for index, entry in enumerate(build_configurations):
                config_location = f"{location}.buildConfigurations[{index}]"
                purpose = entry.get("purpose")
                self.require(
                    purpose in BUILD_CONFIGURATION_PURPOSES,
                    config_location,
                    "invalid build configuration purpose",
                )
                self.require(bool(entry.get("description")), config_location, "description is required")
                self.require(
                    isinstance(entry.get("configuration"), str) and bool(entry.get("configuration")),
                    config_location,
                    "configured codemodel configuration name is required",
                )
                if purpose in {"shipping", "validation"}:
                    self.require(bool(entry.get("preset")), config_location, "preset is required")
                else:
                    self.require(
                        not entry.get("preset"),
                        config_location,
                        "installed SDK consumer must be configured against the installed package, not a source preset",
                    )

            build_products = profile.get("buildProducts", [])
            self.require(bool(build_products), f"{location}.buildProducts", "at least one product is required")
            product_keys: list[tuple[Any, Any]] = []
            product_targets: list[Any] = []
            product_capabilities: set[str] = set()
            first_party_products: list[str] = []
            installed_consumers: list[str] = []
            for index, product in enumerate(build_products):
                product_location = f"{location}.buildProducts[{index}]"
                target = product.get("target")
                build_profile = product.get("buildProfile")
                self.require(isinstance(target, str) and bool(target), product_location, "target is required")
                product_keys.append((build_profile, target))
                product_targets.append(target)
                self.require(
                    build_profile in build_configuration_ids,
                    product_location,
                    f"unknown build profile {build_profile!r}",
                )
                self.require(
                    product.get("kind") in BUILD_PRODUCT_KINDS,
                    product_location,
                    "invalid target kind",
                )
                self.require(
                    product.get("applicability") in BUILD_PRODUCT_APPLICABILITY,
                    product_location,
                    "applicability must be required or shared",
                )
                product_caps = product.get("capabilityIds", [])
                self.require(bool(product_caps), product_location, "capabilityIds must not be empty")
                self.require(
                    len(product_caps) == len(set(product_caps)),
                    product_location,
                    "capabilityIds contains duplicates",
                )
                for value in product_caps:
                    self.require(value in included, product_location, f"{value} is outside this profile")
                    if value in included:
                        product_capabilities.add(value)
                if set(product_caps).intersection(first_party):
                    first_party_products.append(str(target))
                configuration = next(
                    (entry for entry in build_configurations if entry.get("id") == build_profile), {}
                )
                if configuration.get("purpose") == "installed-sdk-consumer":
                    installed_consumers.append(str(target))
                required_options = product.get("requiredOptions")
                self.require(
                    isinstance(required_options, dict),
                    product_location,
                    "requiredOptions must be an object (empty when no option gates the target)",
                )
                if isinstance(required_options, dict):
                    for option_name, option_value in required_options.items():
                        self.require(
                            bool(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", str(option_name))),
                            product_location,
                            f"invalid required option name {option_name!r}",
                        )
                        self.require(
                            str(option_value).upper() in {"ON", "OFF"},
                            product_location,
                            f"required option {option_name!r} must be ON or OFF",
                        )
            self.require(
                len(product_keys) == len(set(product_keys)),
                f"{location}.buildProducts",
                "duplicate target/build-profile mapping",
            )
            self.require(
                len(product_targets) == len(set(product_targets)),
                f"{location}.buildProducts",
                "a target may belong to only one build profile",
            )
            self.require(
                product_capabilities == set(included),
                f"{location}.buildProducts",
                f"product capabilities must equal included capabilities; missing={sorted(set(included) - product_capabilities)}",
            )
            self.require(
                bool(first_party_products),
                f"{location}.buildProducts",
                "must include a product carrying the first-party game capability",
            )
            self.require(
                bool(installed_consumers),
                f"{location}.buildProducts",
                "must include at least one installed public-SDK consumer target",
            )

            option_applicability = profile.get("buildOptionApplicability")
            self.require(
                isinstance(option_applicability, list),
                f"{location}.buildOptionApplicability",
                "must be an explicit list (empty means every mismatch is profile-required)",
            )
            option_names: list[Any] = []
            if isinstance(option_applicability, list):
                for index, entry in enumerate(option_applicability):
                    option_location = f"{location}.buildOptionApplicability[{index}]"
                    option_names.append(entry.get("name"))
                    self.require(
                        bool(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", str(entry.get("name", "")))),
                        option_location,
                        "name must be a CMake option identifier",
                    )
                    self.require(
                        entry.get("applicability") in {"outside", "shared"},
                        option_location,
                        "only outside/shared exceptions are allowed; omitted options are required",
                    )
                    self.require(bool(entry.get("reason")), option_location, "reason is required")
                self.require(
                    len(option_names) == len(set(option_names)),
                    f"{location}.buildOptionApplicability",
                    "duplicate option applicability declaration",
                )
            self.require(bool(scope), f"{location}.scope", "must declare at least one scope dimension")
            for index, dimension in enumerate(scope):
                scope_location = f"{location}.scope[{index}]"
                self.require(bool(dimension.get("label")), scope_location, "label is required")
                self.require(bool(dimension.get("value")), scope_location, "value is required")
                references = dimension.get("capabilityIds", [])
                self.require(bool(references), scope_location, "must name at least one capability")
                for value in references:
                    self.require(value in included, scope_location, f"{value} is not included in this profile")
                evidence = dimension.get("evidence", [])
                self.require(bool(evidence), scope_location, "must carry at least one evidence entry")
                for evidence_index, entry in enumerate(evidence):
                    self.require_path(entry.get("path"), f"{scope_location}.evidence[{evidence_index}]")

            # A gate is either required by the profile or excluded with a reason.
            # Dropping one silently is how a release profile gets weakened.
            required_gates = list(profile.get("requiredGateIds", []))
            excluded_gates = profile.get("excludedGates", [])
            for gate_id in required_gates:
                self.require(gate_id in gate_ids, location, f"unknown required gate {gate_id}")
            for index, entry in enumerate(excluded_gates):
                exclusion_location = f"{location}.excludedGates[{index}]"
                self.require(entry.get("gateId") in gate_ids, exclusion_location, "unknown gate")
                self.require(bool(entry.get("reason")), exclusion_location, "reason is required")
            covered = [*required_gates, *(entry.get("gateId") for entry in excluded_gates)]
            self.require(len(covered) == len(set(covered)), location, "a gate is both required and excluded")
            uncovered = sorted(gate_ids.difference(covered))
            self.require(not uncovered, location, f"gates neither required nor explicitly excluded: {uncovered}")

            expected_gates: set[str] = set()
            expected_items: set[str] = set()
            for capability_id in included:
                capability = capability_by_id.get(capability_id, {})
                expected_gates.update(capability.get("requiredGateIds", []))
                expected_items.update(capability.get("blockingWorkItemIds", []))
            missing_gates = sorted(expected_gates.difference(required_gates))
            self.require(not missing_gates, location, f"included capabilities need excluded gates: {missing_gates}")

            # Called out separately from the general rule above: dropping the module
            # gate is the specific way a profile stops proving its own first-party game.
            for value in first_party:
                dropped = sorted(
                    set(capability_by_id.get(value, {}).get("requiredGateIds", [])).difference(required_gates)
                )
                self.require(
                    not dropped,
                    location,
                    f"first-party game {value} requires gates the profile does not: {dropped}",
                )
            for gate_id in required_gates:
                expected_items.update(gate_by_id.get(gate_id, {}).get("blockingWorkItemIds", []))
            declared_items = list(profile.get("blockingWorkItemIds", []))
            for work_id in declared_items:
                self.require(work_id in item_ids, location, f"unknown blocking work item {work_id}")
            missing_items = sorted(expected_items.difference(declared_items))
            self.require(not missing_items, location, f"blocking work items omit required work: {missing_items}")

            # Applicability is explicit on every work item. A profile's requirement
            # set must be exactly the work classified required/shared for it; names,
            # gate placement, and capability ownership are never used as a proxy.
            applicable_items = {
                work_id
                for work_id, item in item_by_id.items()
                if (item.get("profileApplicability") or {}).get(identifier)
                in {"required", "shared"}
            }
            outside_declared = sorted(
                work_id
                for work_id in declared_items
                if (item_by_id.get(work_id, {}).get("profileApplicability") or {}).get(identifier)
                == "outside"
            )
            self.require(
                not outside_declared,
                location,
                f"blocking work is explicitly outside this profile: {outside_declared}",
            )
            missing_applicable = sorted(applicable_items.difference(declared_items))
            unexpected_declared = sorted(set(declared_items).difference(applicable_items))
            self.require(
                not missing_applicable,
                location,
                f"blocking work omits explicitly applicable items: {missing_applicable}",
            )
            self.require(
                not unexpected_declared,
                location,
                f"blocking work contains non-applicable items: {unexpected_declared}",
            )

            # Close the full dependency graph, not just the direct list. Diagnostics
            # retain the exact path so scope mistakes are actionable.
            dependency_paths: list[list[str]] = []

            def walk_dependencies(current: str, path: list[str]) -> None:
                for dependency in item_by_id.get(current, {}).get("dependencies", []):
                    next_path = [*path, dependency]
                    dependency_paths.append(next_path)
                    if dependency not in path:
                        walk_dependencies(dependency, next_path)

            for work_id in declared_items:
                walk_dependencies(work_id, [work_id])
            outside_paths = [
                path
                for path in dependency_paths
                if (item_by_id.get(path[-1], {}).get("profileApplicability") or {}).get(identifier)
                == "outside"
            ]
            for path in outside_paths:
                self.error(
                    location,
                    f"dependency path {' -> '.join(path)} enters work explicitly outside this profile",
                )

            state = profile.get("state")
            weakest = min(
                (RELEASE_RANK.get(capability_by_id[value].get("release"), 0) for value in included if value in capability_by_id),
                default=0,
            )
            self.require(
                RELEASE_RANK.get(state, 0) <= weakest,
                location,
                "profile state cannot exceed the weakest included capability release state",
            )
            if state == "ready":
                nonpassing = [value for value in required_gates if gate_by_id.get(value, {}).get("state") != "passing"]
                unfinished = [value for value in declared_items if item_by_id.get(value, {}).get("status") != "done"]
                unfinished_dependencies = sorted(
                    {
                        path[-1]
                        for path in dependency_paths
                        if item_by_id.get(path[-1], {}).get("status") != "done"
                    }
                )
                self.require(not nonpassing, location, f"ready profile has non-passing gates: {nonpassing}")
                self.require(not unfinished, location, f"ready profile has unfinished blockers: {unfinished}")
                self.require(
                    not unfinished_dependencies,
                    location,
                    f"ready profile has unfinished transitive dependencies: {unfinished_dependencies}",
                )
                self.require(
                    str(profile.get("owner", "")).strip().lower() not in UNASSIGNED_OWNERS,
                    location,
                    "ready profile requires an assigned owner",
                )
                sign_off = profile.get("signOffEvidence", [])
                self.require(bool(sign_off), location, "ready profile requires sign-off evidence")
            sign_off = profile.get("signOffEvidence", [])
            self.require(isinstance(sign_off, list), location, "signOffEvidence must be an array")
            if isinstance(sign_off, list):
                for index, evidence in enumerate(sign_off):
                    evidence_location = f"{location}.signOffEvidence[{index}]"
                    self.require(isinstance(evidence, dict), evidence_location, "evidence must be an object")
                    if isinstance(evidence, dict):
                        self.require(bool(evidence.get("label")), evidence_location, "label is required")
                        self.require_path(evidence.get("path"), evidence_location)

            for index, path in enumerate(profile.get("documentation", [])):
                self.require_path(path, f"{location}.documentation[{index}]")

            # The supported host set is a declaration, not prose. A host string that
            # smuggles in a breadth token ("Windows 10", "any platform") widens the
            # profile without anyone editing its boundaries.
            rules = profile.get("publicClaimRules") or {}
            breadth_tokens = [str(value).lower() for value in rules.get("breadthTokens", [])]
            forbidden_cells = [str(value) for value in rules.get("forbiddenInProfileCells", [])]
            self.require(
                REQUIRED_BREADTH_TOKENS.issubset(set(breadth_tokens)),
                f"{location}.publicClaimRules.breadthTokens",
                f"mandatory invariants are missing: {sorted(REQUIRED_BREADTH_TOKENS.difference(breadth_tokens))}",
            )
            self.require(
                REQUIRED_FORBIDDEN_PROFILE_CELLS.issubset(set(forbidden_cells)),
                f"{location}.publicClaimRules.forbiddenInProfileCells",
                "mandatory ambiguous profile cells are missing",
            )
            hosts = profile.get("supportedHosts", [])
            self.require(bool(hosts), location, "must declare at least one supported host")
            for index, host in enumerate(hosts):
                host_location = f"{location}.supportedHosts[{index}]"
                self.require(isinstance(host, str) and host.strip(), host_location, "host must be a non-empty string")
                widened = [token for token in breadth_tokens if token in str(host).lower()]
                self.require(not widened, host_location, f"supported host widens the profile: {widened}")
            for index, entry in enumerate(rules.get("conflatedTerms", [])):
                term_location = f"{location}.publicClaimRules.conflatedTerms[{index}]"
                self.require(bool(entry.get("term")), term_location, "term is required")
                self.require(bool(entry.get("conflictsWith")), term_location, "conflictsWith must not be empty")
                self.require(bool(entry.get("reason")), term_location, "reason is required")
            nullrhi_rule = next(
                (
                    entry
                    for entry in rules.get("conflatedTerms", [])
                    if str(entry.get("term", "")).lower() == "nullrhi"
                ),
                None,
            )
            self.require(
                nullrhi_rule is not None,
                f"{location}.publicClaimRules.conflatedTerms",
                "mandatory NullRHI distinction is missing",
            )
            if nullrhi_rule is not None:
                conflicts = {
                    str(value).lower() for value in nullrhi_rule.get("conflictsWith", [])
                }
                self.require(
                    REQUIRED_NULLRHI_CONFLICTS.issubset(conflicts),
                    f"{location}.publicClaimRules.conflatedTerms",
                    "NullRHI must remain distinct from llvmpipe and software rendering",
                )

            # Public support wording must point back at the profile that owns it, must
            # not mark a broader host or platform as inside it, and must not conflate
            # terms the contract declares distinct.
            surfaces = profile.get("publicClaimSurfaces", [])
            self.require(
                set(surfaces) == REQUIRED_PUBLIC_CLAIM_SURFACES,
                location,
                "publicClaimSurfaces must exactly match the independently required support surfaces",
            )
            for index, path in enumerate(surfaces):
                surface_location = f"{location}.publicClaimSurfaces[{index}]"
                self.require_path(path, surface_location)
                resolved = REPO_ROOT / str(path)
                if not resolved.is_file():
                    continue
                text = resolved.read_text(encoding="utf-8", errors="replace")
                self.require(
                    identifier in text,
                    surface_location,
                    f"public surface does not reference release profile {identifier!r}",
                )
                for violation in validate_public_claim_text(profile, surface_location, text):
                    self.errors.append(violation)

        all_profiles_ready = bool(profiles) and all(
            profile.get("state") == "ready" for profile in profiles
        )
        if readiness.get("globalRelease", {}).get("state") == "ready":
            self.require(
                all_profiles_ready,
                "globalRelease.state",
                "global ready requires every declared release profile to be ready",
            )
        if all_profiles_ready:
            self.require(
                readiness.get("globalRelease", {}).get("state") == "ready",
                "globalRelease.state",
                "global release must be ready when every declared release profile is ready",
            )
        return profile_ids

    def validate_build_matrix_evidence(self) -> None:
        """The CI-120 configuration evidence is part of the contract, not beside it.

        These two artifacts decide whether the build matrix is believed, yet
        nothing here used to open them: absent, truncated, or hand-written
        evidence validated exactly as well as real evidence did.
        """
        location = "docs/readiness/ci120"
        inventory_path = REPO_ROOT / "docs" / "readiness" / "ci120-build-matrix-inventory.json"
        report_path = REPO_ROOT / "docs" / "readiness" / "ci120-parity-findings.json"
        for path in (inventory_path, report_path):
            if not path.is_file():
                self.error(location, f"required CI-120 evidence is missing: {path.name}")
                return
        profile = next(
            (
                entry
                for entry in self.contract["readiness"].get("releaseProfiles", [])
                if entry.get("id") == "stable-v1"
            ),
            None,
        )
        for message in build_matrix_evidence_errors(
            load_json(inventory_path), load_json(report_path), profile
        ):
            self.error(location, message)

    def validate_execution(self, item_ids: set[str]) -> None:
        execution = self.contract["readiness"].get("execution", {})
        by_id = {item.get("id"): item for item in self.contract["workItems"]}
        references: list[str] = []
        wave_ids: set[int] = set()
        for index, wave in enumerate(execution.get("waves", [])):
            location = f"execution.waves[{index}]"
            wave_id = wave.get("id")
            self.require(isinstance(wave_id, int), location, "id must be an integer")
            if isinstance(wave_id, int):
                self.require(wave_id not in wave_ids, location, f"duplicate wave id {wave_id}")
                wave_ids.add(wave_id)
            for work_id in wave.get("workItemIds", []):
                self.require(work_id in item_ids, location, f"unknown work item {work_id}")
                if work_id in by_id:
                    self.require(by_id[work_id].get("wave") == wave_id, location, f"{work_id} declares another wave")
                references.append(work_id)
        self.require(set(references) == item_ids, "execution.waves", "must cover every work item")
        self.require(len(references) == len(set(references)), "execution.waves", "work item appears more than once")

        first = execution.get("firstUnblockedWorkItemId")
        self.require(first is None or first in item_ids, "execution.firstUnblockedWorkItemId", "unknown work item")
        if first in by_id:
            unfinished = [dependency for dependency in by_id[first].get("dependencies", []) if by_id[dependency].get("status") != "done"]
            self.require(not unfinished, "execution.firstUnblockedWorkItemId", f"has unfinished dependencies: {unfinished}")
            self.require(by_id[first].get("status") != "done", "execution.firstUnblockedWorkItemId", "item is already done")

    def validate_content(self, capability_ids: set[str], profile_ids: set[str]) -> None:
        content = self.contract["content"]
        for key, value in content.get("links", {}).items():
            if key.endswith("Path"):
                self.require_path(value, f"content.links.{key}")
        home = content.get("home", {})
        metric_groups = (
            home.get("hero", {}).get("metricIds", []),
            home.get("referenceGame", {}).get("metricIds", []),
            home.get("quality", {}).get("metricIds", []),
        )
        for metric_id in (value for group in metric_groups for value in group):
            self.require(metric_id in METRIC_IDS, "content.home metricIds", f"unknown metric {metric_id}")
        status = home.get("status", {})
        profile_id = status.get("releaseProfileId")
        self.require(
            profile_id in profile_ids,
            "content.home.status.releaseProfileId",
            f"unknown release profile {profile_id!r}",
        )
        profile = next(
            (
                candidate
                for candidate in self.contract["readiness"].get("releaseProfiles", [])
                if candidate.get("id") == profile_id
            ),
            {},
        )
        included = set(profile.get("includedCapabilityIds", []))
        groups = status.get("groups", [])
        primary_groups = [group for group in groups if group.get("tone") == "primary"]
        self.require(
            len(primary_groups) == 1,
            "content.home.status.groups",
            f"exactly one primary group is required for {profile_id!r}",
        )
        seen_group_capabilities: list[str] = []
        for group in groups:
            group_ids = group.get("capabilityIds", [])
            self.require(
                len(group_ids) == len(set(group_ids)),
                "content.home.status.groups",
                f"group {group.get('tone')!r} contains duplicate capabilities",
            )
            for capability_id in group_ids:
                self.require(capability_id in capability_ids, "content.home.status.groups", f"unknown capability {capability_id}")
            if group.get("tone") == "primary":
                self.require(
                    set(group_ids) == included,
                    "content.home.status.groups",
                    f"primary group must exactly equal {profile_id!r}; "
                    f"missing={sorted(included.difference(group_ids))} "
                    f"outside={sorted(set(group_ids).difference(included))}",
                )
            else:
                inside = [value for value in group_ids if value in included]
                self.require(
                    not inside,
                    "content.home.status.groups",
                    f"group {group.get('tone')!r} demotes capabilities included in {profile_id!r}: {inside}",
                )
            seen_group_capabilities.extend(group_ids)
        duplicates = sorted(
            capability_id
            for capability_id, count in Counter(seen_group_capabilities).items()
            if count > 1
        )
        self.require(
            not duplicates,
            "content.home.status.groups",
            f"capabilities appear in more than one public group: {duplicates}",
        )
        for capability_id in home.get("status", {}).get("platformCapabilityIds", []):
            self.require(capability_id in capability_ids, "content.home.status.platformCapabilityIds", f"unknown capability {capability_id}")
        for question in content.get("readiness", {}).get("questions", []):
            choices = question.get("choices", [])
            self.require(question.get("defaultValue") in {choice.get("value") for choice in choices}, f"question {question.get('id')}", "defaultValue does not name a choice")
            for choice in choices:
                self.require(choice.get("level") in {0, 1, 2}, f"choice {choice.get('value')}", "level must be 0, 1, or 2")
                for capability_id in choice.get("capabilityIds", []):
                    self.require(capability_id in capability_ids, f"choice {choice.get('value')}", f"unknown capability {capability_id}")
                if choice.get("evidencePath"):
                    self.require_path(choice["evidencePath"], f"choice {choice.get('value')}.evidencePath")
        for track in content.get("learn", {}).get("tracks", []):
            for index, source_path in enumerate(track.get("documentSourcePaths", [])):
                self.require_path(source_path, f"learn.{track.get('id')}.documentSourcePaths[{index}]")

    def validate_legal(self) -> None:
        legal = self.contract["content"].get("legal", {})
        license_path = REPO_ROOT / legal.get("license", {}).get("sourcePath", "")
        self.require(license_path.is_file(), "content.legal.license.sourcePath", "license source must exist")
        if license_path.is_file():
            first_line = license_path.read_text(encoding="utf-8").splitlines()[0].strip()
            self.require(
                legal.get("license", {}).get("name") == first_line,
                "content.legal.license.name",
                f"must exactly match LICENSE first line {first_line!r}",
            )
        for index, document in enumerate(legal.get("documents", [])):
            self.require_path(document.get("sourcePath"), f"content.legal.documents[{index}].sourcePath")

    def validate_asset_surface(self) -> None:
        self.require((REPO_ROOT / "Assets").is_dir(), "Assets", "asset root does not exist")
        self.require((REPO_ROOT / "Shaders").is_dir(), "Shaders", "shader root does not exist")
        self.require((REPO_ROOT / "SparkEngine" / "Source" / "Graphics" / "AssetPipeline.cpp").is_file(), "asset pipeline", "primary asset-pipeline source is absent")

    def validate_docs_surface(self) -> None:
        catalog = self.contract["docsCatalog"]
        count = 0
        for root_value in catalog.get("include", {}).get("recursiveMarkdownRoots", []):
            root = REPO_ROOT / root_value
            if root.is_dir():
                count += sum(1 for path in root.rglob("*.md") if path.is_file())
        self.require(count > 0, "docsCatalog", "catalog resolves no Markdown documents")

    def validate_docs_catalog(self) -> None:
        catalog = self.contract["docsCatalog"]
        include = catalog.get("include", {})
        for index, path in enumerate(include.get("rootDocuments", [])):
            self.require_path(path, f"docsCatalog.rootDocuments[{index}]")
        for index, path in enumerate(include.get("recursiveMarkdownRoots", [])):
            self.require_path(path, f"docsCatalog.recursiveMarkdownRoots[{index}]")
        section_ids = self.unique_ids(catalog.get("sections", []), "docsCatalog.sections")
        for index, rule in enumerate(catalog.get("classificationRules", [])):
            self.require(rule.get("section") in section_ids, f"docsCatalog.classificationRules[{index}]", f"unknown section {rule.get('section')}")
        for index, path in enumerate(catalog.get("featuredSourcePaths", [])):
            self.require_path(path, f"docsCatalog.featuredSourcePaths[{index}]", allow_future=True)
        for path in catalog.get("routeOverrides", {}):
            self.require_path(path, f"docsCatalog.routeOverrides.{path}", allow_future=True)

    def validate_modules(self) -> None:
        discovered = sorted(
            path.name for path in (REPO_ROOT / "GameModules").glob("SparkGame*")
            if path.is_dir() and (path / "CMakeLists.txt").is_file()
        )
        parity = self.contract.get("parityDimensions") or {}
        dimensions = parity.get("dimensions", [])
        scores = parity.get("currentScores", {})
        self.require(bool(dimensions), "parityDimensions.dimensions", "must not be empty")
        self.require(set(discovered) == set(scores), "parityDimensions.currentScores", f"discovered={discovered}; scored={sorted(scores)}")
        for module, values in scores.items():
            self.require(len(values) == len(dimensions), f"parity.{module}", "score count differs from dimensions")
            for value in values:
                self.require(value in {0, 1, 2, 3, "N/A"}, f"parity.{module}", f"invalid score {value!r}")

    def validate(
        self,
        *,
        require_ready: bool = False,
        modules: bool = False,
        assets: bool = False,
        legal: bool = False,
        docs: bool = False,
        capability: str | None = None,
    ) -> None:
        self.validate_schema_versions()
        self.validate_modules()
        item_ids = self.validate_work_items()
        capability_ids, gate_ids = self.validate_readiness(item_ids)
        profile_ids = self.validate_release_profiles(item_ids, capability_ids, gate_ids)
        self.validate_execution(item_ids)
        self.validate_content(capability_ids, profile_ids)
        self.validate_docs_catalog()
        self.validate_build_matrix_evidence()
        self.validate_legal()
        if assets:
            self.validate_asset_surface()
        if docs:
            self.validate_docs_surface()
        if capability:
            self.require(capability in capability_ids, "--capability", f"unknown capability {capability!r}")
        if require_ready:
            self.require(
                self.contract["readiness"].get("globalRelease", {}).get("state") == "ready",
                "globalRelease.state",
                "release is not ready",
            )
        if self.errors:
            detail = "\n".join(f"  - {message}" for message in self.errors)
            raise SiteDataError(f"site-data validation failed with {len(self.errors)} error(s):\n{detail}")


def validate_contract(
    *,
    require_ready: bool = False,
    modules: bool = False,
    assets: bool = False,
    legal: bool = False,
    docs: bool = False,
    capability: str | None = None,
) -> dict[str, Any]:
    contract = load_contract()
    Validator(contract).validate(
        require_ready=require_ready,
        modules=modules,
        assets=assets,
        legal=legal,
        docs=docs,
        capability=capability,
    )
    return contract


def validate_published_bundle(root: Path) -> None:
    root = root.resolve()
    latest_path = root / "latest.json"
    errors: list[str] = []
    try:
        latest_bytes = latest_path.read_bytes()
        latest = json.loads(latest_bytes)
    except (OSError, json.JSONDecodeError) as error:
        raise SiteDataError(f"cannot read published latest.json: {error}") from error
    if len(latest_bytes) > 32 * 1024:
        errors.append(f"latest.json exceeds 32 KiB ({len(latest_bytes)} bytes)")
    if latest.get("schemaVersion") != SCHEMA_VERSION:
        errors.append("latest.json schemaVersion is unsupported")

    def verified(pointer: Any, label: str, maximum: int | None = None) -> tuple[Path | None, bytes | None]:
        if not isinstance(pointer, dict):
            errors.append(f"{label} pointer is not an object")
            return None, None
        relative = pointer.get("path")
        if not isinstance(relative, str):
            errors.append(f"{label} pointer has no path")
            return None, None
        path_value = Path(relative)
        if path_value.is_absolute() or ".." in path_value.parts or "\\" in relative:
            errors.append(f"{label} pointer path is unsafe: {relative!r}")
            return None, None
        path = (root / path_value).resolve()
        try:
            path.relative_to(root)
        except ValueError:
            errors.append(f"{label} escapes publication root")
            return None, None
        try:
            payload = path.read_bytes()
        except OSError as error:
            errors.append(f"{label} cannot be read: {error}")
            return path, None
        digest = hashlib.sha256(payload).hexdigest()
        if pointer.get("bytes") != len(payload):
            errors.append(f"{label} byte count differs: pointer={pointer.get('bytes')} actual={len(payload)}")
        if pointer.get("sha256") != digest:
            errors.append(f"{label} SHA-256 differs")
        if maximum is not None and len(payload) > maximum:
            errors.append(f"{label} exceeds {maximum} bytes ({len(payload)})")
        return path, payload

    bundle_pointer = latest.get("files", {}).get("bundle")
    _, bundle_bytes = verified(bundle_pointer, "bundle", 5 * 1024 * 1024)
    if bundle_bytes is not None:
        try:
            bundle = json.loads(bundle_bytes)
        except json.JSONDecodeError as error:
            errors.append(f"bundle JSON is invalid: {error}")
            bundle = {}
        source_commit = latest.get("source", {}).get("commit")
        if bundle.get("bundleVersion") != latest.get("bundleVersion"):
            errors.append("bundleVersion differs between latest and bundle")
        if bundle.get("source", {}).get("commit") != source_commit:
            errors.append("source commit differs between latest and bundle")
        if latest.get("publication", {}).get("evidenceCommit") != source_commit:
            errors.append("publication evidenceCommit differs from source commit")
        if latest.get("publication", {}).get("state") == "current" and latest.get("publication", {}).get("conclusion") != "success":
            errors.append("current publication does not have a successful conclusion")

        documents = bundle.get("docs", {}).get("documents", [])
        slugs = [document.get("slug") for document in documents]
        for slug, count in Counter(slugs).items():
            if not slug or count > 1:
                errors.append(f"invalid or duplicate document slug: {slug!r}")
        files_by_slug = bundle.get("docs", {}).get("filesBySlug", {})
        for document in documents:
            slug = document.get("slug")
            pointer = document.get("published")
            if pointer != files_by_slug.get(slug):
                errors.append(f"document pointer differs from filesBySlug for {slug!r}")
            if isinstance(pointer, dict):
                if document.get("contentPath") != pointer.get("path") or document.get("contentSha256") != pointer.get("sha256") or document.get("contentBytes") != pointer.get("bytes"):
                    errors.append(f"document compatibility pointer differs for {slug!r}")
                page_path, page_bytes = verified(pointer, f"document {slug}", 2 * 1024 * 1024)
                if page_path is not None and page_path.stem != pointer.get("sha256"):
                    errors.append(f"document {slug!r} is not hash-addressed")
                if page_bytes is not None:
                    try:
                        page = json.loads(page_bytes)
                        if page.get("sourceCommit") != source_commit or page.get("bundleVersion") != latest.get("bundleVersion"):
                            errors.append(f"document {slug!r} identity differs from latest")
                        if page.get("document", {}).get("slug") != slug:
                            errors.append(f"document payload slug differs for {slug!r}")
                    except json.JSONDecodeError as error:
                        errors.append(f"document {slug!r} JSON is invalid: {error}")

        search_pointer = latest.get("files", {}).get("docsSearch")
        verified(search_pointer, "docs search")
        if isinstance(search_pointer, dict):
            if bundle.get("docs", {}).get("searchPath") != search_pointer.get("path") or bundle.get("docs", {}).get("searchSha256") != search_pointer.get("sha256") or bundle.get("docs", {}).get("searchBytes") != search_pointer.get("bytes"):
                errors.append("bundle docs search pointer differs from latest")

    for label, pointer in latest.get("files", {}).items():
        if label == "bundle":
            continue
        verified(pointer, f"latest file {label}")
    if errors:
        detail = "\n".join(f"  - {message}" for message in errors)
        raise SiteDataError(f"published bundle validation failed with {len(errors)} error(s):\n{detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-ready", action="store_true")
    parser.add_argument("--modules", action="store_true")
    parser.add_argument("--assets", action="store_true")
    parser.add_argument("--legal", action="store_true")
    parser.add_argument("--docs", action="store_true")
    parser.add_argument("--capability")
    parser.add_argument("--published", type=Path, help="also verify hashes, identities, and size budgets in generated output")
    args = parser.parse_args()
    try:
        contract = validate_contract(
            require_ready=args.require_ready,
            modules=args.modules,
            assets=args.assets,
            legal=args.legal,
            docs=args.docs,
            capability=args.capability,
        )
        if args.published:
            validate_published_bundle(args.published)
    except SiteDataError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(
        f"Validated {len(contract['readiness']['capabilities'])} capabilities, "
        f"{len(contract['readiness']['gates'])} gates, "
        f"{len(contract['readiness'].get('releaseProfiles', []))} release profiles, "
        f"and {len(contract['workItems'])} work items."
    )
    if args.published:
        print(f"Validated published bundle: {args.published.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
