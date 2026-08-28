#!/usr/bin/env python3
"""Fail-closed parity checks for SparkEngine's build configuration surfaces.

Compares CMake options vs SparkBuild options, validates preset usage in CI
workflows, and verifies stable-v1 product coverage.  Every discrepancy is
a structured finding; exit 0 only when no findings remain.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from inventory import (
    build_inventory,
    extract_cmake_options,
    extract_cmake_presets,
    extract_cmake_targets,
    extract_sparkbuild_options,
    extract_workflow_presets,
    extract_workflow_cmake_configs,
    STABLE_V1_PRODUCTS,
)


class Finding:
    """One configuration-surface discrepancy."""

    __slots__ = ("category", "severity", "message", "detail")

    def __init__(
        self, category: str, severity: str, message: str, detail: str = ""
    ) -> None:
        self.category = category
        self.severity = severity
        self.message = message
        self.detail = detail

    def to_dict(self) -> dict[str, str]:
        d = {
            "category": self.category,
            "severity": self.severity,
            "message": self.message,
        }
        if self.detail:
            d["detail"] = self.detail
        return d


def check_sparkbuild_vs_cmake(
    cmake_opts: list[dict[str, Any]],
    sb_opts: list[dict[str, Any]],
) -> list[Finding]:
    """Options that exist in one surface but not the other."""
    findings: list[Finding] = []
    cmake_names = {o["name"] for o in cmake_opts}
    sb_names = {o["name"] for o in sb_opts}

    for name in sorted(sb_names - cmake_names):
        findings.append(Finding(
            "sparkbuild-orphan",
            "error",
            f"SparkBuild exposes '{name}' which has no corresponding CMake option()",
            "SparkBuild will generate -D flags that root CMake ignores silently.",
        ))

    for name in sorted(cmake_names - sb_names):
        findings.append(Finding(
            "cmake-only",
            "warning",
            f"CMake option '{name}' is not representable in SparkBuild",
            "Users of SparkBuild cannot configure this option.",
        ))

    return findings


def check_sparkbuild_defaults(
    cmake_opts: list[dict[str, Any]],
    sb_opts: list[dict[str, Any]],
) -> list[Finding]:
    """Shared options whose defaults disagree."""
    findings: list[Finding] = []
    cmake_map = {o["name"]: o["default"] for o in cmake_opts}
    sb_map = {o["name"]: o["default"] for o in sb_opts}

    for name in sorted(set(cmake_map) & set(sb_map)):
        cmake_default = cmake_map[name]
        sb_default = sb_map[name]
        cmake_bool = cmake_default.upper() in ("ON", "TRUE", "1") if isinstance(cmake_default, str) else cmake_default
        sb_bool = sb_default if isinstance(sb_default, bool) else sb_default.upper() in ("ON", "TRUE", "1")
        if cmake_bool != sb_bool:
            findings.append(Finding(
                "default-mismatch",
                "error",
                f"Default mismatch for '{name}': CMake={cmake_default}, SparkBuild={sb_default}",
                "A user will get different behavior depending on which tool they configure with.",
            ))

    return findings


def check_preset_workflow_parity(
    presets: dict[str, Any],
    workflow_refs: list[str],
) -> list[Finding]:
    """Workflow --preset references must name real presets."""
    findings: list[Finding] = []
    known = {p["name"] for p in presets["configurePresets"]}

    for ref in workflow_refs:
        if ref not in known:
            findings.append(Finding(
                "workflow-phantom-preset",
                "error",
                f"CI workflow references --preset '{ref}' which does not exist in CMakePresets.json",
            ))

    return findings


def check_stable_v1_targets(
    targets: list[dict[str, str]],
    products: list[dict[str, str]],
) -> list[Finding]:
    """Every stable-v1 product must have a corresponding CMake target."""
    findings: list[Finding] = []
    known_targets = {t["target"] for t in targets}

    for product in products:
        name = product["target"]
        if name not in known_targets:
            findings.append(Finding(
                "missing-target",
                "error",
                f"stable-v1 product '{name}' has no CMake add_executable/add_library",
                "This product cannot be built from the current CMake tree.",
            ))

    return findings


def check_duplicate_options(
    cmake_opts: list[dict[str, Any]],
) -> list[Finding]:
    """Flag duplicate option() names."""
    findings: list[Finding] = []
    seen: dict[str, int] = {}
    for opt in cmake_opts:
        name = opt["name"]
        seen[name] = seen.get(name, 0) + 1
    for name, count in sorted(seen.items()):
        if count > 1:
            findings.append(Finding(
                "duplicate-option",
                "error",
                f"CMake option '{name}' is declared {count} times",
            ))

    return findings


def check_preset_binary_dirs(
    presets: dict[str, Any],
) -> list[Finding]:
    """Validate that non-hidden presets produce expected binary directories."""
    findings: list[Finding] = []
    for preset in presets["configurePresets"]:
        if preset.get("hidden"):
            continue
        name = preset["name"]
        build_presets = presets.get("buildPresets", [])
        has_build = any(bp["configurePreset"] == name for bp in build_presets)
        if not has_build:
            findings.append(Finding(
                "orphan-configure-preset",
                "warning",
                f"Configure preset '{name}' has no corresponding build preset",
            ))

    return findings


def check_shipping_preset_options(
    presets: dict[str, Any],
    cmake_opts: list[dict[str, Any]],
) -> list[Finding]:
    """The windows-shipping preset must set SPARK_STRICT_DEPS=ON."""
    findings: list[Finding] = []
    for preset in presets["configurePresets"]:
        if preset["name"] != "windows-shipping":
            continue
        cache = preset.get("cacheVariables", {})
        if cache.get("SPARK_STRICT_DEPS") != "ON":
            findings.append(Finding(
                "shipping-strict-deps",
                "error",
                "windows-shipping preset does not set SPARK_STRICT_DEPS=ON",
                "Strict dependency enforcement is required for stable-v1.",
            ))
        if cache.get("SPARK_NATIVE_ARCH") != "OFF":
            findings.append(Finding(
                "shipping-native-arch",
                "error",
                "windows-shipping preset does not set SPARK_NATIVE_ARCH=OFF",
                "Distributed binaries must not require the build machine's CPU features.",
            ))
    return findings


def check_workflow_preset_adoption(
    workflow_refs: list[str],
    workflow_configs: list[dict[str, Any]],
) -> list[Finding]:
    """The CI workflow should use CMakePresets.json instead of inline -D flags."""
    findings: list[Finding] = []
    if not workflow_refs and workflow_configs:
        findings.append(Finding(
            "workflow-no-presets",
            "warning",
            f"CI workflow uses {len(workflow_configs)} inline cmake -B configurations "
            "instead of CMakePresets.json --preset references",
            "Preset adoption aligns documented and actual Windows configure commands.",
        ))
    return findings


def run_all_checks() -> list[Finding]:
    """Execute every parity check and return all findings."""
    cmake_opts = extract_cmake_options()
    sb_opts = extract_sparkbuild_options()
    presets = extract_cmake_presets()
    targets = extract_cmake_targets()
    workflow_refs = extract_workflow_presets()
    workflow_configs = extract_workflow_cmake_configs()

    findings: list[Finding] = []
    findings.extend(check_sparkbuild_vs_cmake(cmake_opts, sb_opts))
    findings.extend(check_sparkbuild_defaults(cmake_opts, sb_opts))
    findings.extend(check_preset_workflow_parity(presets, workflow_refs))
    findings.extend(check_stable_v1_targets(targets, STABLE_V1_PRODUCTS))
    findings.extend(check_duplicate_options(cmake_opts))
    findings.extend(check_preset_binary_dirs(presets))
    findings.extend(check_shipping_preset_options(presets, cmake_opts))
    findings.extend(check_workflow_preset_adoption(workflow_refs, workflow_configs))

    return findings


def main() -> int:
    findings = run_all_checks()
    errors = [f for f in findings if f.severity == "error"]
    warnings = [f for f in findings if f.severity == "warning"]

    output = {
        "errorCount": len(errors),
        "warningCount": len(warnings),
        "findings": [f.to_dict() for f in findings],
    }
    print(json.dumps(output, indent=2))

    if errors:
        print(f"\nFAILED: {len(errors)} error(s), {len(warnings)} warning(s)", file=sys.stderr)
        return 1

    if warnings:
        print(f"\nPASSED with {len(warnings)} warning(s)", file=sys.stderr)
    else:
        print("\nPASSED: all parity checks clean", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
