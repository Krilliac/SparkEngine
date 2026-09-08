#!/usr/bin/env python3
"""Fail-closed validator for the module evidence manifest (RDY-010).

The manifest declares which modules a release profile ships and what evidence
each must publish.  This validator refuses to let a declaration stand in for
the thing it declares:

  * CMake targets are proven by a configure-generated File API codemodel, not
    by text matching `add_library(` in a file that may never execute.
  * Lifecycle phases are proven by a recorded ModuleManager run against the
    exact source tree under test, not by a list of strings.
  * Evidence bindings must name the real output path of a real producer.
  * Owners and trackers must resolve to real readiness work items.
  * The revision is supplied from outside the committed file, because a
    committed file cannot attest to its own commit.

Missing evidence is a blocking error.  There is no path through this program
on which an unconfigured tree, an absent artifact, or an unavailable producer
is reported as success.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import artifacts
import lifecycle as lifecycle_mod
import paths as paths_mod
import provenance
import strict_json
import targets as targets_mod
from schema import (
    EVIDENCE_PRODUCERS,
    MAX_BINDINGS_PER_MODULE,
    MAX_DEPENDENCIES_PER_MODULE,
    MAX_MODULES,
    MAX_PROFILES,
    MODULE_NAME_RE,
    OPTIONAL_BINDING_KEYS,
    OPTIONAL_MODULE_KEYS,
    OPTIONAL_PROFILE_KEYS,
    OPTIONAL_SEPARATION_KEYS,
    OPTIONAL_TOP_LEVEL_KEYS,
    PROFILE_ID_RE,
    REQUIRED_BINDING_KEYS,
    REQUIRED_INCLUDED_EVIDENCE,
    REQUIRED_MODULE_KEYS,
    REQUIRED_PROFILE_KEYS,
    REQUIRED_SEPARATION_KEYS,
    REQUIRED_TOP_LEVEL_KEYS,
    SCHEMA_VERSION,
    VALID_LIBRARY_PLATFORMS,
    VALID_MODULE_KINDS,
    VALID_PROFILE_APPLICABILITIES,
    WORK_ITEM_ID_RE,
    expected_library_names,
    load_known_profile_ids,
    load_known_work_item_ids,
)

REPO_ROOT = Path(__file__).resolve().parents[2]


class ManifestError(RuntimeError):
    """A failure that must block release promotion."""


def _bounded_str(value: Any, label: str, errors: list[str], *,
                 pattern=None, max_length: int = 128) -> bool:
    """Require a non-empty, bounded, optionally patterned string."""
    if not isinstance(value, str) or isinstance(value, bool):
        errors.append(f"{label} must be a string, got {type(value).__name__}")
        return False
    if not value:
        errors.append(f"{label} must be non-empty")
        return False
    if len(value) > max_length:
        errors.append(f"{label} exceeds {max_length} characters")
        return False
    if pattern is not None and not pattern.match(value):
        errors.append(f"{label} {value!r} does not match {pattern.pattern}")
        return False
    return True


def _check_keys(obj: dict[str, Any], required: frozenset[str], optional: frozenset[str],
                label: str, errors: list[str]) -> bool:
    """Closed-world key checking: unknown keys are rejected, not ignored."""
    present = set(obj.keys())
    missing = required - present
    if missing:
        errors.append(f"{label}: missing required keys {sorted(missing)}")
    unknown = present - required - optional
    if unknown:
        errors.append(f"{label}: unknown keys {sorted(unknown)}")
    return not missing and not unknown


class ManifestValidator:
    """Fail-closed validator: any unrecognised or unproven state is an error."""

    def __init__(
        self,
        manifest: dict[str, Any],
        repo_root: Path | None = None,
        *,
        target_index: dict[str, Any] | None = None,
        target_error: str | None = None,
        lifecycle_evidence: dict[str, Any] | None = None,
        lifecycle_error: str | None = None,
        expected_sha: str | None = None,
        policy_only: bool = False,
        declared_gaps: dict[str, str] | None = None,
    ):
        # Evidence types acknowledged as having no producer yet, mapped to the
        # work item tracking each.  These are downgraded to warnings so that
        # blocking CI can enforce everything else; an undeclared gap is still a
        # hard failure, and a declared gap whose evidence has become available
        # is also a failure, so the ledger can only shrink.
        self.declared_gaps = declared_gaps or {}
        self.warnings: list[str] = []
        self.manifest = manifest
        self.repo_root = repo_root or REPO_ROOT
        self.target_index = target_index
        self.target_error = target_error
        self.lifecycle_evidence = lifecycle_evidence
        self.lifecycle_error = lifecycle_error
        self.expected_sha = expected_sha
        self.policy_only = policy_only
        self.errors: list[str] = []
        self.known_profiles: set[str] = set()
        self.known_work_items: set[str] = set()

    # -- helpers ----------------------------------------------------------
    def _err(self, msg: str) -> None:
        self.errors.append(msg)

    def _modules(self) -> list[dict[str, Any]]:
        modules = self.manifest.get("modules")
        return [m for m in modules if isinstance(m, dict)] if isinstance(modules, list) else []

    def _profiles(self) -> list[dict[str, Any]]:
        profiles = self.manifest.get("profiles")
        return [p for p in profiles if isinstance(p, dict)] if isinstance(profiles, list) else []

    # -- entry point ------------------------------------------------------
    def validate(self) -> list[str]:
        self.errors = []
        if not isinstance(self.manifest, dict):
            return ["manifest must be a JSON object"]

        self._load_contract()
        _check_keys(self.manifest, REQUIRED_TOP_LEVEL_KEYS, OPTIONAL_TOP_LEVEL_KEYS,
                    "manifest", self.errors)
        if self.errors:
            return self.errors
        if self.manifest.get("schemaVersion") != SCHEMA_VERSION:
            self._err(f"schemaVersion must be {SCHEMA_VERSION!r}, got "
                      f"{self.manifest.get('schemaVersion')!r}")
            return self.errors

        self._check_profiles()
        self._check_modules()
        self._check_partition()
        self._check_duplicate_identities()
        self._check_cross_references()
        self._check_experimental_separation()
        self._check_target_evidence()
        self._check_lifecycle_evidence()
        self._check_artifact_evidence()
        return self.errors

    def _load_contract(self) -> None:
        """Load the identity universes.  An unreadable contract is blocking."""
        profiles, err = load_known_profile_ids(self.repo_root)
        if err:
            self._err(
                f"release profile registry unavailable: {err} — profile "
                f"identities cannot be verified, so none may be accepted"
            )
        self.known_profiles = profiles

        items, err = load_known_work_item_ids(self.repo_root)
        if err:
            self._err(
                f"work item registry unavailable: {err} — ownership and "
                f"tracker identities cannot be verified, so none may be accepted"
            )
        self.known_work_items = items

    # -- profiles ---------------------------------------------------------
    def _check_profiles(self) -> None:
        profiles = self.manifest.get("profiles")
        if not isinstance(profiles, list):
            self._err("profiles must be an array")
            return
        if not profiles:
            self._err("at least one release profile is required")
            return
        if len(profiles) > MAX_PROFILES:
            self._err(f"profiles: {len(profiles)} exceeds the limit of {MAX_PROFILES}")
            return

        seen: dict[str, int] = {}
        for i, profile in enumerate(profiles):
            label = f"profiles[{i}]"
            if not isinstance(profile, dict):
                self._err(f"{label} must be an object")
                continue
            _check_keys(profile, REQUIRED_PROFILE_KEYS, OPTIONAL_PROFILE_KEYS,
                        label, self.errors)

            pid = profile.get("id")
            if _bounded_str(pid, f"{label}.id", self.errors, pattern=PROFILE_ID_RE):
                assert isinstance(pid, str)
                if pid not in self.known_profiles:
                    self._err(
                        f"{label}.id {pid!r} is not a release profile declared in "
                        f"docs/site/readiness.json {sorted(self.known_profiles)} — "
                        f"an invented profile cannot gate a release"
                    )
                folded = pid.casefold()
                if folded in seen:
                    self._err(
                        f"{label}.id {pid!r} collides with profiles[{seen[folded]}].id"
                    )
                seen[folded] = i

            for key in ("includedModules", "excludedModules"):
                self._check_module_name_list(profile.get(key), f"{label}.{key}")

    def _check_module_name_list(self, value: Any, label: str) -> None:
        if not isinstance(value, list):
            self._err(f"{label} must be an array")
            return
        if len(value) > MAX_MODULES:
            self._err(f"{label}: {len(value)} entries exceeds {MAX_MODULES}")
            return
        seen: dict[str, int] = {}
        for i, name in enumerate(value):
            if not _bounded_str(name, f"{label}[{i}]", self.errors,
                                pattern=MODULE_NAME_RE):
                continue
            folded = name.casefold()
            if folded in seen:
                self._err(
                    f"{label}: {name!r} is listed twice (indices {seen[folded]} "
                    f"and {i}) — a duplicate classification is ambiguous"
                )
            seen[folded] = i

    # -- modules ----------------------------------------------------------
    def _check_modules(self) -> None:
        modules = self.manifest.get("modules")
        if not isinstance(modules, list):
            self._err("modules must be an array")
            return
        if not modules:
            self._err("at least one module must be declared")
            return
        if len(modules) > MAX_MODULES:
            self._err(f"modules: {len(modules)} exceeds the limit of {MAX_MODULES}")
            return
        for i, mod in enumerate(modules):
            if not isinstance(mod, dict):
                self._err(f"modules[{i}] must be an object")
                continue
            self._check_module(i, mod)

    def _check_module(self, idx: int, mod: dict[str, Any]) -> None:
        label = f"modules[{idx}]"
        _check_keys(mod, REQUIRED_MODULE_KEYS, OPTIONAL_MODULE_KEYS, label, self.errors)

        name = mod.get("name")
        name_ok = _bounded_str(name, f"{label}.name", self.errors, pattern=MODULE_NAME_RE)

        target = mod.get("cmakeTarget")
        if _bounded_str(target, f"{label}.cmakeTarget", self.errors,
                        pattern=MODULE_NAME_RE) and name_ok and target != name:
            self._err(
                f"{label}: cmakeTarget {target!r} differs from module name "
                f"{name!r} — the target and the module must be one identity"
            )

        if name_ok:
            assert isinstance(name, str)
            self._check_shared_library(label, mod, name)
            src = mod.get("sourceDirectory")
            if isinstance(src, str):
                for err in paths_mod.check_source_directory(src, name, self.repo_root):
                    self._err(f"{label}: {err}")
            else:
                self._err(f"{label}.sourceDirectory must be a string")

        if mod.get("moduleKind") not in VALID_MODULE_KINDS:
            self._err(f"{label}.moduleKind must be one of "
                      f"{sorted(VALID_MODULE_KINDS)}, got {mod.get('moduleKind')!r}")

        self._check_profile_applicability(label, mod)
        self._check_dependencies(label, mod)
        self._check_evidence_bindings(label, mod)
        self._check_owner(label, mod)

    def _check_shared_library(self, label: str, mod: dict[str, Any], name: str) -> None:
        lib = mod.get("sharedLibrary")
        if not isinstance(lib, dict):
            self._err(f"{label}.sharedLibrary must be an object")
            return
        if not lib:
            self._err(f"{label}.sharedLibrary must declare at least one platform")
            return
        target = mod.get("cmakeTarget")
        base = target if isinstance(target, str) and target else name
        expected = expected_library_names(base)
        for platform, filename in lib.items():
            if platform not in VALID_LIBRARY_PLATFORMS:
                self._err(f"{label}.sharedLibrary: unknown platform {platform!r}; "
                          f"expected one of {list(VALID_LIBRARY_PLATFORMS)}")
                continue
            if not _bounded_str(filename, f"{label}.sharedLibrary.{platform}",
                                self.errors):
                continue
            if filename != expected[platform]:
                self._err(
                    f"{label}.sharedLibrary.{platform} is {filename!r} but CMake "
                    f"builds target {base!r} as {expected[platform]!r} — a module "
                    f"may not claim a library identity the build does not produce"
                )

    def _check_profile_applicability(self, label: str, mod: dict[str, Any]) -> None:
        pa = mod.get("profileApplicability")
        if not isinstance(pa, dict):
            self._err(f"{label}.profileApplicability must be an object")
            return
        declared = {p.get("id") for p in self._profiles() if isinstance(p.get("id"), str)}
        keys = set(pa.keys())
        if keys != declared:
            missing = sorted(declared - keys)
            extra = sorted(keys - declared)
            detail = []
            if missing:
                detail.append(f"missing {missing}")
            if extra:
                detail.append(f"undeclared {extra}")
            self._err(
                f"{label}.profileApplicability keys must be exactly the declared "
                f"profiles {sorted(declared)} — {', '.join(detail)}. Every module "
                f"must state its relationship to every profile."
            )
        for pid, applicability in pa.items():
            if applicability not in VALID_PROFILE_APPLICABILITIES:
                self._err(
                    f"{label}.profileApplicability[{pid!r}] must be one of "
                    f"{sorted(VALID_PROFILE_APPLICABILITIES)}, got {applicability!r}"
                )

    def _check_dependencies(self, label: str, mod: dict[str, Any]) -> None:
        deps = mod.get("dependencies", [])
        if not isinstance(deps, list):
            self._err(f"{label}.dependencies must be an array")
            return
        if len(deps) > MAX_DEPENDENCIES_PER_MODULE:
            self._err(f"{label}.dependencies exceeds {MAX_DEPENDENCIES_PER_MODULE}")
            return
        known = {m.get("name") for m in self._modules()}
        seen: dict[str, int] = {}
        for i, dep in enumerate(deps):
            if not _bounded_str(dep, f"{label}.dependencies[{i}]", self.errors,
                                pattern=MODULE_NAME_RE):
                continue
            folded = dep.casefold()
            if folded in seen:
                self._err(f"{label}.dependencies: {dep!r} listed twice")
            seen[folded] = i
            if dep not in known:
                self._err(
                    f"{label}.dependencies[{i}] {dep!r} is not a declared module — "
                    f"a dependency on nothing cannot be satisfied or verified"
                )
            if dep == mod.get("name"):
                self._err(f"{label}.dependencies: module depends on itself")

    def _check_evidence_bindings(self, label: str, mod: dict[str, Any]) -> None:
        bindings = mod.get("evidenceBindings", [])
        if not isinstance(bindings, list):
            self._err(f"{label}.evidenceBindings must be an array")
            return
        if len(bindings) > MAX_BINDINGS_PER_MODULE:
            self._err(f"{label}.evidenceBindings exceeds {MAX_BINDINGS_PER_MODULE}")
            return
        seen: dict[str, int] = {}
        for j, binding in enumerate(bindings):
            blabel = f"{label}.evidenceBindings[{j}]"
            if not isinstance(binding, dict):
                self._err(f"{blabel} must be an object")
                continue
            if not _check_keys(binding, REQUIRED_BINDING_KEYS, OPTIONAL_BINDING_KEYS,
                               blabel, self.errors):
                continue
            btype = binding.get("type")
            if btype not in EVIDENCE_PRODUCERS:
                self._err(
                    f"{blabel}.type must be one of {sorted(EVIDENCE_PRODUCERS)}, "
                    f"got {btype!r} — an evidence type with no producer can "
                    f"never be satisfied"
                )
                continue
            assert isinstance(btype, str)
            if btype in seen:
                self._err(
                    f"{blabel}: duplicate {btype!r} binding (also at index "
                    f"{seen[btype]}) — one evidence type, one artifact"
                )
            seen[btype] = j

            pattern = binding.get("artifactPattern")
            if not isinstance(pattern, str):
                self._err(f"{blabel}.artifactPattern must be a string")
                continue
            for err in paths_mod.check_relative_artifact_path(
                pattern, f"{blabel}.artifactPattern"
            ):
                self._err(err)
            producer = EVIDENCE_PRODUCERS[btype]
            if pattern != producer["artifact"]:
                self._err(
                    f"{blabel}.artifactPattern {pattern!r} is not the output of "
                    f"any producer. {btype!r} is written by {producer['producer']} "
                    f"(defined in {producer['definedIn']}, CI job "
                    f"{producer['ciJob']}) to {producer['artifact']!r}. A binding "
                    f"to a path nothing produces is an unsatisfiable claim."
                )

    def _check_owner(self, label: str, mod: dict[str, Any]) -> None:
        owner = mod.get("packageSmokeOwner")
        if owner is None:
            return
        if not _bounded_str(owner, f"{label}.packageSmokeOwner", self.errors,
                            pattern=WORK_ITEM_ID_RE):
            return
        if owner not in self.known_work_items:
            self._err(
                f"{label}.packageSmokeOwner {owner!r} is not a readiness work "
                f"item — package-smoke ownership must name a real, tracked owner"
            )

    # -- whole-manifest invariants ---------------------------------------
    def _check_partition(self) -> None:
        """included and excluded must exactly partition the declared modules."""
        declared = [m.get("name") for m in self._modules()]
        declared_set = {n for n in declared if isinstance(n, str)}
        for i, profile in enumerate(self._profiles()):
            label = f"profiles[{i}]"
            included = profile.get("includedModules")
            excluded = profile.get("excludedModules")
            if not isinstance(included, list) or not isinstance(excluded, list):
                continue
            inc = {n for n in included if isinstance(n, str)}
            exc = {n for n in excluded if isinstance(n, str)}

            if not inc:
                self._err(
                    f"{label}.includedModules is empty — a profile that ships "
                    f"nothing proves nothing, and would pass every module check "
                    f"vacuously"
                )
            overlap = inc & exc
            if overlap:
                self._err(f"{label}: modules both included and excluded: "
                          f"{sorted(overlap)}")
            unclassified = declared_set - inc - exc
            if unclassified:
                self._err(
                    f"{label}: declared modules are neither included nor "
                    f"excluded: {sorted(unclassified)} — every module must be "
                    f"explicitly classified so none can ship unreviewed"
                )
            unknown = (inc | exc) - declared_set
            if unknown:
                self._err(f"{label}: references undeclared modules "
                          f"{sorted(unknown)}")

    def _check_duplicate_identities(self) -> None:
        names: dict[str, int] = {}
        cmake: dict[str, int] = {}
        libs: dict[str, list[int]] = {}
        for i, mod in enumerate(self._modules()):
            name = mod.get("name")
            if isinstance(name, str):
                folded = name.casefold()
                if folded in names:
                    self._err(
                        f"module name {name!r} collides with the module at index "
                        f"{names[folded]} (case-insensitive) — two modules cannot "
                        f"share an identity on a case-insensitive filesystem"
                    )
                names[folded] = i
            target = mod.get("cmakeTarget")
            if isinstance(target, str):
                folded = target.casefold()
                if folded in cmake:
                    self._err(f"cmakeTarget {target!r} collides with the module at "
                              f"index {cmake[folded]} (case-insensitive)")
                cmake[folded] = i
            lib = mod.get("sharedLibrary")
            if isinstance(lib, dict):
                for platform, filename in lib.items():
                    if isinstance(filename, str):
                        libs.setdefault(f"{platform}:{filename.casefold()}",
                                        []).append(i)
        for key, indices in libs.items():
            if len(indices) > 1:
                self._err(
                    f"shared library identity {key!r} is claimed by modules "
                    f"{indices} — on Windows these are the same file"
                )

    def _check_cross_references(self) -> None:
        modules = {m.get("name"): m for m in self._modules()}
        for i, profile in enumerate(self._profiles()):
            pid = profile.get("id")
            if not isinstance(pid, str):
                continue
            for name in profile.get("includedModules", []) or []:
                mod = modules.get(name)
                if not isinstance(mod, dict):
                    continue
                pa = mod.get("profileApplicability", {})
                if isinstance(pa, dict) and pa.get(pid) == "outside":
                    self._err(
                        f"module {name!r} declares itself outside profile {pid!r} "
                        f"but the profile includes it"
                    )
                self._check_included_module_evidence(name, mod, pid, f"profiles[{i}]")
            for name in profile.get("excludedModules", []) or []:
                mod = modules.get(name)
                if not isinstance(mod, dict):
                    continue
                pa = mod.get("profileApplicability", {})
                if isinstance(pa, dict) and pa.get(pid) in ("required", "shared"):
                    self._err(
                        f"module {name!r} declares profileApplicability "
                        f"{pid!r}={pa[pid]!r} but the profile excludes it"
                    )

    def _check_included_module_evidence(self, name: str, mod: dict[str, Any],
                                        pid: str, label: str) -> None:
        """A module a profile ships must publish the full evidence set."""
        bindings = mod.get("evidenceBindings", [])
        present = {
            b.get("type") for b in bindings
            if isinstance(b, dict) and isinstance(b.get("type"), str)
        }
        missing = REQUIRED_INCLUDED_EVIDENCE - present
        if missing:
            self._err(
                f"module {name!r} is included in profile {pid!r} but publishes no "
                f"{sorted(missing)} evidence — a shipped module must prove it "
                f"builds, loads, tests and packages"
            )
        if mod.get("packageSmokeOwner") is None:
            self._err(
                f"module {name!r} is included in profile {pid!r} but declares no "
                f"packageSmokeOwner — shipped modules need a named owner for "
                f"package smoke"
            )

    def _check_experimental_separation(self) -> None:
        declared = {p.get("id") for p in self._profiles() if isinstance(p.get("id"), str)}
        for i, mod in enumerate(self._modules()):
            label = f"modules[{i}]"
            pa = mod.get("profileApplicability", {})
            if not isinstance(pa, dict):
                continue
            outside_all = bool(declared) and all(
                pa.get(pid) == "outside" for pid in declared
            )
            sep = mod.get("experimentalSeparation")
            if sep is None:
                if outside_all:
                    self._err(
                        f"{label}: module is outside every profile but declares no "
                        f"experimentalSeparation — experimental work must name its "
                        f"tracking work item"
                    )
                continue
            if not isinstance(sep, dict):
                self._err(f"{label}.experimentalSeparation must be an object")
                continue
            if not _check_keys(sep, REQUIRED_SEPARATION_KEYS, OPTIONAL_SEPARATION_KEYS,
                               f"{label}.experimentalSeparation", self.errors):
                continue
            tracker = sep.get("trackedUnder")
            if _bounded_str(tracker, f"{label}.experimentalSeparation.trackedUnder",
                            self.errors, pattern=WORK_ITEM_ID_RE):
                assert isinstance(tracker, str)
                if tracker not in self.known_work_items:
                    self._err(
                        f"{label}.experimentalSeparation.trackedUnder {tracker!r} "
                        f"is not a readiness work item — experimental modules must "
                        f"be tracked by a real, owned item"
                    )
            for pid in declared:
                if pa.get(pid) in ("required", "shared"):
                    self._err(
                        f"{label}: declares experimentalSeparation but is "
                        f"{pa[pid]!r} in profile {pid!r} — experimental modules "
                        f"must be outside every stable profile"
                    )

    # -- runtime evidence -------------------------------------------------
    def _included_modules(self) -> list[dict[str, Any]]:
        included: dict[str, dict[str, Any]] = {}
        by_name = {m.get("name"): m for m in self._modules()}
        for profile in self._profiles():
            for name in profile.get("includedModules", []) or []:
                mod = by_name.get(name)
                if isinstance(mod, dict) and isinstance(name, str):
                    included[name] = mod
        return list(included.values())

    def _record_gap(self, evidence_type: str, available: bool, detail: str) -> bool:
        """Apply the declared-gap ledger to one evidence type.

        Returns True when the caller should stop (the absence is an accepted,
        tracked gap).  Raising the absence as a warning rather than an error is
        the *only* softening in this validator, it applies to explicitly
        enumerated types alone, and it is paired with a ratchet: a declared gap
        whose evidence has appeared is itself an error, so the ledger cannot
        outlive the problem it records.
        """
        tracker = self.declared_gaps.get(evidence_type)
        if tracker is None:
            self._err(detail)
            return True
        if available:
            self._err(
                f"{evidence_type!r} is recorded as a known evidence gap tracked "
                f"under {tracker}, but its evidence is now available — remove "
                f"the entry from evidence-gaps.json and close {tracker}"
            )
            return True
        self.warnings.append(
            f"{evidence_type}: accepted as a known gap tracked under {tracker}. "
            f"This evidence is NOT proven; RDY-010 remains release-blocking."
        )
        return True

    def _check_target_evidence(self) -> None:
        if self.policy_only:
            return
        if self.target_index is None:
            self._record_gap(
                "cmake-target-index", False,
                f"CMake target evidence is unavailable"
                f"{': ' + self.target_error if self.target_error else ''}. "
                f"Target existence is proven by a configure-generated File API "
                f"codemodel; without one the manifest's build claims are "
                f"unverified and must not be treated as satisfied.",
            )
            return
        if "cmake-target-index" in self.declared_gaps:
            self._record_gap("cmake-target-index", True, "")
            return
        for mod in self._included_modules():
            name = mod.get("name")
            target = mod.get("cmakeTarget")
            lib = mod.get("sharedLibrary")
            if not (isinstance(name, str) and isinstance(target, str)
                    and isinstance(lib, dict)):
                continue
            for err in targets_mod.check_target(
                self.target_index, target, name, lib, f"module {name!r}"
            ):
                self._err(err)

    # Evidence types verified by a dedicated structural check above; the rest
    # are verified by requiring the producer's artifact to actually be present.
    _STRUCTURALLY_CHECKED = frozenset({"cmake-target-index", "lifecycle-log"})

    def _check_artifact_evidence(self) -> None:
        """Every other declared artifact must actually have been produced."""
        if self.policy_only:
            return
        for mod in self._included_modules():
            name = mod.get("name")
            for binding in mod.get("evidenceBindings", []) or []:
                if not isinstance(binding, dict):
                    continue
                btype = binding.get("type")
                pattern = binding.get("artifactPattern")
                if (btype in self._STRUCTURALLY_CHECKED
                        or btype not in EVIDENCE_PRODUCERS
                        or not isinstance(pattern, str)):
                    continue
                artifact_path = self.repo_root / pattern
                if artifact_path.is_file():
                    if btype in self.declared_gaps:
                        self._record_gap(btype, True, "")
                        continue
                    semantic_errors = artifacts.validate_artifact(
                        artifact_path, btype, name if isinstance(name, str) else ""
                    )
                    for err in semantic_errors:
                        self._err(f"module {name!r}: {err}")
                    continue
                producer = EVIDENCE_PRODUCERS[btype]
                self._record_gap(
                    btype, False,
                    f"module {name!r}: declared {btype} evidence "
                    f"{pattern!r} was not produced. It is written by "
                    f"{producer['producer']} in CI job {producer['ciJob']}; a "
                    f"binding to an artifact that does not exist proves nothing.",
                )

    def _check_lifecycle_evidence(self) -> None:
        if self.policy_only:
            return
        included = self._included_modules()
        if self.lifecycle_evidence is None:
            self._record_gap(
                "lifecycle-log", False,
                f"runtime lifecycle evidence is unavailable"
                f"{': ' + self.lifecycle_error if self.lifecycle_error else ''}. "
                f"Declared lifecycle phases are not proof that a module loads, "
                f"updates, unloads or shuts down; only a recorded ModuleManager "
                f"run against the source under test is.",
            )
            return
        if "lifecycle-log" in self.declared_gaps:
            self._record_gap("lifecycle-log", True, "")
            return
        if self.expected_sha is None:
            self._err(
                "no revision under test was established, so lifecycle evidence "
                "cannot be bound to the source it claims to cover"
            )
            return

        for err in provenance.check_revision_binding(
            self.lifecycle_evidence.get("commitSHA"), self.expected_sha,
            self.repo_root, "lifecycle evidence commitSHA",
        ):
            self._err(err)
        for err in provenance.check_rfc3339(
            self.lifecycle_evidence.get("generatedAt"),
            "lifecycle evidence generatedAt",
        ):
            self._err(err)

        records = {
            r.get("module"): r
            for r in self.lifecycle_evidence.get("records", [])
            if isinstance(r, dict)
        }
        for mod in included:
            name = mod.get("name")
            src = mod.get("sourceDirectory")
            lib = mod.get("sharedLibrary")
            if not (isinstance(name, str) and isinstance(src, str)
                    and isinstance(lib, dict)):
                continue
            record = records.get(name)
            if record is None:
                self._err(
                    f"module {name!r} is shipped by a profile but no lifecycle "
                    f"record exists for it — an unexercised module is unproven"
                )
                continue
            tree_sha, err = lifecycle_mod.source_tree_sha(
                self.repo_root, self.expected_sha, src
            )
            if err:
                self._err(f"module {name!r}: {err}")
                continue
            assert tree_sha is not None
            for msg in lifecycle_mod.check_record(
                record, name, tree_sha, lib, f"module {name!r}"
            ):
                self._err(msg)


# -- loading ---------------------------------------------------------------
def load_manifest(path: Path | str | None = None) -> dict[str, Any]:
    if path is None:
        path = REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
    try:
        document = strict_json.load_file(Path(path))
    except strict_json.StrictJSONError as exc:
        raise ManifestError(str(exc)) from exc
    if not isinstance(document, dict):
        raise ManifestError(f"{path}: manifest must be a JSON object")
    return document


def load_declared_gaps(path: Path, known_work_items: set[str]) -> dict[str, str]:
    """Load the acknowledged evidence-gap ledger.

    A malformed ledger is fatal rather than empty: silently reading zero gaps
    from a broken file would turn every gap back into a hard failure with no
    explanation, and — worse — a ledger that failed open would let anything
    through.
    """
    document = strict_json.load_file(path)
    if not isinstance(document, dict):
        raise ManifestError(f"{path}: gap ledger must be an object")
    if document.get("schemaVersion") != "evidence-gaps-v1":
        raise ManifestError(
            f"{path}: schemaVersion must be 'evidence-gaps-v1', got "
            f"{document.get('schemaVersion')!r}"
        )
    unknown = set(document) - {"schemaVersion", "comment", "gaps"}
    if unknown:
        raise ManifestError(f"{path}: unknown keys {sorted(unknown)}")
    gaps = document.get("gaps")
    if not isinstance(gaps, list):
        raise ManifestError(f"{path}: gaps must be an array")

    declared: dict[str, str] = {}
    for i, gap in enumerate(gaps):
        label = f"{path}: gaps[{i}]"
        if not isinstance(gap, dict):
            raise ManifestError(f"{label} must be an object")
        unknown = set(gap) - {"evidenceType", "trackedUnder", "reason"}
        if unknown:
            raise ManifestError(f"{label}: unknown keys {sorted(unknown)}")
        etype = gap.get("evidenceType")
        if etype not in EVIDENCE_PRODUCERS:
            raise ManifestError(
                f"{label}: evidenceType must be one of "
                f"{sorted(EVIDENCE_PRODUCERS)}, got {etype!r}"
            )
        if etype in declared:
            raise ManifestError(f"{label}: duplicate gap for {etype!r}")
        tracker = gap.get("trackedUnder")
        if not isinstance(tracker, str) or not WORK_ITEM_ID_RE.match(tracker):
            raise ManifestError(f"{label}: trackedUnder must be a work item id")
        if known_work_items and tracker not in known_work_items:
            raise ManifestError(
                f"{label}: trackedUnder {tracker!r} is not a readiness work "
                f"item — an untracked gap has no owner and no end date"
            )
        reason = gap.get("reason")
        if not isinstance(reason, str) or len(reason) < 40:
            raise ManifestError(
                f"{label}: reason must explain, in at least 40 characters, why "
                f"no producer exists"
            )
        declared[etype] = tracker
    return declared


def _resolve_expected_sha(repo_root: Path, explicit: str | None) -> tuple[str | None, str | None]:
    """Establish the revision under test from CI injection or the checkout."""
    candidate = explicit or os.environ.get("GITHUB_SHA") or None
    if candidate:
        errs = provenance.check_sha_shape(candidate, "expected revision")
        if errs:
            return None, errs[0]
        return candidate, None
    return provenance.resolve_head_sha(repo_root)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the module evidence manifest")
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--repo-root", type=Path, default=None)
    parser.add_argument("--target-evidence", type=Path, default=None,
                        help="module-targets.json from collect_targets.py")
    parser.add_argument("--lifecycle-evidence", type=Path, default=None,
                        help="module-lifecycle.json from a real ModuleManager run")
    parser.add_argument("--expected-sha", default=None,
                        help="Revision under test (CI passes GITHUB_SHA)")
    parser.add_argument(
        "--policy-only", action="store_true",
        help="Validate only the declarative layer, skipping runtime evidence. "
             "For local editing. A --policy-only run does NOT satisfy RDY-010 "
             "and must never be used as the release gate.",
    )
    parser.add_argument(
        "--allow-declared-gaps", type=Path, default=None,
        help="Evidence-gap ledger (tools/module-evidence/evidence-gaps.json). "
             "Downgrades exactly the enumerated evidence types to warnings so "
             "blocking CI can enforce everything else. Undeclared gaps still "
             "fail, and a declared gap whose evidence has appeared also fails.",
    )
    args = parser.parse_args()

    repo_root = (args.repo_root or REPO_ROOT).resolve()

    try:
        manifest = load_manifest(args.manifest)
    except ManifestError as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 1

    declared_gaps: dict[str, str] = {}
    if args.allow_declared_gaps:
        known_items, _ = load_known_work_item_ids(repo_root)
        try:
            declared_gaps = load_declared_gaps(args.allow_declared_gaps, known_items)
        except (ManifestError, strict_json.StrictJSONError) as exc:
            print(f"FATAL: unusable evidence-gap ledger: {exc}", file=sys.stderr)
            return 1

    target_index = target_error = None
    lifecycle_evidence = lifecycle_error = None
    if not args.policy_only:
        try:
            path = args.target_evidence or (
                repo_root / EVIDENCE_PRODUCERS["cmake-target-index"]["artifact"]
            )
            target_index = targets_mod.load_target_index(Path(path))
        except targets_mod.TargetEvidenceUnavailable as exc:
            target_error = str(exc)
        try:
            path = args.lifecycle_evidence or (
                repo_root / EVIDENCE_PRODUCERS["lifecycle-log"]["artifact"]
            )
            lifecycle_evidence = lifecycle_mod.load_lifecycle_evidence(Path(path))
        except lifecycle_mod.LifecycleEvidenceUnavailable as exc:
            lifecycle_error = str(exc)

    expected_sha, sha_error = _resolve_expected_sha(repo_root, args.expected_sha)
    if sha_error and not args.policy_only:
        print(f"WARNING: {sha_error}", file=sys.stderr)

    validator = ManifestValidator(
        manifest, repo_root,
        target_index=target_index, target_error=target_error,
        lifecycle_evidence=lifecycle_evidence, lifecycle_error=lifecycle_error,
        expected_sha=expected_sha, policy_only=args.policy_only,
        declared_gaps=declared_gaps,
    )
    errors = validator.validate()

    for warning in validator.warnings:
        print(f"KNOWN GAP: {warning}", file=sys.stderr)

    if errors:
        print(f"FAIL: {len(errors)} validation error(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    if args.policy_only:
        print("POLICY-ONLY: the declarative layer is internally consistent.\n"
              "             Runtime target and lifecycle evidence were NOT checked.\n"
              "             This run does NOT satisfy RDY-010.")
        return 0
    if validator.warnings:
        print(f"INCOMPLETE: the manifest is consistent and every available "
              f"evidence source agrees, but {len(validator.warnings)} required "
              f"evidence type(s) have no producer yet.\n"
              f"            RDY-010 remains open and release-blocking.")
        return 0
    print("OK: module evidence manifest is valid and backed by runtime evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
