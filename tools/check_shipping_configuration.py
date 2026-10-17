#!/usr/bin/env python3
"""Prove Shipping is a distinct configuration in a configured build graph (BLD-100).

The static contract tests read preset and CMake text. This tool reads what CMake
actually resolved: the CMake File API codemodel-v2 reply of a configured
multi-config MSVC tree, through the hardened reply reader in
Tools/buildmatrix/inventory.py. For SparkEngineLib and SparkEngine it requires:

* MinSizeRel (Shipping) compiles with SPARK_BUILD_SHIPPING and SPARK_SHIPPING=1,
  and Debug and Release define neither;
* every configuration has its own artifact directory (bin/<Config>, lib/<Config>),
  so a Shipping image can never be a Release image under another name;
* the MinSizeRel SparkEngine link carries /DEBUG, /Brepro and a /PDBALTPATH that
  names its own PDB artifact by bare file name (private symbols without a build
  machine path, reproducible timestamps). A /PDBALTPATH built from %_PDB% fails:
  the Visual Studio generator escapes it to %%_PDB%%, which link.exe records as
  "%SparkEngine.pdb%" or worse.

With --require-preset windows-shipping it also requires the preset's OFF toggles
to have reached the graph: MinSizeRel defines none of SPARK_CONSOLE_IN_SHIPPING,
SPARK_DEVCOMMANDS_IN_SHIPPING or PROFILING_ENABLED.

The reply must already exist: build.yml runs this after the capture_provenance
configure of build/windows-shipping, and the ShippingConfiguration_Distinct CTest
tree requests codemodel-v2 with cmake_file_api() during its own configure.

Exit codes: 0 distinct, 1 contract violations, 2 missing or malformed reply.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "Tools" / "buildmatrix"))

import inventory  # noqa: E402

CHECKED_TARGETS = ("SparkEngineLib", "SparkEngine")
CONFIGURATIONS = ("Debug", "Release", "MinSizeRel")
SHIPPING_CONFIGURATION = "MinSizeRel"
SHIPPING_DEFINES = ("SPARK_BUILD_SHIPPING", "SPARK_SHIPPING=1")
SHIPPING_DEFINE_NAMES = ("SPARK_BUILD_SHIPPING", "SPARK_SHIPPING")
SHIPPING_LINK_FLAGS = ("/DEBUG", "/Brepro")
PDBALTPATH_FLAG = "/PDBALTPATH:"
# Development-only defines the windows-shipping preset switches off.
PRESET_FORBIDDEN_DEFINES = {
    "windows-shipping": ("SPARK_CONSOLE_IN_SHIPPING", "SPARK_DEVCOMMANDS_IN_SHIPPING", "PROFILING_ENABLED"),
}


def _define_name(define: str) -> str:
    return define.split("=", 1)[0]


def _target_record(document: dict[str, Any]) -> dict[str, Any]:
    """Defines, artifacts and link flags of one configured target document."""
    defines: set[str] = set()
    for group in document.get("compileGroups", []) or []:
        for entry in group.get("defines", []) or []:
            if isinstance(entry, dict) and isinstance(entry.get("define"), str):
                defines.add(entry["define"])
    artifacts = sorted(
        entry["path"]
        for entry in document.get("artifacts", []) or []
        if isinstance(entry, dict) and isinstance(entry.get("path"), str)
    )
    link_flags: list[str] = []
    link = document.get("link")
    if isinstance(link, dict):
        for fragment in link.get("commandFragments", []) or []:
            if isinstance(fragment, dict) and fragment.get("role") == "flags":
                link_flags.extend(str(fragment.get("fragment", "")).split())
    return {"type": document.get("type"), "defines": defines, "artifacts": artifacts, "linkFlags": link_flags}


def read_configured_targets(build_dir: Path) -> dict[str, dict[str, dict[str, Any]]]:
    """Return {configuration: {target: record}} for CHECKED_TARGETS from the newest reply."""
    label = "shipping-configuration"
    snapshot = inventory._snapshot_reply_directory(build_dir, label)
    if snapshot is None:
        raise inventory.InventoryError(
            f"{build_dir}: no CMake File API reply; configure the tree with a codemodel-v2 query first"
        )
    try:
        indices = sorted(
            name for name in snapshot.entry_names if name.startswith("index-") and name.endswith(".json")
        )
        if not indices:
            raise inventory.InventoryError(f"{build_dir}: CMake File API reply has no index")
        index = inventory._validate_index(
            snapshot.read_json(indices[-1], inventory._MAX_INDEX_BYTES, f"{label} index"), label
        )
        codemodel_file = inventory._reply_object(index, "codemodel", 2, label)
        if not codemodel_file:
            raise inventory.InventoryError(f"{build_dir}: newest File API index has no codemodel-v2 reply")
        codemodel = inventory._require_mapping(
            snapshot.read_json(codemodel_file, inventory._MAX_CODEMODEL_BYTES, f"{label} codemodel"),
            f"{label} codemodel-v2",
        )
        configurations = inventory._require_list(
            codemodel.get("configurations"), f"{label} configurations", inventory._MAX_CONFIGURATIONS
        )
        result: dict[str, dict[str, dict[str, Any]]] = {}
        for configuration in configurations:
            configuration = inventory._require_mapping(configuration, f"{label} configuration")
            name = configuration.get("name")
            if name not in CONFIGURATIONS:
                continue
            records: dict[str, dict[str, Any]] = {}
            references = inventory._require_list(
                configuration.get("targets"), f"{label} {name} targets", inventory._MAX_TARGET_REFERENCES
            )
            for reference in references:
                reference = inventory._require_mapping(reference, f"{label} {name} target reference")
                if reference.get("name") not in CHECKED_TARGETS:
                    continue
                document = inventory._require_mapping(
                    snapshot.read_json(reference.get("jsonFile"), inventory._MAX_TARGET_BYTES, f"{label} target"),
                    f"{label} target {reference.get('name')!r}",
                )
                records[str(reference["name"])] = _target_record(document)
            result[str(name)] = records
        snapshot.assert_stable()
        return result
    finally:
        snapshot.close()


def check_distinct(
    configured: dict[str, dict[str, dict[str, Any]]], require_preset: str | None = None
) -> list[str]:
    """Contract violations; an empty list means Shipping is a distinct configuration."""
    problems: list[str] = []
    for configuration in CONFIGURATIONS:
        if configuration not in configured:
            problems.append(f"configuration {configuration} is absent from the codemodel")
    for target in CHECKED_TARGETS:
        records = {
            configuration: configured[configuration][target]
            for configuration in CONFIGURATIONS
            if target in configured.get(configuration, {})
        }
        for configuration in CONFIGURATIONS:
            if configuration in configured and configuration not in records:
                problems.append(f"{target} is not configured for {configuration}")
        for configuration, record in records.items():
            defines = record["defines"]
            names = {_define_name(define) for define in defines}
            if configuration == SHIPPING_CONFIGURATION:
                for required in SHIPPING_DEFINES:
                    if required not in defines:
                        problems.append(f"{target} {configuration} does not define {required}")
                for forbidden in PRESET_FORBIDDEN_DEFINES.get(require_preset or "", ()):
                    if forbidden in names:
                        problems.append(
                            f"{target} {configuration} defines {forbidden}, which {require_preset} turns off"
                        )
            else:
                for forbidden in SHIPPING_DEFINE_NAMES:
                    if forbidden in names:
                        problems.append(f"{target} {configuration} defines Shipping-only {forbidden}")
            if not record["artifacts"]:
                problems.append(f"{target} {configuration} has no artifact")
            elif not all(configuration in Path(path).parts for path in record["artifacts"]):
                problems.append(
                    f"{target} {configuration} artifacts are outside a {configuration} directory: "
                    + ", ".join(record["artifacts"])
                )
        seen: dict[str, str] = {}
        for configuration, record in records.items():
            for path in record["artifacts"]:
                key = path.casefold()
                if key in seen and seen[key] != configuration:
                    problems.append(f"{target} {seen[key]} and {configuration} share artifact {path}")
                seen.setdefault(key, configuration)
    shipping_engine = configured.get(SHIPPING_CONFIGURATION, {}).get("SparkEngine")
    if shipping_engine is not None:
        flags = {flag.casefold() for flag in shipping_engine["linkFlags"]}
        for required in SHIPPING_LINK_FLAGS:
            if required.casefold() not in flags:
                problems.append(f"SparkEngine {SHIPPING_CONFIGURATION} link does not carry {required}")
        pdb_names = {
            Path(path).name.casefold() for path in shipping_engine["artifacts"] if path.casefold().endswith(".pdb")
        }
        alt_paths = [
            flag[len(PDBALTPATH_FLAG) :]
            for flag in shipping_engine["linkFlags"]
            if flag.casefold().startswith(PDBALTPATH_FLAG.casefold())
        ]
        if not alt_paths:
            problems.append(f"SparkEngine {SHIPPING_CONFIGURATION} link does not carry {PDBALTPATH_FLAG}")
        for value in alt_paths:
            if value.casefold() not in pdb_names:
                problems.append(
                    f"SparkEngine {SHIPPING_CONFIGURATION} {PDBALTPATH_FLAG}{value} is not the bare name of its PDB "
                    f"artifact ({', '.join(sorted(pdb_names)) or 'none'})"
                )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, required=True, help="configured multi-config build tree")
    parser.add_argument(
        "--require-preset",
        choices=sorted(PRESET_FORBIDDEN_DEFINES),
        help="also require the named preset's development toggles to be off in MinSizeRel",
    )
    args = parser.parse_args(argv)
    try:
        configured = read_configured_targets(args.build_dir)
    except inventory.InventoryError as error:
        print(f"ShippingConfiguration: ERROR: {error}", file=sys.stderr)
        return 2
    problems = check_distinct(configured, args.require_preset)
    for configuration in CONFIGURATIONS:
        for target, record in sorted(configured.get(configuration, {}).items()):
            shipping = sorted(d for d in record["defines"] if _define_name(d) in SHIPPING_DEFINE_NAMES)
            print(f"{configuration:>10} {target}: shipping defines {shipping or 'none'}; "
                  f"artifacts {', '.join(record['artifacts']) or 'none'}")
    if problems:
        for problem in problems:
            print(f"ShippingConfiguration: FAIL: {problem}", file=sys.stderr)
        return 1
    checked = sum(len(configured[configuration]) for configuration in CONFIGURATIONS)
    print(f"ShippingConfiguration: distinct ({checked} target configurations checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
