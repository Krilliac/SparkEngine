#!/usr/bin/env python3
"""Stable-v1 module evidence manifest schema.

The manifest is a *policy declaration*: it states which modules a release
profile includes, what each module's production identity is, and which
evidence each must publish.  It deliberately carries no revision, timestamp,
or observation of its own — a committed file cannot truthfully attest to the
commit that contains it, and a declared observation is not an observation.
Those live in generated run evidence; see `provenance.py` and `lifecycle.py`.

Every identity the manifest references — profile ids, package-smoke owners,
experimental trackers — is resolved against the readiness contract in
`docs/`, so an invented identifier cannot satisfy a binding.
"""

from __future__ import annotations

import re
from pathlib import Path

import strict_json

SCHEMA_VERSION = "stable-v2"

VALID_PROFILE_APPLICABILITIES = frozenset({"required", "shared", "outside"})

VALID_MODULE_KINDS = frozenset({"Game", "Addon"})

# --- Module ABI surface ----------------------------------------------------
# These are the three free `extern "C"` exports the loader resolves by name out
# of the shared library (SparkSDK/Include/Spark/IModule.h, ModuleABI.h).
MODULE_EXPORTS = (
    "SparkGetModuleCompatibility",
    "CreateModule",
    "DestroyModule",
)

# These are C++ virtual methods on IModule, not exported symbols.  Six of them
# (OnFixedUpdate, OnRender, OnImGui, OnResize, OnPause, OnResume) have no-op
# defaults in the base class, so declaring one proves nothing whatsoever about
# the module: every module "has" them.  Declaring them is therefore not
# evidence, and the manifest no longer treats a declared phase list as proof.
# What must be proven is in lifecycle.REQUIRED_RUNTIME_PHASES.
MODULE_VIRTUALS = (
    "GetModuleInfo",
    "OnLoad",
    "OnUpdate",
    "OnFixedUpdate",
    "OnRender",
    "OnImGui",
    "OnResize",
    "OnPause",
    "OnResume",
    "CanUnload",
    "OnUnload",
)

# --- Evidence --------------------------------------------------------------
# Each evidence type names the producer that writes it.  A binding whose type
# has no producer in this table cannot be satisfied by anything, and a binding
# whose declared artifact does not match its producer's real output path is a
# claim about a file nobody creates.
EVIDENCE_PRODUCERS: dict[str, dict[str, str]] = {
    "junit-xml": {
        "producer": "ctest:SparkEngineTests",
        "definedIn": "Tests/CMakeLists.txt",
        "artifact": "build/SparkTests-junit.xml",
        "ciJob": "build-linux-gcc",
    },
    "sanitizer-report": {
        "producer": "sh:.github/scripts/run-sanitizer-tests.sh",
        "definedIn": ".github/workflows/build.yml",
        "artifact": "build/asan-ubsan-lsan-results.txt",
        "ciJob": "build-linux-asan",
    },
    "cmake-target-index": {
        "producer": "py:tools/module-evidence/collect_targets.py",
        "definedIn": ".github/workflows/build.yml",
        "artifact": "build/module-evidence/module-targets.json",
        "ciJob": "module-evidence-targets",
    },
    "lifecycle-log": {
        "producer": "py:tools/module-evidence/collect_lifecycle.py",
        "definedIn": ".github/workflows/build.yml",
        "artifact": "build/module-evidence/module-lifecycle.json",
        "ciJob": "module-profile-lifecycle",
    },
    "package-smoke-log": {
        "producer": "ctest:SparkInstalledPackageSmoke",
        "definedIn": "Tests/PackageSmoke/CMakeLists.txt",
        "artifact": "build/module-evidence/package-smoke.log",
        "ciJob": "module-profile-package-smoke",
    },
}

VALID_EVIDENCE_TYPES = frozenset(EVIDENCE_PRODUCERS)

# Evidence every module a profile *includes* must publish.  An included module
# with no bindings is an unproven module presented as a proven one.
REQUIRED_INCLUDED_EVIDENCE = frozenset(
    {"cmake-target-index", "lifecycle-log", "junit-xml", "package-smoke-log"}
)

# --- Key sets (closed world: unknown keys are rejected at every level) ------
REQUIRED_TOP_LEVEL_KEYS = frozenset({"schemaVersion", "modules", "profiles"})
OPTIONAL_TOP_LEVEL_KEYS: frozenset[str] = frozenset()

REQUIRED_MODULE_KEYS = frozenset({
    "name", "cmakeTarget", "sharedLibrary", "sourceDirectory",
    "moduleKind", "profileApplicability",
})
OPTIONAL_MODULE_KEYS = frozenset({
    "dependencies", "packageSmokeOwner", "evidenceBindings", "experimentalSeparation",
})

REQUIRED_PROFILE_KEYS = frozenset({"id", "includedModules", "excludedModules"})
OPTIONAL_PROFILE_KEYS: frozenset[str] = frozenset()

REQUIRED_BINDING_KEYS = frozenset({"type", "artifactPattern"})
OPTIONAL_BINDING_KEYS: frozenset[str] = frozenset()

REQUIRED_SEPARATION_KEYS = frozenset({"trackedUnder"})
OPTIONAL_SEPARATION_KEYS: frozenset[str] = frozenset()

VALID_LIBRARY_PLATFORMS = ("windows", "linux", "macos")

MODULE_NAME_RE = re.compile(r"^[A-Z][A-Za-z0-9]{2,63}$")
WORK_ITEM_ID_RE = re.compile(r"^[A-Z]{2,6}-[0-9]{3}$")
PROFILE_ID_RE = re.compile(r"^[a-z][a-z0-9-]{1,63}$")

# Bounds on collection sizes, so a manifest cannot become a resource problem.
MAX_MODULES = 128
MAX_PROFILES = 16
MAX_BINDINGS_PER_MODULE = 16
MAX_DEPENDENCIES_PER_MODULE = 32


def expected_library_names(cmake_target: str) -> dict[str, str]:
    """The only per-platform filenames a target may legitimately declare.

    CMake derives these from the target name via CMAKE_SHARED_LIBRARY_PREFIX
    and _SUFFIX, so a manifest naming anything else is claiming an identity the
    build does not produce.
    """
    return {
        "windows": f"{cmake_target}.dll",
        "linux": f"lib{cmake_target}.so",
        "macos": f"lib{cmake_target}.dylib",
    }


def load_known_profile_ids(repo_root: Path) -> tuple[set[str], str | None]:
    """Release profile ids declared by the readiness contract."""
    path = repo_root / "docs" / "site" / "readiness.json"
    try:
        document = strict_json.load_file(path, limits=strict_json.CONTRACT_LIMITS)
    except strict_json.StrictJSONError as exc:
        return set(), f"cannot read release profiles from {path}: {exc}"
    profiles = document.get("releaseProfiles")
    if not isinstance(profiles, list) or not profiles:
        return set(), f"{path} declares no releaseProfiles"
    ids = {p.get("id") for p in profiles if isinstance(p, dict) and p.get("id")}
    if not ids:
        return set(), f"{path} declares no usable release profile ids"
    return ids, None


def load_known_work_item_ids(repo_root: Path) -> tuple[set[str], str | None]:
    """Work item ids declared by the readiness contract.

    Owners and trackers must resolve to one of these.  Without this, any
    invented string satisfies an ownership requirement.
    """
    directory = repo_root / "docs" / "readiness" / "work-items"
    if not directory.is_dir():
        return set(), f"work item directory not found: {directory}"
    ids: set[str] = set()
    errors: list[str] = []
    for path in sorted(directory.glob("*.json")):
        try:
            document = strict_json.load_file(path, limits=strict_json.CONTRACT_LIMITS)
        except strict_json.StrictJSONError as exc:
            errors.append(f"{path.name}: {exc}")
            continue
        items = document.get("workItems") if isinstance(document, dict) else None
        if items is None and isinstance(document, list):
            items = document
        if not isinstance(items, list):
            continue
        for item in items:
            if isinstance(item, dict) and isinstance(item.get("id"), str):
                ids.add(item["id"])
    if errors:
        return ids, "; ".join(errors)
    if not ids:
        return set(), (
            f"no work items found under {directory} — ownership cannot be "
            f"verified, so ownership claims must not be accepted"
        )
    return ids, None


def _load_rooted_contract_json(
    root: strict_json.NoFollowDirectoryLease, relative: str,
) -> object:
    """Decode a committed contract file through held relative bytes."""
    data = root.read_relative_bytes(
        relative, max_bytes=strict_json.CONTRACT_LIMITS.document_bytes,
    )
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise strict_json.NoFollowAuthorityError(
            f"rooted contract {relative!r} is not UTF-8: {exc}"
        ) from exc
    return strict_json.loads(
        text, origin=relative, limits=strict_json.CONTRACT_LIMITS,
    )


def load_known_profile_ids_rooted(
    root: strict_json.NoFollowDirectoryLease,
) -> tuple[set[str], str | None]:
    """Read release profile ids from held root-relative contract bytes."""
    relative = "docs/site/readiness.json"
    try:
        document = _load_rooted_contract_json(root, relative)
    except strict_json.StrictJSONError as exc:
        return set(), f"cannot read rooted release profiles from {relative}: {exc}"
    profiles = document.get("releaseProfiles") if isinstance(document, dict) else None
    if not isinstance(profiles, list) or not profiles:
        return set(), f"{relative} declares no releaseProfiles"
    ids = {profile.get("id") for profile in profiles if isinstance(profile, dict) and profile.get("id")}
    if not ids:
        return set(), f"{relative} declares no usable release profile ids"
    return ids, None


def load_known_work_item_ids_rooted(
    root: strict_json.NoFollowDirectoryLease,
) -> tuple[set[str], str | None]:
    """Read work-item ids through held POSIX root-relative authority."""
    directory = "docs/readiness/work-items"
    try:
        names = root.list_relative_names(directory)
    except strict_json.StrictJSONError as exc:
        return set(), f"cannot list rooted work item directory {directory}: {exc}"
    json_names = [name for name in names if name.endswith(".json")]
    if not json_names:
        return set(), (
            f"no work items found under rooted {directory} — ownership cannot be "
            "verified, so ownership claims must not be accepted"
        )
    ids: set[str] = set()
    errors: list[str] = []
    for name in json_names:
        relative = f"{directory}/{name}"
        try:
            document = _load_rooted_contract_json(root, relative)
        except strict_json.StrictJSONError as exc:
            errors.append(f"{name}: {exc}")
            continue
        items = document.get("workItems") if isinstance(document, dict) else None
        if items is None and isinstance(document, list):
            items = document
        if not isinstance(items, list):
            continue
        for item in items:
            if isinstance(item, dict) and isinstance(item.get("id"), str):
                ids.add(item["id"])
    if errors:
        return ids, "; ".join(errors)
    if not ids:
        return set(), (
            f"no usable work items found under rooted {directory} — ownership cannot "
            "be verified, so ownership claims must not be accepted"
        )
    return ids, None
