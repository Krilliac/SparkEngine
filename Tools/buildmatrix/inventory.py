#!/usr/bin/env python3
"""Deterministic, fail-closed build-matrix inventory for SparkEngine.

The canonical stable-v1 product contract lives in docs/site/readiness.json.
Source declarations prove that a target or option is declared; configured CMake
File API codemodel replies separately prove that a target exists in a concrete
configuration. The two forms of evidence are deliberately never conflated.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE_ROOT = REPO_ROOT / "CMakeLists.txt"
PRESETS_PATH = REPO_ROOT / "CMakePresets.json"
SPARKBUILD_CONFIG = REPO_ROOT / "SparkBuild" / "src" / "Config.cpp"
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "build.yml"
READINESS_PATH = REPO_ROOT / "docs" / "site" / "readiness.json"

WINDOWS_CONTEXT: dict[str, bool] = {
    "WIN32": True,
    "MSVC": True,
    "UNIX": False,
    "APPLE": False,
    "LINUX": False,
}

_BOOL_TRUE = {"1", "ON", "YES", "TRUE", "Y"}
_BOOL_FALSE = {"0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", ""}
_TARGET_KIND_MAP = {
    "EXECUTABLE": "executable",
    "STATIC_LIBRARY": "static_library",
    "SHARED_LIBRARY": "shared_library",
    "MODULE_LIBRARY": "module_library",
    "OBJECT_LIBRARY": "object_library",
    "INTERFACE_LIBRARY": "interface_library",
    "UNKNOWN_LIBRARY": "unknown_library",
}


class InventoryError(RuntimeError):
    """The inventory cannot safely interpret an authoritative input."""


@dataclass
class _ConditionFrame:
    identifier: str
    branches: list[str | None]
    current: int = 0


def _line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _read_bracket(text: str, start: int) -> tuple[str, int] | None:
    match = re.match(r"\[(=*)\[", text[start:])
    if not match:
        return None
    marker = "]" + match.group(1) + "]"
    content_start = start + match.end()
    end = text.find(marker, content_start)
    if end < 0:
        raise InventoryError(f"unterminated CMake bracket argument at line {_line_number(text, start)}")
    return text[content_start:end], end + len(marker)


def _iter_cmake_commands(text: str, source: str) -> Iterable[dict[str, Any]]:
    """Yield CMake commands without silently skipping command syntax."""
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char.isspace():
            index += 1
            continue
        if char == "#":
            bracket = _read_bracket(text, index + 1)
            if bracket:
                _, index = bracket
            else:
                newline = text.find("\n", index)
                index = length if newline < 0 else newline + 1
            continue
        if not (char.isalpha() or char == "_"):
            index += 1
            continue

        start = index
        index += 1
        while index < length and (text[index].isalnum() or text[index] == "_"):
            index += 1
        name = text[start:index]
        while index < length and text[index].isspace():
            index += 1
        if index >= length or text[index] != "(":
            continue

        depth = 1
        body_start = index + 1
        index = body_start
        quoted = False
        escaped = False
        while index < length and depth:
            char = text[index]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
                index += 1
                continue
            if char == '"':
                quoted = True
                index += 1
                continue
            if char == "#":
                bracket = _read_bracket(text, index + 1)
                if bracket:
                    _, index = bracket
                else:
                    newline = text.find("\n", index)
                    index = length if newline < 0 else newline + 1
                continue
            bracket = _read_bracket(text, index)
            if bracket:
                _, index = bracket
                continue
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    body = text[body_start:index]
                    index += 1
                    yield {
                        "name": name.lower(),
                        "spelling": name,
                        "body": body,
                        "file": source,
                        "line": _line_number(text, start),
                    }
                    break
            index += 1
        if depth:
            raise InventoryError(f"{source}:{_line_number(text, start)}: unterminated {name}() command")


def _tokenize_cmake_arguments(body: str) -> list[str]:
    tokens: list[str] = []
    index = 0
    length = len(body)
    while index < length:
        while index < length and body[index].isspace():
            index += 1
        if index >= length:
            break
        if body[index] == "#":
            newline = body.find("\n", index)
            index = length if newline < 0 else newline + 1
            continue
        bracket = _read_bracket(body, index)
        if bracket:
            value, index = bracket
            tokens.append(value)
            continue
        if body[index] == '"':
            index += 1
            value: list[str] = []
            while index < length:
                if body[index] == "\\" and index + 1 < length:
                    value.append(body[index + 1])
                    index += 2
                elif body[index] == '"':
                    index += 1
                    break
                else:
                    value.append(body[index])
                    index += 1
            else:
                raise InventoryError("unterminated quoted CMake argument")
            tokens.append("".join(value))
            continue
        start = index
        while index < length and not body[index].isspace():
            index += 1
        tokens.append(body[start:index])
    return tokens


def _commands_with_conditions(text: str, source: str) -> Iterable[dict[str, Any]]:
    stack: list[_ConditionFrame] = []
    for command in _iter_cmake_commands(text, source):
        name = command["name"]
        body = command["body"].strip()
        if name == "if":
            stack.append(_ConditionFrame(f"{source}:{command['line']}", [body]))
            continue
        if name == "elseif":
            if not stack:
                raise InventoryError(f"{source}:{command['line']}: elseif() without if()")
            stack[-1].branches.append(body)
            stack[-1].current = len(stack[-1].branches) - 1
            continue
        if name == "else":
            if not stack:
                raise InventoryError(f"{source}:{command['line']}: else() without if()")
            stack[-1].branches.append(None)
            stack[-1].current = len(stack[-1].branches) - 1
            continue
        if name == "endif":
            if not stack:
                raise InventoryError(f"{source}:{command['line']}: endif() without if()")
            stack.pop()
            continue
        command["conditionFrames"] = [
            {"id": frame.identifier, "branch": frame.current, "branches": list(frame.branches)} for frame in stack
        ]
        yield command
    if stack:
        raise InventoryError(f"{source}: unterminated if() block")


def _condition_tokens(expression: str) -> list[str]:
    return re.findall(r"\(|\)|\bAND\b|\bOR\b|\bNOT\b|[A-Za-z_][A-Za-z0-9_]*|[01]", expression, re.I)


def _evaluate_condition(expression: str, context: dict[str, bool]) -> bool:
    tokens = _condition_tokens(expression)
    if not tokens:
        raise InventoryError(f"cannot evaluate empty CMake condition {expression!r}")
    position = 0

    def parse_atom() -> bool:
        nonlocal position
        if position >= len(tokens):
            raise InventoryError(f"incomplete CMake condition {expression!r}")
        token = tokens[position]
        upper = token.upper()
        if upper == "NOT":
            position += 1
            return not parse_atom()
        if token == "(":
            position += 1
            value = parse_or()
            if position >= len(tokens) or tokens[position] != ")":
                raise InventoryError(f"unbalanced CMake condition {expression!r}")
            position += 1
            return value
        position += 1
        if upper in _BOOL_TRUE:
            return True
        if upper in _BOOL_FALSE:
            return False
        if upper not in context:
            raise InventoryError(f"unsupported CMake condition token {token!r} in {expression!r}")
        return bool(context[upper])

    def parse_and() -> bool:
        nonlocal position
        value = parse_atom()
        while position < len(tokens) and tokens[position].upper() == "AND":
            position += 1
            rhs = parse_atom()
            value = value and rhs
        return value

    def parse_or() -> bool:
        nonlocal position
        value = parse_and()
        while position < len(tokens) and tokens[position].upper() == "OR":
            position += 1
            rhs = parse_and()
            value = value or rhs
        return value

    result = parse_or()
    if position != len(tokens):
        raise InventoryError(f"unsupported CMake condition syntax {expression!r}")
    return result


def _frame_is_active(frame: dict[str, Any], context: dict[str, bool]) -> bool:
    branches: list[str | None] = frame["branches"]
    current = int(frame["branch"])
    for index, expression in enumerate(branches):
        if index == current:
            return True if expression is None else _evaluate_condition(expression, context)
        if expression is not None and _evaluate_condition(expression, context):
            return False
    return False


def _declaration_is_active(declaration: dict[str, Any], context: dict[str, bool]) -> bool:
    return all(_frame_is_active(frame, context) for frame in declaration.get("conditionFrames", []))


def declarations_are_mutually_exclusive(left: dict[str, Any], right: dict[str, Any]) -> bool:
    left_frames = {frame["id"]: frame["branch"] for frame in left.get("conditionFrames", [])}
    right_frames = {frame["id"]: frame["branch"] for frame in right.get("conditionFrames", [])}
    return any(left_frames[key] != right_frames[key] for key in left_frames.keys() & right_frames.keys())


def _source_label(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def extract_cmake_options_text(text: str, source: str = "<memory>") -> list[dict[str, Any]]:
    """Return every option() declaration in a pure text input."""
    declarations: list[dict[str, Any]] = []
    for command in _commands_with_conditions(text, source):
        if command["name"] != "option":
            continue
        args = _tokenize_cmake_arguments(command["body"])
        if len(args) not in (2, 3):
            raise InventoryError(
                f"{source}:{command['line']}: option() requires variable, help text, and optional default"
            )
        name, description = args[:2]
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise InventoryError(f"{source}:{command['line']}: unsupported option name {name!r}")
        default = args[2] if len(args) == 3 else "OFF"
        upper = default.upper()
        if upper in _BOOL_TRUE:
            default = "ON"
        elif upper in _BOOL_FALSE:
            default = "OFF"
        declarations.append(
            {
                "name": name,
                "description": description,
                "default": default,
                "file": source,
                "line": command["line"],
                "conditionFrames": command["conditionFrames"],
            }
        )
    return sorted(declarations, key=lambda item: (item["name"], item["file"], item["line"]))


def extract_cmake_options(path: Path | None = None) -> list[dict[str, Any]]:
    """Return every root option() declaration with location and branch metadata."""
    source_path = path or CMAKE_ROOT
    return extract_cmake_options_text(
        source_path.read_text(encoding="utf-8"), _source_label(source_path)
    )


def _evaluate_default(value: Any, context: dict[str, bool]) -> bool | str:
    if isinstance(value, bool):
        return value
    text = str(value)
    variable = re.fullmatch(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", text)
    if variable:
        name = variable.group(1).upper()
        return bool(context[name]) if name in context else f"unresolved:{text}"
    upper = text.upper()
    if upper in _BOOL_TRUE:
        return True
    if upper in _BOOL_FALSE:
        return False
    return f"unresolved:{text}"


def effective_cmake_options(
    declarations: list[dict[str, Any]], context: dict[str, bool] | None = None
) -> list[dict[str, Any]]:
    context = context or WINDOWS_CONTEXT
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for declaration in declarations:
        grouped[declaration["name"]].append(declaration)
    effective: list[dict[str, Any]] = []
    for name, group in sorted(grouped.items()):
        if len(group) == 1:
            active = group
        else:
            try:
                active = [item for item in group if _declaration_is_active(item, context)]
            except InventoryError:
                active = []
        if len(active) == 1:
            default: bool | str = _evaluate_default(active[0]["default"], context)
            description = active[0]["description"]
        else:
            default = "unresolved"
            description = group[0]["description"]
        effective.append(
            {
                "name": name,
                "description": description,
                "default": default,
                "declarationCount": len(group),
                "activeDeclarationCount": len(active),
            }
        )
    return effective


def extract_cmake_presets(path: Path | None = None) -> dict[str, Any]:
    data = json.loads((path or PRESETS_PATH).read_text(encoding="utf-8"))
    configure = []
    for preset in data.get("configurePresets", []):
        if not preset.get("name"):
            raise InventoryError("configure preset without a name")
        entry: dict[str, Any] = {"name": preset["name"], "hidden": preset.get("hidden", False)}
        for key in ("displayName", "inherits", "generator", "architecture", "toolset", "condition"):
            if key in preset:
                entry[key] = preset[key]
        if "cacheVariables" in preset:
            entry["cacheVariables"] = dict(sorted(preset["cacheVariables"].items()))
        configure.append(entry)

    def dependent(kind: str) -> list[dict[str, Any]]:
        result = []
        for preset in data.get(kind, []):
            if not preset.get("name") or not preset.get("configurePreset"):
                raise InventoryError(f"{kind} entry lacks name/configurePreset")
            entry = {"name": preset["name"], "configurePreset": preset["configurePreset"]}
            if "configuration" in preset:
                entry["configuration"] = preset["configuration"]
            result.append(entry)
        return result

    return {
        "configurePresets": configure,
        "buildPresets": dependent("buildPresets"),
        "testPresets": dependent("testPresets"),
    }


def resolve_configure_preset(presets: dict[str, Any], name: str) -> dict[str, Any]:
    by_name = {preset["name"]: preset for preset in presets.get("configurePresets", [])}
    if name not in by_name:
        raise InventoryError(f"configure preset {name!r} does not exist")

    def resolve(current: str, path: tuple[str, ...]) -> dict[str, Any]:
        if current in path:
            raise InventoryError(f"configure preset inheritance cycle: {' -> '.join((*path, current))}")
        preset = by_name.get(current)
        if preset is None:
            raise InventoryError(f"configure preset {current!r} does not exist")
        merged: dict[str, Any] = {"cacheVariables": {}}
        parents = preset.get("inherits", [])
        if isinstance(parents, str):
            parents = [parents]
        if not isinstance(parents, list):
            raise InventoryError(f"configure preset {current!r} has invalid inherits value")
        for parent in parents:
            inherited = resolve(parent, (*path, current))
            merged.update({key: value for key, value in inherited.items() if key != "cacheVariables"})
            merged["cacheVariables"].update(inherited.get("cacheVariables", {}))
        merged.update({key: value for key, value in preset.items() if key not in {"cacheVariables", "inherits"}})
        merged["cacheVariables"].update(preset.get("cacheVariables", {}))
        merged["name"] = current
        return merged

    return resolve(name, ())


def _tracked_cmake_lists() -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-files", "--", "*CMakeLists.txt"],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise InventoryError(f"git ls-files failed while enumerating CMake inputs: {result.stderr.strip()}")
    paths = []
    for relative in result.stdout.splitlines():
        normalized = relative.replace("\\", "/")
        if normalized.startswith("ThirdParty/"):
            continue
        paths.append(REPO_ROOT / Path(normalized))
    return sorted(paths, key=lambda path: path.relative_to(REPO_ROOT).as_posix())


def extract_cmake_targets(paths: Iterable[Path] | None = None) -> list[dict[str, Any]]:
    declarations: list[dict[str, Any]] = []
    for cmake_file in paths or _tracked_cmake_lists():
        source = _source_label(cmake_file)
        text = cmake_file.read_text(encoding="utf-8", errors="strict")
        for command in _commands_with_conditions(text, source):
            if command["name"] not in {"add_executable", "add_library"}:
                continue
            args = _tokenize_cmake_arguments(command["body"])
            if not args:
                raise InventoryError(f"{source}:{command['line']}: empty {command['spelling']}()")
            target = args[0]
            if "${" in target or "$<" in target:
                continue
            if command["name"] == "add_executable":
                kind = "executable"
            else:
                library_type = args[1].upper() if len(args) > 1 else ""
                kind = {
                    "STATIC": "static_library",
                    "SHARED": "shared_library",
                    "MODULE": "module_library",
                    "OBJECT": "object_library",
                    "INTERFACE": "interface_library",
                    "UNKNOWN": "unknown_library",
                }.get(library_type, "library")
            declarations.append(
                {
                    "target": target,
                    "kind": kind,
                    "file": source,
                    "line": command["line"],
                    "conditionFrames": command["conditionFrames"],
                }
            )
    return sorted(declarations, key=lambda item: (item["target"], item["file"], item["line"]))


def aggregate_cmake_targets(declarations: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for declaration in declarations:
        grouped[declaration["target"]].append(declaration)
    result = []
    for target, group in sorted(grouped.items()):
        kinds = sorted({item["kind"] for item in group})
        result.append(
            {
                "target": target,
                "kind": kinds[0] if len(kinds) == 1 else "ambiguous",
                "declarations": [
                    {"file": item["file"], "line": item["line"], "kind": item["kind"]} for item in group
                ],
            }
        )
    return result


_SPARKBUILD_OPTION_RE = re.compile(
    r'\{"([A-Za-z_][A-Za-z0-9_]*)",\s*"((?:\\.|[^"\\])*)",\s*'
    r'"((?:\\.|[^"\\])*)",\s*(true|false),\s*(true|false),\s*'
    r'OptionCategory::([A-Za-z_][A-Za-z0-9_]*)\}',
    re.DOTALL,
)


def extract_sparkbuild_options(path: Path | None = None) -> list[dict[str, Any]]:
    source_path = path or SPARKBUILD_CONFIG
    text = source_path.read_text(encoding="utf-8")
    expected = len(re.findall(r"config\.options\.push_back\s*\(", text))
    matches = list(_SPARKBUILD_OPTION_RE.finditer(text))
    if len(matches) != expected:
        raise InventoryError(
            f"{_source_label(source_path)}: parsed {len(matches)} of {expected} SparkBuild option declarations"
        )
    results = []
    for match in matches:
        name, display, description, default_value, _current, category = match.groups()
        results.append(
            {
                "name": name,
                "displayName": display,
                "description": description,
                "default": default_value == "true",
                "category": category,
                "file": _source_label(source_path),
                "line": _line_number(text, match.start()),
            }
        )
    return sorted(results, key=lambda item: (item["name"], item["line"]))


def load_stable_profile_data(data: dict[str, Any]) -> dict[str, Any]:
    """Validate and normalize the stable-v1 build contract from parsed JSON."""
    profiles = [profile for profile in data.get("releaseProfiles", []) if profile.get("id") == "stable-v1"]
    if len(profiles) != 1:
        raise InventoryError("canonical readiness contract must contain exactly one stable-v1 profile")
    profile = profiles[0]
    included = set(profile.get("includedCapabilityIds", []))
    configurations = profile.get("buildConfigurations", [])
    products = profile.get("buildProducts", [])
    if not configurations or not products:
        raise InventoryError("stable-v1 must declare buildConfigurations and buildProducts")
    config_ids = [item.get("id") for item in configurations]
    if any(not item for item in config_ids) or len(config_ids) != len(set(config_ids)):
        raise InventoryError("stable-v1 build configuration IDs must be nonempty and unique")
    purposes = [item.get("purpose") for item in configurations]
    required_purposes = {"shipping", "validation", "installed-sdk-consumer"}
    if set(purposes) != required_purposes or len(purposes) != len(required_purposes):
        raise InventoryError("stable-v1 must declare shipping, validation, and installed-sdk-consumer exactly once")
    for configuration in configurations:
        if not configuration.get("description"):
            raise InventoryError(f"stable-v1 build configuration {configuration.get('id')!r} lacks description")
        if not isinstance(configuration.get("configuration"), str) or not configuration.get("configuration"):
            raise InventoryError(
                f"stable-v1 build configuration {configuration.get('id')!r} lacks codemodel configuration name"
            )
        if configuration.get("purpose") in {"shipping", "validation"} and not configuration.get("preset"):
            raise InventoryError(f"stable-v1 build configuration {configuration['id']!r} lacks a preset")
        if configuration.get("purpose") == "installed-sdk-consumer" and configuration.get("preset"):
            raise InventoryError("installed-sdk-consumer must not use a source-tree preset")
    seen_targets: set[tuple[str, str]] = set()
    target_names: set[str] = set()
    covered: set[str] = set()
    for product in products:
        target = product.get("target")
        build_profile = product.get("buildProfile")
        key = (str(build_profile), str(target))
        if not target or build_profile not in config_ids or key in seen_targets:
            raise InventoryError(f"invalid or duplicate stable-v1 build product {key}")
        seen_targets.add(key)
        if target in target_names:
            raise InventoryError(f"stable-v1 target {target!r} belongs to more than one build profile")
        target_names.add(target)
        if product.get("kind") not in set(_TARGET_KIND_MAP.values()):
            raise InventoryError(f"stable-v1 product {target!r} has invalid kind")
        if product.get("applicability") not in {"required", "shared"}:
            raise InventoryError(f"stable-v1 product {target!r} must be required or shared")
        capability_ids = set(product.get("capabilityIds", []))
        if not capability_ids or not capability_ids.issubset(included):
            raise InventoryError(f"stable-v1 product {target!r} references out-of-profile capabilities")
        covered.update(capability_ids)
        if not isinstance(product.get("requiredOptions"), dict):
            raise InventoryError(f"stable-v1 product {target!r} requiredOptions must be an object")
        for option_name, option_value in product["requiredOptions"].items():
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", str(option_name)):
                raise InventoryError(f"stable-v1 product {target!r} has invalid required option name")
            if str(option_value).upper() not in {"ON", "OFF"}:
                raise InventoryError(f"stable-v1 product {target!r} required options must be ON/OFF")
    if covered != included:
        raise InventoryError(
            "stable-v1 build-product capabilities must exactly equal included capabilities; "
            f"missing={sorted(included - covered)} outside={sorted(covered - included)}"
        )
    first_party = set(profile.get("firstPartyGameCapabilityIds", []))
    if not any(first_party.intersection(product.get("capabilityIds", [])) for product in products):
        raise InventoryError("stable-v1 build products omit the first-party game")
    if not any(
        product.get("target") == "SparkGameFPS"
        and first_party.intersection(product.get("capabilityIds", []))
        for product in products
    ):
        raise InventoryError("stable-v1 must map its first-party capability to SparkGameFPS")
    consumer_ids = {
        item["id"] for item in configurations if item.get("purpose") == "installed-sdk-consumer"
    }
    if not any(product.get("buildProfile") in consumer_ids for product in products):
        raise InventoryError("stable-v1 build products omit an installed public-SDK consumer")
    option_applicability = profile.get("buildOptionApplicability")
    if not isinstance(option_applicability, list):
        raise InventoryError("stable-v1 buildOptionApplicability must be an explicit list")
    exception_names: set[str] = set()
    for entry in option_applicability:
        name = entry.get("name")
        if (
            not isinstance(name, str)
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name)
            or name in exception_names
            or entry.get("applicability") not in {"outside", "shared"}
            or not entry.get("reason")
        ):
            raise InventoryError(f"invalid build option applicability exception {entry!r}")
        exception_names.add(name)
    return {
        "id": profile["id"],
        "includedCapabilityIds": sorted(included),
        "buildConfigurations": sorted(configurations, key=lambda item: item["id"]),
        "buildProducts": sorted(products, key=lambda item: (item["buildProfile"], item["target"])),
        "optionApplicability": sorted(
            option_applicability, key=lambda item: item.get("name", "")
        ),
    }


def load_stable_profile(path: Path | None = None) -> dict[str, Any]:
    data = json.loads((path or READINESS_PATH).read_text(encoding="utf-8"))
    return load_stable_profile_data(data)


def _extract_run_blocks_text(text: str) -> list[dict[str, str]]:
    lines = text.splitlines()
    blocks: list[dict[str, str]] = []
    in_jobs = False
    current_job = ""
    current_step = ""
    current_shell = "bash"
    index = 0
    while index < len(lines):
        line = lines[index]
        if re.match(r"^jobs:\s*(?:#.*)?$", line):
            in_jobs = True
            index += 1
            continue
        if in_jobs and re.match(r"^[^\s]", line) and line.strip():
            in_jobs = False
        job_match = re.match(r"^  ([A-Za-z0-9_-]+):\s*(?:#.*)?$", line) if in_jobs else None
        if job_match:
            current_job = job_match.group(1)
            current_step = ""
            current_shell = "bash"
            index += 1
            continue
        step_match = re.match(r"^    - name:\s*(.+?)\s*$", line)
        if step_match:
            current_step = step_match.group(1).strip('"\'')
            current_shell = "bash"
            index += 1
            continue
        shell_match = re.match(r"^      shell:\s*(\S+)", line)
        if shell_match:
            current_shell = shell_match.group(1)
            index += 1
            continue
        run_match = re.match(r"^(\s*)(?:-\s+)?run:\s*(.*)$", line)
        if not (in_jobs and current_job and run_match):
            index += 1
            continue
        indentation = len(run_match.group(1))
        marker = run_match.group(2).strip()
        if marker in {"|", "|-", "|+", ">", ">-", ">+"}:
            index += 1
            collected: list[str] = []
            while index < len(lines):
                candidate = lines[index]
                if candidate.strip() and len(candidate) - len(candidate.lstrip()) <= indentation:
                    break
                collected.append(candidate)
                index += 1
            nonblank_indents = [len(value) - len(value.lstrip()) for value in collected if value.strip()]
            trim = min(nonblank_indents) if nonblank_indents else indentation + 2
            content_lines = [value[trim:] if len(value) >= trim else "" for value in collected]
            text = " ".join(value.strip() for value in content_lines) if marker.startswith(">") else "\n".join(content_lines)
        else:
            if marker.startswith("*") or marker.startswith("${{"):
                raise InventoryError(f"{current_job}/{current_step or 'unnamed'}: dynamic or aliased run scalar is unsupported")
            if marker.startswith('"'):
                try:
                    decoded = json.loads(marker)
                except json.JSONDecodeError as error:
                    raise InventoryError(
                        f"{current_job}/{current_step or 'unnamed'}: invalid quoted run scalar: {error}"
                    ) from error
                if not isinstance(decoded, str):
                    raise InventoryError(f"{current_job}/{current_step or 'unnamed'}: run scalar must be text")
                text = decoded
            elif marker.startswith("'"):
                if len(marker) < 2 or not marker.endswith("'"):
                    raise InventoryError(f"{current_job}/{current_step or 'unnamed'}: unterminated quoted run scalar")
                text = marker[1:-1].replace("''", "'")
            else:
                text = marker
            index += 1
        blocks.append({"job": current_job, "step": current_step or "unnamed", "shell": current_shell, "run": text})
    return blocks


def _extract_run_blocks(path: Path) -> list[dict[str, str]]:
    return _extract_run_blocks_text(path.read_text(encoding="utf-8"))


def _logical_shell_lines(script: str) -> list[str]:
    lines: list[str] = []
    pending = ""
    for raw in script.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.endswith("\\"):
            pending += stripped[:-1] + " "
        else:
            lines.append(pending + stripped)
            pending = ""
    if pending:
        raise InventoryError("shell run block ends with an unterminated line continuation")
    return lines


def _shell_tokens(command: str) -> list[str]:
    protected = re.sub(
        r"\$\{\{.*?\}\}", lambda match: match.group(0).replace(" ", "__GH_SPACE__"), command
    )
    try:
        lexer = shlex.shlex(protected, posix=True, punctuation_chars="|&;")
        lexer.whitespace_split = True
        lexer.commenters = "#"
        return [token.replace("__GH_SPACE__", " ") for token in lexer]
    except ValueError as error:
        raise InventoryError(f"cannot lex workflow command {command!r}: {error}") from error


def _parse_configure_segment(tokens: list[str], job: str, step: str, command: str) -> dict[str, Any] | None:
    if "cmake" not in tokens:
        return None
    command_boundaries = {"|", "||", "&&", ";", "&", "then", "do", "if", "!"}
    starts = [
        index
        for index, token in enumerate(tokens)
        if token == "cmake" and (index == 0 or tokens[index - 1] in command_boundaries)
    ]
    if not starts:
        installers = {"apt", "apt-get", "brew", "dnf", "yum", "choco", "winget"}
        if any(token in installers for token in tokens) and "install" in tokens:
            return None
        raise InventoryError(f"{job}/{step}: unparsed command containing cmake: {command}")
    if len(starts) != 1:
        raise InventoryError(f"{job}/{step}: multiple cmake commands on one shell line are unsupported: {command}")
    start = starts[0]
    end = len(tokens)
    for index in range(start + 1, len(tokens)):
        if tokens[index] in {"|", "||", "&&", ";", "&"}:
            end = index
            break
    args = tokens[start + 1:end]
    if not args:
        raise InventoryError(f"{job}/{step}: empty cmake command")
    non_configure = {
        "--build", "--install", "--open", "--workflow", "--find-package",
        "-P", "-E", "--version", "--help", "--list-presets",
    }
    if any(argument in non_configure for argument in args):
        return None
    if not any(
        argument == "-B" or argument.startswith("-B") or argument == "--preset" or argument.startswith("--preset=")
        for argument in args
    ):
        raise InventoryError(f"{job}/{step}: unparsed configure-looking cmake command: {command}")

    result: dict[str, Any] = {"job": job, "step": step, "command": command, "options": {}}
    index = 0
    while index < len(args):
        argument = args[index]
        if argument in {"-B", "-S", "-G", "-A", "-T", "--preset"}:
            if index + 1 >= len(args):
                raise InventoryError(f"{job}/{step}: {argument} lacks a value")
            key = {
                "-B": "buildDir",
                "-S": "sourceDir",
                "-G": "generator",
                "-A": "architecture",
                "-T": "toolset",
                "--preset": "preset",
            }[argument]
            result[key] = args[index + 1]
            index += 2
            continue
        if argument.startswith("--preset="):
            result["preset"] = argument.split("=", 1)[1]
        elif argument == "--fresh":
            result["fresh"] = True
        elif argument.startswith("-B") and len(argument) > 2:
            result["buildDir"] = argument[2:]
        elif argument.startswith("-S") and len(argument) > 2:
            result["sourceDir"] = argument[2:]
        elif argument == "-D":
            if index + 1 >= len(args):
                raise InventoryError(f"{job}/{step}: -D lacks a cache assignment")
            assignment = args[index + 1]
            if "=" not in assignment:
                raise InventoryError(f"{job}/{step}: CMake -D argument lacks '=': {assignment}")
            name, value = assignment.split("=", 1)
            name = name.split(":", 1)[0]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                raise InventoryError(f"{job}/{step}: unsupported CMake cache variable {name!r}")
            result["options"][name] = value
            index += 2
            continue
        elif argument.startswith("-D"):
            assignment = argument[2:]
            if "=" not in assignment:
                raise InventoryError(f"{job}/{step}: CMake -D argument lacks '=': {argument}")
            name, value = assignment.split("=", 1)
            name = name.split(":", 1)[0]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                raise InventoryError(f"{job}/{step}: unsupported CMake cache variable {name!r}")
            result["options"][name] = value
        index += 1
    return result


def extract_workflow_cmake_configs_text(text: str) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    for block in _extract_run_blocks_text(text):
        for command in _logical_shell_lines(block["run"]):
            tokens = _shell_tokens(command)
            parsed = _parse_configure_segment(tokens, block["job"], block["step"], command)
            if parsed is not None:
                results.append(parsed)
    return results


def extract_workflow_cmake_configs(path: Path | None = None) -> list[dict[str, Any]]:
    workflow = path or WORKFLOW_PATH
    return extract_workflow_cmake_configs_text(workflow.read_text(encoding="utf-8"))


def extract_workflow_presets(path: Path | None = None) -> list[str]:
    return sorted(
        {
            config["preset"]
            for config in extract_workflow_cmake_configs(path)
            if config.get("preset") and "${{" not in config["preset"]
        }
    )


def parse_codemodel_targets(
    profile: str,
    codemodel: dict[str, Any],
    target_documents: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    """Parse configured target evidence from already loaded File API documents."""
    targets: dict[tuple[str, str], dict[str, str]] = {}
    for configuration in codemodel.get("configurations", []):
        config_name = configuration.get("name", "")
        for reference in configuration.get("targets", []):
            target_file = reference.get("jsonFile")
            if not target_file:
                raise InventoryError(f"{profile}: codemodel target {reference.get('name')!r} has no jsonFile")
            target = target_documents.get(target_file)
            if target is None:
                raise InventoryError(f"{profile}: codemodel target reply {target_file!r} is missing")
            cmake_type = target.get("type")
            kind = _TARGET_KIND_MAP.get(cmake_type)
            if not kind:
                raise InventoryError(f"{profile}: unsupported codemodel target type {cmake_type!r}")
            key = (reference["name"], config_name)
            value = {"target": reference["name"], "kind": kind, "configuration": config_name}
            if key in targets:
                raise InventoryError(f"{profile}: duplicate codemodel target {key}")
            targets[key] = value
    return {
        "profile": profile,
        "status": "available",
        "targets": sorted(targets.values(), key=lambda item: (item["target"], item["configuration"])),
    }


def extract_codemodel_targets(build_dir: Path, profile: str) -> dict[str, Any]:
    reply_dir = build_dir / ".cmake" / "api" / "v1" / "reply"
    if not reply_dir.is_dir():
        return {"profile": profile, "status": "absent", "targets": []}
    indices = sorted(reply_dir.glob("index-*.json"))
    if not indices:
        raise InventoryError(f"{profile}: CMake File API reply has no index")
    index = json.loads(indices[-1].read_text(encoding="utf-8"))
    codemodel_file = (index.get("reply", {}).get("codemodel-v2") or {}).get("jsonFile")
    if not codemodel_file:
        for obj in index.get("objects", []):
            if obj.get("kind") == "codemodel" and obj.get("version", {}).get("major") == 2:
                codemodel_file = obj.get("jsonFile")
                break
    if not codemodel_file:
        raise InventoryError(f"{profile}: CMake File API index has no codemodel-v2 reply")
    codemodel = json.loads((reply_dir / codemodel_file).read_text(encoding="utf-8"))
    target_documents: dict[str, dict[str, Any]] = {}
    for configuration in codemodel.get("configurations", []):
        for reference in configuration.get("targets", []):
            target_file = reference.get("jsonFile")
            if not target_file:
                raise InventoryError(f"{profile}: codemodel target {reference.get('name')!r} has no jsonFile")
            if target_file not in target_documents:
                target_documents[target_file] = json.loads(
                    (reply_dir / target_file).read_text(encoding="utf-8")
                )
    return parse_codemodel_targets(profile, codemodel, target_documents)


def build_inventory(codemodels: dict[str, Path] | None = None) -> dict[str, Any]:
    profile = load_stable_profile()
    option_declarations = extract_cmake_options()
    target_declarations = extract_cmake_targets()
    requested = codemodels or {}
    unknown_codemodels = sorted(set(requested) - {item["id"] for item in profile["buildConfigurations"]})
    if unknown_codemodels:
        raise InventoryError(f"codemodel evidence names unknown profiles: {unknown_codemodels}")
    codemodel_evidence = []
    for configuration in profile["buildConfigurations"]:
        identifier = configuration["id"]
        if identifier in requested:
            codemodel_evidence.append(extract_codemodel_targets(requested[identifier], identifier))
        else:
            codemodel_evidence.append({"profile": identifier, "status": "absent", "targets": []})
    workflow_configs = extract_workflow_cmake_configs()
    return {
        "schemaVersion": 2,
        "profile": profile,
        "cmakeOptionDeclarations": option_declarations,
        "cmakeOptions": effective_cmake_options(option_declarations),
        "cmakePresets": extract_cmake_presets(),
        "cmakeTargetDeclarations": target_declarations,
        "cmakeTargets": aggregate_cmake_targets(target_declarations),
        "configuredTargetEvidence": codemodel_evidence,
        "sparkBuildOptions": extract_sparkbuild_options(),
        "workflowPresets": sorted({item["preset"] for item in workflow_configs if item.get("preset")}),
        "workflowCmakeConfigs": workflow_configs,
        "stableV1Products": profile["buildProducts"],
    }


def render_inventory(inventory: dict[str, Any]) -> bytes:
    return (json.dumps(inventory, indent=2, sort_keys=False) + "\n").encode("utf-8")


def _parse_codemodel_args(values: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise InventoryError("--codemodel expects PROFILE=BUILD_DIR")
        profile, directory = value.split("=", 1)
        if not profile or not directory or profile in result:
            raise InventoryError(f"invalid duplicate --codemodel value {value!r}")
        result[profile] = Path(directory)
    return result


def _write_atomic(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(payload)
    temporary.replace(path)


def render_internal_error(error: Exception) -> bytes:
    return (
        json.dumps(
            {"schemaVersion": 2, "state": "internal-error", "internalError": str(error)},
            indent=2,
            sort_keys=False,
        )
        + "\n"
    ).encode("utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codemodel", action="append", default=[], metavar="PROFILE=BUILD_DIR")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", type=Path, help="fail if generated bytes differ from this file")
    args = parser.parse_args(argv)
    try:
        if args.output and args.check and args.output.resolve() == args.check.resolve():
            raise InventoryError("--output and --check must name distinct paths")
        payload = render_inventory(build_inventory(_parse_codemodel_args(args.codemodel)))
        if args.output:
            _write_atomic(args.output, payload)
        else:
            sys.stdout.buffer.write(payload)
        if args.check and (not args.check.is_file() or args.check.read_bytes() != payload):
            print(f"inventory drift: regenerate {args.check}", file=sys.stderr)
            return 1
        return 0
    except (InventoryError, OSError, ValueError, json.JSONDecodeError) as error:
        sys.stdout.buffer.write(render_internal_error(error))
        print(f"INTERNAL ERROR: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
