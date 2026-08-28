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
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import workflow as workflow_tool


REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE_ROOT = REPO_ROOT / "CMakeLists.txt"
PRESETS_PATH = REPO_ROOT / "CMakePresets.json"
SPARKBUILD_CONFIG = REPO_ROOT / "SparkBuild" / "src" / "Config.cpp"
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "build.yml"
READINESS_PATH = REPO_ROOT / "docs" / "site" / "readiness.json"

# Platform predicates and string variables that are *knowable* for the canonical
# Windows/MSVC stable-v1 profile. Anything absent here is deliberately treated as
# indeterminate rather than silently assumed, so a declaration guarded by an
# unmodelled condition can never be quietly promoted to "active".
WINDOWS_CONTEXT: dict[str, bool | str] = {
    "WIN32": True,
    "MSVC": True,
    "UNIX": False,
    "APPLE": False,
    "LINUX": False,
    "MINGW": False,
    "MSYS": False,
    "CYGWIN": False,
    "ANDROID": False,
    "IOS": False,
    "EMSCRIPTEN": False,
    "BORLAND": False,
    "WATCOM": False,
    "CMAKE_C_COMPILER_ID": "MSVC",
    "CMAKE_CXX_COMPILER_ID": "MSVC",
    "CMAKE_SYSTEM_NAME": "Windows",
    "CMAKE_HOST_SYSTEM_NAME": "Windows",
    "CMAKE_HOST_WIN32": True,
    "CMAKE_HOST_UNIX": False,
    "CMAKE_HOST_APPLE": False,
}

ACTIVE = "active"
INACTIVE = "inactive"
INDETERMINATE = "indeterminate"

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
    definitions: list[dict[str, Any]] = []
    for command in _iter_cmake_commands(text, source):
        name = command["name"]
        body = command["body"].strip()
        if name in {"function", "macro"}:
            arguments = _tokenize_cmake_arguments(command["body"])
            definitions.append(
                {
                    "kind": name,
                    "name": arguments[0] if arguments else "",
                    "parameters": arguments[1:],
                    "line": command["line"],
                }
            )
            continue
        if name in {"endfunction", "endmacro"}:
            if not definitions:
                raise InventoryError(f"{source}:{command['line']}: {name}() without a definition")
            definitions.pop()
            continue
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
        command["definitionScope"] = [
            {"kind": item["kind"], "name": item["name"], "parameters": list(item["parameters"])}
            for item in definitions
        ]
        yield command
    if stack:
        raise InventoryError(f"{source}: unterminated if() block")
    if definitions:
        raise InventoryError(f"{source}: unterminated {definitions[-1]['kind']}() definition")


def _condition_tokens(expression: str) -> list[tuple[str, bool]]:
    """Lex a CMake if() body into (text, was_quoted) tokens.

    Parentheses become their own tokens only when unquoted, so
    ``MATCHES "GNU|Clang"`` keeps its regex intact while ``(A OR B)`` still
    groups correctly.
    """
    tokens: list[tuple[str, bool]] = []
    index = 0
    length = len(expression)
    while index < length:
        char = expression[index]
        if char.isspace():
            index += 1
            continue
        if char in "()":
            tokens.append((char, False))
            index += 1
            continue
        bracket = _read_bracket(expression, index)
        if bracket:
            value, index = bracket
            tokens.append((value, True))
            continue
        if char == '"':
            index += 1
            value_chars: list[str] = []
            while index < length:
                if expression[index] == "\\" and index + 1 < length:
                    value_chars.append(expression[index + 1])
                    index += 2
                elif expression[index] == '"':
                    index += 1
                    break
                else:
                    value_chars.append(expression[index])
                    index += 1
            else:
                raise InventoryError(f"unterminated quoted token in CMake condition {expression!r}")
            tokens.append(("".join(value_chars), True))
            continue
        start = index
        while index < length and not expression[index].isspace() and expression[index] not in "()":
            index += 1
        tokens.append((expression[start:index], False))
    return tokens


# Binary if() operators the evaluator understands well enough to decide, or to
# honestly refuse. Anything else is a hard parse error, never a silent True.
_STRING_BINARY_OPERATORS = {"STREQUAL", "MATCHES", "EQUAL", "STRLESS", "STRGREATER"}
_UNDECIDABLE_BINARY_OPERATORS = {
    "VERSION_LESS", "VERSION_GREATER", "VERSION_EQUAL",
    "VERSION_LESS_EQUAL", "VERSION_GREATER_EQUAL",
    "LESS", "GREATER", "LESS_EQUAL", "GREATER_EQUAL",
    "STRLESS_EQUAL", "STRGREATER_EQUAL", "IN_LIST", "PATH_EQUAL",
    "IS_NEWER_THAN",
}
_UNARY_TESTS = {
    "DEFINED", "COMMAND", "POLICY", "TARGET", "TEST", "EXISTS",
    "IS_DIRECTORY", "IS_SYMLINK", "IS_ABSOLUTE",
}


def _constant_truth(text: str) -> bool | None:
    """CMake constant truthiness, or None when the text is not a constant."""
    upper = text.upper()
    if upper in _BOOL_TRUE:
        return True
    if upper in _BOOL_FALSE or upper.endswith("-NOTFOUND"):
        return False
    return None


def _variable_value(name: str, context: dict[str, bool | str]) -> bool | str | None:
    """Modelled value of a CMake variable, or None when unmodelled."""
    if name in context:
        return context[name]
    return context.get(name.upper())


def _dereference(token: str, quoted: bool, context: dict[str, bool | str]) -> str | None:
    """Operand value for a string comparison; None when unmodelled."""
    if quoted:
        return token
    value = _variable_value(token, context)
    if value is None:
        return token if _constant_truth(token) is not None else None
    if value is True:
        return "ON"
    if value is False:
        return "OFF"
    return str(value)


def _atom_truth(token: str, quoted: bool, context: dict[str, bool | str]) -> bool | None:
    if quoted:
        # if("string") is true only for a true constant.
        return bool(_constant_truth(token))
    constant = _constant_truth(token)
    if constant is not None:
        return constant
    value = _variable_value(token, context)
    if value is None:
        return None
    if isinstance(value, bool):
        return value
    return _constant_truth(value) is not False


def _evaluate_condition(expression: str, context: dict[str, bool | str] | None = None) -> bool | None:
    """Evaluate a CMake if() condition under Kleene three-valued logic.

    Returns True/False when the outcome is decided and None when the condition
    references something this inventory does not model. None is never coerced to
    a decision: callers surface it as an explicit indeterminate finding.
    Genuinely unparseable syntax raises InventoryError.
    """
    context = WINDOWS_CONTEXT if context is None else context
    tokens = _condition_tokens(expression)
    if not tokens:
        raise InventoryError(f"cannot evaluate empty CMake condition {expression!r}")
    position = 0

    def peek() -> tuple[str, bool] | None:
        return tokens[position] if position < len(tokens) else None

    def parse_atom() -> bool | None:
        nonlocal position
        head = peek()
        if head is None:
            raise InventoryError(f"incomplete CMake condition {expression!r}")
        token, quoted = head
        upper = token.upper()
        if not quoted and upper == "NOT":
            position += 1
            value = parse_atom()
            return None if value is None else not value
        if not quoted and token == "(":
            position += 1
            value = parse_or()
            closing = peek()
            if closing is None or closing[1] or closing[0] != ")":
                raise InventoryError(f"unbalanced CMake condition {expression!r}")
            position += 1
            return value
        if not quoted and upper in _UNARY_TESTS:
            position += 1
            operand = peek()
            if operand is None:
                raise InventoryError(f"{upper} lacks an operand in {expression!r}")
            position += 1
            if upper == "DEFINED":
                return True if _variable_value(operand[0], context) is not None else None
            return None
        position += 1
        following = peek()
        if following is not None and not following[1]:
            operator = following[0].upper()
            if operator in _UNDECIDABLE_BINARY_OPERATORS:
                position += 1
                if peek() is None:
                    raise InventoryError(f"{operator} lacks a right operand in {expression!r}")
                position += 1
                return None
            if operator in _STRING_BINARY_OPERATORS:
                position += 1
                right = peek()
                if right is None:
                    raise InventoryError(f"{operator} lacks a right operand in {expression!r}")
                position += 1
                left_value = _dereference(token, quoted, context)
                right_value = _dereference(right[0], right[1], context)
                if left_value is None or right_value is None:
                    return None
                if operator == "STREQUAL":
                    return left_value == right_value
                if operator == "MATCHES":
                    try:
                        return re.search(right_value, left_value) is not None
                    except re.error as error:
                        raise InventoryError(
                            f"invalid MATCHES regex {right_value!r} in {expression!r}: {error}"
                        ) from error
                if operator == "EQUAL":
                    try:
                        return float(left_value) == float(right_value)
                    except ValueError:
                        return None
                if operator == "STRLESS":
                    return left_value < right_value
                return left_value > right_value
        return _atom_truth(token, quoted, context)

    def parse_and() -> bool | None:
        nonlocal position
        value = parse_atom()
        while True:
            head = peek()
            if head is None or head[1] or head[0].upper() != "AND":
                return value
            position += 1
            rhs = parse_atom()
            if value is False or rhs is False:
                value = False
            elif value is None or rhs is None:
                value = None
            else:
                value = True

    def parse_or() -> bool | None:
        nonlocal position
        value = parse_and()
        while True:
            head = peek()
            if head is None or head[1] or head[0].upper() != "OR":
                return value
            position += 1
            rhs = parse_and()
            if value is True or rhs is True:
                value = True
            elif value is None or rhs is None:
                value = None
            else:
                value = False

    result = parse_or()
    if position != len(tokens):
        raise InventoryError(f"unsupported CMake condition syntax {expression!r}")
    return result


def _frame_activation(frame: dict[str, Any], context: dict[str, bool | str]) -> str:
    """Whether the branch a declaration sits in is taken, under Kleene logic."""
    branches: list[str | None] = frame["branches"]
    current = int(frame["branch"])
    for index, expression in enumerate(branches):
        if index == current:
            if expression is None:
                # An else() branch is reached only once every prior test failed,
                # and the loop below already proved each of those false.
                return ACTIVE
            value = _evaluate_condition(expression, context)
            return INDETERMINATE if value is None else (ACTIVE if value else INACTIVE)
        value = True if expression is None else _evaluate_condition(expression, context)
        if value is None:
            return INDETERMINATE
        if value:
            return INACTIVE
    return INACTIVE


def declaration_activation(
    declaration: dict[str, Any], context: dict[str, bool | str] | None = None
) -> str:
    """Classify a declaration as active/inactive/indeterminate for a context.

    A declaration nested in a branch the context does not take is INACTIVE even
    when it is the only declaration of its name. That is precisely the case that
    used to turn a non-MSVC-only option into a false Windows blocker.
    """
    context = WINDOWS_CONTEXT if context is None else context
    verdicts = {_frame_activation(frame, context) for frame in declaration.get("conditionFrames", [])}
    if INACTIVE in verdicts:
        return INACTIVE
    if INDETERMINATE in verdicts:
        return INDETERMINATE
    return ACTIVE


def _declaration_is_active(declaration: dict[str, Any], context: dict[str, bool | str]) -> bool:
    return declaration_activation(declaration, context) == ACTIVE


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
                "definitionScope": command.get("definitionScope", []),
            }
        )
    return sorted(declarations, key=lambda item: (item["name"], item["file"], item["line"]))


def extract_cmake_options(path: Path | None = None) -> list[dict[str, Any]]:
    """Return every root option() declaration with location and branch metadata."""
    source_path = path or CMAKE_ROOT
    return extract_cmake_options_text(
        source_path.read_text(encoding="utf-8"), _source_label(source_path)
    )


def extract_all_cmake_options(paths: Iterable[Path] | None = None) -> list[dict[str, Any]]:
    """Every option() across all tracked CMake inputs, root and subdirectory alike.

    The root file is the parity surface SparkBuild must mirror; subdirectory and
    module options are still recorded so they cannot be configuration surface
    that nothing ever sees.
    """
    declarations: list[dict[str, Any]] = []
    for cmake_file in paths if paths is not None else _tracked_cmake_inputs():
        declarations.extend(
            extract_cmake_options_text(
                cmake_file.read_text(encoding="utf-8"), _source_label(cmake_file)
            )
        )
    return sorted(declarations, key=lambda item: (item["name"], item["file"], item["line"]))


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
    declarations: list[dict[str, Any]], context: dict[str, bool | str] | None = None
) -> list[dict[str, Any]]:
    """Collapse option declarations to their effective value for one context.

    Every declaration's branch is evaluated, including when a name has only one
    declaration. An option whose sole declaration sits in a branch this context
    does not take is reported ``platform-inactive`` and is not part of the
    context's configuration surface at all -- it is neither a default mismatch
    nor a missing-from-SparkBuild blocker.
    """
    context = WINDOWS_CONTEXT if context is None else context
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for declaration in declarations:
        grouped[declaration["name"]].append(declaration)
    effective: list[dict[str, Any]] = []
    for name, group in sorted(grouped.items()):
        activations = [declaration_activation(item, context) for item in group]
        active = [item for item, state in zip(group, activations) if state == ACTIVE]
        indeterminate = [item for item, state in zip(group, activations) if state == INDETERMINATE]
        if len(active) == 1:
            status = ACTIVE
            default: bool | str = _evaluate_default(active[0]["default"], context)
            description = active[0]["description"]
        elif not active and indeterminate:
            status = INDETERMINATE
            default = "unresolved"
            description = indeterminate[0]["description"]
        elif not active:
            status = "platform-inactive"
            default = "inactive"
            description = group[0]["description"]
        else:
            status = "ambiguous"
            default = "unresolved"
            description = group[0]["description"]
        if active and indeterminate:
            status = INDETERMINATE if status == ACTIVE else status
        effective.append(
            {
                "name": name,
                "description": description,
                "default": default,
                "status": status,
                "declarationCount": len(group),
                "activeDeclarationCount": len(active),
                "indeterminateDeclarationCount": len(indeterminate),
                "declaredIn": sorted({item["file"] for item in group}),
            }
        )
    return effective


# Fields a preset never inherits, per cmake-presets(7).
_NON_INHERITED_PRESET_FIELDS = {"name", "hidden", "inherits", "description", "displayName"}

# Configure-preset fields the inventory binds. `binaryDir` is material: it is the
# build directory that codemodel evidence must have been produced in.
_CONFIGURE_PRESET_FIELDS = (
    "displayName", "description", "inherits", "generator", "architecture", "toolset",
    "condition", "binaryDir", "installDir", "toolchainFile", "environment",
)
_BUILD_PRESET_FIELDS = (
    "displayName", "inherits", "configuration", "targets", "jobs",
    "nativeToolOptions", "cleanFirst", "verbose", "condition", "environment",
)
_TEST_PRESET_FIELDS = (
    "displayName", "inherits", "configuration", "output", "execution", "filter",
    "condition", "environment",
)


def _normalize_inherits(value: Any, label: str) -> list[str]:
    """CMake treats a bare string as a one-element inherits list."""
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    if isinstance(value, list) and all(isinstance(item, str) for item in value):
        return list(value)
    raise InventoryError(f"{label} has invalid inherits value {value!r}")


def extract_cmake_presets(path: Path | None = None) -> dict[str, Any]:
    data = json.loads((path or PRESETS_PATH).read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise InventoryError("CMakePresets.json must contain an object")
    configure = []
    seen_configure: set[str] = set()
    for preset in data.get("configurePresets", []):
        if not preset.get("name"):
            raise InventoryError("configure preset without a name")
        if preset["name"] in seen_configure:
            raise InventoryError(f"duplicate configure preset name {preset['name']!r}")
        seen_configure.add(preset["name"])
        entry: dict[str, Any] = {"name": preset["name"], "hidden": bool(preset.get("hidden", False))}
        for key in _CONFIGURE_PRESET_FIELDS:
            if key in preset:
                entry[key] = _normalize_inherits(preset[key], f"configure preset {preset['name']!r}") if key == "inherits" else preset[key]
        if "cacheVariables" in preset:
            if not isinstance(preset["cacheVariables"], dict):
                raise InventoryError(f"configure preset {preset['name']!r} has non-object cacheVariables")
            entry["cacheVariables"] = dict(sorted(preset["cacheVariables"].items()))
        configure.append(entry)

    def dependent(kind: str, fields: tuple[str, ...]) -> list[dict[str, Any]]:
        result = []
        seen: set[str] = set()
        for preset in data.get(kind, []):
            if not preset.get("name"):
                raise InventoryError(f"{kind} entry lacks a name")
            if preset["name"] in seen:
                raise InventoryError(f"duplicate {kind} name {preset['name']!r}")
            seen.add(preset["name"])
            inherits = _normalize_inherits(preset.get("inherits"), f"{kind} {preset['name']!r}")
            if not preset.get("configurePreset") and not inherits:
                raise InventoryError(f"{kind} entry {preset['name']!r} lacks name/configurePreset")
            entry: dict[str, Any] = {"name": preset["name"]}
            if preset.get("configurePreset"):
                entry["configurePreset"] = preset["configurePreset"]
            entry["hidden"] = bool(preset.get("hidden", False))
            for key in fields:
                if key in preset:
                    entry[key] = inherits if key == "inherits" else preset[key]
            result.append(entry)
        return result

    return {
        "presetsVersion": data.get("version"),
        "configurePresets": configure,
        "buildPresets": dependent("buildPresets", _BUILD_PRESET_FIELDS),
        "testPresets": dependent("testPresets", _TEST_PRESET_FIELDS),
        "packagePresets": dependent("packagePresets", _BUILD_PRESET_FIELDS),
        "workflowPresets": [
            {"name": preset.get("name"), "steps": preset.get("steps", [])}
            for preset in data.get("workflowPresets", [])
        ],
    }


def expand_preset_macros(value: str, preset_name: str, source_dir: str = "${sourceDir}") -> str:
    """Expand the preset-context macros the inventory must compare on.

    ``${presetName}`` is deliberately expanded in the *inheriting* preset's
    context, which is what CMake does; leaving it literal would make twenty
    presets that inherit one ``binaryDir`` look like a twenty-way collision.
    """
    return value.replace("${presetName}", preset_name).replace("${sourceDir}", source_dir)


def _resolve_preset_in(
    entries: list[dict[str, Any]], name: str, kind: str
) -> dict[str, Any]:
    """Resolve one preset's inheritance using CMake's documented precedence.

    cmake-presets(7): "If multiple ``inherits`` presets provide conflicting
    values for the same field, the earlier preset in the ``inherits`` array will
    be preferred."  Applying parents in reverse order and letting each later
    application overwrite therefore leaves the *earliest* parent's value in
    place.  ``name``/``hidden``/``inherits``/``description``/``displayName`` are
    never inherited.  A cacheVariable explicitly set to ``null`` is unset.
    """
    by_name = {preset["name"]: preset for preset in entries}
    if name not in by_name:
        raise InventoryError(f"{kind} preset {name!r} does not exist")

    def resolve(current: str, path: tuple[str, ...]) -> dict[str, Any]:
        if current in path:
            raise InventoryError(f"{kind} preset inheritance cycle: {' -> '.join((*path, current))}")
        preset = by_name.get(current)
        if preset is None:
            raise InventoryError(f"{kind} preset {current!r} does not exist")
        merged: dict[str, Any] = {"cacheVariables": {}}
        parents = _normalize_inherits(preset.get("inherits"), f"{kind} preset {current!r}")
        # Reversed: the last application wins, so the earliest parent survives.
        for parent in reversed(parents):
            inherited = resolve(parent, (*path, current))
            merged.update(
                {
                    key: value
                    for key, value in inherited.items()
                    if key != "cacheVariables" and key not in _NON_INHERITED_PRESET_FIELDS
                }
            )
            merged["cacheVariables"].update(inherited.get("cacheVariables", {}))
        merged.update(
            {key: value for key, value in preset.items() if key not in {"cacheVariables", "inherits"}}
        )
        merged["cacheVariables"].update(preset.get("cacheVariables", {}))
        merged["cacheVariables"] = {
            key: value for key, value in merged["cacheVariables"].items() if value is not None
        }
        merged["name"] = current
        merged["hidden"] = bool(preset.get("hidden", False))
        return merged

    return resolve(name, ())


def resolve_configure_preset(presets: dict[str, Any], name: str) -> dict[str, Any]:
    resolved = _resolve_preset_in(presets.get("configurePresets", []), name, "configure")
    binary_dir = resolved.get("binaryDir")
    if isinstance(binary_dir, str):
        resolved["resolvedBinaryDir"] = expand_preset_macros(binary_dir, name)
    return resolved


def resolve_dependent_preset(presets: dict[str, Any], kind: str, name: str) -> dict[str, Any]:
    """Resolve a build/test preset and the configure preset it is actually bound to."""
    entries = presets.get(kind, [])
    resolved = _resolve_preset_in(entries, name, kind)
    configure_name = resolved.get("configurePreset")
    if not configure_name:
        raise InventoryError(f"{kind} preset {name!r} resolves to no configurePreset")
    resolved["configurePreset"] = configure_name
    return resolved


def _git_ls_files(*patterns: str) -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-files", "--", *patterns],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise InventoryError(f"git ls-files failed while enumerating CMake inputs: {result.stderr.strip()}")
    paths = []
    for relative in result.stdout.splitlines():
        normalized = relative.replace("\\", "/")
        if not normalized or normalized.startswith("ThirdParty/"):
            continue
        paths.append(REPO_ROOT / Path(normalized))
    return sorted(paths, key=lambda path: path.relative_to(REPO_ROOT).as_posix())


def _tracked_cmake_lists() -> list[Path]:
    return _git_ls_files("*CMakeLists.txt")


def _tracked_cmake_inputs() -> list[Path]:
    """CMakeLists plus the .cmake modules that also declare targets and options.

    Module files such as cmake/BuildImGui.cmake declare real build targets that a
    CMakeLists-only scan cannot see; omitting them is a silent false negative.
    """
    return _git_ls_files("*CMakeLists.txt", "*.cmake")


_LIBRARY_TYPE_KEYWORDS = {
    "STATIC": "static_library",
    "SHARED": "shared_library",
    "MODULE": "module_library",
    "OBJECT": "object_library",
    "INTERFACE": "interface_library",
    "UNKNOWN": "unknown_library",
}
# Keywords that mean "this is not a build product of this project".
_NON_BUILD_TARGET_KEYWORDS = {"IMPORTED", "ALIAS"}


def _wrapper_definitions(commands: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Map function/macro name -> the target it creates from its first parameter.

    ``spark_add_plugin(NAME ...)`` really does declare a target; resolving the
    wrapper is what keeps its call sites from vanishing.
    """
    wrappers: dict[str, dict[str, Any]] = {}
    for command in commands:
        scope = command.get("definitionScope") or []
        if not scope or command["name"] not in {"add_executable", "add_library"}:
            continue
        definition = scope[-1]
        parameters = definition.get("parameters") or []
        if not parameters:
            continue
        args = _tokenize_cmake_arguments(command["body"])
        if not args or args[0] != "${" + parameters[0] + "}":
            continue
        wrappers[definition["name"].lower()] = {
            "kind": _classify_target(command["name"], args),
            "definedIn": command["file"],
            "definedAt": definition.get("line", command["line"]),
        }
    return wrappers


def _classify_target(command_name: str, args: list[str]) -> str:
    if command_name == "add_executable":
        return "executable"
    keyword = args[1].upper() if len(args) > 1 else ""
    return _LIBRARY_TYPE_KEYWORDS.get(keyword, "library")


def extract_cmake_targets(paths: Iterable[Path] | None = None) -> list[dict[str, Any]]:
    """Every target declaration across CMakeLists and .cmake module inputs.

    Declarations carry the metadata needed to tell three different things apart
    that used to look identical or be dropped outright: a concrete top-level
    target, a target declared inside a function body (a template, not yet a
    product), and a target whose name is a variable this scanner cannot resolve.
    """
    inputs = list(paths) if paths is not None else _tracked_cmake_inputs()
    parsed: list[dict[str, Any]] = []
    for cmake_file in inputs:
        source = _source_label(cmake_file)
        text = cmake_file.read_text(encoding="utf-8", errors="strict")
        parsed.extend(_commands_with_conditions(text, source))

    wrappers = _wrapper_definitions(parsed)
    declarations: list[dict[str, Any]] = []
    for command in parsed:
        name = command["name"]
        scope = command.get("definitionScope") or []
        if name in wrappers and not scope:
            args = _tokenize_cmake_arguments(command["body"])
            if not args:
                continue
            target = args[0]
            if "${" in target or "$<" in target:
                declarations.append(
                    _target_record(target, "unresolved", command, scope, origin="wrapper-call", resolved=False)
                )
                continue
            declarations.append(
                _target_record(target, wrappers[name]["kind"], command, scope, origin="wrapper-call")
            )
            continue
        if name not in {"add_executable", "add_library"}:
            continue
        args = _tokenize_cmake_arguments(command["body"])
        if not args:
            raise InventoryError(f"{command['file']}:{command['line']}: empty {command['spelling']}()")
        target = args[0]
        keywords = {value.upper() for value in args[1:]}
        origin = "function-template" if scope else "declaration"
        if keywords & _NON_BUILD_TARGET_KEYWORDS:
            kind = "imported" if "IMPORTED" in keywords else "alias"
            declarations.append(
                _target_record(target, kind, command, scope, origin="non-build", resolved=False)
            )
            continue
        if "${" in target or "$<" in target:
            # Never a silent continue: an unresolvable name is recorded so the
            # parity checker can raise it rather than mistake it for absence.
            declarations.append(
                _target_record(target, "unresolved", command, scope, origin=origin, resolved=False)
            )
            continue
        declarations.append(_target_record(target, _classify_target(name, args), command, scope, origin=origin))
    return sorted(declarations, key=lambda item: (item["target"], item["file"], item["line"]))


def _target_record(
    target: str,
    kind: str,
    command: dict[str, Any],
    scope: list[dict[str, Any]],
    origin: str,
    resolved: bool = True,
) -> dict[str, Any]:
    return {
        "target": target,
        "kind": kind,
        "file": command["file"],
        "line": command["line"],
        "conditionFrames": command["conditionFrames"],
        "definitionScope": [item["name"] for item in scope],
        "origin": origin,
        "resolved": resolved,
    }


def aggregate_cmake_targets(declarations: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Collapse declarations per target, keeping the evidence class explicit.

    ``buildable`` means: a resolvable name, declared outside a function body, and
    not an IMPORTED/ALIAS shim. Only those may satisfy a stable-v1 product.
    """
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for declaration in declarations:
        grouped[declaration["target"]].append(declaration)
    result = []
    for target, group in sorted(grouped.items()):
        buildable = [
            item
            for item in group
            if item.get("resolved", True)
            and not item.get("definitionScope")
            and item.get("origin") != "non-build"
        ]
        kinds = sorted({item["kind"] for item in (buildable or group)})
        result.append(
            {
                "target": target,
                "kind": kinds[0] if len(kinds) == 1 else "ambiguous",
                "buildable": bool(buildable),
                "origins": sorted({item.get("origin", "declaration") for item in group}),
                "declarations": [
                    {
                        "file": item["file"],
                        "line": item["line"],
                        "kind": item["kind"],
                        "origin": item.get("origin", "declaration"),
                        "definitionScope": item.get("definitionScope", []),
                        "resolved": item.get("resolved", True),
                    }
                    for item in group
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
    supported_hosts = profile.get("supportedHosts")
    if not isinstance(supported_hosts, list) or not supported_hosts:
        raise InventoryError("stable-v1 must declare the hosts it supports")
    return {
        "id": profile["id"],
        "supportedHosts": sorted(str(host) for host in supported_hosts),
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


def extract_workflow_record(path: Path | None = None) -> dict[str, Any]:
    """The bound semantic record of the build workflow."""
    workflow_path = path or WORKFLOW_PATH
    return extract_workflow_record_text(
        workflow_path.read_text(encoding="utf-8"), _source_label(workflow_path)
    )


def extract_workflow_record_text(text: str, source: str = ".github/workflows/build.yml") -> dict[str, Any]:
    try:
        return workflow_tool.build_workflow_record(text, source)
    except workflow_tool.WorkflowError as error:
        raise InventoryError(f"{source}: {error}") from error


def extract_workflow_cmake_configs_text(text: str) -> list[dict[str, Any]]:
    record = extract_workflow_record_text(text)
    return [item for item in record["cmakeInvocations"] if item["kind"] == "configure"]


def extract_workflow_cmake_configs(path: Path | None = None) -> list[dict[str, Any]]:
    return [item for item in extract_workflow_record(path)["cmakeInvocations"] if item["kind"] == "configure"]


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


# Cache variables worth binding evidence to. Bounded on purpose: the reply's
# cache is attacker-sized input, and an unbounded copy would bloat the artifact.
_BOUND_CACHE_PREFIXES = ("SPARK_", "ENABLE_", "BUILD_")
_BOUND_CACHE_NAMES = {
    "CMAKE_BUILD_TYPE",
    "CMAKE_GENERATOR",
    "CMAKE_GENERATOR_PLATFORM",
    "CMAKE_GENERATOR_TOOLSET",
    "CMAKE_HOME_DIRECTORY",
    "CMAKE_SYSTEM_NAME",
    "CMAKE_SIZEOF_VOID_P",
}
_MAX_CACHE_ENTRIES = 4096


def _normalize_directory(value: Any) -> str:
    if not isinstance(value, str) or not value:
        return ""
    return Path(value).as_posix().rstrip("/")


def _reply_object(index: dict[str, Any], kind: str, major: int) -> str:
    reply = index.get("reply", {})
    entry = reply.get(f"{kind}-v{major}")
    if isinstance(entry, dict) and entry.get("jsonFile"):
        return str(entry["jsonFile"])
    for obj in index.get("objects", []):
        if obj.get("kind") == kind and obj.get("version", {}).get("major") == major:
            if obj.get("jsonFile"):
                return str(obj["jsonFile"])
    return ""


def _bound_cache_entries(cache: dict[str, Any], profile: str) -> dict[str, str]:
    entries = cache.get("entries", [])
    if not isinstance(entries, list):
        raise InventoryError(f"{profile}: CMake File API cache reply has no entry list")
    if len(entries) > _MAX_CACHE_ENTRIES:
        raise InventoryError(
            f"{profile}: CMake File API cache reply has {len(entries)} entries, above the {_MAX_CACHE_ENTRIES} bound"
        )
    result: dict[str, str] = {}
    for entry in entries:
        name = entry.get("name")
        if not isinstance(name, str):
            raise InventoryError(f"{profile}: CMake File API cache entry lacks a name")
        if name in _BOUND_CACHE_NAMES or name.startswith(_BOUND_CACHE_PREFIXES):
            if name in result:
                raise InventoryError(f"{profile}: duplicate cache entry {name!r} in File API reply")
            result[name] = str(entry.get("value", ""))
    return dict(sorted(result.items()))


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


def extract_codemodel_targets(
    build_dir: Path, profile: str, source_commit: str = ""
) -> dict[str, Any]:
    """Read one File API reply and record what it can be bound to.

    The reply alone proves nothing about *which* tree produced it, so every
    binding fact the reply does carry -- its own source and build directories,
    its generator, and its cache -- is recorded here for the parity checker to
    compare against the profile that is claiming it. The commit is not in the
    reply at all, so it must be asserted by the caller and is otherwise reported
    as unverified rather than quietly accepted.
    """
    reply_dir = build_dir / ".cmake" / "api" / "v1" / "reply"
    if not reply_dir.is_dir():
        return {
            "profile": profile,
            "status": "absent",
            "evidenceDirectory": _normalize_directory(str(build_dir)),
            "targets": [],
        }
    indices = sorted(reply_dir.glob("index-*.json"))
    if not indices:
        raise InventoryError(f"{profile}: CMake File API reply has no index")
    index = json.loads(indices[-1].read_text(encoding="utf-8"))
    codemodel_file = _reply_object(index, "codemodel", 2)
    if not codemodel_file:
        raise InventoryError(f"{profile}: CMake File API index has no codemodel-v2 reply")
    codemodel = json.loads((reply_dir / codemodel_file).read_text(encoding="utf-8"))

    cache_file = _reply_object(index, "cache", 2)
    if not cache_file:
        raise InventoryError(
            f"{profile}: CMake File API reply has no cache-v2 object; evidence cannot be bound to a configuration"
        )
    cache = json.loads((reply_dir / cache_file).read_text(encoding="utf-8"))
    cache_values = _bound_cache_entries(cache, profile)

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
    evidence = parse_codemodel_targets(profile, codemodel, target_documents)
    paths = codemodel.get("paths", {})
    evidence.update(
        {
            "evidenceDirectory": _normalize_directory(str(build_dir)),
            "replyIndex": indices[-1].name,
            "sourceDirectory": _normalize_directory(paths.get("source")),
            "buildDirectory": _normalize_directory(paths.get("build")),
            "generator": cache_values.get("CMAKE_GENERATOR", ""),
            "architecture": cache_values.get("CMAKE_GENERATOR_PLATFORM", ""),
            "toolset": cache_values.get("CMAKE_GENERATOR_TOOLSET", ""),
            "cacheVariables": cache_values,
            "configurations": sorted(
                {
                    configuration.get("name", "")
                    for configuration in codemodel.get("configurations", [])
                }
            ),
            "assertedSourceCommit": source_commit,
        }
    )
    return evidence


def _repository_provenance() -> dict[str, Any]:
    """The commit and cleanliness the inventory is being generated against."""

    def git(*arguments: str) -> tuple[int, str]:
        completed = subprocess.run(
            ["git", "-C", str(REPO_ROOT), *arguments],
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        return completed.returncode, completed.stdout.strip()

    head_code, head = git("rev-parse", "HEAD")
    status_code, status = git("status", "--porcelain", "--untracked-files=no")
    if head_code != 0 or status_code != 0:
        raise InventoryError("git provenance is unavailable; evidence cannot be bound to a commit")
    return {
        "root": _normalize_directory(str(REPO_ROOT)),
        "commit": head,
        "clean": not status,
    }


def build_inventory(
    codemodels: dict[str, Path] | None = None,
    codemodel_commits: dict[str, str] | None = None,
) -> dict[str, Any]:
    profile = load_stable_profile()
    option_declarations = extract_cmake_options()
    all_option_declarations = extract_all_cmake_options()
    target_declarations = extract_cmake_targets()
    requested = codemodels or {}
    commits = codemodel_commits or {}
    known_profiles = {item["id"] for item in profile["buildConfigurations"]}
    unknown_codemodels = sorted((set(requested) | set(commits)) - known_profiles)
    if unknown_codemodels:
        raise InventoryError(f"codemodel evidence names unknown profiles: {unknown_codemodels}")
    codemodel_evidence = []
    for configuration in profile["buildConfigurations"]:
        identifier = configuration["id"]
        if identifier in requested:
            codemodel_evidence.append(
                extract_codemodel_targets(requested[identifier], identifier, commits.get(identifier, ""))
            )
        else:
            codemodel_evidence.append(
                {"profile": identifier, "status": "absent", "evidenceDirectory": "", "targets": []}
            )
    workflow_record = extract_workflow_record()
    workflow_configs = [
        item for item in workflow_record["cmakeInvocations"] if item["kind"] == "configure"
    ]
    # Repository provenance is recorded only when configured evidence is actually
    # present. Embedding a commit in the evidence-free baseline would make the
    # artifact invalidate itself on the very commit that lands it.
    inventory: dict[str, Any] = {"schemaVersion": 3}
    if any(entry.get("status") == "available" for entry in codemodel_evidence):
        inventory["repository"] = _repository_provenance()
    inventory.update({
        "profile": profile,
        "cmakeOptionDeclarations": option_declarations,
        "cmakeOptions": effective_cmake_options(option_declarations),
        "allCmakeOptionDeclarations": all_option_declarations,
        "cmakePresets": extract_cmake_presets(),
        "cmakeTargetDeclarations": target_declarations,
        "cmakeTargets": aggregate_cmake_targets(target_declarations),
        "configuredTargetEvidence": codemodel_evidence,
        "sparkBuildOptions": extract_sparkbuild_options(),
        "workflow": workflow_record,
        "workflowPresets": sorted({item["preset"] for item in workflow_configs if item.get("preset")}),
        "workflowCmakeConfigs": workflow_configs,
        "stableV1Products": profile["buildProducts"],
    })
    return inventory


def render_inventory(inventory: dict[str, Any]) -> bytes:
    return (json.dumps(inventory, indent=2, sort_keys=False) + "\n").encode("utf-8")


def _parse_key_value_args(values: list[str], flag: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        if "=" not in value:
            raise InventoryError(f"{flag} expects PROFILE=VALUE")
        profile, payload = value.split("=", 1)
        if not profile or not payload or profile in result:
            raise InventoryError(f"invalid duplicate {flag} value {value!r}")
        result[profile] = payload
    return result


def _parse_codemodel_args(values: list[str]) -> dict[str, Path]:
    return {
        profile: Path(directory)
        for profile, directory in _parse_key_value_args(values, "--codemodel").items()
    }


def _write_atomic(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(payload)
    temporary.replace(path)


def render_internal_error(error: Exception) -> bytes:
    return (
        json.dumps(
            {"schemaVersion": 3, "state": "internal-error", "internalError": str(error)},
            indent=2,
            sort_keys=False,
        )
        + "\n"
    ).encode("utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codemodel", action="append", default=[], metavar="PROFILE=BUILD_DIR")
    parser.add_argument(
        "--codemodel-commit",
        action="append",
        default=[],
        metavar="PROFILE=COMMIT",
        help="commit the named profile's codemodel evidence was configured from",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", type=Path, help="fail if generated bytes differ from this file")
    args = parser.parse_args(argv)
    try:
        if args.output and args.check and args.output.resolve() == args.check.resolve():
            raise InventoryError("--output and --check must name distinct paths")
        payload = render_inventory(
            build_inventory(
                _parse_codemodel_args(args.codemodel),
                _parse_key_value_args(args.codemodel_commit, "--codemodel-commit"),
            )
        )
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
