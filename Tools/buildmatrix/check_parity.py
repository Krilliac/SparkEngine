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


def _option_severity(name: str, entries: Iterable[dict[str, Any]]) -> tuple[str, str]:
    exception = _exception_map(entries).get(name)
    if exception and exception.get("applicability") in {"outside", "shared"}:
        return "warning", (
            f"Explicitly classified {exception['applicability']}: {exception.get('reason', 'no reason')}"
        )
    return "error", "No outside/shared exception exists in the stable-v1 profile."


def _product_severity(product: dict[str, Any]) -> str:
    return "error" if product.get("applicability") == "required" else "warning"


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
    build_profiles = {entry["configurePreset"] for entry in presets.get("buildPresets", [])}
    return [
        Finding(
            "orphan-configure-preset",
            "warning",
            f"Configure preset '{entry['name']}' has no corresponding build preset",
        )
        for entry in presets.get("configurePresets", [])
        if not entry.get("hidden") and entry["name"] not in build_profiles
    ]


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
    return {entry["name"]: entry.get("default") for entry in data.get("cmakeOptions", [])}


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
    known = {entry["target"]: entry for entry in targets}
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
        severity = "error" if any(item.get("applicability") == "required" for item in products) else "warning"
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


def _validate_inventory_shape(data: dict[str, Any]) -> None:
    if data.get("schemaVersion") != 2:
        raise inventory_tool.InventoryError("inventory schemaVersion must be 2")
    required = {
        "profile", "cmakeOptionDeclarations", "cmakeOptions", "cmakePresets",
        "cmakeTargetDeclarations", "cmakeTargets", "configuredTargetEvidence",
        "sparkBuildOptions", "workflowCmakeConfigs", "stableV1Products",
    }
    missing = sorted(required - data.keys())
    if missing:
        raise inventory_tool.InventoryError(f"inventory lacks required fields: {missing}")


def run_all_checks(data: dict[str, Any] | None = None) -> list[Finding]:
    current = data if data is not None else inventory_tool.build_inventory()
    _validate_inventory_shape(current)
    profile = current["profile"]
    option_applicability = profile.get("optionApplicability", [])
    findings: list[Finding] = []
    findings.extend(check_profile_contract(current))
    findings.extend(
        check_sparkbuild_vs_cmake(
            current["cmakeOptions"], current["sparkBuildOptions"], option_applicability
        )
    )
    findings.extend(
        check_sparkbuild_defaults(
            current["cmakeOptions"], current["sparkBuildOptions"], option_applicability
        )
    )
    findings.extend(check_duplicate_options(current["cmakeOptionDeclarations"]))
    findings.extend(check_preset_binary_dirs(current["cmakePresets"]))
    findings.extend(check_profile_presets(current))
    findings.extend(check_shipping_preset_options(current["cmakePresets"]))
    findings.extend(check_stable_v1_targets(current["cmakeTargets"], profile["buildProducts"]))
    findings.extend(check_product_preset_activation(current))
    findings.extend(check_configured_targets(current))
    findings.extend(check_workflow_adoption(current))
    return findings


def build_report(data: dict[str, Any] | None = None) -> dict[str, Any]:
    findings = run_all_checks(data)
    errors = [finding for finding in findings if finding.severity == "error"]
    warnings = [finding for finding in findings if finding.severity == "warning"]
    return {
        "schemaVersion": 2,
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
            "schemaVersion": 2,
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
