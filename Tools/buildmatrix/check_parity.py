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
import json
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
        for entry in evidence.get("targets", []):
            key = (str(entry.get("target")), str(entry.get("configuration", "")))
            if key in configured:
                duplicate_keys.add(key)
            configured[key] = entry
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
    workflow_refs = [entry.get("preset") for entry in configs if entry.get("preset")]
    findings.extend(check_preset_workflow_parity(presets, workflow_refs))
    canonical = data["profile"].get("buildConfigurations", [])
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
        matching = [entry for entry in configures if preset and entry.get("preset") == preset]
        blocking = [
            entry
            for entry in matching
            if entry.get("gating") == "blocking"
            and entry.get("stepIf") == "absent"
            and entry.get("stepContinueOnError") == "absent"
        ]
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
        supported_builds = [
            entry
            for entry in builds
            if entry.get("configuration") == configuration
            and entry.get("gating") == "blocking"
            and (not supported or entry.get("runnerOs") in supported)
        ]
        if configuration and not supported_builds:
            findings.append(
                Finding(
                    "workflow-configuration-not-built",
                    severity,
                    f"No blocking CI build on {sorted(supported) or 'any host'} produces configuration "
                    f"'{configuration}' for profile '{identifier}'",
                    "Configuring a profile does not build it, and a lane on an unsupported host cannot stand in.",
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
            if entry.get("preset") == shipping.get("preset") and entry.get("runnerOs") == "windows"
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
        if evidence.get("status") != "available":
            continue
        identifier = str(evidence.get("profile"))
        config = configs.get(identifier, {})
        root = str(repository.get("root", ""))

        provenance = evidence.get("producerProvenance")
        if not isinstance(provenance, dict) or provenance.get("state") != "verified":
            findings.append(
                Finding(
                    "codemodel-provenance-missing",
                    "error",
                    f"Profile '{identifier}' has no verified producer provenance record",
                    "Run capture_provenance.py after CMake configure; caller-supplied commit text is not evidence.",
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
        elif isinstance(provenance, dict) and provenance.get("state") == "verified":
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
            else inherited_toolchain
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
            if declared and name not in declared:
                findings.append(
                    Finding(
                        "configured-target-undeclared",
                        "error",
                        f"Profile '{identifier}' evidence contains target '{name}' that no CMake input declares",
                        "A configured target with no source declaration is fabricated evidence.",
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
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(payload)
    temporary.replace(path)


def _load_inventory(path: Path | None) -> dict[str, Any]:
    if path is None:
        return inventory_tool.build_inventory()
    value = json.loads(path.read_text(encoding="utf-8"))
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
                baseline_value = json.loads(args.baseline.read_text(encoding="utf-8"))
                baseline_payload = render_report(baseline_value)
            except (OSError, ValueError, json.JSONDecodeError) as error:
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
