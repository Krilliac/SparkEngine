#!/usr/bin/env python3
"""Bind experimental Shipping and installer ownership to executable workflows."""

from __future__ import annotations

import functools
import re
import sys
from pathlib import Path
from typing import Any

from common import REPO_ROOT, SiteDataError, read_bytes_stable

sys.path.insert(0, str(REPO_ROOT / "Tools" / "buildmatrix"))
import workflow  # noqa: E402

# Existing runtime test hosts, not platform certification. Each entry must still
# be named by the owning work item's CI contract; moving or adding a job fails.
SHIPPING_TEST_HOSTS = {
    ("build.yml", "security-runtime", "linux-shipping"): "SEC-100",
    ("build.yml", "network-security", "linux-shipping"): "NET-100",
    ("build.yml", "network-integration", "linux-shipping"): "SEC-100",
    ("build.yml", "service-contract", "linux-shipping"): "NET-110",
    ("operations-scheduled.yml", "recovery-drill", "linux-shipping"): "DATA-120",
    ("operations-scheduled.yml", "server-soak", "linux-shipping"): "OPS-110",
    ("operations-scheduled.yml", "soak-scheduled", "linux-shipping"): "PERF-100",
}
PLATFORM_SHIPPING_JOBS = {("build.yml", "build-macos-shipping", "macos-shipping"): "PLT-220"}
INSTALLER_PLATFORMS = {
    "Linux": ("platform.linux", "PLT-210"),
    "macOS": ("platform.macos", "PLT-220"),
}


@functools.lru_cache(maxsize=1)
def workflow_documents() -> dict[str, dict[str, Any]]:
    paths = sorted((REPO_ROOT / ".github" / "workflows").glob("*.y*ml"))
    if not paths:
        raise SiteDataError("experimental ownership: no workflow files")
    try:
        return {path.name: workflow.parse_workflow_yaml(
            read_bytes_stable(path, 4 * 1024 * 1024, str(path)).decode("utf-8")) for path in paths}
    except (workflow.WorkflowError, UnicodeError) as exc:
        raise SiteDataError(f"experimental ownership: {exc}") from exc


def required_jobs(document: dict[str, Any]) -> set[str]:
    """Include transitive dependencies: they can also block Required CI Gate."""
    jobs = document.get("jobs", {})
    pending = ["required-ci-gate"]
    found: set[str] = set()
    while pending:
        name = pending.pop()
        if name in found:
            continue
        found.add(name)
        needs = jobs.get(name, {}).get("needs", [])
        pending.extend([needs] if isinstance(needs, str) else needs)
    return found - {"required-ci-gate"}


def shipping_workflow_errors(
    contract: dict[str, Any], owners: dict[str, str], documents: dict[str, dict[str, Any]] | None = None,
) -> list[str]:
    documents = workflow_documents() if documents is None else documents
    items = {row["id"]: row for row in contract.get("workItems", [])}
    blocking = required_jobs(documents.get("build.yml", {}))
    errors: list[str] = []
    seen: set[tuple[str, str, str]] = set()
    for filename, document in sorted(documents.items()):
        for job_id, job in document.get("jobs", {}).items():
            combinations, _, _ = workflow._expand_matrix(job.get("strategy", {}).get("matrix"))
            for step in job.get("steps", []):
                script = step.get("run", "")
                if "--preset" not in script:
                    continue
                for combination in combinations:
                    env = {**document.get("env", {}), **job.get("env", {}), **step.get("env", {})}
                    env = {key: workflow._substitute_matrix(str(value), combination) for key, value in env.items()}
                    # Parse only preset-bearing logical commands. Unrelated shell
                    # heredocs and package-install scripts are not CMake programs.
                    for line in workflow.logical_shell_lines(script, "bash"):
                        if "--preset" not in line:
                            continue
                        # Shell environment variables are resolved only from the
                        # declared step/job/workflow environment, never guessed.
                        line = re.sub(r"\$(?:\{([A-Za-z_]\w*)\}|([A-Za-z_]\w*))",
                                      lambda match: str(env.get(match[1] or match[2], match[0])), line)
                        try:
                            commands, unresolved = workflow.parse_commands(
                                line.rstrip("\\`"), "bash", env, combination, {"job": job_id},
                            )
                        except workflow.WorkflowError as exc:
                            errors.append(f"{filename}/{job_id}: cannot resolve preset ownership: {exc}")
                            continue
                        if unresolved:
                            errors.append(f"{filename}/{job_id}: cannot resolve preset ownership")
                        for command in commands:
                            preset = command.get("preset", "")
                            if "$" in preset:
                                errors.append(f"{filename}/{job_id}: unresolved Shipping preset {preset}")
                            if preset not in owners:
                                continue
                            key = (filename, job_id, preset)
                            if key in seen:
                                continue
                            seen.add(key)
                            location = f"experimentalShipping.{preset}:{filename}/{job_id}"
                            test_owner = SHIPPING_TEST_HOSTS.get(key)
                            if test_owner:
                                if job_id not in items.get(test_owner, {}).get("requiredCiJobs", []):
                                    errors.append(f"{location}: test host must remain owned by {test_owner}")
                                continue
                            owner_id = owners[preset]
                            owner_jobs = [*items.get(owner_id, {}).get("requiredCiJobs", []),
                                          *items.get(owner_id, {}).get("plannedCiJobs", [])]
                            owned = (job_id in owner_jobs or PLATFORM_SHIPPING_JOBS.get(key) == owner_id)
                            if not owned:
                                errors.append(f"{location}: workflow job has no platform owner {owner_id}")
                            if filename == "build.yml" and job_id in blocking:
                                errors.append(f"{location}: platform certification must not block stable-v1")
                            if job.get("continue-on-error") is not True:
                                errors.append(f"{location}: platform Shipping job must be explicitly advisory")
    return errors


def installer_workflow_errors(
    contract: dict[str, Any], documents: dict[str, dict[str, Any]] | None = None,
) -> list[str]:
    documents = workflow_documents() if documents is None else documents
    readiness = contract.get("readiness", {})
    products = readiness.get("experimentalInstallerProducts", [])
    capabilities = {row["id"]: row for row in readiness.get("capabilities", [])}
    items = {row["id"]: row for row in contract.get("workItems", [])}
    job = documents.get("release.yml", {}).get("jobs", {}).get("build-installer", {})
    matrix = job.get("strategy", {}).get("matrix", {}).get("include", [])
    errors: list[str] = []
    if not matrix:
        return ["experimentalInstallers: release.yml/build-installer has no artifact matrix"]
    artifacts: set[str] = set()
    for row in matrix:
        artifact = row.get("artifact_name", "")
        if artifact == "SparkInstaller-Windows-x64.exe" and row.get("platform_name") == "Windows":
            continue
        platform = row.get("platform_name")
        if platform not in INSTALLER_PLATFORMS:
            errors.append(f"experimentalInstallers.{artifact}: unknown installer platform {platform}")
            continue
        capability, owner = INSTALLER_PLATFORMS[platform]
        artifacts.add(artifact)
        matches = [product for product in products if product.get("artifact") == artifact]
        if len(matches) != 1:
            errors.append(f"experimentalInstallers.{artifact}: requires exactly one platform-owned build product")
            continue
        product = matches[0]
        expected = {"kind": "installer", "applicability": "experimental", "capabilityIds": [capability],
                    "ownerWorkItemId": owner, "workflow": "release.yml", "job": "build-installer"}
        if any(product.get(key) != value for key, value in expected.items()):
            errors.append(f"experimentalInstallers.{artifact}: must remain experimental under {capability}/{owner}")
        if owner not in capabilities.get(capability, {}).get("blockingWorkItemIds", []):
            errors.append(f"experimentalInstallers.{artifact}: capability is detached from {owner}")
        if owner not in items or items[owner].get("profileApplicability", {}).get("stable-v1") != "outside":
            errors.append(f"experimentalInstallers.{artifact}: owner {owner} must remain outside stable-v1")
    for product in products:
        if product.get("artifact") not in artifacts:
            errors.append(f"experimentalInstallers.{product.get('artifact')}: no release matrix artifact")
    return errors
