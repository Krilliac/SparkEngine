"""Candidate qualification is distinct from completed public release evidence.

Only profile-declared, typed publication finalizers may remain incomplete. This
module does not change the contract or turn a successful publication into ready.
"""
from __future__ import annotations

from typing import Any
import re

PUBLICATION_PHASE = "publication-finalization"
PUBLICATION_ENVIRONMENT = "stable-release"
_COMMIT_RE = re.compile(r"^[0-9a-fA-F]{40}$")


def _dependencies(items: dict, roots: set[str], excluded: set[str] | None = None) -> set[str]:
    excluded = excluded or set()
    visited: set[str] = set()
    pending = list(roots)
    while pending:
        current = pending.pop()
        if current not in visited and current not in excluded:
            visited.add(current)
            pending.extend(items.get(current, {}).get("dependencies", []))
    return visited


def _string_list(value: Any, *, nonempty: bool = False) -> tuple[list[str] | None, str | None]:
    """Return a safe string list without allowing malformed JSON to throw."""
    if not isinstance(value, list) or (nonempty and not value):
        return None, "must be a nonempty string array" if nonempty else "must be a string array"
    if any(not isinstance(entry, str) or not entry for entry in value):
        return None, "must contain only nonempty strings"
    if len(value) != len(set(value)):
        return None, "must contain unique strings"
    return value, None


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


def predecessor_candidate_readiness_errors(contract: dict[str, Any]) -> list[str]:
    """Qualify an explicit signed predecessor without exempting v1 work.

    The predecessor is deliberately a separate, ledger-owned stage.  It may
    replace only the v1 publication finalizer (for example, REL-200) with its
    own finalizer; every v1 qualification gate and every non-publication
    blocking item remains mandatory.  Keeping this check separate avoids the
    tempting, unsafe ``N-1 is N/A`` switch in the normal candidate evaluator.
    """
    readiness = contract.get("readiness", {})
    if not isinstance(readiness, dict):
        return ["predecessorRelease: readiness must be an object"]
    stage = readiness.get("predecessorRelease")
    errors: list[str] = []
    if not isinstance(stage, dict):
        return ["predecessorRelease: explicit predecessor stage is required"]

    required = {
        "id", "profileId", "state", "owner", "signOffEvidence",
        "sourceCommitEvidence", "requiredGateIds", "blockingWorkItemIds",
        "publicationFinalization", "qualificationSubstitutions",
    }
    if set(stage) != required:
        errors.append("predecessorRelease: requires exactly the reviewed predecessor stage fields")
        return errors

    work_items = contract.get("workItems", [])
    release_profiles = readiness.get("releaseProfiles", [])
    gates_data = readiness.get("gates", [])
    if not isinstance(work_items, list) or any(not isinstance(item, dict) for item in work_items):
        return ["predecessorRelease: workItems must be an array of objects"]
    if not isinstance(release_profiles, list) or any(not isinstance(profile, dict) for profile in release_profiles):
        return ["predecessorRelease: releaseProfiles must be an array of objects"]
    if not isinstance(gates_data, list) or any(not isinstance(gate, dict) for gate in gates_data):
        return ["predecessorRelease: gates must be an array of objects"]
    malformed_items = []
    for item in work_items:
        if not isinstance(item.get("id"), str) or not item["id"]:
            malformed_items.append("work item id must be a nonempty string")
        dependencies, dependency_error = _string_list(item.get("dependencies", []))
        if dependency_error:
            malformed_items.append(f"{item.get('id', '?')}.dependencies: {dependency_error}")
    malformed_gates = []
    for gate in gates_data:
        if not isinstance(gate.get("id"), str) or not gate["id"]:
            malformed_gates.append("gate id must be a nonempty string")
        blockers, blocker_error = _string_list(gate.get("blockingWorkItemIds", []))
        if blocker_error:
            malformed_gates.append(f"{gate.get('id', '?')}.blockingWorkItemIds: {blocker_error}")
    if malformed_items or malformed_gates:
        return [f"predecessorRelease: {error}" for error in malformed_items + malformed_gates]
    malformed_profiles = [
        "profile id must be a nonempty string"
        for profile_entry in release_profiles
        if not isinstance(profile_entry.get("id"), str) or not profile_entry["id"]
    ]
    if malformed_profiles:
        return [f"predecessorRelease: {error}" for error in malformed_profiles]
    items = {item["id"]: item for item in work_items}
    profiles = {profile.get("id"): profile for profile in release_profiles}
    profile_id = stage.get("profileId")
    if not isinstance(profile_id, str) or not profile_id:
        return ["predecessorRelease.profileId: must be a nonempty string"]
    profile = profiles.get(profile_id)
    if profile is None:
        return ["predecessorRelease.profileId: must identify an existing release profile"]
    global_release = readiness.get("globalRelease")
    if not isinstance(global_release, dict) or global_release.get("state") != "candidate":
        errors.append("globalRelease.state: predecessor qualification requires candidate state")
    if profile.get("state") != "candidate":
        errors.append(f"releaseProfiles.{profile_id}.state: predecessor qualification requires candidate state")
    if (not isinstance(profile.get("owner"), str) or not profile.get("owner").strip()
            or profile.get("owner").strip().lower() in {"unassigned", "none", "tbd", "todo"}):
        errors.append(f"releaseProfiles.{profile_id}.owner: reviewed owner is required")
    if not isinstance(profile.get("signOffEvidence"), list) or not profile.get("signOffEvidence"):
        errors.append(f"releaseProfiles.{profile_id}.signOffEvidence: reviewed qualification evidence is required")
    if stage.get("state") != "candidate":
        errors.append("predecessorRelease.state: predecessor must remain candidate until publication")
    if (not isinstance(stage.get("owner"), str) or not stage.get("owner").strip()
            or stage.get("owner").strip().lower() in {"unassigned", "none", "tbd", "todo"}):
        errors.append("predecessorRelease.owner: reviewed owner is required")
    if not isinstance(stage.get("signOffEvidence"), list) or not stage.get("signOffEvidence"):
        errors.append("predecessorRelease.signOffEvidence: reviewed qualification evidence is required")

    source = stage.get("sourceCommitEvidence")
    if (not isinstance(source, dict) or set(source) != {"commit", "reviewPath"}
            or not isinstance(source.get("commit"), str) or not _COMMIT_RE.fullmatch(source["commit"])
            or not isinstance(source.get("reviewPath"), str) or not source["reviewPath"].strip()):
        errors.append("predecessorRelease.sourceCommitEvidence: reviewed immutable source commit and reviewPath are required")

    substitutions = stage.get("qualificationSubstitutions")
    if not isinstance(substitutions, dict):
        errors.append("predecessorRelease.qualificationSubstitutions: must map v1 item IDs to predecessor equivalents")
        substitutions = {}
    elif any(not isinstance(source_id, str) or not source_id or not isinstance(target_id, str) or not target_id
             for source_id, target_id in substitutions.items()):
        errors.append("predecessorRelease.qualificationSubstitutions: keys and values must be nonempty strings")
        substitutions = {}

    profile_finalization = profile.get("publicationFinalization", {})
    profile_finalizer_list, profile_finalizer_error = _string_list(
        profile_finalization.get("workItemIds") if isinstance(profile_finalization, dict) else None,
        nonempty=True,
    )
    profile_gate_list, profile_gate_error = _string_list(profile.get("requiredGateIds"), nonempty=True)
    profile_blocking_list, profile_blocking_error = _string_list(profile.get("blockingWorkItemIds"), nonempty=True)
    if profile_finalizer_error or profile_gate_error or profile_blocking_error:
        if profile_finalizer_error:
            errors.append(f"releaseProfiles.{profile_id}.publicationFinalization.workItemIds: {profile_finalizer_error}")
        if profile_gate_error:
            errors.append(f"releaseProfiles.{profile_id}.requiredGateIds: {profile_gate_error}")
        if profile_blocking_error:
            errors.append(f"releaseProfiles.{profile_id}.blockingWorkItemIds: {profile_blocking_error}")
        return errors
    profile_finalizers = set(profile_finalizer_list)
    profile_gate_ids = set(profile_gate_list)
    profile_blocking_ids = set(profile_blocking_list)
    profile_terminal_gate = profile_finalization.get("gateId")
    if not isinstance(profile_terminal_gate, str) or not profile_terminal_gate:
        errors.append(f"releaseProfiles.{profile_id}.publicationFinalization.gateId: must be a nonempty string")
        return errors
    # The predecessor keeps every v1 qualification gate and swaps only the
    # v1 terminal publication gate for its own terminal gate.
    expected_gates = profile_gate_ids - {profile_terminal_gate}
    stage_gates, stage_gate_error = _string_list(stage.get("requiredGateIds"), nonempty=True)
    if stage_gate_error:
        errors.append(f"predecessorRelease.requiredGateIds: {stage_gate_error}")

    stage_finalization = stage.get("publicationFinalization")
    if not isinstance(stage_finalization, dict) or set(stage_finalization) != {"workItemIds", "gateId", "environment"}:
        errors.append("predecessorRelease.publicationFinalization: requires exactly workItemIds, gateId and environment")
        return errors
    stage_finalizers, stage_finalizer_error = _string_list(
        stage_finalization.get("workItemIds"), nonempty=True
    )
    if stage_finalizer_error:
        errors.append(f"predecessorRelease.publicationFinalization.workItemIds: {stage_finalizer_error}")
        return errors
    stage_finalizer_ids = set(stage_finalizers)
    if stage_finalizer_ids & profile_finalizers:
        errors.append("predecessorRelease.publicationFinalization: must use a predecessor-specific finalizer")
    if stage_finalization.get("environment") != PUBLICATION_ENVIRONMENT:
        errors.append(f"predecessorRelease.publicationFinalization.environment: must be {PUBLICATION_ENVIRONMENT}")
    stage_gate = stage_finalization.get("gateId")
    if not isinstance(stage_gate, str) or not stage_gate:
        errors.append("predecessorRelease.publicationFinalization.gateId: must be a nonempty string")
        return errors
    if stage_gates is not None:
        expected_gates.add(stage_gate)
        if set(stage_gates) != expected_gates:
            errors.append("predecessorRelease.requiredGateIds: may replace only the v1 publication-finalization gate")
    gates = {gate.get("id"): gate for gate in gates_data}
    if stage_gate not in (stage_gates or []) or gates.get(stage_gate, {}).get("completionPhase") != PUBLICATION_PHASE:
        errors.append("predecessorRelease.publicationFinalization.gateId: must be a required publication-finalization gate")

    common_items = profile_blocking_ids - profile_finalizers
    common_qualification = _dependencies(items, common_items)
    for source_id, target_id in substitutions.items():
        if source_id in profile_finalizers or source_id not in common_qualification:
            errors.append(f"predecessorRelease.qualificationSubstitutions: {source_id} is not a v1 qualification dependency")
        target_item = items.get(target_id)
        target_applicability = target_item.get("profileApplicability", {}).get(profile_id) if isinstance(target_item, dict) else None
        if (target_item is None or target_id in profile_finalizers
                or (target_applicability not in {"required", "shared"}
                    and not (target_applicability == "outside" and target_item.get("predecessorOnly") is True))):
            errors.append(f"predecessorRelease.qualificationSubstitutions: {target_id} is not a valid predecessor equivalent")
    stage_blocking, stage_blocking_error = _string_list(stage.get("blockingWorkItemIds"), nonempty=True)
    if stage_blocking_error:
        errors.append(f"predecessorRelease.blockingWorkItemIds: {stage_blocking_error}")
        stage_blocking = []
    if set(stage_blocking) - stage_finalizer_ids != common_items:
        errors.append("predecessorRelease.blockingWorkItemIds: must retain every v1 non-publication blocker")
    if not stage_finalizer_ids <= set(stage_blocking):
        errors.append("predecessorRelease.blockingWorkItemIds: must include every predecessor finalizer")

    for item_id in stage_finalizer_ids:
        item = items.get(item_id)
        if not item:
            errors.append(f"predecessorRelease: unknown finalizer {item_id}")
            continue
        if (item.get("completionPhase") != PUBLICATION_PHASE or item.get("area") != "release"
                or item.get("blocking") is not True or item.get("status") != "in-progress"):
            errors.append(f"predecessorRelease: finalizer {item_id} is not an in-progress publication finalizer")
        if item.get("plannedCiJobs") or item.get("plannedTestSelectors"):
            errors.append(f"predecessorRelease: finalizer {item_id} conceals planned qualification")

    # The v1 publication finalizer is not a predecessor requirement.  Other
    # v1 gates may mention it transitively, so remove it from dependency
    # traversal as well; only the predecessor's own finalizer may remain
    # in-progress.
    excluded = set(substitutions) | profile_finalizers
    required_items = _dependencies(items, set(stage_blocking), excluded)
    for target_id in substitutions.values():
        required_items.update(_dependencies(items, {target_id}))
    for gate_id in stage_gates or []:
        gate = gates.get(gate_id, {})
        blockers = set(gate.get("blockingWorkItemIds", []))
        required_items.update(_dependencies(items, blockers))
        expected = "at-risk" if blockers & stage_finalizer_ids else "passing"
        if gate.get("state") != expected:
            errors.append(f"predecessorRelease: gate {gate_id} must be {expected}")
    for item_id in sorted(required_items - stage_finalizer_ids):
        item = items.get(item_id, {})
        if item.get("status") != "done":
            errors.append(f"predecessorRelease: unfinished common qualification item {item_id}")
        if item.get("plannedCiJobs") or item.get("plannedTestSelectors"):
            errors.append(f"predecessorRelease: common qualification item {item_id} still has planned verification")
    return errors
