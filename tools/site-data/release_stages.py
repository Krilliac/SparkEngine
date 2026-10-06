"""Candidate qualification is distinct from completed public release evidence.

Only profile-declared, typed publication finalizers may remain incomplete. This
module does not change the contract or turn a successful publication into ready.
"""
from __future__ import annotations

from fnmatch import fnmatchcase
from typing import Any
import re

from common import ACCEPTANCE_CI_REFERENCE, criterion_digest

PUBLICATION_PHASE = "publication-finalization"
PUBLICATION_ENVIRONMENT = "stable-release"
_COMMIT_RE = re.compile(r"^[0-9a-fA-F]{40}$")
# OD-18: the stable-v1 items that need a previously published, signed
# predecessor (N-1 upgrade, rollback and migration). The predecessor stage
# substitutes them away, so their results can never qualify the predecessor.
N_MINUS_ONE_ITEM_IDS = ("REL-192", "INST-131")
# The only step that fetches the N-1 predecessor MSI; the predecessor path has
# no older release to provision.
N_MINUS_ONE_PROVISIONER = ".github/scripts/provision-previous-windows-msi.py"
# The qualifier mode that proves bootstrap recovery for the first predecessor.
BOOTSTRAP_QUALIFIER_MODE = "--bootstrap-repair"
# N-1 work needs a real immutable predecessor, so it cannot finish before one exists.
PUBLISHED_PREDECESSOR_STATES = frozenset({"published", "ready"})
_EVIDENCE_TOKEN = re.compile(r"[A-Za-z0-9_*./-]+")


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
    # The v1 stage never applies qualificationSubstitutions: REL-191 cannot
    # stand in for REL-192 here, and predecessor results cannot be reused.
    errors.extend(predecessor_evidence_reuse_errors(contract))
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
    if (not isinstance(source, dict) or set(source) != {"baselineCommit", "reviewPath"}
            or not isinstance(source.get("baselineCommit"), str) or not _COMMIT_RE.fullmatch(source["baselineCommit"])
            or not isinstance(source.get("reviewPath"), str) or not source["reviewPath"].strip()):
        errors.append("predecessorRelease.sourceCommitEvidence: reviewed baseline commit and reviewPath are required")

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
        required_items.update(_dependencies(items, blockers, excluded))
        expected = "at-risk" if blockers & stage_finalizer_ids else "passing"
        if gate.get("state") != expected:
            errors.append(f"predecessorRelease: gate {gate_id} must be {expected}")
    for item_id in sorted(required_items - stage_finalizer_ids):
        item = items.get(item_id, {})
        if item.get("status") != "done":
            errors.append(f"predecessorRelease: unfinished common qualification item {item_id}")
        if item.get("plannedCiJobs") or item.get("plannedTestSelectors"):
            errors.append(f"predecessorRelease: common qualification item {item_id} still has planned verification")
    errors.extend(nminus1_evidence_errors(contract))
    return errors


def _text_values(value: Any) -> list[str]:
    """Every string inside a JSON value, so nested evidence cannot hide a reference."""
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        return [text for entry in value.values() for text in _text_values(entry)]
    if isinstance(value, list):
        return [text for entry in value for text in _text_values(entry)]
    return []


def _item_selectors(item: dict[str, Any]) -> set[str]:
    selectors: set[str] = set()
    for key in ("testSelectors", "plannedTestSelectors"):
        values = item.get(key, [])
        if isinstance(values, list):
            selectors.update(value for value in values if isinstance(value, str) and value)
    return selectors


def _item_digests(item: dict[str, Any]) -> set[str]:
    """The item's criterion digests, from its criteria and its recorded acceptanceStatus."""
    digests = {criterion_digest(text) for text in item.get("acceptanceCriteria", []) if isinstance(text, str)}
    for entry in item.get("acceptanceStatus", []):
        if isinstance(entry, dict) and isinstance(entry.get("criterionDigest"), str):
            digests.add(entry["criterionDigest"])
    return digests


def _acceptance_texts(item: dict[str, Any]) -> list[str]:
    """The evidence and note strings an item presents for its criteria."""
    texts: list[str] = []
    for entry in item.get("acceptanceStatus", []):
        if isinstance(entry, dict):
            texts.extend(_text_values(entry.get("evidence", [])))
            texts.extend(_text_values(entry.get("note", "")))
    return texts


def _foreign_reference(text: str, selectors: set[str], literals: set[str]) -> str | None:
    """Name the first selector (glob-matched) or literal (path, digest, flag) that ``text`` cites."""
    normalized = text.replace("\\", "/")
    for literal in sorted(literals):
        if literal in normalized:
            return literal
    for token in _EVIDENCE_TOKEN.findall(normalized):
        token = token.rstrip(".")
        for selector in sorted(selectors):
            if token == selector or fnmatchcase(token, selector):
                return selector
    return None


def _ci_references(value: Any) -> set[tuple[str, str, str]]:
    """Extract well-formed CI references as (workflow, run, commit) tuples."""
    references: set[tuple[str, str, str]] = set()
    for text in _text_values(value):
        if ACCEPTANCE_CI_REFERENCE.fullmatch(text):
            workflow_run, commit = text[3:].split("@", 1)
            workflow, run = workflow_run.split("/", 1)
            references.add((workflow, run, commit))
    return references


def _evidence_boundary(contract: dict[str, Any]) -> tuple[dict, dict, dict, dict] | None:
    """(stage, items, substitution sources, predecessor-only items), or None when malformed.

    Malformed ledgers are reported by the schema and stage checks; this
    boundary only polices well-formed evidence.
    """
    readiness = contract.get("readiness")
    stage = readiness.get("predecessorRelease") if isinstance(readiness, dict) else None
    work_items = contract.get("workItems")
    if not isinstance(stage, dict) or not isinstance(work_items, list):
        return None
    items = {item["id"]: item for item in work_items if isinstance(item, dict) and isinstance(item.get("id"), str)}
    substitutions = stage.get("qualificationSubstitutions")
    if not isinstance(substitutions, dict):
        return None
    sources = {key: items[key] for key in substitutions if key in items}
    predecessor_only = {key: item for key, item in items.items() if item.get("predecessorOnly") is True}
    return stage, items, sources, predecessor_only


def nminus1_evidence_errors(contract: dict[str, Any]) -> list[str]:
    """Refuse N-1 upgrade or rollback results presented as predecessor evidence (REL-191).

    The vocabulary comes from the ledger: every selector the N-1 items declare
    (minus selectors a predecessor-only item also owns, such as the shared
    interruption drill), every N-1 criterion digest, and the N-1 provisioner.
    The predecessor sign-off and every predecessor-only item's acceptance
    evidence and notes must name none of them, and no predecessor-only item may
    depend on an N-1 item.
    """
    boundary = _evidence_boundary(contract)
    if boundary is None:
        return []
    stage, items, _, predecessor_only = boundary
    errors: list[str] = []
    n_minus_one = {item_id: items[item_id] for item_id in N_MINUS_ONE_ITEM_IDS if item_id in items}
    shared = set().union(*(_item_selectors(item) for item in predecessor_only.values()))
    selectors = set().union(*(_item_selectors(item) for item in n_minus_one.values())) - shared
    literals = set().union(*(_item_digests(item) for item in n_minus_one.values())) | {N_MINUS_ONE_PROVISIONER}

    for text in _text_values(stage.get("signOffEvidence", [])):
        cited = _foreign_reference(text, selectors, literals)
        if cited:
            errors.append(f"predecessorRelease.signOffEvidence: cites N-1 evidence {cited}")
    for item_id, item in sorted(predecessor_only.items()):
        for text in _acceptance_texts(item):
            cited = _foreign_reference(text, selectors, literals)
            if cited:
                errors.append(f"{item_id}.acceptanceStatus: predecessor evidence cites N-1 evidence {cited}")
        for dependency in sorted(_dependencies(items, {item_id}) & set(n_minus_one)):
            errors.append(f"{item_id}: predecessor-only work cannot depend on N-1 item {dependency}")
    return errors


def predecessor_evidence_reuse_errors(contract: dict[str, Any]) -> list[str]:
    """Refuse predecessor results presented as evidence for the work they replace (REL-192).

    The mirror of ``nminus1_evidence_errors``. Every item the predecessor stage
    substitutes away (REL-190, REL-192, INST-131), and every N-1 item even if
    a ledger edit stops substituting it, must not cite a CI run at the
    predecessor baseline commit, a predecessor-only selector or criterion digest
    (minus selectors it shares), or the bootstrap qualifier mode. An N-1 item
    also cannot be evidenced or done until a real predecessor is published.
    """
    boundary = _evidence_boundary(contract)
    if boundary is None:
        return []
    stage, items, sources, predecessor_only = boundary
    sources = {**sources, **{key: items[key] for key in N_MINUS_ONE_ITEM_IDS if key in items}}
    shared = set().union(*(_item_selectors(item) for item in sources.values()))
    selectors = set().union(*(_item_selectors(item) for item in predecessor_only.values())) - shared
    literals = set().union(*(_item_digests(item) for item in predecessor_only.values())) | {BOOTSTRAP_QUALIFIER_MODE}
    source = stage.get("sourceCommitEvidence")
    baseline = source.get("baselineCommit") if isinstance(source, dict) else None
    baseline = baseline.lower() if isinstance(baseline, str) and _COMMIT_RE.fullmatch(baseline) else None
    source_is_reviewed = (
        isinstance(source, dict)
        and bool(baseline)
        and isinstance(stage.get("signOffEvidence"), list)
        and bool(stage.get("signOffEvidence"))
    )
    published = stage.get("state") in PUBLISHED_PREDECESSOR_STATES and source_is_reviewed

    errors: list[str] = []
    if stage.get("state") in PUBLISHED_PREDECESSOR_STATES and not source_is_reviewed:
        errors.append("predecessorRelease: published state requires a reviewed baselineCommit and signOffEvidence")
    predecessor_refs = _ci_references(stage.get("signOffEvidence", []))
    for predecessor_item in predecessor_only.values():
        predecessor_refs.update(_ci_references(_acceptance_texts(predecessor_item)))
    predecessor_runs = {(workflow, run) for workflow, run, _ in predecessor_refs}
    predecessor_commits = {commit for _, _, commit in predecessor_refs}
    for item_id, item in sorted(sources.items()):
        for text in _acceptance_texts(item):
            if baseline and ACCEPTANCE_CI_REFERENCE.match(text) and text.rsplit("@", 1)[1] == baseline:
                errors.append(f"{item_id}.acceptanceStatus: cites a CI run at the predecessor baseline commit")
            cited = _foreign_reference(text, selectors, literals)
            if cited:
                errors.append(f"{item_id}.acceptanceStatus: substituted v1 work cites predecessor evidence {cited}")
        for workflow, run, commit in _ci_references(_acceptance_texts(item)):
            if (workflow, run) in predecessor_runs or commit in predecessor_commits:
                errors.append(
                    f"{item_id}.acceptanceStatus: reuses predecessor sign-off CI evidence "
                    f"ci:{workflow}/{run}@{commit}"
                )
        if item_id in N_MINUS_ONE_ITEM_IDS and not published:
            states = [entry.get("state") for entry in item.get("acceptanceStatus", []) if isinstance(entry, dict)]
            if item.get("status") == "done" or "evidenced" in states:
                errors.append(f"{item_id}: N-1 work cannot be evidenced or done before a real predecessor is published")
    return errors
