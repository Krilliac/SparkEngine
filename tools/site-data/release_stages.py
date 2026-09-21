"""Candidate qualification is distinct from completed public release evidence.

Only profile-declared, typed publication finalizers may remain incomplete. This
module does not change the contract or turn a successful publication into ready.
"""
from __future__ import annotations

from typing import Any

PUBLICATION_PHASE = "publication-finalization"
PUBLICATION_ENVIRONMENT = "stable-release"


def _dependencies(items: dict, roots: set[str]) -> set[str]:
    visited: set[str] = set()
    pending = list(roots)
    while pending:
        current = pending.pop()
        if current not in visited:
            visited.add(current)
            pending.extend(items.get(current, {}).get("dependencies", []))
    return visited


def finalization_contract_errors(contract: dict[str, Any]) -> list[str]:
    """Validate the exemption's identity, ownership and dependency boundary."""
    errors: list[str] = []
    items = {item["id"]: item for item in contract["workItems"]}
    readiness = contract["readiness"]
    gates = {gate["id"]: gate for gate in readiness["gates"]}
    for item in items.values():
        phase = item.get("completionPhase", "qualification")
        if phase not in {"qualification", PUBLICATION_PHASE}:
            errors.append(f"{item['id']}: invalid completionPhase")
        if phase == PUBLICATION_PHASE and (item.get("area") != "release" or item.get("blocking") is not True):
            errors.append(f"{item['id']}: publication finalizer must be blocking release work")
        if phase == PUBLICATION_PHASE and (item.get("plannedCiJobs") or item.get("plannedTestSelectors")):
            errors.append(f"{item['id']}: publication finalizer cannot conceal planned qualification verification")
    for profile in readiness["releaseProfiles"]:
        label = f"releaseProfiles.{profile['id']}.publicationFinalization"
        metadata = profile.get("publicationFinalization")
        if not isinstance(metadata, dict) or set(metadata) != {"workItemIds", "gateId", "environment"}:
            errors.append(f"{label}: requires exactly workItemIds, gateId and environment")
            continue
        finalizers = metadata["workItemIds"]
        if (not isinstance(finalizers, list) or not finalizers
                or any(not isinstance(value, str) or not value for value in finalizers)
                or len(set(finalizers)) != len(finalizers)):
            errors.append(f"{label}: workItemIds must be a nonempty unique string array")
            continue
        finalizers = set(finalizers)
        declared = set(profile.get("blockingWorkItemIds", []))
        typed = {key for key, item in items.items()
                 if item.get("completionPhase") == PUBLICATION_PHASE
                 and item.get("profileApplicability", {}).get(profile["id"]) in {"required", "shared"}}
        if finalizers != typed or not finalizers <= declared:
            errors.append(f"{label}: workItemIds must exactly name the applicable typed publication finalizers")
        terminal_gate = metadata["gateId"]
        if (not isinstance(terminal_gate, str) or terminal_gate not in profile.get("requiredGateIds", [])
                or gates.get(terminal_gate, {}).get("completionPhase") != PUBLICATION_PHASE
                or not finalizers <= set(gates.get(terminal_gate, {}).get("blockingWorkItemIds", []))):
            errors.append(f"{label}: terminal gate must be required, typed, and block the declared finalizers")
        if metadata["environment"] != PUBLICATION_ENVIRONMENT:
            errors.append(f"{label}: environment must be {PUBLICATION_ENVIRONMENT}")
        for item_id in declared - finalizers:
            if _dependencies(items, {item_id}) & finalizers:
                errors.append(f"{label}: technical work {item_id} depends on deferred publication")
    return errors


def candidate_readiness_errors(contract: dict[str, Any]) -> list[str]:
    """Require every qualification result while keeping publication unclaimed."""
    errors = finalization_contract_errors(contract)
    if errors:
        return errors
    readiness = contract["readiness"]
    items = {item["id"]: item for item in contract["workItems"]}
    gates = {gate["id"]: gate for gate in readiness["gates"]}
    if readiness.get("globalRelease", {}).get("state") != "candidate":
        errors.append("globalRelease.state: publication requires candidate, never blocked or prematurely ready")
    for profile in readiness["releaseProfiles"]:
        label = f"releaseProfiles.{profile['id']}"
        if profile.get("state") != "candidate":
            errors.append(f"{label}: publication requires candidate state")
        if str(profile.get("owner", "")).strip().lower() in {"", "unassigned", "none", "tbd", "todo"}:
            errors.append(f"{label}: candidate requires an assigned owner")
        if not profile.get("signOffEvidence"):
            errors.append(f"{label}: candidate requires reviewed qualification sign-off evidence")
        finalizers = set(profile["publicationFinalization"]["workItemIds"])
        for item_id in finalizers:
            if items[item_id].get("status") != "in-progress":
                errors.append(f"{label}: publication finalizer {item_id} must remain in-progress, not done")
        required_items = _dependencies(items, set(profile["blockingWorkItemIds"]))
        for gate_id in profile["requiredGateIds"]:
            gate = gates.get(gate_id, {})
            blockers = set(gate.get("blockingWorkItemIds", []))
            required_items.update(_dependencies(items, blockers))
            expected = "at-risk" if blockers & finalizers else "passing"
            if gate.get("state") != expected:
                errors.append(f"{label}: gate {gate_id} must be {expected} before publication")
        for item_id in sorted(required_items - finalizers):
            item = items.get(item_id, {})
            if item.get("status") != "done":
                errors.append(f"{label}: unfinished qualification item or transitive dependency {item_id}")
            if item.get("plannedCiJobs") or item.get("plannedTestSelectors"):
                errors.append(f"{label}: qualification item {item_id} still has planned verification")
    return errors
