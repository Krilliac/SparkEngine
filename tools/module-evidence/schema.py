#!/usr/bin/env python3
"""Stable-v1 module evidence manifest schema definition.

This module defines the deterministic machine-readable schema for declaring
which modules belong to which release profile, what their production identity
is, and what evidence bindings exist.  The schema is versioned and pinned;
validators reject unknown keys and ambiguous identities.
"""

from __future__ import annotations

SCHEMA_VERSION = "stable-v1"

VALID_PROFILE_APPLICABILITIES = frozenset({"required", "shared", "outside"})

VALID_LIFECYCLE_PHASES = (
    "SparkGetModuleCompatibility",
    "CreateModule",
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
    "DestroyModule",
)

REQUIRED_LIFECYCLE_PHASES = frozenset({
    "SparkGetModuleCompatibility",
    "CreateModule",
    "GetModuleInfo",
    "OnLoad",
    "OnUpdate",
    "OnUnload",
    "DestroyModule",
})

VALID_MODULE_KINDS = frozenset({"Game", "Addon"})

VALID_EVIDENCE_TYPES = frozenset({
    "junit-xml",
    "sanitizer-report",
    "coverage-lcov",
    "package-smoke-log",
    "lifecycle-log",
})

REQUIRED_TOP_LEVEL_KEYS = frozenset({
    "schemaVersion",
    "generatedAt",
    "commitSHA",
    "modules",
    "profiles",
})

REQUIRED_MODULE_KEYS = frozenset({
    "name",
    "cmakeTarget",
    "sharedLibrary",
    "sourceDirectory",
    "moduleKind",
    "lifecyclePhases",
    "profileApplicability",
})

OPTIONAL_MODULE_KEYS = frozenset({
    "dependencies",
    "packageSmokeOwner",
    "evidenceBindings",
    "experimentalSeparation",
})

REQUIRED_PROFILE_KEYS = frozenset({
    "id",
    "includedModules",
    "excludedModules",
})

SHARED_LIBRARY_PATTERNS = {
    "windows": r"^[A-Za-z][A-Za-z0-9_-]*\.dll$",
    "linux": r"^lib[A-Za-z][A-Za-z0-9_-]*\.so$",
    "macos": r"^lib[A-Za-z][A-Za-z0-9_-]*\.dylib$",
}
