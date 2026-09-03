#!/usr/bin/env python3
"""Fail-closed parity checks for SparkEngine build configuration surfaces.

Exit codes are part of the CI contract:
  0: no blocking findings
  1: reviewed, current configuration findings remain
  2: the reviewed baseline is malformed or has drifted
  3: the checker or an authoritative input failed internally

JSON is the only stdout payload. Human diagnostics are written to stderr.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import inventory as inventory_tool


EXIT_FINDINGS = 1
EXIT_BASELINE_DRIFT = 2
EXIT_INTERNAL = 3


@dataclass(frozen=True)
class Finding:
    category: str
    severity: str
    message: str
    detail: str = ""

    def to_dict(self) -> dict[str, str]:
        result = {
            "category": self.category,
            "severity": self.severity,
            "message": self.message,
        }
        if self.detail:
            result["detail"] = self.detail
        return result


def _exception_map(entries: Iterable[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {str(entry.get("name")): entry for entry in entries}


def _active_options(options: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Options that are part of *this* context's configuration surface.

    An option whose only declaration sits in a branch the Windows/MSVC context
    never takes is not a Windows configuration knob at all, so comparing it
    against SparkBuild manufactures a blocker out of a option that does not exist
    here. Options the evaluator could not decide are handled separately and
    loudly by ``check_option_resolution``.
    """
    return [entry for entry in options if entry.get("status", "active") == "active"]


def check_option_resolution(options: Iterable[dict[str, Any]]) -> list[Finding]:
    """Undecidable or doubly-active option declarations must be raised, not guessed."""
    findings: list[Finding] = []
    for entry in options:
        status = entry.get("status", "active")
        if status == "indeterminate":
            findings.append(
                Finding(
                    "option-condition-indeterminate",
                    "error",
                    f"CMake option '{entry['name']}' sits behind a condition this inventory cannot decide",
                    f"Declared in {', '.join(entry.get('declaredIn', []))}. "
                    "An undecidable guard is reported rather than assumed active or inactive.",
                )
            )
        elif status == "ambiguous":
            findings.append(
                Finding(
                    "option-ambiguous-declaration",
                    "error",
                    f"CMake option '{entry['name']}' has {entry.get('activeDeclarationCount')} simultaneously active declarations",
                    f"Declared in {', '.join(entry.get('declaredIn', []))}.",
                )
            )
    return findings


def _option_severity(name: str, entries: Iterable[dict[str, Any]]) -> tuple[str, str]:
    exception = _exception_map(entries).get(name)
    if exception and exception.get("applicability") in {"outside", "shared"}:
        return "warning", (
            f"Explicitly classified {exception['applicability']}: {exception.get('reason', 'no reason')}"
        )
    return "error", "No outside/shared exception exists in the stable-v1 profile."


def _product_severity(product: dict[str, Any]) -> str:
    """Canonical products block whether they are required or shared.

    The readiness support contract sets a profile's blocker set to its
    required *and* shared work; a shared build product is supported surface, not
    an optional extra. Downgrading a missing shared product to a warning would
    let a report say "clean" while omitting a product the profile ships.
    ``outside`` is the only non-blocking classification, and no product may
    declare it.
    """
    return "warning" if product.get("applicability") == "outside" else "error"


def _as_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    upper = str(value).upper()
    if upper in {"1", "ON", "YES", "TRUE", "Y"}:
        return True
    if upper in {"0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", ""}:
        return False
    return None


def check_sparkbuild_vs_cmake(
    cmake_options: list[dict[str, Any]],
    sparkbuild_options: list[dict[str, Any]],
    option_applicability: Iterable[dict[str, Any]] = (),
) -> list[Finding]:
    findings: list[Finding] = []
    cmake_names = {entry["name"] for entry in cmake_options}
    sparkbuild_names = {entry["name"] for entry in sparkbuild_options}
    for name in sorted(sparkbuild_names - cmake_names):
        severity, scope_detail = _option_severity(name, option_applicability)
        findings.append(
            Finding(
                "sparkbuild-orphan",
                severity,
                f"SparkBuild exposes '{name}' but root CMake has no option() declaration",
                "SparkBuild would emit a cache variable that the authoritative root does not consume. "
                + scope_detail,
            )
        )
    for name in sorted(cmake_names - sparkbuild_names):
        severity, scope_detail = _option_severity(name, option_applicability)
        findings.append(
            Finding(
                "cmake-only",
                severity,
                f"CMake option '{name}' is not representable in SparkBuild",
                "The supported configurator cannot reproduce the root configuration surface. " + scope_detail,
            )
        )
    return findings


def check_sparkbuild_defaults(
    cmake_options: list[dict[str, Any]],
    sparkbuild_options: list[dict[str, Any]],
    option_applicability: Iterable[dict[str, Any]] = (),
) -> list[Finding]:
    findings: list[Finding] = []
    cmake = {entry["name"]: entry for entry in cmake_options}
    sparkbuild = {entry["name"]: entry for entry in sparkbuild_options}
    for name in sorted(cmake.keys() & sparkbuild.keys()):
        left = _as_bool(cmake[name].get("default"))
        right = _as_bool(sparkbuild[name].get("default"))
        severity, scope_detail = _option_severity(name, option_applicability)
        if left is None:
            findings.append(
                Finding(
                    "unresolved-cmake-default",
                    severity,
                    f"CMake default for '{name}' cannot be resolved for Windows stable-v1",
                    scope_detail,
                )
            )
        elif right is None:
            findings.append(
                Finding(
                    "unresolved-sparkbuild-default",
                    severity,
                    f"SparkBuild default for '{name}' is not Boolean",
                    scope_detail,
                )
            )
        elif left != right:
            findings.append(
                Finding(
                    "default-mismatch",
                    severity,
                    f"Default mismatch for '{name}': CMake={cmake[name].get('default')}, "
                    f"SparkBuild={sparkbuild[name].get('default')}",
                    "The selected configuration depends on which supported surface the user invokes. "
                    + scope_detail,
                )
            )
    return findings


def check_duplicate_options(declarations: list[dict[str, Any]]) -> list[Finding]:
    """Allow only structurally exclusive conditional declarations."""
    findings: list[Finding] = []
    groups: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for declaration in declarations:
        groups[declaration["name"]].append(declaration)
    for name, group in sorted(groups.items()):
        if len(group) < 2:
            continue
        conflicts: list[tuple[dict[str, Any], dict[str, Any]]] = []
        for index, left in enumerate(group):
            for right in group[index + 1 :]:
                if not inventory_tool.declarations_are_mutually_exclusive(left, right):
                    conflicts.append((left, right))
        if conflicts:
            locations = sorted({f"{item['file']}:{item['line']}" for pair in conflicts for item in pair})
            findings.append(
                Finding(
                    "duplicate-option",
                    "error",
                    f"CMake option '{name}' has overlapping declarations",
                    "Conflicting declarations: " + ", ".join(locations),
                )
            )
    return findings


def check_preset_workflow_parity(
    presets: dict[str, Any], workflow_references: list[str]
) -> list[Finding]:
    known = {entry["name"] for entry in presets.get("configurePresets", [])}
    return [
        Finding(
            "workflow-phantom-preset",
            "error",
            f"CI workflow references --preset '{reference}' which does not exist",
        )
        for reference in sorted(set(workflow_references) - known)
    ]


def check_preset_binary_dirs(presets: dict[str, Any]) -> list[Finding]:
    """Every non-hidden configure preset needs a build preset, a resolvable
    binaryDir, and a build directory it does not share with another preset."""
    findings: list[Finding] = []
    build_profiles = {
        entry["configurePreset"] for entry in presets.get("buildPresets", []) if entry.get("configurePreset")
    }
    for entry in presets.get("configurePresets", []):
        if entry.get("hidden"):
            continue
        if entry["name"] not in build_profiles:
            findings.append(
                Finding(
                    "orphan-configure-preset",
                    "warning",
                    f"Configure preset '{entry['name']}' has no corresponding build preset",
                )
            )

    directories: dict[str, list[str]] = defaultdict(list)
    for entry in presets.get("configurePresets", []):
        if entry.get("hidden"):
            continue
        try:
            resolved = inventory_tool.resolve_configure_preset(presets, entry["name"])
        except inventory_tool.InventoryError as error:
            findings.append(
                Finding(
                    "preset-unresolvable",
                    "error",
                    f"Configure preset '{entry['name']}' cannot be resolved",
                    str(error),
                )
            )
            continue
        binary_dir = resolved.get("resolvedBinaryDir")
        if not binary_dir:
            findings.append(
                Finding(
                    "preset-missing-binary-dir",
                    "error",
                    f"Configure preset '{entry['name']}' resolves to no binaryDir",
                    "Codemodel evidence is addressed by build directory; without one it cannot be bound.",
                )
            )
            continue
        directories[binary_dir].append(entry["name"])
    for directory, owners in sorted(directories.items()):
        if len(owners) > 1:
            findings.append(
                Finding(
                    "preset-binary-dir-collision",
                    "error",
                    f"Configure presets {sorted(owners)} all resolve to build directory '{directory}'",
                    "Two configurations sharing one build directory make codemodel evidence ambiguous.",
                )
            )
    return findings


def check_dependent_preset_linkage(presets: dict[str, Any]) -> list[Finding]:
    """Build and test presets must bind to a real, usable configure preset.

    Name existence is not linkage: a build preset pointing at a missing preset,
    or at a hidden one that need not carry a binaryDir, cannot actually build.
    """
    findings: list[Finding] = []
    configure_by_name = {entry["name"]: entry for entry in presets.get("configurePresets", [])}
    for kind in ("buildPresets", "testPresets", "packagePresets"):
        for entry in presets.get(kind, []):
            try:
                resolved = inventory_tool.resolve_dependent_preset(presets, kind, entry["name"])
            except inventory_tool.InventoryError as error:
                findings.append(
                    Finding(
                        "dependent-preset-unresolvable",
                        "error",
                        f"{kind[:-7]} preset '{entry['name']}' cannot be resolved",
                        str(error),
                    )
                )
                continue
            target = resolved["configurePreset"]
            configure = configure_by_name.get(target)
            if configure is None:
                findings.append(
                    Finding(
                        "dependent-preset-phantom-configure",
                        "error",
                        f"{kind[:-7]} preset '{entry['name']}' names configure preset '{target}', which does not exist",
                    )
                )
                continue
            if configure.get("hidden"):
                findings.append(
                    Finding(
                        "dependent-preset-hidden-configure",
                        "error",
                        f"{kind[:-7]} preset '{entry['name']}' binds to hidden configure preset '{target}'",
                        "A hidden preset need not define a binaryDir, so the build directory is not guaranteed.",
                    )
                )
    return findings


def check_shipping_preset_options(
    presets: dict[str, Any], _cmake_options: list[dict[str, Any]] | None = None
) -> list[Finding]:
    findings: list[Finding] = []
    try:
        shipping = inventory_tool.resolve_configure_preset(presets, "windows-shipping")
    except inventory_tool.InventoryError:
        return [
            Finding(
                "missing-shipping-preset",
                "error",
                "Required inherited configure preset 'windows-shipping' does not exist",
            )
        ]
    cache = shipping.get("cacheVariables", {})
    for name, expected, detail in (
        ("SPARK_STRICT_DEPS", "ON", "Stable-v1 must fail on a missing critical dependency."),
        ("SPARK_NATIVE_ARCH", "OFF", "Distributed binaries cannot inherit the build host CPU."),
    ):
        if str(cache.get(name, "")).upper() != expected:
            findings.append(
                Finding(
                    "shipping-preset-option",
                    "error",
                    f"Resolved windows-shipping preset requires {name}={expected}, got {cache.get(name)!r}",
                    detail,
                )
            )
    return findings


def check_profile_presets(data: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    for config in data["profile"].get("buildConfigurations", []):
        preset_name = config.get("preset")
        if not preset_name:
            continue
        try:
            inventory_tool.resolve_configure_preset(data["cmakePresets"], preset_name)
        except inventory_tool.InventoryError as error:
            findings.append(
                Finding(
                    "profile-preset-invalid",
                    "error",
                    f"Canonical build profile '{config['id']}' cannot resolve preset '{preset_name}'",
                    str(error),
                )
            )
    return findings


def _effective_option_map(data: dict[str, Any]) -> dict[str, Any]:
    return {
        entry["name"]: entry.get("default")
        for entry in data.get("cmakeOptions", [])
        if entry.get("status", "active") == "active"
    }


def _profile_config_map(profile: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {entry["id"]: entry for entry in profile.get("buildConfigurations", [])}


def check_profile_contract(data: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    profile = data.get("profile", {})
    products = profile.get("buildProducts", [])
    if data.get("stableV1Products") != products:
        findings.append(
            Finding(
                "inventory-profile-drift",
                "error",
                "Inventory product projection differs from the canonical stable-v1 profile",
            )
        )
    included = set(profile.get("includedCapabilityIds", []))
    covered = {value for product in products for value in product.get("capabilityIds", [])}
    if covered != included:
        findings.append(
            Finding(
                "profile-product-capability-drift",
                "error",
                "Build-product capabilities do not exactly equal stable-v1 included capabilities",
                f"missing={sorted(included - covered)} outside={sorted(covered - included)}",
            )
        )
    if not any(
        product.get("target") == "SparkGameFPS" and "modules.fps" in product.get("capabilityIds", [])
        for product in products
    ):
        findings.append(
            Finding(
                "missing-first-party-product",
                "error",
                "Canonical stable-v1 products omit SparkGameFPS as the first-party FPS slice",
            )
        )
    configs = _profile_config_map(profile)
    consumer_profiles = {
        identifier for identifier, entry in configs.items() if entry.get("purpose") == "installed-sdk-consumer"
    }
    if not any(product.get("buildProfile") in consumer_profiles for product in products):
        findings.append(
            Finding(
                "missing-installed-sdk-consumer",
                "error",
                "Canonical stable-v1 products omit an installed public-SDK consumer",
            )
        )
    return findings


def check_stable_v1_targets(
    targets: list[dict[str, Any]], products: list[dict[str, Any]]
) -> list[Finding]:
    findings: list[Finding] = []
    known = {entry["target"]: entry for entry in targets if entry.get("buildable", True)}
    for product in products:
        target = product["target"]
        severity = _product_severity(product)
        declaration = known.get(target)
        if declaration is None:
            findings.append(
                Finding(
                    "missing-target",
                    severity,
                    f"stable-v1 product '{target}' has no CMake target declaration",
                    f"Expected {product['kind']} in build profile {product['buildProfile']}.",
                )
            )
        elif declaration.get("kind") != product.get("kind"):
            findings.append(
                Finding(
                    "target-kind-mismatch",
                    severity,
                    f"stable-v1 product '{target}' expects {product['kind']}, "
                    f"source declares {declaration.get('kind')}",
                    f"Build profile: {product['buildProfile']}.",
                )
            )
    return findings


def check_target_declaration_resolution(declarations: list[dict[str, Any]]) -> list[Finding]:
    """A target name this scanner cannot resolve must be raised, never dropped.

    Silently skipping ``add_library(${TARGET_NAME} ...)`` makes an unresolvable
    declaration indistinguishable from no declaration at all.
    """
    findings: list[Finding] = []
    unresolved = sorted(
        {
            (entry["target"], entry["file"], entry["line"])
            for entry in declarations
            if not entry.get("resolved", True) and entry.get("origin") != "non-build"
        }
    )
    for target, file, line in unresolved:
        findings.append(
            Finding(
                "target-name-unresolved",
                "warning" if "${" in target else "error",
                f"Target name '{target}' at {file}:{line} cannot be resolved statically",
                "Recorded as an explicit unknown so it is not mistaken for an absent declaration.",
            )
        )
    return findings


def check_product_preset_activation(data: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    profile = data["profile"]
    configs = _profile_config_map(profile)
    presets = data["cmakePresets"]
    defaults = _effective_option_map(data)
    resolved: dict[str, dict[str, Any] | None] = {}
    for identifier, config in configs.items():
        preset_name = config.get("preset")
        if not preset_name:
            resolved[identifier] = None
            continue
        try:
            resolved[identifier] = inventory_tool.resolve_configure_preset(presets, preset_name)
        except inventory_tool.InventoryError:
            resolved[identifier] = None
    for product in profile.get("buildProducts", []):
        preset = resolved.get(product["buildProfile"])
        if preset is None:
            continue
        cache = preset.get("cacheVariables", {})
        for name, expected in sorted(product.get("requiredOptions", {}).items()):
            if name not in defaults:
                findings.append(
                    Finding(
                        "profile-product-option-undeclared",
                        _product_severity(product),
                        f"Target '{product['target']}' requires undeclared root CMake option '{name}'",
                        f"Build profile: {product['buildProfile']}.",
                    )
                )
                continue
            actual = cache[name] if name in cache else defaults.get(name)
            actual_bool = _as_bool(actual)
            expected_bool = _as_bool(expected)
            if actual_bool is None or expected_bool is None or actual_bool != expected_bool:
                findings.append(
                    Finding(
                        "profile-product-option",
                        _product_severity(product),
                        f"Preset '{preset['name']}' disables or cannot prove target '{product['target']}': "
                        f"requires {name}={expected}, resolved {actual!r}",
                        "A declared product must be enabled by its own canonical build profile.",
                    )
                )
    return findings


def check_configured_targets(data: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    profile = data["profile"]
    supported_hosts = profile.get("supportedHosts", [])
    windows_only = bool(supported_hosts) and all(
        "windows" in str(host).lower() for host in supported_hosts
    )
    windows_suffixes = {
        "executable": ".exe",
        "static_library": ".lib",
        "shared_library": ".dll",
        "module_library": ".dll",
    }
    products_by_profile: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for product in profile.get("buildProducts", []):
        products_by_profile[product["buildProfile"]].append(product)
    evidence_entries: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for entry in data.get("configuredTargetEvidence", []):
        evidence_entries[str(entry.get("profile"))].append(entry)
    for config in profile.get("buildConfigurations", []):
        identifier = config["id"]
        evidence_group = evidence_entries.get(identifier, [])
        if len(evidence_group) != 1:
            findings.append(
                Finding(
                    "configured-evidence-cardinality",
                    "error",
                    f"Build profile '{identifier}' has {len(evidence_group)} configured codemodel evidence records",
                    "Exactly one explicit available/absent record is required.",
                )
            )
            continue
        evidence = evidence_group[0]
        products = products_by_profile.get(identifier, [])
        severity = "warning" if all(item.get("applicability") == "outside" for item in products) else "error"
        if evidence.get("status") == "absent":
            findings.append(
                Finding(
                    "configured-evidence-absent",
                    severity,
                    f"Build profile '{identifier}' has no CMake File API codemodel evidence",
                    "Source declarations cannot prove that option/dependency evaluation produced the target.",
                )
            )
            continue
        if evidence.get("status") != "available":
            findings.append(
                Finding(
                    "configured-evidence-invalid",
                    "error",
                    f"Build profile '{identifier}' has invalid configured evidence status {evidence.get('status')!r}",
                )
            )
            continue
        configured: dict[tuple[str, str], dict[str, Any]] = {}
        duplicate_keys: set[tuple[str, str]] = set()
        artifact_owners: dict[tuple[str, str], tuple[str, str, str]] = {}
        reported_aliases: set[tuple[str, str]] = set()
        for entry in evidence.get("targets", []):
            key = (str(entry.get("target")), str(entry.get("configuration", "")))
            if key in configured:
                duplicate_keys.add(key)
            configured[key] = entry
            owner = (
                str(entry.get("id", "")),
                str(entry.get("configuration", "")),
                str(entry.get("target", "")),
            )
            artifacts = entry.get("artifacts")
            if not isinstance(artifacts, list):
                continue
            for artifact in artifacts:
                if not isinstance(artifact, str) or not artifact:
                    continue
                ownership_key = (
                    owner[1],
                    inventory_tool._windows_artifact_key(artifact),
                )
                previous_owner = artifact_owners.get(ownership_key)
                if previous_owner is not None and ownership_key not in reported_aliases:
                    if previous_owner != owner:
                        message = (
                            f"Configured targets '{previous_owner[2]}' and '{owner[2]}' "
                            "claim the same artifact path"
                        )
                    else:
                        message = (
                            f"Configured target '{owner[2]}' repeats the same artifact path"
                        )
                    findings.append(
                        Finding(
                            "configured-target-artifact-alias",
                            "error",
                            message,
                            artifact,
                        )
                    )
                    reported_aliases.add(ownership_key)
                artifact_owners[ownership_key] = owner
        for target, configuration in sorted(duplicate_keys):
            findings.append(
                Finding(
                    "configured-target-duplicate",
                    "error",
                    f"Configured profile '{identifier}' repeats target '{target}' for '{configuration}'",
                )
            )
        expected_configuration = str(config.get("configuration", ""))
        for product in products:
            entry = configured.get((product["target"], expected_configuration))
            if entry is None:
                findings.append(
                    Finding(
                        "configured-target-missing",
                        _product_severity(product),
                        f"Configured profile '{identifier}' omits target '{product['target']}' "
                        f"for configuration '{expected_configuration}'",
                    )
                )
            elif entry.get("kind") != product.get("kind"):
                findings.append(
                    Finding(
                        "configured-target-kind-mismatch",
                        _product_severity(product),
                        f"Configured target '{product['target']}' is {entry.get('kind')}, expected {product['kind']}",
                        f"Build profile: {identifier}.",
                    )
                )
            elif product.get("kind") in {
                "executable", "static_library", "shared_library", "module_library"
            }:
                identity_missing = [
                    field
                    for field in ("id", "nameOnDisk", "artifacts", "artifactState")
                    if not entry.get(field)
                ]
                artifacts = entry.get("artifacts")
                if not isinstance(artifacts, list) or not artifacts:
                    identity_missing.append("artifacts")
                if identity_missing:
                    findings.append(
                        Finding(
                            "configured-target-identity-incomplete",
                            _product_severity(product),
                            f"Configured target '{product['target']}' lacks complete File API identity",
                            f"Missing or empty: {sorted(set(identity_missing))}.",
                        )
                    )
                else:
                    artifact_state = entry.get("artifactState")
                    artifact_problem = ""
                    if artifact_state not in {
                        "declared-not-built",
                        "locally-observed-post-build",
                        "externally-attested-post-build",
                    }:
                        artifact_problem = f"unsupported artifactState {artifact_state!r}"
                    expected_suffix = windows_suffixes.get(str(product.get("kind")))
                    if (
                        not artifact_problem
                        and windows_only
                        and expected_suffix
                        and not str(entry.get("nameOnDisk", "")).casefold().endswith(expected_suffix)
                    ):
                        artifact_problem = (
                            f"nameOnDisk is inconsistent with Windows {product.get('kind')}"
                        )
                    elif not any(
                        Path(str(path)).name.casefold()
                        == str(entry.get("nameOnDisk", "")).casefold()
                        for path in artifacts
                    ):
                        artifact_problem = "no artifact matches nameOnDisk"
                    build_directory = str(evidence.get("buildDirectory", ""))
                    for artifact in artifacts:
                        if not isinstance(artifact, str) or not artifact or not Path(artifact).is_absolute():
                            artifact_problem = "artifact paths must be absolute"
                            break
                        try:
                            common = os.path.commonpath((build_directory, artifact))
                        except ValueError:
                            artifact_problem = "artifact is on another volume from its build tree"
                            break
                        if os.path.normcase(common) != os.path.normcase(build_directory):
                            artifact_problem = "artifact escapes its build tree"
                            break
                    if artifact_state in {
                        "locally-observed-post-build",
                        "externally-attested-post-build",
                    }:
                        identities = entry.get("artifactIdentities")
                        identity_paths = (
                            [identity.get("path") for identity in identities]
                            if isinstance(identities, list)
                            and all(isinstance(identity, dict) for identity in identities)
                            else []
                        )
                        if (
                            not isinstance(identities, list)
                            or len(identities) != len(artifacts)
                            or not all(isinstance(path, str) and path for path in artifacts)
                            or not all(isinstance(path, str) and path for path in identity_paths)
                            or len(set(artifacts)) != len(artifacts)
                            or len(set(identity_paths)) != len(identity_paths)
                            or set(identity_paths) != set(artifacts)
                        ):
                            artifact_problem = "post-build artifact claim lacks one identity per artifact"
                        elif any(
                            not isinstance(identity, dict)
                            or set(identity) != {"path", "bytes", "sha256"}
                            or identity.get("path") not in artifacts
                            or type(identity.get("bytes")) is not int
                            or identity.get("bytes") < 0
                            or not re.fullmatch(r"[0-9a-f]{64}", str(identity.get("sha256", "")))
                            for identity in identities
                        ):
                            artifact_problem = "post-build artifact identity is malformed"
                    if artifact_problem:
                        findings.append(
                            Finding(
                                "configured-target-artifact-mismatch",
                                _product_severity(product),
                                f"Configured target '{product['target']}' artifact evidence is invalid",
                                artifact_problem,
                            )
                        )
    unknown = sorted(set(evidence_entries) - set(_profile_config_map(profile)))
    for identifier in unknown:
        findings.append(
            Finding(
                "configured-evidence-unknown-profile",
                "error",
                f"Configured codemodel evidence references unknown profile '{identifier}'",
            )
        )
    return findings


def check_workflow_adoption(data: dict[str, Any]) -> list[Finding]:
    findings: list[Finding] = []
    configs = data.get("workflowCmakeConfigs", [])
    presets = data.get("cmakePresets", {})
    canonical = data["profile"].get("buildConfigurations", [])
    canonical_by_id = {entry.get("id"): entry for entry in canonical}
    producer_profiles = [
        entry.get("profile")
        for entry in (data.get("workflow") or {}).get("buildMatrixInvocations", [])
        if entry.get("kind") == "producer" and entry.get("executable") and entry.get("build")
    ]
    producer_presets = [
        canonical_by_id[profile].get("preset")
        for profile in producer_profiles
        if profile in canonical_by_id and canonical_by_id[profile].get("preset")
    ]
    workflow_refs = [entry.get("preset") for entry in configs if entry.get("preset")] + producer_presets
    findings.extend(check_preset_workflow_parity(presets, workflow_refs))
    for config in canonical:
        preset = config.get("preset")
        if preset and not any(entry.get("preset") == preset for entry in configs):
            findings.append(
                Finding(
                    "profile-preset-not-in-workflow",
                    "error",
                    f"No CI configure command uses canonical preset '{preset}' for '{config['id']}'",
                    "Inline flags do not prove adoption of the reviewed inherited preset.",
                )
            )
    shipping = next((entry for entry in canonical if entry.get("purpose") == "shipping"), None)
    if shipping and not any(entry.get("preset") == shipping.get("preset") for entry in configs):
        findings.append(
            Finding(
                "workflow-missing-shipping-lane",
                "error",
                "CI has no configure invocation for the canonical Windows Shipping profile",
            )
        )
    if configs and not workflow_refs:
        findings.append(
            Finding(
                "workflow-no-presets",
                "error",
                f"CI contains {len(configs)} configure invocations but none uses a CMake preset",
                "Every configure invocation is retained with its owning job and step in the inventory.",
            )
        )
    return findings


def _canonical_configs(data: dict[str, Any]) -> list[dict[str, Any]]:
    return data["profile"].get("buildConfigurations", [])


def _required_runner_os(profile: dict[str, Any]) -> set[str]:
    """Runner OS classes the profile's own supportedHosts admit.

    stable-v1 supports Windows only, so a Linux lane building the same
    configuration name is not evidence for it however green it is.
    """
    classes: set[str] = set()
    for host in profile.get("supportedHosts", []):
        lowered = str(host).lower()
        if "windows" in lowered:
            classes.add("windows")
        elif "macos" in lowered or "darwin" in lowered:
            classes.add("macos")
        elif "linux" in lowered or "ubuntu" in lowered:
            classes.add("linux")
    return classes


def check_workflow_semantics(data: dict[str, Any]) -> list[Finding]:
    """Bind the workflow facts that decide whether CI actually proves anything.

    A configure command line is identical whether it runs on Windows or Ubuntu,
    for one configuration or two, in a required job or one that is
    ``if: false``, ``continue-on-error``, or filtered out by ``paths-ignore``.
    These checks make each of those a finding rather than a silence.
    """
    findings: list[Finding] = []
    workflow = data.get("workflow") or {}

    for event in workflow.get("events", []):
        if event.get("pathFiltered") and event.get("event") in {"push", "pull_request"}:
            filters = {
                key: value
                for key, value in (event.get("filters") or {}).items()
                if key in {"paths", "paths-ignore"}
            }
            findings.append(
                Finding(
                    "workflow-path-filter",
                    "error",
                    f"Build workflow '{event['event']}' trigger is path-filtered",
                    f"{filters}. A path filter can skip the entire build for a change set, "
                    "so a green required check would prove nothing about it.",
                )
            )

    def branch_filter_matches(pattern: Any, branch: str) -> bool:
        return isinstance(pattern, str) and fnmatch.fnmatchcase(branch, pattern)

    def reaches_working(event: dict[str, Any]) -> bool:
        if event.get("event") not in {"push", "pull_request", "merge_group"}:
            return False
        filters = event.get("filters") or {}
        if event.get("event") == "push" and "tags" in filters and "branches" not in filters:
            return False
        branches = filters.get("branches") or []
        if branches:
            positives = [pattern for pattern in branches if not str(pattern).startswith("!")]
            negatives = [str(pattern)[1:] for pattern in branches if str(pattern).startswith("!")]
            if not any(branch_filter_matches(pattern, "Working") for pattern in positives):
                return False
            if any(branch_filter_matches(pattern, "Working") for pattern in negatives):
                return False
        ignored = filters.get("branches-ignore") or []
        if any(branch_filter_matches(pattern, "Working") for pattern in ignored):
            return False
        event_types = filters.get("types") or []
        if event.get("event") == "pull_request" and event_types and "synchronize" not in event_types:
            return False
        if event.get("event") == "merge_group" and event_types and "checks_requested" not in event_types:
            return False
        return True

    if not any(reaches_working(event) for event in workflow.get("events", [])):
        findings.append(
            Finding(
                "workflow-working-branch-unreachable",
                "error",
                "Build workflow cannot run automatically for the protected Working branch",
                "A manually dispatched or differently filtered workflow cannot serve as its required CI gate.",
            )
        )

    for job in workflow.get("jobs", []):
        if job.get("matrixResolved") is not True:
            findings.append(
                Finding(
                    "workflow-matrix-unresolved",
                    "error",
                    f"CI job '{job.get('id')}' has a matrix this inventory cannot expand",
                    "An unresolved matrix may produce no matching leg, so its configure/build commands are not evidence.",
                )
            )

    for entry in workflow.get("unresolvedInvocations", []):
        findings.append(
            Finding(
                "workflow-unresolved-invocation",
                "error",
                f"{entry.get('job')}/{entry.get('step')}: {entry.get('reason')}",
                f"command: {entry.get('command') or entry.get('commandWord')}",
            )
        )

    configures = workflow.get("configureInvocations", [])
    builds = workflow.get("buildInvocations", [])
    presets = data.get("cmakePresets", {})

    def mandatory(entry: dict[str, Any]) -> bool:
        """True only when both the job and step must execute and propagate failure."""
        return (
            entry.get("gating") == "blocking"
            and entry.get("jobIf") == "absent"
            and entry.get("jobContinueOnError") == "absent"
            and entry.get("stepIf") == "absent"
            and entry.get("stepContinueOnError") == "absent"
            and entry.get("matrixResolved") is True
        )

    def directory_key(value: Any) -> str:
        if not isinstance(value, str) or not value:
            return ""
        normalized = value.replace("\\", "/").rstrip("/")
        normalized = normalized.replace("${sourceDir}/", "").removeprefix("./")
        root = str(data.get("repository", {}).get("root", "")).replace("\\", "/").rstrip("/")
        if root and normalized.casefold().startswith(root.casefold() + "/"):
            normalized = normalized[len(root) + 1 :]
        return normalized.casefold()

    def configured_directory(entry: dict[str, Any]) -> str:
        if entry.get("buildDir"):
            return directory_key(entry.get("buildDir"))
        preset_name = entry.get("preset")
        if not preset_name:
            return ""
        try:
            resolved = inventory_tool.resolve_configure_preset(presets, str(preset_name))
        except inventory_tool.InventoryError:
            return ""
        return directory_key(resolved.get("resolvedBinaryDir"))

    def build_binding(entry: dict[str, Any]) -> tuple[str, str, str]:
        """Return configure preset, build directory and configuration for a build."""
        configure_preset = ""
        build_directory = directory_key(entry.get("buildDir"))
        configuration = str(entry.get("configuration", ""))
        build_preset = entry.get("preset")
        if build_preset:
            try:
                resolved_build = inventory_tool.resolve_dependent_preset(
                    presets, "buildPresets", str(build_preset)
                )
                configure_preset = str(resolved_build.get("configurePreset", ""))
                configuration = configuration or str(resolved_build.get("configuration", ""))
                resolved_configure = inventory_tool.resolve_configure_preset(
                    presets, configure_preset
                )
                build_directory = directory_key(resolved_configure.get("resolvedBinaryDir"))
            except inventory_tool.InventoryError:
                return "", "", configuration
        return configure_preset, build_directory, configuration

    def same_expansion(left: dict[str, Any], right: dict[str, Any]) -> bool:
        return (
            left.get("job") == right.get("job")
            and left.get("matrix", {}) == right.get("matrix", {})
            and left.get("runnerOs") == right.get("runnerOs")
        )

    def occurs_after(build: dict[str, Any], configure: dict[str, Any]) -> bool:
        return (
            int(build.get("stepIndex", -1)),
            int(build.get("commandIndex", 0)),
        ) > (
            int(configure.get("stepIndex", -1)),
            int(configure.get("commandIndex", 0)),
        )

    products_by_profile: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for product in data["profile"].get("buildProducts", []):
        products_by_profile[product["buildProfile"]].append(product)

    for config in _canonical_configs(data):
        identifier = config["id"]
        preset = config.get("preset")
        configuration = str(config.get("configuration", ""))
        severity = max(
            (_product_severity(product) for product in products_by_profile.get(identifier, [])),
            key=lambda value: 0 if value == "warning" else 1,
            default="error",
        )
        declared_dir = directory_key(config.get("buildDirectory"))
        declared_source = directory_key(config.get("sourceDirectory"))
        matching = [
            entry
            for entry in configures
            if (
                (preset and entry.get("preset") == preset)
                or (
                    not preset
                    and declared_dir
                    and configured_directory(entry) == declared_dir
                    and (
                        not declared_source
                        or directory_key(entry.get("sourceDir")) == declared_source
                    )
                )
            )
        ]
        blocking = [entry for entry in matching if mandatory(entry)]
        if preset and matching and not blocking:
            findings.append(
                Finding(
                    "workflow-lane-not-blocking",
                    severity,
                    f"Every CI configure of canonical preset '{preset}' is advisory or conditionally skipped",
                    "A lane that cannot fail the run does not evidence the profile.",
                )
            )
        supported = _required_runner_os(data["profile"])
        supported_builds: list[dict[str, Any]] = []
        for build in builds:
            if not mandatory(build) or (supported and build.get("runnerOs") not in supported):
                continue
            build_preset, build_directory, built_configuration = build_binding(build)
            if built_configuration != configuration:
                continue
            for configure in blocking:
                if not same_expansion(build, configure) or not occurs_after(build, configure):
                    continue
                if build_preset and build_preset != configure.get("preset"):
                    continue
                configure_directory = configured_directory(configure)
                if not configure_directory or build_directory != configure_directory:
                    continue
                supported_builds.append(build)
                break
        if configuration and not supported_builds:
            findings.append(
                Finding(
                    "workflow-configuration-not-built",
                    severity,
                    f"No blocking CI build on {sorted(supported) or 'any host'} produces configuration "
                    f"'{configuration}' for profile '{identifier}'",
                    "A supported build must follow its live configure in the same job/matrix leg, "
                    "use the same build tree, and propagate failure.",
                )
            )
            continue
        expected = {product["target"] for product in products_by_profile.get(identifier, [])}
        covered = any(
            entry.get("buildsAllTargets") or expected.issubset(set(entry.get("targets", [])))
            for entry in supported_builds
        )
        if expected and not covered:
            findings.append(
                Finding(
                    "workflow-products-not-built",
                    severity,
                    f"No blocking CI build covers every stable-v1 product of profile '{identifier}'",
                    f"Expected all of {sorted(expected)} or an all-targets build in configuration "
                    f"'{configuration}'.",
                )
            )

    shipping = next((item for item in _canonical_configs(data) if item.get("purpose") == "shipping"), None)
    if shipping:
        lanes = [
            entry
            for entry in configures
            if entry.get("preset") == shipping.get("preset")
            and entry.get("runnerOs") == "windows"
            and mandatory(entry)
        ]
        if not lanes:
            findings.append(
                Finding(
                    "workflow-shipping-runner-os",
                    "error",
                    "No Windows-hosted CI job configures the canonical Windows Shipping profile",
                    "The shipping profile is Windows-only; a Linux lane cannot stand in for it.",
                )
            )
    return findings


def check_codemodel_provenance(data: dict[str, Any]) -> list[Finding]:
    """Bind configured evidence to the tree, commit, preset and cache it claims.

    A CMake File API reply is just a directory. Without these checks, any build
    tree -- a different source tree, a different generator, a different set of
    options, or a hand-written one -- can be relabelled as the canonical profile
    and will read as clean configured evidence.
    """
    def path_key(value: Any) -> str:
        if not isinstance(value, str) or not value:
            return ""
        return Path(value).as_posix().rstrip("/").casefold()

    def preset_cache_value(value: Any) -> str:
        if isinstance(value, dict) and "value" in value:
            value = value["value"]
        return str(value)

    findings: list[Finding] = []
    repository = data.get("repository") or {}
    presets = data.get("cmakePresets", {})
    configs = _profile_config_map(data["profile"])
    declared = {
        entry["target"] for entry in data.get("cmakeTargets", []) if entry.get("buildable", True)
    }

    # A presetless installed-package consumer still belongs to the same stable
    # toolchain row. Derive that row only when every canonical source preset
    # agrees, rather than silently leaving generator/platform/toolset unchecked.
    stable_toolchains: set[tuple[str, str, str]] = set()
    for config in configs.values():
        preset_name = config.get("preset")
        if not preset_name:
            continue
        try:
            resolved = inventory_tool.resolve_configure_preset(presets, preset_name)
        except inventory_tool.InventoryError:
            continue
        stable_toolchains.add(
            tuple(str(resolved.get(name, "")) for name in ("generator", "architecture", "toolset"))
        )
    inherited_toolchain = next(iter(stable_toolchains)) if len(stable_toolchains) == 1 else ("", "", "")

    for evidence in data.get("configuredTargetEvidence", []):
        status = evidence.get("status")
        if status != "available":
            if status != "absent":
                findings.append(
                    Finding(
                        "codemodel-provenance-unavailable",
                        "error",
                        f"Profile '{evidence.get('profile')}' cannot present producer provenance "
                        f"because its configured evidence is {status!r}",
                        str(evidence.get("rejection", "No usable CMake File API reply was supplied.")),
                    )
                )
            continue
        identifier = str(evidence.get("profile"))
        evidence_cache = evidence.get("cacheVariables")
        if not isinstance(evidence_cache, dict):
            evidence_cache = {}
        corroborated = declared | inventory_tool.reviewed_required_target_references(
            data.get("cmakeTargetDeclarations", []), identifier, evidence_cache
        )
        config = configs.get(identifier, {})
        root = str(repository.get("root", ""))

        provenance = evidence.get("producerProvenance")
        provenance_state = provenance.get("state") if isinstance(provenance, dict) else ""
        if not isinstance(provenance, dict):
            findings.append(
                Finding(
                    "codemodel-provenance-missing",
                    "error",
                    f"Profile '{identifier}' has no verified producer provenance record",
                    "Run capture_provenance.py so it owns the query and configure transaction; "
                    "caller-supplied commit text is not evidence.",
                )
            )
        elif provenance_state == "unavailable":
            if (
                provenance.get("authority") != inventory_tool._BUILD_MATRIX_EXTERNAL_AUTHORITY
                or provenance.get("structuralState") != "validated"
                or not isinstance(provenance.get("authorityReason"), str)
                or not provenance.get("authorityReason")
            ):
                findings.append(
                    Finding(
                        "codemodel-provenance-incomplete",
                        "error",
                        f"Profile '{identifier}' unavailable producer summary is malformed",
                        "An unavailable state is only useful when it explicitly identifies the missing "
                        "independent external authority boundary.",
                    )
                )
            else:
                findings.append(
                    Finding(
                        "codemodel-producer-authority-unavailable",
                        "error",
                        f"Profile '{identifier}' has structurally validated but untrusted build-matrix evidence",
                        "The producer job's checkout, environment, provenance JSON, artifacts, hashes, and "
                        "same-job OIDC audience are self-authored. A protected external attestation verifier "
                        "must independently validate the artifact before producer evidence can be accepted.",
                    )
                )
        elif provenance_state == "verified":
            findings.append(
                Finding(
                    "codemodel-producer-authority-unverifiable",
                    "error",
                    f"Profile '{identifier}' declares producer-verified evidence without an independent verifier",
                    "The build-matrix tooling intentionally has no parser path that accepts a job-local receipt, OIDC token, "
                    "environment, artifact path, or hash as protected external authority.",
                )
            )
            if not (
            provenance.get("producer") == inventory_tool._PROVENANCE_PRODUCER
            and provenance.get("profile") == identifier
            and provenance.get("sourceClean") is True
            and provenance.get("untrackedPolicy") == "all-nonignored"
            and isinstance(provenance.get("recordFile"), str)
            and re.fullmatch(r"[0-9a-f]{64}", str(provenance.get("recordSha256", "")))
            and re.fullmatch(r"[0-9a-f]{64}", str(provenance.get("replyDigest", "")))
            and re.fullmatch(
                re.escape(inventory_tool._CAPTURE_CLIENT_PREFIX) + r"[0-9a-f]{32}",
                str(provenance.get("queryClient", "")),
            )
            and isinstance(provenance.get("configureArgv"), list)
            and bool(provenance.get("configureArgv"))
            and all(
                isinstance(argument, str) and argument
                for argument in provenance.get("configureArgv", [])
            )
            and path_key(provenance.get("configureArgv", [""])[0])
            == path_key(provenance.get("cmakeExecutable"))
            and isinstance(provenance.get("cmakeVersion"), str)
            and bool(provenance.get("cmakeVersion"))
            and provenance.get("artifactState") == "externally-attested-post-build"
            and provenance.get("ciProvider") == "github-actions"
            and isinstance(provenance.get("ciRepository"), str)
            and bool(provenance.get("ciRepository"))
            and isinstance(provenance.get("ciRunId"), str)
            and provenance.get("ciRunId", "").isdigit()
            and isinstance(provenance.get("ciRunAttempt"), str)
            and provenance.get("ciRunAttempt", "").isdigit()
            and isinstance(provenance.get("ciWorkflowRef"), str)
            and bool(provenance.get("ciWorkflowRef"))
            and provenance.get("ciJob") == inventory_tool._BUILD_MATRIX_PRODUCER_JOB
            and provenance.get("ciRunnerOs") == "Windows"
            ):
                findings.append(
                    Finding(
                        "codemodel-provenance-incomplete",
                        "error",
                        f"Profile '{identifier}' verified producer summary is incomplete or malformed",
                        "A verified label is not sufficient without independently validated external "
                        "attestation, query, executable, argv, repository-cleanliness, post-build artifacts, "
                        "and reply-digest bindings.",
                    )
                )
        else:
            findings.append(
                Finding(
                    "codemodel-provenance-missing",
                    "error",
                    f"Profile '{identifier}' has no verified producer provenance record",
                    "Run capture_provenance.py for structural evidence, then supply a protected external "
                    "attestation verifier; caller-supplied status text is not authority.",
                )
            )
        if not repository.get("commit"):
            findings.append(
                Finding(
                    "codemodel-repository-provenance-missing",
                    "error",
                    f"Profile '{identifier}' presents configured evidence with no current repository provenance",
                    "Evidence cannot be compared to the tree being reported.",
                )
            )
        elif isinstance(provenance, dict) and provenance_state in {"unavailable", "verified"}:
            if provenance.get("sourceCommit") != repository.get("commit"):
                findings.append(
                    Finding(
                        "codemodel-commit-mismatch",
                        "error",
                        f"Profile '{identifier}' evidence was configured from {provenance.get('sourceCommit')}, "
                        f"not {repository.get('commit')}",
                    )
                )
            if path_key(provenance.get("repositoryRoot")) != path_key(root):
                findings.append(
                    Finding(
                        "codemodel-provenance-root-mismatch",
                        "error",
                        f"Profile '{identifier}' producer record names another repository root",
                    )
                )
            if provenance.get("replyDigest") != evidence.get("replyDigest"):
                findings.append(
                    Finding(
                        "codemodel-provenance-digest-mismatch",
                        "error",
                        f"Profile '{identifier}' reply digest disagrees with its producer record",
                    )
                )
            if provenance.get("sourceClean") is not True:
                findings.append(
                    Finding(
                        "codemodel-source-worktree-dirty",
                        "error",
                        f"Profile '{identifier}' was configured from a modified worktree",
                    )
                )
        if repository.get("clean") is not True:
            findings.append(
                Finding(
                    "codemodel-worktree-dirty",
                    "error",
                    f"Profile '{identifier}' is being reported from a modified worktree",
                    "The current commit does not describe the inventory inputs.",
                )
            )
        elif (
            repository.get("untrackedPolicy") != "all-nonignored"
            or repository.get("statusSha256") != hashlib.sha256(b"").hexdigest()
        ):
            findings.append(
                Finding(
                    "codemodel-worktree-cleanliness-incomplete",
                    "error",
                    f"Profile '{identifier}' current repository cleanliness omits untracked files",
                    "Clean evidence must bind git status --untracked-files=all.",
                )
            )

        source = str(evidence.get("sourceDirectory", ""))
        actual_dir = str(evidence.get("buildDirectory", ""))
        evidence_dir = str(evidence.get("evidenceDirectory", ""))
        for field_name, value in (
            ("source directory", source),
            ("build directory", actual_dir),
            ("generator", evidence.get("generator")),
            ("architecture", evidence.get("architecture")),
            ("toolset", evidence.get("toolset")),
        ):
            if not value:
                findings.append(
                    Finding(
                        "codemodel-material-field-missing",
                        "error",
                        f"Profile '{identifier}' evidence omits its {field_name}",
                        "An absent binding fact is an unknown, not a successful comparison.",
                    )
                )
        if actual_dir and evidence_dir and path_key(actual_dir) != path_key(evidence_dir):
            findings.append(
                Finding(
                    "codemodel-evidence-dir-mismatch",
                    "error",
                    f"Profile '{identifier}' reply claims build directory '{actual_dir}' but was read from '{evidence_dir}'",
                    "A copied or relabelled reply is not evidence for the directory claiming it.",
                )
            )

        expected_configuration = str(config.get("configuration", ""))
        if expected_configuration and expected_configuration not in evidence.get("configurations", []):
            findings.append(
                Finding(
                    "codemodel-configuration-missing",
                    "error",
                    f"Profile '{identifier}' evidence has no '{expected_configuration}' configuration",
                    f"Configurations present: {evidence.get('configurations')}.",
                )
            )

        preset: dict[str, Any] | None = None
        preset_name = config.get("preset")
        if preset_name:
            try:
                preset = inventory_tool.resolve_configure_preset(presets, preset_name)
            except inventory_tool.InventoryError as error:
                findings.append(
                    Finding(
                        "codemodel-preset-unresolvable",
                        "error",
                        f"Profile '{identifier}' names preset '{preset_name}' which cannot be resolved",
                        str(error),
                    )
                )

        expected_source = ""
        if root and config.get("sourceDirectory"):
            expected_source = str(Path(root) / str(config["sourceDirectory"]))
        elif preset is not None:
            expected_source = root
        if expected_source and source and path_key(source) != path_key(expected_source):
            findings.append(
                Finding(
                    "codemodel-source-mismatch",
                    "error",
                    f"Profile '{identifier}' evidence was configured from '{source}', "
                    f"not its declared source '{expected_source}'",
                )
            )

        expected_dir = ""
        if root and config.get("buildDirectory"):
            expected_dir = str(Path(root) / str(config["buildDirectory"]))
        elif preset is not None:
            expected_dir = str(preset.get("resolvedBinaryDir", "")).replace(
                "${sourceDir}", root or "${sourceDir}"
            )
        if expected_dir and actual_dir and path_key(expected_dir) != path_key(actual_dir):
            findings.append(
                Finding(
                    "codemodel-build-dir-mismatch",
                    "error",
                    f"Profile '{identifier}' evidence lives in '{actual_dir}', "
                    f"not its declared build directory '{expected_dir}'",
                    "An arbitrary build directory relabelled as a canonical profile is not evidence.",
                )
            )

        expected_toolchain = (
            tuple(str(preset.get(name, "")) for name in ("generator", "architecture", "toolset"))
            if preset is not None
            else tuple(str(config.get(name, fallback)) for name, fallback in zip(
                ("generator", "architecture", "toolset"), inherited_toolchain
            ))
        )
        for key, expected_value in zip(("generator", "architecture", "toolset"), expected_toolchain):
            observed = str(evidence.get(key, ""))
            if expected_value and observed and expected_value != observed:
                findings.append(
                    Finding(
                        "codemodel-generator-mismatch",
                        "error",
                        f"Profile '{identifier}' evidence {key} is '{observed}', stable profile requires '{expected_value}'",
                    )
                )

        observed_cache = evidence.get("cacheVariables", {})
        if not isinstance(observed_cache, dict):
            observed_cache = {}
        material_cache = {
            "CMAKE_GENERATOR": str(evidence.get("generator", "")),
            "CMAKE_GENERATOR_PLATFORM": str(evidence.get("architecture", "")),
            "CMAKE_GENERATOR_TOOLSET": str(evidence.get("toolset", "")),
            "CMAKE_HOME_DIRECTORY": source,
        }
        if preset is not None:
            material_cache.update(
                {
                    name: preset_cache_value(expected)
                    for name, expected in preset.get("cacheVariables", {}).items()
                }
            )
        elif config.get("purpose") == "installed-sdk-consumer" and root:
            material_cache.update(
                {
                    "SparkEngine_DIR": str(Path(root) / str(config.get("packageDirectory", ""))),
                    "SPARK_EXPECTED_ENGINE_VERSION": str(config.get("expectedEngineVersion", "")),
                }
            )
        for name, expected_value in sorted(material_cache.items()):
            if name not in observed_cache or observed_cache[name] == "":
                findings.append(
                    Finding(
                        "codemodel-cache-missing",
                        "error",
                        f"Profile '{identifier}' evidence omits material cache value {name}",
                    )
                )
            elif expected_value and str(observed_cache[name]).upper() != expected_value.upper():
                findings.append(
                    Finding(
                        "codemodel-cache-mismatch",
                        "error",
                        f"Profile '{identifier}' evidence has {name}={observed_cache[name]!r}, "
                        f"requires {expected_value!r}",
                    )
                )

        for entry in evidence.get("targets", []):
            name = str(entry.get("target"))
            if corroborated and name not in corroborated:
                findings.append(
                    Finding(
                        "configured-target-undeclared",
                        "error",
                        f"Profile '{identifier}' evidence contains target '{name}' that no CMake input declares",
                        "A configured target with no source declaration is fabricated evidence.",
                    )
                )
    return findings


def check_build_matrix_producer_chain(data: dict[str, Any]) -> list[Finding]:
    """Require the real Windows producer to run before its structural validator.

    A checked-in JSON receipt is not authority.  This checks local workflow
    reachability only: capture, inventory, and parity must run in one blocking
    Windows job and feed the aggregate.  It intentionally does *not* treat
    that job's OIDC capability as a proof that the job really produced its own
    reply or artifacts; external authority is checked fail-closed elsewhere.
    """
    findings: list[Finding] = []
    workflow = data.get("workflow") or {}
    invocations = workflow.get("buildMatrixInvocations", [])
    jobs = {job.get("id"): job for job in workflow.get("jobs", [])}

    def normalized(value: Any) -> str:
        return str(value or "").replace("\\", "/").removeprefix("./").rstrip("/").casefold()

    def mandatory(entry: dict[str, Any]) -> bool:
        return (
            entry.get("gating") == "blocking"
            and entry.get("jobIf") == "absent"
            and entry.get("jobContinueOnError") == "absent"
            and entry.get("stepIf") == "absent"
            and entry.get("stepContinueOnError") == "absent"
            and entry.get("matrixResolved") is True
        )

    producer_job = jobs.get(inventory_tool._BUILD_MATRIX_PRODUCER_JOB)
    required_producers = {
        "windows-shipping": "build/windows-shipping",
        "windows-validation": "build/windows-release",
        "installed-sdk-consumer": "build/installed-sdk-consumer",
    }
    trusted_producers: dict[str, dict[str, Any]] = {}
    for profile, build_dir in required_producers.items():
        producer_candidates = [
            entry
            for entry in invocations
            if entry.get("kind") == "producer"
            and entry.get("job") == inventory_tool._BUILD_MATRIX_PRODUCER_JOB
            and entry.get("profile") == profile
            and normalized(entry.get("buildDir")) == build_dir
            and entry.get("build") is True
        ]
        producer = next(
            (
                entry
                for entry in producer_candidates
                if entry.get("executable") is True
                and entry.get("launcherProvenance") == "literal"
                and mandatory(entry)
                and entry.get("runnerOs") == "windows"
            ),
            None,
        )
        if producer is None:
            detail = "No canonical capture_provenance.py --build invocation was found."
            if producer_candidates:
                detail = "The candidate producer is conditional, advisory, wrapped, or not Windows-hosted."
            findings.append(
                Finding(
                    "build-matrix-producer-missing",
                    "error",
                    f"The build matrix has no mandatory {profile!r} provenance producer",
                    detail,
                )
            )
        else:
            trusted_producers[profile] = producer
    if len(trusted_producers) != len(required_producers):
        return findings
    if not isinstance(producer_job, dict) or producer_job.get("gating") != "blocking":
        findings.append(
            Finding(
                "build-matrix-producer-job-weak",
                "error",
                "build-matrix provenance producer job is not a blocking job",
            )
        )
    if isinstance(producer_job, dict) and producer_job.get("permissions", {}).get("id-token") == "write":
        findings.append(
            Finding(
                "build-matrix-producer-same-job-oidc-forbidden",
                "error",
                "build-matrix structural producer must not mint a same-job OIDC proof",
                "GitHub signs audiences requested by the mutable producer job; that identity proves the job, "
                "not that CMake or capture_provenance.py produced the receipt. Use a protected external "
                "attestation verifier instead.",
            )
        )

    producer_step = max(int(entry["stepIndex"]) for entry in trusted_producers.values())

    def later(kind: str, predicate: Any) -> bool:
        return any(
            entry.get("kind") == kind
            and entry.get("job") == inventory_tool._BUILD_MATRIX_PRODUCER_JOB
            and isinstance(entry.get("stepIndex"), int)
            and entry["stepIndex"] > producer_step
            and entry.get("executable") is True
            and entry.get("launcherProvenance") == "literal"
            and mandatory(entry)
            and predicate(entry)
            for entry in invocations
        )

    inventory_after = later(
        "inventory",
        lambda entry: {
            normalized(value) for value in entry.get("codemodels", [])
        }
        == {
            "windows-shipping=build/windows-shipping",
            "windows-validation=build/windows-release",
            "installed-sdk-consumer=build/installed-sdk-consumer",
        },
    )
    parity_after = later(
        "parity",
        lambda entry: normalized(entry.get("inventory")) == "build-matrix-inventory.json"
        and normalized(entry.get("baseline")) == ""
        and normalized(entry.get("output")) == "build-matrix-parity-findings.json",
    )
    pending_after = later(
        "pending-authority",
        lambda entry: normalized(entry.get("inventory")) == "build-matrix-inventory.json"
        and normalized(entry.get("report")) == "build-matrix-parity-findings.json"
        and normalized(entry.get("output")) == "build-matrix-pending-receipt.json",
    )
    if not inventory_after or not parity_after or not pending_after:
        missing = []
        if not inventory_after:
            missing.append("inventory")
        if not parity_after:
            missing.append("parity validator")
        if not pending_after:
            missing.append("pending-authority validator")
        findings.append(
            Finding(
                "build-matrix-producer-validator-disconnected",
                "error",
                "build-matrix producer is not followed by its mandatory local validator",
                f"Missing after producer: {', '.join(missing)}.",
            )
        )

    aggregate = jobs.get("required-ci-gate")
    if (
        not isinstance(aggregate, dict)
        or inventory_tool._BUILD_MATRIX_PRODUCER_JOB not in aggregate.get("needs", [])
    ):
        findings.append(
            Finding(
                "build-matrix-producer-not-required",
                "error",
                "Required CI Gate does not depend on the build-matrix structural producer job",
                "A green aggregate could otherwise bypass missing or skipped configured evidence.",
            )
        )
    return findings


def _validate_inventory_shape(data: dict[str, Any]) -> None:
    if data.get("schemaVersion") != 3:
        raise inventory_tool.InventoryError("inventory schemaVersion must be 3")
    required = {
        "profile", "cmakeOptionDeclarations", "cmakeOptions", "allCmakeOptionDeclarations",
        "cmakePresets", "cmakeTargetDeclarations", "cmakeTargets", "configuredTargetEvidence",
        "sparkBuildOptions", "workflow", "workflowCmakeConfigs", "stableV1Products",
    }
    missing = sorted(required - data.keys())
    if missing:
        raise inventory_tool.InventoryError(f"inventory lacks required fields: {missing}")
    workflow = data["workflow"]
    workflow_required = {
        "events", "jobs", "summary", "cmakeInvocations", "unresolvedInvocations",
        "configureInvocations", "buildInvocations", "testInvocations", "pathFilteredEvents",
        "buildMatrixInvocations",
    }
    workflow_missing = sorted(workflow_required - set(workflow))
    if workflow_missing:
        raise inventory_tool.InventoryError(f"inventory workflow record lacks fields: {workflow_missing}")
    if any(entry.get("status") == "available" for entry in data["configuredTargetEvidence"]):
        if not isinstance(data.get("repository"), dict) or not data["repository"].get("commit"):
            raise inventory_tool.InventoryError(
                "configured evidence is present but the inventory carries no repository provenance"
            )


def run_all_checks(data: dict[str, Any] | None = None) -> list[Finding]:
    current = data if data is not None else inventory_tool.build_inventory()
    _validate_inventory_shape(current)
    profile = current["profile"]
    option_applicability = profile.get("optionApplicability", [])
    active_options = _active_options(current["cmakeOptions"])
    findings: list[Finding] = []
    findings.extend(check_profile_contract(current))
    findings.extend(check_option_resolution(current["cmakeOptions"]))
    findings.extend(
        check_sparkbuild_vs_cmake(active_options, current["sparkBuildOptions"], option_applicability)
    )
    findings.extend(
        check_sparkbuild_defaults(active_options, current["sparkBuildOptions"], option_applicability)
    )
    findings.extend(check_duplicate_options(current["cmakeOptionDeclarations"]))
    findings.extend(check_preset_binary_dirs(current["cmakePresets"]))
    findings.extend(check_dependent_preset_linkage(current["cmakePresets"]))
    findings.extend(check_profile_presets(current))
    findings.extend(check_shipping_preset_options(current["cmakePresets"]))
    findings.extend(check_stable_v1_targets(current["cmakeTargets"], profile["buildProducts"]))
    findings.extend(check_target_declaration_resolution(current["cmakeTargetDeclarations"]))
    findings.extend(check_product_preset_activation(current))
    findings.extend(check_configured_targets(current))
    findings.extend(check_codemodel_provenance(current))
    findings.extend(check_workflow_adoption(current))
    findings.extend(check_workflow_semantics(current))
    findings.extend(check_build_matrix_producer_chain(current))
    return findings


def build_report(data: dict[str, Any] | None = None) -> dict[str, Any]:
    findings = run_all_checks(data)
    errors = [finding for finding in findings if finding.severity == "error"]
    warnings = [finding for finding in findings if finding.severity == "warning"]
    return {
        "schemaVersion": 3,
        "profile": "stable-v1",
        "state": "blocked" if errors else "clean",
        "errorCount": len(errors),
        "warningCount": len(warnings),
        "findings": [finding.to_dict() for finding in findings],
    }


def render_report(report: dict[str, Any]) -> bytes:
    return (json.dumps(report, indent=2, sort_keys=False) + "\n").encode("utf-8")


def render_internal_error(error: Exception) -> bytes:
    return render_report(
        {
            "schemaVersion": 3,
            "profile": "stable-v1",
            "state": "internal-error",
            "errorCount": 0,
            "warningCount": 0,
            "findings": [],
            "internalError": str(error),
        }
    )


def _write_atomic(path: Path, payload: bytes) -> None:
    inventory_tool._write_atomic(path, payload)


def _load_inventory(path: Path | None) -> dict[str, Any]:
    if path is None:
        return inventory_tool.build_inventory()
    value = inventory_tool._read_bounded_json_file(
        path, inventory_tool._MAX_INVENTORY_BYTES, "build-matrix inventory"
    )
    if not isinstance(value, dict):
        raise inventory_tool.InventoryError("inventory JSON must be an object")
    return value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, help="validate an already generated inventory")
    parser.add_argument("--output", type=Path, help="write deterministic report JSON")
    parser.add_argument("--baseline", type=Path, help="require exact equality with reviewed report JSON")
    args = parser.parse_args(argv)
    try:
        if args.output:
            output = args.output.resolve()
            for label, candidate in (("--inventory", args.inventory), ("--baseline", args.baseline)):
                if candidate and output == candidate.resolve():
                    raise inventory_tool.InventoryError(f"--output and {label} must name distinct paths")
        report = build_report(_load_inventory(args.inventory))
        payload = render_report(report)
        if args.output:
            _write_atomic(args.output, payload)
        sys.stdout.buffer.write(payload)
        if args.baseline:
            try:
                baseline_value = inventory_tool._read_bounded_json_file(
                    args.baseline, inventory_tool._MAX_REPORT_BYTES, "build-matrix findings baseline"
                )
                baseline_payload = render_report(baseline_value)
            except (inventory_tool.InventoryError, OSError, ValueError, json.JSONDecodeError) as error:
                print(f"BASELINE DRIFT: cannot parse {args.baseline}: {error}", file=sys.stderr)
                return EXIT_BASELINE_DRIFT
            if baseline_payload != payload or args.baseline.read_bytes() != baseline_payload:
                print(f"BASELINE DRIFT: regenerate and review {args.baseline}", file=sys.stderr)
                return EXIT_BASELINE_DRIFT
        if report["errorCount"]:
            print(
                f"REVIEWED FINDINGS: {report['errorCount']} blocking, {report['warningCount']} advisory",
                file=sys.stderr,
            )
            return EXIT_FINDINGS
        print(f"CLEAN: {report['warningCount']} advisory finding(s)", file=sys.stderr)
        return 0
    except (inventory_tool.InventoryError, OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        sys.stdout.buffer.write(render_internal_error(error))
        print(f"INTERNAL ERROR: {error}", file=sys.stderr)
        return EXIT_INTERNAL


if __name__ == "__main__":
    sys.exit(main())
