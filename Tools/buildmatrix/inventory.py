#!/usr/bin/env python3
"""Deterministic build-matrix inventory for SparkEngine.

Extracts CMake options, presets, targets, and SparkBuild options from
source files and produces a canonical JSON inventory.  Two runs on the
same tree must produce byte-identical output (sorted keys, no
timestamps).
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE_ROOT = REPO_ROOT / "CMakeLists.txt"
PRESETS_PATH = REPO_ROOT / "CMakePresets.json"
SPARKBUILD_CONFIG = REPO_ROOT / "SparkBuild" / "src" / "Config.cpp"
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "build.yml"

STABLE_V1_PRODUCTS: list[dict[str, str]] = [
    {"target": "SparkEngine", "kind": "executable", "profile": "required"},
    {"target": "SparkEditor", "kind": "executable", "profile": "required"},
    {"target": "SparkConsole", "kind": "executable", "profile": "required"},
    {"target": "SparkDaemon", "kind": "executable", "profile": "required"},
    {"target": "SparkCollabServer", "kind": "executable", "profile": "required"},
    {"target": "SparkOrchestrator", "kind": "executable", "profile": "required"},
    {"target": "SparkBuild", "kind": "executable", "profile": "required"},
    {"target": "SparkCrashReporter", "kind": "executable", "profile": "required"},
    {"target": "SparkAutomation", "kind": "executable", "profile": "required"},
    {"target": "SparkCooker", "kind": "executable", "profile": "required"},
    {"target": "SparkWorker", "kind": "executable", "profile": "required"},
    {"target": "SparkShaderCompiler", "kind": "executable", "profile": "required"},
    {"target": "SparkLauncher", "kind": "executable", "profile": "required"},
    {"target": "SparkServer", "kind": "executable", "profile": "required"},
    {"target": "SparkGateway", "kind": "executable", "profile": "required"},
    {"target": "SparkInstaller", "kind": "executable", "profile": "required"},
    {"target": "SparkTests", "kind": "executable", "profile": "test"},
    {"target": "SparkEngineLib", "kind": "static_library", "profile": "required"},
]

_OPTION_RE = re.compile(
    r'^\s*option\(\s*(\w+)\s+"([^"]+)"\s+(ON|OFF|\$\{[^}]+\})\s*\)',
    re.MULTILINE,
)

_SPARKBUILD_OPTION_RE = re.compile(
    r'\{"(\w+)",\s*"([^"]*)",\s*\n?\s*"([^"]*)",\s*(true|false),\s*(true|false),\s*\n?\s*OptionCategory::(\w+)\}',
    re.DOTALL,
)


def extract_cmake_options(path: Path | None = None) -> list[dict[str, Any]]:
    """Parse option() declarations from the root CMakeLists.txt.

    Platform-conditional option() pairs (if/else branches declaring the
    same name with different defaults) are collapsed into a single entry
    whose default is marked ``platform-conditional``.
    """
    text = (path or CMAKE_ROOT).read_text(encoding="utf-8")
    seen: dict[str, dict[str, Any]] = {}
    for match in _OPTION_RE.finditer(text):
        name, description, default = match.groups()
        if name in seen:
            seen[name]["default"] = "platform-conditional"
        else:
            seen[name] = {
                "name": name,
                "description": description,
                "default": default,
            }
    return sorted(seen.values(), key=lambda o: o["name"])


def extract_cmake_presets(path: Path | None = None) -> dict[str, Any]:
    """Parse CMakePresets.json and return configure/build/test presets."""
    data = json.loads((path or PRESETS_PATH).read_text(encoding="utf-8"))
    configure = []
    for preset in data.get("configurePresets", []):
        entry: dict[str, Any] = {
            "name": preset["name"],
            "hidden": preset.get("hidden", False),
        }
        if "displayName" in preset:
            entry["displayName"] = preset["displayName"]
        if "inherits" in preset:
            entry["inherits"] = preset["inherits"]
        cond = preset.get("condition", {})
        if cond.get("type") == "equals" and cond.get("lhs") == "${hostSystemName}":
            entry["platform"] = cond["rhs"]
        cache = preset.get("cacheVariables", {})
        if cache:
            entry["cacheVariables"] = dict(sorted(cache.items()))
        configure.append(entry)

    build = []
    for preset in data.get("buildPresets", []):
        entry = {"name": preset["name"], "configurePreset": preset["configurePreset"]}
        if "configuration" in preset:
            entry["configuration"] = preset["configuration"]
        build.append(entry)

    test = []
    for preset in data.get("testPresets", []):
        entry = {"name": preset["name"], "configurePreset": preset["configurePreset"]}
        test.append(entry)

    return {
        "configurePresets": configure,
        "buildPresets": build,
        "testPresets": test,
    }


def extract_cmake_targets() -> list[dict[str, str]]:
    """Scan all CMakeLists.txt for add_executable / add_library."""
    targets: list[dict[str, str]] = []
    seen: set[str] = set()
    exe_re = re.compile(r"^\s*add_executable\(\s*(\w+)", re.MULTILINE)
    shared_re = re.compile(r"^\s*add_library\(\s*(\w+)\s+SHARED", re.MULTILINE)
    static_re = re.compile(r"^\s*add_library\(\s*(\w+)\s+STATIC", re.MULTILINE)

    for cmake_file in sorted(REPO_ROOT.rglob("CMakeLists.txt")):
        rel = cmake_file.relative_to(REPO_ROOT).as_posix()
        if rel.startswith("ThirdParty/"):
            continue
        text = cmake_file.read_text(encoding="utf-8", errors="replace")
        for match in exe_re.finditer(text):
            name = match.group(1)
            if name not in seen:
                seen.add(name)
                targets.append({"target": name, "kind": "executable", "file": rel})
        for match in shared_re.finditer(text):
            name = match.group(1)
            if name not in seen:
                seen.add(name)
                targets.append({"target": name, "kind": "shared_library", "file": rel})
        for match in static_re.finditer(text):
            name = match.group(1)
            if name not in seen:
                seen.add(name)
                targets.append({"target": name, "kind": "static_library", "file": rel})

    return sorted(targets, key=lambda t: t["target"])


def extract_sparkbuild_options(path: Path | None = None) -> list[dict[str, Any]]:
    """Parse SparkBuild Config.cpp for its option registry."""
    text = (path or SPARKBUILD_CONFIG).read_text(encoding="utf-8")
    results: list[dict[str, Any]] = []
    for match in _SPARKBUILD_OPTION_RE.finditer(text):
        cmake_var, display, desc, default_val, _current, category = match.groups()
        results.append({
            "name": cmake_var,
            "displayName": display,
            "description": desc,
            "default": default_val == "true",
            "category": category,
        })
    return sorted(results, key=lambda o: o["name"])


def extract_workflow_presets(path: Path | None = None) -> list[str]:
    """Extract --preset references from the CI workflow."""
    text = (path or WORKFLOW_PATH).read_text(encoding="utf-8")
    return sorted(set(re.findall(r"--preset\s+(\S+)", text)))


def extract_workflow_cmake_configs(path: Path | None = None) -> list[dict[str, Any]]:
    """Extract cmake -B configure commands from the CI workflow.

    Returns a list of {job, buildDir, options} dicts showing how the
    workflow actually configures CMake (often without presets).
    """
    text = (path or WORKFLOW_PATH).read_text(encoding="utf-8")
    results: list[dict[str, Any]] = []
    seen_cmds: set[str] = set()
    for match in re.finditer(r"cmake\s+-B\s+(\S+)(.*?)(?:\n\s*(?!-)|\n[^ ])", text, re.DOTALL):
        build_dir = match.group(1).rstrip("\\")
        rest = match.group(2)
        options = sorted(set(re.findall(r"-D(\w+)=(\w+)", rest)))
        cmd_key = f"{build_dir}:{options}"
        if cmd_key not in seen_cmds:
            seen_cmds.add(cmd_key)
            results.append({
                "buildDir": build_dir,
                "options": {k: v for k, v in options},
            })
    return results


def build_inventory() -> dict[str, Any]:
    """Build the complete deterministic inventory."""
    return {
        "cmakeOptions": extract_cmake_options(),
        "cmakePresets": extract_cmake_presets(),
        "cmakeTargets": extract_cmake_targets(),
        "sparkBuildOptions": extract_sparkbuild_options(),
        "workflowPresets": extract_workflow_presets(),
        "workflowCmakeConfigs": extract_workflow_cmake_configs(),
        "stableV1Products": STABLE_V1_PRODUCTS,
    }


def main() -> int:
    inventory = build_inventory()
    print(json.dumps(inventory, indent=2, sort_keys=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
