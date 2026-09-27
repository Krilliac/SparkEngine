#!/usr/bin/env python3
"""Qualify one release candidate and record why (REL-190).

``profile-required-gates`` runs this before a versioned publication. It is pure
logic over repository files plus the exact-SHA record that
``.github/scripts/verify-exact-required-gate.py`` wrote for the candidate; it
makes no network request. The candidate qualifies only when:

1. the stage's readiness contract holds (``release_stages``:
   ``candidate_readiness_errors`` for ``stable-v1``,
   ``predecessor_candidate_readiness_errors`` for ``predecessor``);
2. every ``requiredCiJobs`` entry of every qualification item in the stage's
   transitive required set names a job whose failure blocks publication: a job
   the build.yml ``required-ci-gate`` aggregate needs, a job the exact-SHA
   verifier certifies, or a job on the release.yml ``release`` job's ``needs``
   chain. Any other job can be skipped without stopping the release, so it
   cannot be qualification evidence;
3. the exact-CI record is complete and well formed (closed key set, no empty,
   duplicate or line-broken field) and yields a valid exact-evidence manifest
   for the candidate commit (``exact_evidence.build_manifest``).

The verdict is written as a closed, deterministic ``spark.release-qualification/1``
JSON report to a path that must not exist yet, including on refusal, so the
workflow can retain it. Exit status: 0 qualified, 1 refused or unwritable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import contract_selectors  # noqa: E402
import exact_evidence  # noqa: E402
from common import SiteDataError, load_contract  # noqa: E402
from release_stages import (  # noqa: E402
    _dependencies,
    candidate_readiness_errors,
    predecessor_candidate_readiness_errors,
)

SCHEMA = "spark.release-qualification/1"
STAGES = ("stable-v1", "predecessor")
RELEASE_WORKFLOW = contract_selectors.WORKFLOW_ROOT / "release.yml"
RELEASE_JOB = "release"
# The exact-SHA verifier the profile-required-gates job runs first refuses the
# candidate unless these jobs succeeded for it (see SOURCE_JOB_NAME,
# VERIFIER_JOB_NAME, CODEQL_SOURCE_JOB_NAMES and CODEQL_REPORTER_JOB_NAME in
# .github/scripts/verify-exact-required-gate.py), so they block publication too.
EXACT_GATE_CERTIFIED_JOBS = {
    "build.yml": ("required-ci-gate", "build-windows-shipping"),
    "build-matrix-verifier.yml": ("verify",),
    "codeql.yml": ("analyze",),
    "codeql-report.yml": ("report",),
}
_CANDIDATE_SHA = re.compile(r"[0-9a-f]{40}")


def release_needs_chain(needs: dict[str, frozenset[str]]) -> frozenset[str]:
    """Every job the release job transitively needs: all must succeed before it runs."""
    if RELEASE_JOB not in needs:
        raise SiteDataError(f"release.yml defines no {RELEASE_JOB} job")
    return frozenset(_dependencies({job: {"dependencies": sorted(needed)} for job, needed in needs.items()},
                                   set(needs[RELEASE_JOB])))


def exact_gate_certified_jobs() -> frozenset[str]:
    """Jobs whose success verify-exact-required-gate.py certifies for the candidate SHA."""
    certified: set[str] = set()
    for workflow, jobs in EXACT_GATE_CERTIFIED_JOBS.items():
        defined = contract_selectors.workflow_job_needs(contract_selectors.WORKFLOW_ROOT / workflow)
        for job in jobs:
            if job not in defined:
                raise SiteDataError(f"{workflow} defines no {job} job for the exact-SHA gate to certify")
            certified.add(job)
    return frozenset(certified)


def publication_blocking_jobs() -> frozenset[str]:
    return (
        contract_selectors.required_gate_jobs()
        | exact_gate_certified_jobs()
        | release_needs_chain(contract_selectors.workflow_job_needs(RELEASE_WORKFLOW))
    )


def qualification_items(contract: dict[str, Any], stage: str) -> set[str]:
    """The stage's transitive required qualification set, publication finalizers excluded.

    Mirrors the traversal in ``release_stages`` so the job check covers exactly
    the items whose completion the readiness check requires.
    """
    readiness = contract["readiness"]
    items = {item["id"]: item for item in contract["workItems"]}
    gates = {gate["id"]: gate for gate in readiness["gates"]}
    profiles = {profile["id"]: profile for profile in readiness["releaseProfiles"]}
    if stage == "stable-v1":
        profile = profiles["stable-v1"]
        finalizers = set(profile["publicationFinalization"]["workItemIds"])
        required = _dependencies(items, set(profile["blockingWorkItemIds"]))
        for gate_id in profile["requiredGateIds"]:
            required |= _dependencies(items, set(gates.get(gate_id, {}).get("blockingWorkItemIds", [])))
        return required - finalizers
    predecessor = readiness["predecessorRelease"]
    profile = profiles[predecessor["profileId"]]
    substitutions = predecessor["qualificationSubstitutions"]
    excluded = set(substitutions) | set(profile["publicationFinalization"]["workItemIds"])
    required = _dependencies(items, set(predecessor["blockingWorkItemIds"]), excluded)
    for target_id in substitutions.values():
        required |= _dependencies(items, {target_id})
    for gate_id in predecessor["requiredGateIds"]:
        blockers = set(gates.get(gate_id, {}).get("blockingWorkItemIds", []))
        required |= _dependencies(items, blockers, excluded)
    return required - set(predecessor["publicationFinalization"]["workItemIds"])


def ci_job_errors(contract: dict[str, Any], checked: set[str], blocking: frozenset[str]) -> list[str]:
    items = {item["id"]: item for item in contract["workItems"]}
    errors: list[str] = []
    for item_id in sorted(checked):
        for job in items.get(item_id, {}).get("requiredCiJobs", []):
            if job not in blocking:
                errors.append(
                    f"{item_id}: required CI job {job} can be skipped without blocking publication "
                    "(not needed by required-ci-gate or by the release job)"
                )
    return errors


def exact_ci_errors(path: Path, repository: str, candidate_sha: str) -> tuple[list[str], str | None]:
    """Validate the exact-CI record; return errors and the manifest digest when valid."""
    try:
        values = exact_evidence.values_from_gate_output(path, repository=repository, source_commit=candidate_sha)
        manifest = exact_evidence.build_manifest(values)
    except exact_evidence.ExactEvidenceError as error:
        return [f"exact-ci record: {error}"], None
    return [], "sha256:" + hashlib.sha256(exact_evidence.canonical_bytes(manifest)).hexdigest()


def qualify(
    contract: dict[str, Any],
    stage: str,
    candidate_sha: str,
    exact_ci: Path,
    repository: str,
    blocking: frozenset[str],
) -> dict[str, Any]:
    errors: list[str] = []
    checked: set[str] = set()
    if not _CANDIDATE_SHA.fullmatch(candidate_sha):
        errors.append("candidate SHA must be a lowercase 40-character commit")
    readiness_check = candidate_readiness_errors if stage == "stable-v1" else predecessor_candidate_readiness_errors
    try:
        errors.extend(f"readiness: {message}" for message in readiness_check(contract))
        checked = qualification_items(contract, stage)
    except (KeyError, TypeError, AttributeError) as error:
        errors.append(f"readiness: contract cannot be evaluated for {stage}: {error!r}")
    errors.extend(ci_job_errors(contract, checked, blocking))
    record_errors, manifest_digest = exact_ci_errors(exact_ci, repository, candidate_sha)
    errors.extend(record_errors)
    return {
        "schema": SCHEMA,
        "stage": stage,
        "candidateSha": candidate_sha,
        "exactCiManifestDigest": manifest_digest,
        "checkedItems": sorted(checked),
        "errors": sorted(set(errors)),
        "verdict": "refused" if errors else "qualified",
    }


def report_bytes(report: dict[str, Any]) -> bytes:
    return (json.dumps(report, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("utf-8")


def write_new(path: Path, payload: bytes) -> None:
    """Create ``path`` with ``payload``; never replace an existing file or link."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o644)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stage", required=True, choices=STAGES)
    parser.add_argument("--candidate-sha", required=True)
    parser.add_argument("--exact-ci", required=True, type=Path,
                        help="key=value record written by verify-exact-required-gate.py")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""),
                        help="owner/name (default: $GITHUB_REPOSITORY)")
    arguments = parser.parse_args(argv)
    try:
        report = qualify(load_contract(), arguments.stage, arguments.candidate_sha, arguments.exact_ci,
                         arguments.repository, publication_blocking_jobs())
    except SiteDataError as error:
        print(f"release_qualification: {error}", file=sys.stderr)
        return 1
    try:
        write_new(arguments.output, report_bytes(report))
    except FileExistsError:
        print(f"release_qualification: report {arguments.output} already exists; refusing to replace it",
              file=sys.stderr)
        return 1
    except OSError as error:
        print(f"release_qualification: cannot write report {arguments.output}: {error}", file=sys.stderr)
        return 1
    for error in report["errors"]:
        print(f"release_qualification: {error}", file=sys.stderr)
    print(f"release_qualification: {report['stage']} candidate {report['candidateSha']} {report['verdict']} "
          f"({len(report['checkedItems'])} qualification items, {len(report['errors'])} errors)")
    return 0 if report["verdict"] == "qualified" else 1


if __name__ == "__main__":
    raise SystemExit(main())
