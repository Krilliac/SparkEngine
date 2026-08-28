#!/usr/bin/env python3
"""Deterministic, fail-closed build-matrix inventory for SparkEngine.

The canonical stable-v1 product contract lives in docs/site/readiness.json.
Source declarations prove that a target or option is declared; configured CMake
File API codemodel replies separately prove that a target exists in a concrete
configuration. The two forms of evidence are deliberately never conflated.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from collections import defaultdict
from dataclasses import dataclass, field
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
_WINDOWS_PRODUCT_SUFFIX = {
    "EXECUTABLE": ".exe",
    "STATIC_LIBRARY": ".lib",
    "SHARED_LIBRARY": ".dll",
    "MODULE_LIBRARY": ".dll",
}


class InventoryError(RuntimeError):
    """The inventory cannot safely interpret an authoritative input."""


class ReplyValidationError(InventoryError):
    """A supplied CMake File API reply is unsafe, malformed, or unstable."""


class ConcurrentReplyUpdate(ReplyValidationError):
    """A compliant concurrent CMake generation invalidated the selected index."""


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
    data = _read_bounded_json_file(
        path or PRESETS_PATH, _MAX_PRESETS_BYTES, "CMakePresets.json"
    )
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
        if configuration.get("purpose") == "installed-sdk-consumer":
            for field_name in ("sourceDirectory", "buildDirectory"):
                value = configuration.get(field_name)
                if (
                    not isinstance(value, str)
                    or not value
                    or Path(value).is_absolute()
                    or "\\" in value
                    or any(part in {"", ".", ".."} for part in value.split("/"))
                ):
                    raise InventoryError(
                        f"installed-sdk-consumer must declare a safe relative {field_name}"
                    )
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
    data = _read_bounded_json_file(
        path or READINESS_PATH, _MAX_READINESS_BYTES, "stable-v1 readiness contract"
    )
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
_MAX_REPLY_FILES = 8192
_MAX_REPLY_INDICES = 32
_MAX_INDEX_OBJECTS = 512
_MAX_CONFIGURATIONS = 128
_MAX_TARGET_REFERENCES = 8192
_MAX_ARTIFACTS_PER_TARGET = 32
_MAX_INDEX_BYTES = 2 * 1024 * 1024
_MAX_CODEMODEL_BYTES = 16 * 1024 * 1024
_MAX_CACHE_BYTES = 8 * 1024 * 1024
_MAX_TARGET_BYTES = 16 * 1024 * 1024
_MAX_PROVENANCE_BYTES = 1024 * 1024
_MAX_REPLY_FILE_BYTES = 32 * 1024 * 1024
_MAX_REPLY_DIRECTORY_BYTES = 256 * 1024 * 1024
_MAX_CONSUMED_REPLY_BYTES = 128 * 1024 * 1024
_MAX_JSON_DEPTH = 64
_MAX_JSON_NODES = 250_000
_MAX_JSON_STRING_BYTES = 2 * 1024 * 1024
_MAX_PRESETS_BYTES = 4 * 1024 * 1024
_MAX_READINESS_BYTES = 16 * 1024 * 1024
_MAX_INVENTORY_BYTES = 128 * 1024 * 1024
_MAX_REPORT_BYTES = 64 * 1024 * 1024
_PROVENANCE_FILE = "spark-ci120-provenance-v4.json"
_PROVENANCE_SCHEMA = 4
_PROVENANCE_PRODUCER = "spark-buildmatrix-configure-build-transaction-v4"
_CAPTURE_CLIENT_PREFIX = "client-spark-ci120-"
_MAX_CAPTURE_RESTARTS = 3
_QUERY_CLEANUP_ATTEMPTS = 4
_QUERY_CLEANUP_RETRY_DELAY_SECONDS = 0.05
_REPARSE_POINT_ATTRIBUTE = 0x400
_CI120_WORKFLOW_PATH = ".github/workflows/build.yml"
_CI120_PRODUCER_JOB = "build-windows-shipping"
_CI120_EXTERNAL_AUTHORITY = "external-attestation-required"


def _normalize_directory(value: Any) -> str:
    if not isinstance(value, str) or not value:
        return ""
    return Path(value).as_posix().rstrip("/")


def _github_actions_context() -> dict[str, str] | None:
    """Return the producer's claimed runner context for structural checks only.

    Environment variables are producer-controlled text.  They help detect a
    record replayed in a different job, but they are deliberately not an
    authority boundary and must never promote a receipt to ``verified``.
    """
    if os.environ.get("GITHUB_ACTIONS") != "true":
        return None
    raw = {
        "repository": os.environ.get("GITHUB_REPOSITORY", ""),
        "sourceCommit": os.environ.get("GITHUB_SHA", "").lower(),
        "runId": os.environ.get("GITHUB_RUN_ID", ""),
        "runAttempt": os.environ.get("GITHUB_RUN_ATTEMPT", ""),
        "workflowRef": os.environ.get("GITHUB_WORKFLOW_REF", ""),
        "job": os.environ.get("GITHUB_JOB", ""),
        "runnerOs": os.environ.get("RUNNER_OS", ""),
    }
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", raw["repository"]):
        raise InventoryError("CI-120 producer has no valid GITHUB_REPOSITORY")
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", raw["sourceCommit"]):
        raise InventoryError("CI-120 producer has no full GITHUB_SHA")
    if not raw["runId"].isdigit() or not raw["runAttempt"].isdigit():
        raise InventoryError("CI-120 producer has no valid GitHub run identity")
    expected_workflow = f"{raw['repository']}/{_CI120_WORKFLOW_PATH}@"
    if not raw["workflowRef"].startswith(expected_workflow):
        raise InventoryError(
            "CI-120 producer must be invoked by .github/workflows/build.yml, not an arbitrary workflow"
        )
    if raw["job"] != _CI120_PRODUCER_JOB or raw["runnerOs"] != "Windows":
        raise InventoryError(
            "CI-120 producer must run in the blocking Windows build-windows-shipping job"
        )
    return {"provider": "github-actions", **raw}


def _absolute_directory(path: Path) -> Path:
    """Normalize spelling without following links or reparses."""
    return Path(os.path.abspath(os.fspath(path)))


def _is_reparse(metadata: os.stat_result) -> bool:
    return bool(int(getattr(metadata, "st_file_attributes", 0)) & _REPARSE_POINT_ATTRIBUTE)


def _stat_token(metadata: os.stat_result) -> tuple[int, ...]:
    """File identity and mutation fields shared by path and handle APIs.

    Size is not mutation state.  In particular, a same-size in-place rewrite
    retains the file id and used to survive every pre/open/post comparison.
    CMake promises never to rewrite a reply filename with different content,
    so any timestamp change is a protocol violation and must invalidate the
    snapshot.
    """
    return (
        int(metadata.st_dev),
        int(metadata.st_ino),
        int(metadata.st_mode),
        int(metadata.st_size),
        int(getattr(metadata, "st_mtime_ns", int(metadata.st_mtime * 1_000_000_000))),
        int(getattr(metadata, "st_ctime_ns", int(metadata.st_ctime * 1_000_000_000))),
        int(getattr(metadata, "st_file_attributes", 0)),
    )


def _identity_token(metadata: os.stat_result) -> tuple[int, ...]:
    """Identity fields comparable between Windows path and handle stat views.

    CPython's Windows ``lstat`` and ``fstat`` adapters can report different
    synthesized mode/attribute bits for the same file.  File id, volume id,
    and size are the shared identity boundary; file kind and reparse state are
    validated independently on both views before this comparison.
    """
    return (
        int(metadata.st_dev),
        int(metadata.st_ino),
        int(metadata.st_size),
    )


def _identity_from_stat_token(token: tuple[int, ...]) -> tuple[int, ...]:
    return (token[0], token[1], token[3])


def _directory_stat_token(metadata: os.stat_result) -> tuple[int, ...]:
    """Directory identity plus mutation metadata; directory reads do not alter it."""
    return (
        int(metadata.st_dev),
        int(metadata.st_ino),
        int(metadata.st_mode),
        int(getattr(metadata, "st_mtime_ns", int(metadata.st_mtime * 1_000_000_000))),
        int(getattr(metadata, "st_ctime_ns", int(metadata.st_ctime * 1_000_000_000))),
        int(getattr(metadata, "st_file_attributes", 0)),
    )


def _validate_directory(metadata: os.stat_result, label: str) -> None:
    if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata):
        raise ReplyValidationError(f"{label} must not be a symlink or reparse point")
    if not stat.S_ISDIR(metadata.st_mode):
        raise ReplyValidationError(f"{label} is not a directory")


def _validate_regular_file(metadata: os.stat_result, label: str) -> None:
    if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata):
        raise ReplyValidationError(f"{label} must not be a symlink or reparse point")
    if not stat.S_ISREG(metadata.st_mode):
        raise ReplyValidationError(f"{label} is not a regular file")
    # Some Windows filesystems report st_nlink=0 through the CRT. The opened
    # handle is checked with GetFileInformationByHandle below; any other value
    # here is already decisively unsafe.
    if metadata.st_nlink not in {0, 1}:
        raise ReplyValidationError(
            f"{label} has {metadata.st_nlink} hard links; reply evidence must have exactly one"
        )


def _opened_link_count(descriptor: int, metadata: os.stat_result) -> int:
    if os.name != "nt":
        return int(metadata.st_nlink)

    import ctypes
    import msvcrt
    from ctypes import wintypes

    class _FileTime(ctypes.Structure):
        _fields_ = [("low", wintypes.DWORD), ("high", wintypes.DWORD)]

    class _ByHandleFileInformation(ctypes.Structure):
        _fields_ = [
            ("attributes", wintypes.DWORD),
            ("creationTime", _FileTime),
            ("lastAccessTime", _FileTime),
            ("lastWriteTime", _FileTime),
            ("volumeSerialNumber", wintypes.DWORD),
            ("fileSizeHigh", wintypes.DWORD),
            ("fileSizeLow", wintypes.DWORD),
            ("numberOfLinks", wintypes.DWORD),
            ("fileIndexHigh", wintypes.DWORD),
            ("fileIndexLow", wintypes.DWORD),
        ]

    information = _ByHandleFileInformation()
    handle = wintypes.HANDLE(msvcrt.get_osfhandle(descriptor))
    get_information = ctypes.windll.kernel32.GetFileInformationByHandle
    get_information.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ByHandleFileInformation)]
    get_information.restype = wintypes.BOOL
    if not get_information(handle, ctypes.byref(information)):
        raise ReplyValidationError("cannot determine the opened reply file's hard-link count")
    return int(information.numberOfLinks)


def _windows_directory_information(handle: int) -> tuple[int, int, int]:
    """Return volume, file id, and attributes for an open Windows directory."""
    import ctypes
    from ctypes import wintypes

    class _FileTime(ctypes.Structure):
        _fields_ = [("low", wintypes.DWORD), ("high", wintypes.DWORD)]

    class _ByHandleFileInformation(ctypes.Structure):
        _fields_ = [
            ("attributes", wintypes.DWORD),
            ("creationTime", _FileTime),
            ("lastAccessTime", _FileTime),
            ("lastWriteTime", _FileTime),
            ("volumeSerialNumber", wintypes.DWORD),
            ("fileSizeHigh", wintypes.DWORD),
            ("fileSizeLow", wintypes.DWORD),
            ("numberOfLinks", wintypes.DWORD),
            ("fileIndexHigh", wintypes.DWORD),
            ("fileIndexLow", wintypes.DWORD),
        ]

    information = _ByHandleFileInformation()
    get_information = ctypes.windll.kernel32.GetFileInformationByHandle
    get_information.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ByHandleFileInformation)]
    get_information.restype = wintypes.BOOL
    if not get_information(wintypes.HANDLE(handle), ctypes.byref(information)):
        raise ReplyValidationError("cannot identify a held CMake File API directory")
    return (
        int(information.volumeSerialNumber),
        (int(information.fileIndexHigh) << 32) | int(information.fileIndexLow),
        int(information.attributes),
    )


@dataclass
class _HeldDirectory:
    """A no-delete-share handle that prevents ancestor replacement mid-read."""

    path: Path
    path_token: tuple[int, ...]
    handle_token: tuple[int, ...]
    native_handle: int = -1
    descriptor: int = -1

    @classmethod
    def open(cls, path: Path, label: str) -> "_HeldDirectory":
        before = os.lstat(path)
        _validate_directory(before, label)
        path_token = _directory_stat_token(before)
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            create_file = ctypes.windll.kernel32.CreateFileW
            create_file.argtypes = [
                wintypes.LPCWSTR,
                wintypes.DWORD,
                wintypes.DWORD,
                wintypes.LPVOID,
                wintypes.DWORD,
                wintypes.DWORD,
                wintypes.HANDLE,
            ]
            create_file.restype = wintypes.HANDLE
            handle = create_file(
                str(path),
                0x80,  # FILE_READ_ATTRIBUTES
                0x1 | 0x2,  # share reads/writes, deliberately deny delete/rename
                None,
                3,  # OPEN_EXISTING
                0x02000000 | 0x00200000,  # BACKUP_SEMANTICS | OPEN_REPARSE_POINT
                None,
            )
            invalid = ctypes.c_void_p(-1).value
            if handle == invalid or handle is None:
                raise ReplyValidationError(f"cannot hold {label} against replacement")
            handle_value = int(handle)
            try:
                handle_token = _windows_directory_information(handle_value)
                if not (handle_token[2] & 0x10) or handle_token[2] & 0x400:
                    raise ReplyValidationError(f"held {label} is not a non-reparse directory")
                after = os.lstat(path)
                _validate_directory(after, label)
                if _directory_stat_token(after) != path_token:
                    raise ReplyValidationError(f"{label} changed while its handle was acquired")
                return cls(path, path_token, handle_token, native_handle=handle_value)
            except Exception:
                ctypes.windll.kernel32.CloseHandle(wintypes.HANDLE(handle_value))
                raise

        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
        try:
            opened = os.fstat(descriptor)
            _validate_directory(opened, f"opened {label}")
            if _identity_token(opened)[:2] != _identity_token(before)[:2]:
                raise ReplyValidationError(f"{label} changed while its handle was acquired")
            after = os.lstat(path)
            if _directory_stat_token(after) != path_token:
                raise ReplyValidationError(f"{label} changed while its handle was acquired")
            return cls(path, path_token, _directory_stat_token(opened), descriptor=descriptor)
        except Exception:
            os.close(descriptor)
            raise

    def assert_stable(self, label: str) -> None:
        if self.native_handle != -1:
            if _windows_directory_information(self.native_handle) != self.handle_token:
                raise ConcurrentReplyUpdate(f"held {label} identity changed")
        elif self.descriptor != -1:
            opened = os.fstat(self.descriptor)
            if _directory_stat_token(opened) != self.handle_token:
                raise ConcurrentReplyUpdate(f"held {label} identity changed")
        if _directory_token(self.path, label) != self.path_token:
            raise ConcurrentReplyUpdate(f"{label} changed while replies were read")

    def close(self) -> None:
        if self.native_handle != -1:
            import ctypes
            from ctypes import wintypes

            ctypes.windll.kernel32.CloseHandle(wintypes.HANDLE(self.native_handle))
            self.native_handle = -1
        if self.descriptor != -1:
            os.close(self.descriptor)
            self.descriptor = -1

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass


def _hold_directory_chain(path: Path, label: str) -> list[_HeldDirectory]:
    absolute = _absolute_directory(path)
    current = Path(absolute.anchor)
    paths = [current]
    for component in absolute.parts[1:]:
        current /= component
        paths.append(current)
    held: list[_HeldDirectory] = []
    try:
        for component in paths:
            held.append(_HeldDirectory.open(component, f"{label} ancestor {component}"))
    except Exception:
        for item in reversed(held):
            item.close()
        raise
    return held


def _directory_token(path: Path, label: str) -> tuple[int, ...]:
    try:
        metadata = os.lstat(path)
    except OSError as error:
        raise ReplyValidationError(f"cannot inspect {label}: {error}") from error
    _validate_directory(metadata, label)
    return _directory_stat_token(metadata)


def _validate_directory_chain(path: Path, label: str) -> tuple[Path, tuple[int, ...]] | None:
    """Validate every existing ancestor without following a reparse point.

    Checking only the leaf lets ``trusted/build`` escape when ``trusted`` is a
    junction.  Walk from the volume/filesystem anchor so every component that
    the kernel will traverse is checked before the leaf is trusted.
    """
    absolute = _absolute_directory(path)
    anchor = Path(absolute.anchor)
    current = anchor
    final_metadata: os.stat_result | None = None
    for offset, component in enumerate(absolute.parts[1:], start=1):
        current /= component
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            return None
        except OSError as error:
            raise ReplyValidationError(f"cannot inspect {label} ancestor {current}: {error}") from error
        _validate_directory(metadata, f"{label} ancestor {current}")
        final_metadata = metadata
    if final_metadata is None:
        try:
            final_metadata = os.lstat(anchor)
        except OSError as error:
            raise ReplyValidationError(f"cannot inspect {label}: {error}") from error
        _validate_directory(final_metadata, label)
    return absolute, _directory_stat_token(final_metadata)


def _reply_filename(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ReplyValidationError(f"{label} must be a nonempty filename")
    if len(value) > 255 or value in {".", ".."} or "/" in value or "\\" in value:
        raise ReplyValidationError(f"{label} must be one plain filename inside the reply directory")
    if not value.endswith(".json"):
        raise ReplyValidationError(f"{label} must name a JSON file")
    return value


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number {value}")


def _decode_bounded_json(payload: bytes, label: str) -> Any:
    def utf8_size(value: str, kind: str) -> int:
        try:
            return len(value.encode("utf-8"))
        except UnicodeEncodeError as error:
            raise ReplyValidationError(
                f"{label} contains invalid Unicode in a JSON {kind}"
            ) from error

    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        decoded = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=_reject_json_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
        raise ReplyValidationError(f"{label} is not strict bounded JSON: {error}") from error

    nodes = 0
    stack: list[tuple[Any, int]] = [(decoded, 1)]
    while stack:
        value, depth = stack.pop()
        nodes += 1
        if nodes > _MAX_JSON_NODES:
            raise ReplyValidationError(f"{label} exceeds the {_MAX_JSON_NODES} JSON-node bound")
        if depth > _MAX_JSON_DEPTH:
            raise ReplyValidationError(f"{label} exceeds the {_MAX_JSON_DEPTH} JSON-depth bound")
        if isinstance(value, dict):
            for key, child in value.items():
                if utf8_size(key, "key") > _MAX_JSON_STRING_BYTES:
                    raise ReplyValidationError(f"{label} contains an oversized JSON key")
                stack.append((child, depth + 1))
        elif isinstance(value, list):
            stack.extend((child, depth + 1) for child in value)
        elif isinstance(value, str):
            if utf8_size(value, "string") > _MAX_JSON_STRING_BYTES:
                raise ReplyValidationError(f"{label} contains an oversized JSON string")
        elif isinstance(value, float) and not math.isfinite(value):
            raise ReplyValidationError(f"{label} contains a non-finite JSON number")
    return decoded


@dataclass(frozen=True)
class _ReplyFile:
    path: Path
    token: tuple[int, ...]
    size: int
    snapshot_sha256: str = ""

    def __post_init__(self) -> None:
        """Bind enumeration to content, not merely file-system metadata.

        Some Windows volumes defer or coalesce timestamp updates, so an
        in-place same-size rewrite can preserve every ``stat`` field exposed by
        Python.  Hash once when the directory entry is captured and require the
        bytes consumed later to retain that identity.
        """
        if self.snapshot_sha256:
            return
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(self.path, flags)
        try:
            opened = os.fstat(descriptor)
            _validate_regular_file(opened, f"snapshotted reply file {self.path.name!r}")
            if _opened_link_count(descriptor, opened) != 1:
                raise ReplyValidationError(
                    f"snapshotted reply file {self.path.name!r} must have exactly one hard link"
                )
            if _identity_token(opened) != _identity_from_stat_token(self.token):
                raise ReplyValidationError(
                    f"snapshotted reply file {self.path.name!r} was replaced during enumeration"
                )
            opened_token = _stat_token(opened)
            remaining = self.size
            digest = hashlib.sha256()
            while remaining:
                chunk = os.read(descriptor, min(remaining, 1024 * 1024))
                if not chunk:
                    raise ReplyValidationError(
                        f"snapshotted reply file {self.path.name!r} was truncated"
                    )
                digest.update(chunk)
                remaining -= len(chunk)
            if os.read(descriptor, 1):
                raise ReplyValidationError(f"snapshotted reply file {self.path.name!r} grew")
            if _stat_token(os.fstat(descriptor)) != opened_token:
                raise ReplyValidationError(
                    f"snapshotted reply file {self.path.name!r} changed while hashing"
                )
        finally:
            os.close(descriptor)
        after = os.lstat(self.path)
        if _stat_token(after) != self.token:
            raise ReplyValidationError(
                f"snapshotted reply file {self.path.name!r} changed after enumeration"
            )
        object.__setattr__(self, "snapshot_sha256", digest.hexdigest())


@dataclass
class _ReplySnapshot:
    build_directory: Path
    directory: Path
    directory_token: tuple[int, ...]
    files: dict[str, _ReplyFile]
    entry_names: set[str] = field(default_factory=set)
    ancestor_tokens: list[tuple[Path, tuple[int, ...]]] = field(default_factory=list)
    directory_holds: list[_HeldDirectory] = field(default_factory=list)
    payloads: dict[str, bytes] = field(default_factory=dict)
    records: dict[str, dict[str, Any]] = field(default_factory=dict)
    consumed_bytes: int = 0

    def assert_stable(self) -> None:
        for held in self.directory_holds:
            held.assert_stable(f"CMake File API ancestor {held.path}")
        for path, token in self.ancestor_tokens:
            if _directory_token(path, f"CMake File API ancestor {path}") != token:
                raise ConcurrentReplyUpdate(
                    f"CMake File API ancestor {path} changed while replies were read"
                )
        if _directory_token(self.directory, "CMake File API reply directory") != self.directory_token:
            raise ConcurrentReplyUpdate("CMake File API reply directory changed while it was being read")

    def close(self) -> None:
        for held in reversed(self.directory_holds):
            held.close()
        self.directory_holds.clear()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _capture_file(self, name: str, label: str) -> _ReplyFile:
        if name not in self.entry_names:
            raise ConcurrentReplyUpdate(
                f"{label} {name!r} is missing from the captured reply snapshot"
            )
        path = self.directory / name
        try:
            metadata = os.lstat(path)
        except FileNotFoundError as error:
            raise ConcurrentReplyUpdate(f"{label} {name!r} disappeared") from error
        except OSError as error:
            raise ReplyValidationError(f"cannot inspect {label} {name!r}: {error}") from error
        _validate_regular_file(metadata, f"{label} {name!r}")
        if metadata.st_size > _MAX_REPLY_FILE_BYTES:
            raise ReplyValidationError(
                f"{label} {name!r} is above the {_MAX_REPLY_FILE_BYTES}-byte bound"
            )
        captured = _ReplyFile(path, _stat_token(metadata), metadata.st_size)
        self.files[name] = captured
        return captured

    def read_bytes(self, name: str, maximum: int, label: str) -> bytes:
        name = _reply_filename(name, label)
        known = self.files.get(name)
        if known is None:
            known = self._capture_file(name, label)
        if known.size > maximum:
            raise ReplyValidationError(f"{label} {name!r} is {known.size} bytes, above the {maximum} bound")
        if name in self.payloads:
            payload = self.payloads[name]
            if len(payload) > maximum:
                raise ReplyValidationError(f"{label} {name!r} exceeds its use-specific size bound")
            return payload

        try:
            before = os.lstat(known.path)
        except FileNotFoundError as error:
            raise ConcurrentReplyUpdate(f"{label} {name!r} disappeared before reading") from error
        except OSError as error:
            raise ReplyValidationError(f"cannot re-check {label} {name!r}: {error}") from error
        _validate_regular_file(before, f"{label} {name!r}")
        if _stat_token(before) != known.token:
            raise ReplyValidationError(f"{label} {name!r} changed after reply enumeration")

        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(known.path, flags)
        except FileNotFoundError as error:
            raise ConcurrentReplyUpdate(f"{label} {name!r} disappeared while opening") from error
        except OSError as error:
            raise ReplyValidationError(f"cannot open {label} {name!r} without following links: {error}") from error
        try:
            opened = os.fstat(descriptor)
            _validate_regular_file(opened, f"opened {label} {name!r}")
            link_count = _opened_link_count(descriptor, opened)
            if link_count != 1:
                raise ReplyValidationError(
                    f"opened {label} {name!r} has {link_count} hard links; exactly one is required"
                )
            if _identity_token(opened) != _identity_from_stat_token(known.token):
                raise ReplyValidationError(f"{label} {name!r} was replaced between inspection and open")
            opened_token = _stat_token(opened)
            try:
                after_open = os.lstat(known.path)
            except OSError as error:
                raise ReplyValidationError(f"cannot verify opened {label} {name!r}: {error}") from error
            if _stat_token(after_open) != known.token:
                raise ReplyValidationError(f"{label} {name!r} was replaced while being opened")

            remaining = known.size
            chunks: list[bytes] = []
            while remaining:
                chunk = os.read(descriptor, min(remaining, 1024 * 1024))
                if not chunk:
                    raise ReplyValidationError(f"{label} {name!r} was truncated while being read")
                chunks.append(chunk)
                remaining -= len(chunk)
            if os.read(descriptor, 1):
                raise ReplyValidationError(f"{label} {name!r} grew while being read")
            payload = b"".join(chunks)
            if _stat_token(os.fstat(descriptor)) != opened_token:
                raise ReplyValidationError(f"{label} {name!r} changed while being read")
            if hashlib.sha256(payload).hexdigest() != known.snapshot_sha256:
                raise ReplyValidationError(
                    f"{label} {name!r} content changed after reply enumeration"
                )
        finally:
            os.close(descriptor)

        try:
            after_read = os.lstat(known.path)
        except FileNotFoundError as error:
            raise ConcurrentReplyUpdate(f"{label} {name!r} disappeared after reading") from error
        except OSError as error:
            raise ReplyValidationError(f"cannot verify read {label} {name!r}: {error}") from error
        if _stat_token(after_read) != known.token:
            raise ReplyValidationError(f"{label} {name!r} changed after it was read")
        self.assert_stable()
        self.consumed_bytes += len(payload)
        if self.consumed_bytes > _MAX_CONSUMED_REPLY_BYTES:
            raise ReplyValidationError(
                f"consumed reply data exceeds the {_MAX_CONSUMED_REPLY_BYTES}-byte aggregate bound"
            )
        self.payloads[name] = payload
        self.records[name] = {
            "name": name,
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        }
        return payload

    def read_json(self, name: str, maximum: int, label: str) -> Any:
        return _decode_bounded_json(self.read_bytes(name, maximum, label), f"{label} {name!r}")

    def consumed_records(self) -> list[dict[str, Any]]:
        return [dict(self.records[name]) for name in sorted(self.records) if name != _PROVENANCE_FILE]


def _read_bounded_json_file(path: Path, maximum: int, label: str) -> Any:
    """Read one authoritative JSON file with stable identity and strict syntax."""
    absolute = _absolute_directory(path)
    parent_state = _validate_directory_chain(absolute.parent, f"{label} parent")
    if parent_state is None:
        raise InventoryError(f"{label} parent does not exist")
    parent, parent_token = parent_state
    try:
        metadata = os.lstat(absolute)
    except OSError as error:
        raise InventoryError(f"cannot inspect {label}: {error}") from error
    _validate_regular_file(metadata, label)
    if metadata.st_size > maximum:
        raise InventoryError(f"{label} is {metadata.st_size} bytes, above the {maximum} bound")
    name = _reply_filename(absolute.name, label)
    reply_file = _ReplyFile(absolute, _stat_token(metadata), metadata.st_size)
    snapshot = _ReplySnapshot(parent, parent, parent_token, {name: reply_file}, {name})
    return snapshot.read_json(name, maximum, label)


def _snapshot_reply_directory(build_dir: Path, profile: str) -> _ReplySnapshot | None:
    build = _validate_directory_chain(build_dir, f"{profile} build directory")
    if build is None:
        return None
    build_directory, _ = build
    reply_directory = build_directory
    directory_token: tuple[int, ...] = ()
    for component in (".cmake", "api", "v1", "reply"):
        reply_directory /= component
        try:
            metadata = os.lstat(reply_directory)
        except FileNotFoundError:
            return None
        except OSError as error:
            raise ReplyValidationError(
                f"{profile}: cannot inspect File API directory component {reply_directory}: {error}"
            ) from error
        _validate_directory(metadata, f"{profile} File API directory component {reply_directory}")
        directory_token = _directory_stat_token(metadata)

    directory_holds = _hold_directory_chain(
        reply_directory, f"{profile} CMake File API directory"
    )
    try:
        with os.scandir(reply_directory) as iterator:
            entries = list(iterator)
    except OSError as error:
        raise ReplyValidationError(f"{profile}: cannot enumerate CMake File API reply: {error}") from error
    if len(entries) > _MAX_REPLY_FILES:
        raise ReplyValidationError(
            f"{profile}: reply has {len(entries)} files, above the {_MAX_REPLY_FILES} bound"
        )

    files: dict[str, _ReplyFile] = {}
    entry_names: set[str] = set()
    casefolded: set[str] = set()
    directory_bytes = 0
    for entry in entries:
        name = _reply_filename(entry.name, f"{profile} reply entry")
        folded = name.casefold()
        if folded in casefolded:
            raise ReplyValidationError(f"{profile}: reply contains case-colliding filename {name!r}")
        casefolded.add(folded)
        entry_names.add(name)
        try:
            # os.DirEntry.stat() can return zeroed file identities on Windows
            # filter-driver volumes. lstat(path) provides the real file ID that
            # the open/fstat/post-lstat checks can bind to.
            metadata = os.lstat(entry.path)
        except OSError as error:
            raise ReplyValidationError(f"{profile}: cannot inspect reply entry {name!r}: {error}") from error
        _validate_regular_file(metadata, f"{profile} reply entry {name!r}")
        if metadata.st_size > _MAX_REPLY_FILE_BYTES:
            raise ReplyValidationError(
                f"{profile}: reply entry {name!r} is above the {_MAX_REPLY_FILE_BYTES}-byte bound"
            )
        directory_bytes += metadata.st_size
        if directory_bytes > _MAX_REPLY_DIRECTORY_BYTES:
            raise ReplyValidationError(
                f"{profile}: reply directory is above the {_MAX_REPLY_DIRECTORY_BYTES}-byte aggregate bound"
            )
        # Do not open or hash arbitrary reply documents during enumeration.
        # The reader captures only the selected index and the documents named
        # by that index's exact client response.

    if _directory_token(reply_directory, f"{profile} CMake File API reply directory") != directory_token:
        raise ReplyValidationError(f"{profile}: reply directory changed during enumeration")
    ancestor_tokens: list[tuple[Path, tuple[int, ...]]] = []
    current = Path(reply_directory.anchor)
    for component in reply_directory.parts[1:-1]:
        current /= component
        metadata = os.lstat(current)
        _validate_directory(metadata, f"{profile} File API ancestor {current}")
        ancestor_tokens.append((current, _directory_stat_token(metadata)))
    return _ReplySnapshot(
        build_directory,
        reply_directory,
        directory_token,
        files,
        entry_names,
        ancestor_tokens,
        directory_holds,
    )


def _require_mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ReplyValidationError(f"{label} must be a JSON object")
    return value


def _require_list(value: Any, label: str, maximum: int) -> list[Any]:
    if not isinstance(value, list):
        raise ReplyValidationError(f"{label} must be a JSON array")
    if len(value) > maximum:
        raise ReplyValidationError(f"{label} has {len(value)} entries, above the {maximum} bound")
    return value


def _validate_index(index: Any, profile: str) -> dict[str, Any]:
    root = _require_mapping(index, f"{profile} File API index")
    _require_mapping(root.get("reply", {}), f"{profile} File API index reply")
    objects = _require_list(root.get("objects", []), f"{profile} File API index objects", _MAX_INDEX_OBJECTS)
    for offset, value in enumerate(objects):
        entry = _require_mapping(value, f"{profile} File API index object {offset}")
        if not isinstance(entry.get("kind"), str):
            raise ReplyValidationError(f"{profile}: File API index object {offset} has no kind")
        version = _require_mapping(entry.get("version"), f"{profile} File API index object {offset} version")
        if type(version.get("major")) is not int:
            raise ReplyValidationError(f"{profile}: File API index object {offset} has no integer major version")
        _reply_filename(entry.get("jsonFile"), f"{profile} File API index object {offset} jsonFile")
    return root


def _reply_object(index: dict[str, Any], kind: str, major: int, profile: str) -> str:
    candidates: list[str] = []
    reply = _require_mapping(index.get("reply", {}), f"{profile} File API index reply")
    entry = reply.get(f"{kind}-v{major}")
    if entry is not None:
        mapping = _require_mapping(entry, f"{profile} File API reply {kind}-v{major}")
        candidates.append(
            _reply_filename(mapping.get("jsonFile"), f"{profile} File API reply {kind}-v{major} jsonFile")
        )
    for offset, value in enumerate(index.get("objects", [])):
        obj = _require_mapping(value, f"{profile} File API index object {offset}")
        version = _require_mapping(obj.get("version"), f"{profile} File API index object {offset} version")
        if obj.get("kind") == kind and version.get("major") == major:
            candidates.append(
                _reply_filename(obj.get("jsonFile"), f"{profile} File API {kind}-v{major} jsonFile")
            )
    unique = sorted(set(candidates))
    if len(unique) > 1:
        raise ReplyValidationError(
            f"{profile}: File API index gives conflicting {kind}-v{major} files {unique}"
        )
    return unique[0] if unique else ""


def _client_reply_objects(
    index: dict[str, Any],
    client_name: str,
    query: dict[str, Any],
    profile: str,
) -> dict[str, str]:
    """Validate the exact stateful query mirror and return only its responses."""
    reply = _require_mapping(index.get("reply"), f"{profile} File API index reply")
    client = _require_mapping(
        reply.get(client_name), f"{profile} File API client reply {client_name!r}"
    )
    query_reply = _require_mapping(
        client.get("query.json"), f"{profile} File API stateful query reply"
    )
    if set(query_reply) != {"client", "requests", "responses"}:
        raise ReplyValidationError(
            f"{profile}: stateful client query response has unexpected or missing fields"
        )
    if query_reply.get("client") != query.get("client"):
        raise ReplyValidationError(f"{profile}: File API client identity was not mirrored exactly")
    requests = _require_list(
        query_reply.get("requests"), f"{profile} File API mirrored requests", 8
    )
    if requests != query.get("requests"):
        raise ReplyValidationError(f"{profile}: File API client requests were not mirrored exactly")
    responses = _require_list(
        query_reply.get("responses"), f"{profile} File API client responses", 8
    )
    if len(responses) != len(requests):
        raise ReplyValidationError(f"{profile}: File API client response cardinality differs")
    result: dict[str, str] = {}
    for offset, (request_value, response_value) in enumerate(zip(requests, responses)):
        request = _require_mapping(request_value, f"{profile} File API request {offset}")
        response = _require_mapping(response_value, f"{profile} File API response {offset}")
        if "error" in response:
            raise ReplyValidationError(
                f"{profile}: File API request {offset} failed: {response.get('error')}"
            )
        kind = request.get("kind")
        if kind not in {"codemodel", "cache"} or kind in result:
            raise ReplyValidationError(f"{profile}: malformed or duplicate client request kind {kind!r}")
        version = _require_mapping(
            response.get("version"), f"{profile} File API response {offset} version"
        )
        required_major = 2
        if response.get("kind") != kind or version.get("major") != required_major:
            raise ReplyValidationError(
                f"{profile}: File API response {offset} does not match its request"
            )
        result[str(kind)] = _reply_filename(
            response.get("jsonFile"), f"{profile} File API response {offset} jsonFile"
        )
    if set(result) != {"codemodel", "cache"}:
        raise ReplyValidationError(f"{profile}: client reply does not contain codemodel and cache")
    return result


def _index_cmake_identity(index: dict[str, Any], profile: str) -> dict[str, Any]:
    cmake = _require_mapping(index.get("cmake"), f"{profile} File API index cmake")
    version = _require_mapping(cmake.get("version"), f"{profile} File API index cmake version")
    paths = _require_mapping(cmake.get("paths"), f"{profile} File API index cmake paths")
    generator = _require_mapping(
        cmake.get("generator"), f"{profile} File API index generator"
    )
    version_string = version.get("string")
    executable = paths.get("cmake")
    generator_name = generator.get("name")
    if not all(isinstance(value, str) and value for value in (version_string, executable, generator_name)):
        raise ReplyValidationError(f"{profile}: File API index omits CMake producer identity")
    multi_config = generator.get("multiConfig")
    if type(multi_config) is not bool:
        raise ReplyValidationError(f"{profile}: File API index has invalid multiConfig")
    return {
        "executable": _normalize_directory(executable),
        "version": version_string,
        "generator": generator_name,
        "platform": str(generator.get("platform", "")),
        "multiConfig": multi_config,
    }


def _bound_cache_entries(cache: dict[str, Any], profile: str) -> dict[str, str]:
    entries = _require_list(
        cache.get("entries"), f"{profile} CMake File API cache entries", _MAX_CACHE_ENTRIES
    )
    material_names = set(_BOUND_CACHE_NAMES)
    profile_data = load_stable_profile()
    profile_config = next(
        (item for item in profile_data["buildConfigurations"] if item["id"] == profile), None
    )
    if profile_config and profile_config.get("preset"):
        preset = resolve_configure_preset(extract_cmake_presets(), profile_config["preset"])
        material_names.update(preset.get("cacheVariables", {}))
    result: dict[str, str] = {}
    all_names: set[str] = set()
    for offset, value in enumerate(entries):
        entry = _require_mapping(value, f"{profile} CMake File API cache entry {offset}")
        name = entry.get("name")
        if not isinstance(name, str) or not name or len(name) > 1024:
            raise ReplyValidationError(f"{profile}: CMake File API cache entry {offset} has an invalid name")
        if name in all_names:
            raise ReplyValidationError(f"{profile}: duplicate cache entry {name!r} in File API reply")
        all_names.add(name)
        value_text = entry.get("value")
        if not isinstance(value_text, str):
            raise ReplyValidationError(f"{profile}: cache entry {name!r} has a non-string value")
        if name in material_names or name.startswith(_BOUND_CACHE_PREFIXES):
            result[name] = value_text
    return dict(sorted(result.items()))


def parse_codemodel_targets(
    profile: str,
    codemodel: dict[str, Any],
    target_documents: dict[str, dict[str, Any]],
    build_directory: Path | None = None,
) -> dict[str, Any]:
    """Parse configured target evidence from already loaded File API documents."""
    if not isinstance(codemodel, dict):
        raise InventoryError(f"{profile}: codemodel must be an object")
    configurations = codemodel.get("configurations")
    if not isinstance(configurations, list) or len(configurations) > _MAX_CONFIGURATIONS:
        raise InventoryError(f"{profile}: codemodel configurations are missing or above the bound")
    targets: dict[tuple[str, str], dict[str, str]] = {}
    seen_configurations: set[str] = set()
    id_bindings: dict[str, tuple[str, str]] = {}
    profile_data = load_stable_profile()
    known_profile = any(
        item.get("id") == profile for item in profile_data.get("buildConfigurations", [])
    )
    supported_hosts = profile_data.get("supportedHosts", [])
    windows_only = (
        known_profile
        and bool(supported_hosts)
        and all("windows" in str(host).lower() for host in supported_hosts)
    )
    reference_count = 0
    for configuration in configurations:
        if not isinstance(configuration, dict):
            raise InventoryError(f"{profile}: codemodel configuration must be an object")
        config_name = configuration.get("name")
        if not isinstance(config_name, str) or not config_name:
            raise InventoryError(f"{profile}: codemodel configuration has no nonempty name")
        if config_name in seen_configurations:
            raise InventoryError(f"{profile}: duplicate codemodel configuration {config_name!r}")
        seen_configurations.add(config_name)
        references = configuration.get("targets")
        if not isinstance(references, list):
            raise InventoryError(f"{profile}: codemodel configuration {config_name!r} has no target list")
        reference_count += len(references)
        if reference_count > _MAX_TARGET_REFERENCES:
            raise InventoryError(f"{profile}: codemodel target reference count is above the bound")
        for reference in references:
            if not isinstance(reference, dict):
                raise InventoryError(f"{profile}: codemodel target reference must be an object")
            name = reference.get("name")
            if not isinstance(name, str) or not name:
                raise InventoryError(f"{profile}: codemodel target reference has no name")
            reference_id = reference.get("id")
            if not isinstance(reference_id, str) or not reference_id or len(reference_id) > 4096:
                raise InventoryError(f"{profile}: codemodel target reference {name!r} has no valid id")
            try:
                target_file = _reply_filename(
                    reference.get("jsonFile"), f"{profile} codemodel target {name!r} jsonFile"
                )
            except ReplyValidationError as error:
                raise InventoryError(str(error)) from error
            binding = (name, target_file)
            previous_binding = id_bindings.setdefault(reference_id, binding)
            if previous_binding != binding:
                raise InventoryError(
                    f"{profile}: codemodel target id {reference_id!r} identifies multiple targets"
                )
            target = target_documents.get(target_file)
            if target is None:
                raise InventoryError(f"{profile}: codemodel target reply {target_file!r} is missing")
            if not isinstance(target, dict):
                raise InventoryError(f"{profile}: codemodel target reply {target_file!r} must be an object")
            if target.get("name") != name:
                raise InventoryError(
                    f"{profile}: codemodel reference {name!r} disagrees with target document {target.get('name')!r}"
                )
            if target.get("id") != reference_id:
                raise InventoryError(
                    f"{profile}: codemodel reference id for {name!r} disagrees with its target document"
                )
            cmake_type = target.get("type")
            kind = _TARGET_KIND_MAP.get(cmake_type)
            if not kind:
                raise InventoryError(f"{profile}: unsupported codemodel target type {cmake_type!r}")
            name_on_disk = target.get("nameOnDisk")
            artifacts_value = target.get("artifacts")
            product_type = cmake_type in {
                "EXECUTABLE", "STATIC_LIBRARY", "SHARED_LIBRARY", "MODULE_LIBRARY"
            }
            artifact_paths: list[str] = []
            if product_type:
                if (
                    not isinstance(name_on_disk, str)
                    or not name_on_disk
                    or len(name_on_disk) > 255
                    or Path(name_on_disk).name != name_on_disk
                    or name_on_disk in {".", ".."}
                    or ":" in name_on_disk
                    or any(ord(character) < 32 for character in name_on_disk)
                    or name_on_disk.endswith((" ", "."))
                ):
                    raise InventoryError(
                        f"{profile}: linked target {name!r} has no safe nameOnDisk"
                    )
                expected_suffix = _WINDOWS_PRODUCT_SUFFIX.get(str(cmake_type))
                if windows_only and expected_suffix and not name_on_disk.casefold().endswith(
                    expected_suffix
                ):
                    raise InventoryError(
                        f"{profile}: target {name!r} nameOnDisk is inconsistent with {cmake_type}"
                    )
                artifacts = _require_list(
                    artifacts_value,
                    f"{profile} target {name!r} artifacts",
                    _MAX_ARTIFACTS_PER_TARGET,
                )
                if not artifacts:
                    raise InventoryError(f"{profile}: linked target {name!r} has no artifacts")
                if build_directory is None:
                    raise InventoryError(
                        f"{profile}: cannot validate artifacts for {name!r} without its build directory"
                    )
                build_root = _absolute_directory(build_directory)
                seen_artifacts: set[str] = set()
                for artifact_offset, artifact_value in enumerate(artifacts):
                    artifact = _require_mapping(
                        artifact_value,
                        f"{profile} target {name!r} artifact {artifact_offset}",
                    )
                    artifact_path = artifact.get("path")
                    if not isinstance(artifact_path, str) or not artifact_path or "\\" in artifact_path:
                        raise InventoryError(
                            f"{profile}: target {name!r} artifact {artifact_offset} has an unsafe path"
                        )
                    candidate = Path(artifact_path)
                    if (
                        (candidate.drive and not candidate.is_absolute())
                        or any(part in {"", ".", ".."} for part in candidate.parts)
                    ):
                        raise InventoryError(
                            f"{profile}: target {name!r} artifact {artifact_path!r} is not contained"
                        )
                    absolute_artifact = (
                        _absolute_directory(candidate)
                        if candidate.is_absolute()
                        else _absolute_directory(build_root / candidate)
                    )
                    try:
                        common = os.path.commonpath((os.fspath(build_root), os.fspath(absolute_artifact)))
                    except ValueError as error:
                        raise InventoryError(
                            f"{profile}: target {name!r} artifact {artifact_path!r} is on another volume"
                        ) from error
                    if os.path.normcase(common) != os.path.normcase(os.fspath(build_root)):
                        raise InventoryError(
                            f"{profile}: target {name!r} artifact {artifact_path!r} escapes the build directory"
                        )
                    normalized_artifact = _normalize_directory(str(absolute_artifact))
                    artifact_key = os.path.normcase(normalized_artifact)
                    if artifact_key in seen_artifacts:
                        raise InventoryError(
                            f"{profile}: target {name!r} repeats artifact {artifact_path!r}"
                        )
                    seen_artifacts.add(artifact_key)
                    artifact_paths.append(normalized_artifact)
                if not any(
                    Path(path).name.casefold() == name_on_disk.casefold()
                    for path in artifact_paths
                ):
                    raise InventoryError(
                        f"{profile}: target {name!r} nameOnDisk is absent from its artifact list"
                    )
            key = (name, config_name)
            value: dict[str, Any] = {
                "target": name,
                "id": reference_id,
                "kind": kind,
                "configuration": config_name,
                "artifactState": "declared-not-built",
                "nameOnDisk": name_on_disk if isinstance(name_on_disk, str) else "",
                "artifacts": sorted(artifact_paths),
            }
            if key in targets:
                raise InventoryError(f"{profile}: duplicate codemodel target {key}")
            targets[key] = value
    return {
        "profile": profile,
        "status": "available",
        "targets": sorted(targets.values(), key=lambda item: (item["target"], item["configuration"])),
    }


def _reply_records_digest(records: list[dict[str, Any]]) -> str:
    payload = json.dumps(records, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _extract_reply_core_once(
    build_dir: Path,
    profile: str,
    *,
    selected_index: str = "",
    client_name: str = "",
    query: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], _ReplySnapshot, list[dict[str, Any]]] | None:
    snapshot = _snapshot_reply_directory(build_dir, profile)
    if snapshot is None:
        return None
    indices = sorted(
        name for name in snapshot.entry_names if name.startswith("index-") and name.endswith(".json")
    )
    if len(indices) > _MAX_REPLY_INDICES:
        raise ReplyValidationError(
            f"{profile}: CMake File API reply has {len(indices)} indices, above the {_MAX_REPLY_INDICES} bound"
        )
    if not indices:
        raise ReplyValidationError(f"{profile}: CMake File API reply has no index")
    if selected_index:
        if selected_index not in indices:
            raise ConcurrentReplyUpdate(
                f"{profile}: selected CMake File API index {selected_index!r} is no longer present"
            )
        candidate_names = [selected_index]
    else:
        candidate_names = list(reversed(indices))

    index_name = ""
    index: dict[str, Any] | None = None
    client_files: dict[str, str] = {}
    cmake_identity: dict[str, Any] = {}
    if client_name and query is None:
        raise ReplyValidationError(f"{profile}: client-bound reply requires its exact query")
    for candidate_name in candidate_names:
        candidate = _validate_index(
            snapshot.read_json(candidate_name, _MAX_INDEX_BYTES, f"{profile} index"),
            profile,
        )
        if client_name:
            reply = _require_mapping(candidate.get("reply"), f"{profile} File API index reply")
            if client_name not in reply:
                if selected_index:
                    raise ReplyValidationError(
                        f"{profile}: selected index has no reply for client {client_name!r}"
                    )
                continue
            client_files = _client_reply_objects(candidate, client_name, query, profile)
            cmake_identity = _index_cmake_identity(candidate, profile)
        index_name = candidate_name
        index = candidate
        break
    if index is None:
        raise ConcurrentReplyUpdate(
            f"{profile}: no captured File API index contains the exact client response"
        )

    codemodel_file = (
        client_files["codemodel"]
        if client_name
        else _reply_object(index, "codemodel", 2, profile)
    )
    if not codemodel_file:
        raise ReplyValidationError(f"{profile}: CMake File API index has no codemodel-v2 reply")
    codemodel = _require_mapping(
        snapshot.read_json(codemodel_file, _MAX_CODEMODEL_BYTES, f"{profile} codemodel"),
        f"{profile} codemodel-v2",
    )
    configurations = _require_list(
        codemodel.get("configurations"), f"{profile} codemodel configurations", _MAX_CONFIGURATIONS
    )
    paths = _require_mapping(codemodel.get("paths", {}), f"{profile} codemodel paths")
    for field_name in ("source", "build"):
        if field_name in paths and not isinstance(paths[field_name], str):
            raise ReplyValidationError(f"{profile}: codemodel path {field_name!r} must be a string")

    cache_file = client_files["cache"] if client_name else _reply_object(index, "cache", 2, profile)
    if not cache_file:
        raise ReplyValidationError(
            f"{profile}: CMake File API reply has no cache-v2 object; evidence cannot be bound to a configuration"
        )
    cache = _require_mapping(
        snapshot.read_json(cache_file, _MAX_CACHE_BYTES, f"{profile} cache"),
        f"{profile} cache-v2",
    )
    cache_values = _bound_cache_entries(cache, profile)

    target_documents: dict[str, dict[str, Any]] = {}
    reference_count = 0
    for offset, value in enumerate(configurations):
        configuration = _require_mapping(value, f"{profile} codemodel configuration {offset}")
        references = _require_list(
            configuration.get("targets"), f"{profile} codemodel configuration {offset} targets", _MAX_TARGET_REFERENCES
        )
        reference_count += len(references)
        if reference_count > _MAX_TARGET_REFERENCES:
            raise ReplyValidationError(
                f"{profile}: codemodel has more than {_MAX_TARGET_REFERENCES} target references"
            )
        for target_offset, reference_value in enumerate(references):
            reference = _require_mapping(
                reference_value, f"{profile} codemodel target reference {offset}:{target_offset}"
            )
            target_file = _reply_filename(
                reference.get("jsonFile"), f"{profile} codemodel target reference jsonFile"
            )
            if target_file not in target_documents:
                target_documents[target_file] = _require_mapping(
                    snapshot.read_json(target_file, _MAX_TARGET_BYTES, f"{profile} target"),
                    f"{profile} target {target_file!r}",
                )

    try:
        evidence = parse_codemodel_targets(
            profile, codemodel, target_documents, snapshot.build_directory
        )
    except InventoryError as error:
        raise ReplyValidationError(str(error)) from error
    evidence.update(
        {
            "evidenceDirectory": _normalize_directory(str(snapshot.build_directory)),
            "replyIndex": index_name,
            "sourceDirectory": _normalize_directory(paths.get("source")),
            "buildDirectory": _normalize_directory(paths.get("build")),
            "generator": cache_values.get("CMAKE_GENERATOR", ""),
            "architecture": cache_values.get("CMAKE_GENERATOR_PLATFORM", ""),
            "toolset": cache_values.get("CMAKE_GENERATOR_TOOLSET", ""),
            "cacheVariables": cache_values,
            "configurations": sorted(
                {
                    configuration.get("name", "")
                    for configuration in configurations
                    if isinstance(configuration, dict)
                }
            ),
            "queryClient": client_name,
            "cmakeProducer": cmake_identity,
        }
    )
    records = snapshot.consumed_records()
    evidence["replyDigest"] = _reply_records_digest(records)
    evidence["replyFileCount"] = len(records)
    snapshot.assert_stable()
    return evidence, snapshot, records


def _extract_reply_core(
    build_dir: Path,
    profile: str,
    *,
    selected_index: str = "",
    client_name: str = "",
    query: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], _ReplySnapshot, list[dict[str, Any]]] | None:
    """Read one coherent reply, restarting only onto CMake's newest index."""
    last_error: ConcurrentReplyUpdate | None = None
    for attempt in range(_MAX_CAPTURE_RESTARTS):
        try:
            return _extract_reply_core_once(
                build_dir,
                profile,
                selected_index=selected_index,
                client_name=client_name,
                query=query,
            )
        except ConcurrentReplyUpdate as error:
            last_error = error
            # A provenance record names one immutable index.  Moving it to a
            # later index would mix transactions; only an in-flight capture may
            # restart from CMake's newest matching index.
            if selected_index or attempt + 1 >= _MAX_CAPTURE_RESTARTS:
                raise
    assert last_error is not None
    raise last_error


def _provenance_path(build_dir: Path, profile: str) -> Path:
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", profile):
        raise ReplyValidationError(f"invalid provenance profile name {profile!r}")
    return _absolute_directory(build_dir) / ".cmake" / "api" / "v1" / "provenance" / (
        f"{profile}-{_PROVENANCE_FILE}"
    )


def _load_provenance_document(build_dir: Path, profile: str) -> dict[str, Any] | None:
    path = _provenance_path(build_dir, profile)
    try:
        os.lstat(path)
    except FileNotFoundError:
        return None
    except OSError as error:
        raise ReplyValidationError(f"cannot inspect {profile} provenance record: {error}") from error
    return _require_mapping(
        _read_bounded_json_file(path, _MAX_PROVENANCE_BYTES, f"{profile} provenance"),
        f"{profile} provenance record",
    )


def _provenance_selection(
    record: dict[str, Any], profile: str
) -> tuple[str, str, dict[str, Any]]:
    if record.get("schemaVersion") != _PROVENANCE_SCHEMA:
        raise ReplyValidationError(f"{profile}: unsupported provenance schemaVersion")
    if record.get("producer") != _PROVENANCE_PRODUCER or record.get("profile") != profile:
        raise ReplyValidationError(f"{profile}: provenance record has an unknown producer or profile")
    transaction = _require_mapping(record.get("transaction"), f"{profile} transaction")
    query = _require_mapping(transaction.get("query"), f"{profile} transaction query")
    client_name = transaction.get("queryClient")
    run_id = transaction.get("runId")
    if (
        not isinstance(run_id, str)
        or not re.fullmatch(r"[0-9a-f]{32}", run_id)
        or client_name != _CAPTURE_CLIENT_PREFIX + run_id
    ):
        raise ReplyValidationError(f"{profile}: provenance transaction has invalid client identity")
    if query != _capture_query(profile, run_id):
        raise ReplyValidationError(
            f"{profile}: provenance transaction does not contain the exact owned client query"
        )
    query_payload = (json.dumps(query, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
    if transaction.get("querySha256") != hashlib.sha256(query_payload).hexdigest():
        raise ReplyValidationError(f"{profile}: provenance transaction query digest differs")
    reply = _require_mapping(record.get("reply"), f"{profile} provenance reply")
    index_name = _reply_filename(reply.get("index"), f"{profile} provenance reply index")
    return index_name, str(client_name), query


def _load_producer_provenance(
    record: dict[str, Any] | None,
    profile: str,
    evidence: dict[str, Any],
    records: list[dict[str, Any]],
) -> dict[str, Any]:
    if record is None:
        return {"state": "missing", "recordFile": _PROVENANCE_FILE}
    required = {
        "schemaVersion", "producer", "profile", "evidenceDirectory", "ci",
        "transaction", "observed", "artifacts", "reply",
    }
    if set(record) != required:
        raise ReplyValidationError(
            f"{profile}: provenance record fields must be exactly {sorted(required)}"
        )
    index_name, client_name, query = _provenance_selection(record, profile)
    if _normalize_directory(record.get("evidenceDirectory")) != evidence["evidenceDirectory"]:
        raise ReplyValidationError(f"{profile}: provenance record names another build directory")

    ci = _require_mapping(record.get("ci"), f"{profile} CI producer context")
    ci_fields = {
        "provider", "repository", "sourceCommit", "runId", "runAttempt", "workflowRef", "job", "runnerOs"
    }
    if set(ci) != ci_fields:
        raise ReplyValidationError(f"{profile}: CI producer context fields are incomplete")
    try:
        runtime_ci = _github_actions_context()
    except InventoryError as error:
        raise ReplyValidationError(str(error)) from error
    if runtime_ci is None:
        raise ReplyValidationError(
            f"{profile}: configured evidence has no live GitHub Actions producer context"
        )
    if ci != runtime_ci:
        raise ReplyValidationError(
            f"{profile}: producer record was not created by this GitHub Actions run/job"
        )
    # This equality is a containment/replay check, not evidence that this job
    # actually invoked CMake.  The job owns its environment, checkout, File API
    # reply, record, artifacts, and every former same-job OIDC audience.  Do
    # not let their agreement manufacture a producer authority claim.

    transaction = _require_mapping(record.get("transaction"), f"{profile} transaction")
    transaction_required = {
        "runId", "queryClient", "querySha256", "query", "profile", "preset",
        "configuration", "sourceDirectory", "buildDirectory", "configure",
        "repositoryBefore", "repositoryAfter",
    }
    if set(transaction) != transaction_required:
        raise ReplyValidationError(f"{profile}: provenance transaction fields are incomplete")
    if transaction.get("profile") != profile or transaction.get("queryClient") != client_name:
        raise ReplyValidationError(f"{profile}: provenance transaction profile/client differs")
    profile_data = load_stable_profile()
    profile_config = next(
        (item for item in profile_data["buildConfigurations"] if item.get("id") == profile),
        None,
    )
    if profile_config is None:
        raise ReplyValidationError(f"{profile}: provenance transaction names an unknown profile")
    expected_preset = str(profile_config.get("preset", ""))
    expected_configuration = str(profile_config.get("configuration", ""))
    if (
        transaction.get("preset") != expected_preset
        or transaction.get("configuration") != expected_configuration
    ):
        raise ReplyValidationError(f"{profile}: provenance transaction preset/configuration differs")
    configure = _require_mapping(transaction.get("configure"), f"{profile} configure transaction")
    configure_required = {"executable", "executableIdentity", "version", "argv", "cwd", "exitCode"}
    if set(configure) != configure_required or configure.get("exitCode") != 0:
        raise ReplyValidationError(f"{profile}: configure transaction is incomplete or unsuccessful")
    argv = configure.get("argv")
    if (
        not isinstance(argv, list)
        or not argv
        or any(not isinstance(item, str) or not item for item in argv)
        or _normalize_directory(argv[0]) != _normalize_directory(configure.get("executable"))
    ):
        raise ReplyValidationError(f"{profile}: configure argv does not bind its executable")
    if expected_preset:
        expected_argv = [configure.get("executable"), "--preset", expected_preset]
    else:
        expected_argv = [
            configure.get("executable"),
            "-S",
            transaction.get("sourceDirectory"),
            "-B",
            transaction.get("buildDirectory"),
        ]
    if argv != expected_argv:
        raise ReplyValidationError(f"{profile}: configure argv is not the canonical profile invocation")
    executable_identity = _require_mapping(
        configure.get("executableIdentity"), f"{profile} CMake executable identity"
    )
    if set(executable_identity) != {"bytes", "sha256"}:
        raise ReplyValidationError(f"{profile}: CMake executable identity fields are invalid")
    if type(executable_identity.get("bytes")) is not int or not re.fullmatch(
        r"[0-9a-f]{64}", str(executable_identity.get("sha256", ""))
    ):
        raise ReplyValidationError(f"{profile}: CMake executable identity is invalid")
    try:
        current_executable_identity = _executable_identity(Path(str(configure.get("executable"))))
    except (InventoryError, OSError, ValueError) as error:
        raise ReplyValidationError(
            f"{profile}: recorded CMake executable cannot be revalidated: {error}"
        ) from error
    if current_executable_identity != executable_identity:
        raise ReplyValidationError(f"{profile}: CMake executable identity no longer matches producer record")

    before = _require_mapping(transaction.get("repositoryBefore"), f"{profile} pre-configure repository")
    after = _require_mapping(transaction.get("repositoryAfter"), f"{profile} post-configure repository")
    repository_fields = {"root", "commit", "clean", "untrackedPolicy", "statusSha256"}
    if set(before) != repository_fields or set(after) != repository_fields or before != after:
        raise ReplyValidationError(f"{profile}: repository identity changed across configure")
    root = _normalize_directory(before.get("root"))
    commit = before.get("commit")
    if (
        not root
        or not isinstance(commit, str)
        or not re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", commit)
        or before.get("clean") is not True
        or before.get("untrackedPolicy") != "all-nonignored"
        or before.get("statusSha256") != hashlib.sha256(b"").hexdigest()
    ):
        raise ReplyValidationError(f"{profile}: configure did not bind an exact clean repository state")
    if str(commit).lower() != ci["sourceCommit"]:
        raise ReplyValidationError(f"{profile}: producer CI SHA differs from configured repository commit")

    observed = _require_mapping(record.get("observed"), f"{profile} observed configure state")
    observed_fields = {
        "sourceDirectory", "buildDirectory", "preset", "configuration", "generator",
        "architecture", "toolset", "cacheVariables", "cmakeProducer",
    }
    if set(observed) != observed_fields:
        raise ReplyValidationError(f"{profile}: observed configure fields are incomplete")
    if (
        observed.get("preset") != expected_preset
        or observed.get("configuration") != expected_configuration
    ):
        raise ReplyValidationError(f"{profile}: observed preset/configuration differs")
    for field_name in (
        "sourceDirectory", "buildDirectory", "generator", "architecture", "toolset",
        "cacheVariables", "cmakeProducer",
    ):
        if observed.get(field_name) != evidence.get(field_name):
            raise ReplyValidationError(
                f"{profile}: observed {field_name} differs from the selected client reply"
            )
    if transaction.get("sourceDirectory") != observed.get("sourceDirectory") or (
        transaction.get("buildDirectory") != observed.get("buildDirectory")
    ):
        raise ReplyValidationError(f"{profile}: configured paths differ from the transaction")
    cmake_producer = _require_mapping(observed.get("cmakeProducer"), f"{profile} CMake producer")
    if (
        _normalize_directory(cmake_producer.get("executable"))
        != _normalize_directory(configure.get("executable"))
        or cmake_producer.get("version") != configure.get("version")
        or cmake_producer.get("generator") != observed.get("generator")
    ):
        raise ReplyValidationError(f"{profile}: CMake index identity differs from invoked CMake")

    reply = _require_mapping(record.get("reply"), f"{profile} provenance reply")
    if set(reply) != {"index", "files", "digest"}:
        raise ReplyValidationError(f"{profile}: provenance reply record has invalid fields")
    files = _require_list(reply.get("files"), f"{profile} provenance reply files", _MAX_REPLY_FILES)
    normalized_files: list[dict[str, Any]] = []
    seen: set[str] = set()
    for offset, value in enumerate(files):
        file_record = _require_mapping(value, f"{profile} provenance reply file {offset}")
        if set(file_record) != {"name", "bytes", "sha256"}:
            raise ReplyValidationError(f"{profile}: provenance reply file {offset} has invalid fields")
        name = _reply_filename(file_record.get("name"), f"{profile} provenance reply file {offset} name")
        size = file_record.get("bytes")
        digest = file_record.get("sha256")
        if name in seen:
            raise ReplyValidationError(f"{profile}: provenance reply file list is duplicated")
        if type(size) is not int or size < 0 or size > _MAX_REPLY_FILE_BYTES:
            raise ReplyValidationError(f"{profile}: provenance reply file {name!r} has invalid size")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ReplyValidationError(f"{profile}: provenance reply file {name!r} has invalid digest")
        seen.add(name)
        normalized_files.append({"name": name, "bytes": size, "sha256": digest})
    digest = _reply_records_digest(records)
    if normalized_files != records or reply.get("digest") != digest or index_name != evidence["replyIndex"]:
        raise ReplyValidationError(f"{profile}: selected reply files differ from configure provenance")

    artifacts = _require_mapping(record.get("artifacts"), f"{profile} provenance artifacts")
    if set(artifacts) != {"state", "build", "targets"}:
        raise ReplyValidationError(f"{profile}: artifact provenance fields are invalid")
    artifact_state = artifacts.get("state")
    if artifact_state not in {"declared-not-built", "locally-observed-post-build"}:
        raise ReplyValidationError(f"{profile}: artifact provenance has an invalid state")
    expected_build_argv = [
        configure.get("executable"), "--build", evidence["buildDirectory"],
        "--config", expected_configuration, "--parallel",
    ]
    if artifact_state == "declared-not-built":
        if artifacts.get("build") is not None or artifacts.get("targets") != []:
            raise ReplyValidationError(f"{profile}: declared artifacts carry post-build data")
    else:
        build = _require_mapping(artifacts.get("build"), f"{profile} build transaction")
        if set(build) != {"argv", "exitCode"} or build.get("exitCode") != 0:
            raise ReplyValidationError(f"{profile}: artifact build transaction is incomplete or unsuccessful")
        if build.get("argv") != expected_build_argv:
            raise ReplyValidationError(f"{profile}: artifact build argv is not the canonical profile invocation")
        claimed_targets = _require_list(
            artifacts.get("targets"), f"{profile} verified artifact targets", _MAX_TARGET_REFERENCES
        )
        try:
            actual_targets = _capture_artifact_manifest(
                evidence, Path(evidence["evidenceDirectory"])
            )
        except InventoryError as error:
            raise ReplyValidationError(f"{profile}: built artifact identity is unavailable: {error}") from error
        if claimed_targets != actual_targets:
            raise ReplyValidationError(f"{profile}: post-build artifact identities differ from producer record")
        identities_by_target = {
            (item["id"], item["configuration"], item["target"]): item["artifactIdentities"]
            for item in actual_targets
        }
        for target in evidence.get("targets", []):
            key = (target["id"], target["configuration"], target["target"])
            target["artifactState"] = "locally-observed-post-build"
            target["artifactIdentities"] = identities_by_target[key]

    return {
        # A local receipt may be internally consistent, but nothing in this
        # producer job is independent of the source it is evaluating.  In
        # particular, GitHub will sign an OIDC token for an audience selected
        # by that same job.  Keep the structural record for diagnosis while
        # making the missing protected external verifier machine-readable.
        "state": "unavailable",
        "authority": _CI120_EXTERNAL_AUTHORITY,
        "authorityReason": (
            "A same-job GitHub Actions token, environment, checkout, provenance record, "
            "artifact path, and hash are producer-controlled inputs. A protected external "
            "attestation verifier must independently validate the captured artifact before "
            "CI-120 can report producer-verified evidence."
        ),
        "structuralState": "validated",
        "recordFile": _provenance_path(Path(evidence["evidenceDirectory"]), profile).name,
        "recordSha256": hashlib.sha256(
            (json.dumps(record, indent=2, sort_keys=False) + "\n").encode("utf-8")
        ).hexdigest(),
        "producer": _PROVENANCE_PRODUCER,
        "profile": profile,
        "repositoryRoot": root,
        "sourceCommit": str(commit).lower(),
        "sourceClean": True,
        "untrackedPolicy": before["untrackedPolicy"],
        "replyDigest": digest,
        "queryClient": client_name,
        "configureArgv": argv,
        "cmakeExecutable": configure["executable"],
        "cmakeVersion": configure["version"],
        "ciProvider": ci["provider"],
        "ciRepository": ci["repository"],
        "ciRunId": ci["runId"],
        "ciRunAttempt": ci["runAttempt"],
        "ciWorkflowRef": ci["workflowRef"],
        "ciJob": ci["job"],
        "ciRunnerOs": ci["runnerOs"],
        "artifactState": artifact_state,
    }


def extract_codemodel_targets(
    build_dir: Path, profile: str, ignored_caller_commit: str = ""
) -> dict[str, Any]:
    """Read bounded File API evidence and verify its producer record.

    ``ignored_caller_commit`` remains only so older callers fail closed during
    migration. It is deliberately never recorded or trusted; commit identity
    comes exclusively from ``capture_provenance.py``'s producer record.
    """
    del ignored_caller_commit
    snapshot: _ReplySnapshot | None = None
    try:
        provenance_record = _load_provenance_document(build_dir, profile)
        selection: tuple[str, str, dict[str, Any]] | None = None
        if provenance_record is not None:
            selection = _provenance_selection(provenance_record, profile)
        core = _extract_reply_core(
            build_dir,
            profile,
            selected_index=selection[0] if selection else "",
            client_name=selection[1] if selection else "",
            query=selection[2] if selection else None,
        )
        if core is None:
            return {
                "profile": profile,
                "status": "absent",
                "evidenceDirectory": _normalize_directory(str(_absolute_directory(build_dir))),
                "targets": [],
            }
        evidence, snapshot, records = core
        evidence["producerProvenance"] = _load_producer_provenance(
            provenance_record, profile, evidence, records
        )
        snapshot.assert_stable()
        return evidence
    except ReplyValidationError as error:
        return {
            "profile": profile,
            "status": "invalid",
            "evidenceDirectory": _normalize_directory(str(_absolute_directory(build_dir))),
            "targets": [],
            "rejection": str(error),
        }
    finally:
        if snapshot is not None:
            snapshot.close()


def _capture_material_errors(evidence: dict[str, Any], profile: str) -> list[str]:
    errors = [
        field_name
        for field_name in ("sourceDirectory", "buildDirectory", "generator", "architecture", "toolset")
        if not evidence.get(field_name)
    ]
    cache = evidence.get("cacheVariables", {})
    for name in (
        "CMAKE_GENERATOR",
        "CMAKE_GENERATOR_PLATFORM",
        "CMAKE_GENERATOR_TOOLSET",
        "CMAKE_HOME_DIRECTORY",
    ):
        if not isinstance(cache, dict) or not cache.get(name):
            errors.append(f"cacheVariables.{name}")
    if (
        evidence.get("buildDirectory")
        and evidence.get("evidenceDirectory")
        and Path(str(evidence["buildDirectory"])).as_posix().rstrip("/").casefold()
        != Path(str(evidence["evidenceDirectory"])).as_posix().rstrip("/").casefold()
    ):
        errors.append("buildDirectory!=evidenceDirectory")
    if (
        isinstance(cache, dict)
        and cache.get("CMAKE_HOME_DIRECTORY")
        and evidence.get("sourceDirectory")
        and Path(str(cache["CMAKE_HOME_DIRECTORY"])).as_posix().rstrip("/").casefold()
        != Path(str(evidence["sourceDirectory"])).as_posix().rstrip("/").casefold()
    ):
        errors.append("cacheVariables.CMAKE_HOME_DIRECTORY!=sourceDirectory")
    profile_data = load_stable_profile()
    config = next((item for item in profile_data["buildConfigurations"] if item["id"] == profile), None)
    if config is None:
        errors.append("knownProfile")
    elif not evidence.get("configurations"):
        errors.append("configurations")
    if config and config.get("preset"):
        preset = resolve_configure_preset(extract_cmake_presets(), config["preset"])
        for name in preset.get("cacheVariables", {}):
            if not isinstance(cache, dict) or name not in cache:
                errors.append(f"cacheVariables.{name}")
    return sorted(set(errors))


def _ensure_safe_directory(path: Path, trust_root: Path, label: str) -> Path:
    """Create missing descendants while refusing links/reparses at every level."""
    target = _absolute_directory(path)
    root = _absolute_directory(trust_root)
    root_state = _validate_directory_chain(root, f"{label} trust root")
    if root_state is None:
        raise InventoryError(f"{label} trust root does not exist")
    try:
        common = os.path.commonpath((os.fspath(root), os.fspath(target)))
    except ValueError as error:
        raise InventoryError(f"{label} is on another volume from its trust root") from error
    if os.path.normcase(common) != os.path.normcase(os.fspath(root)):
        raise InventoryError(f"{label} escapes its trust root")
    current = root
    for component in target.relative_to(root).parts:
        current /= component
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            try:
                os.mkdir(current)
            except FileExistsError:
                pass
            metadata = os.lstat(current)
        _validate_directory(metadata, f"{label} component {current}")
    final = _validate_directory_chain(target, label)
    if final is None:
        raise InventoryError(f"{label} could not be created")
    return final[0]


def _capture_query(profile: str, run_id: str) -> dict[str, Any]:
    return {
        "requests": [
            {"kind": "codemodel", "version": 2, "client": {"runId": run_id}},
            {"kind": "cache", "version": 2, "client": {"runId": run_id}},
        ],
        "client": {"producer": _PROVENANCE_PRODUCER, "profile": profile, "runId": run_id},
    }


def _cleanup_owned_query_client(
    query_path: Path, client_directory: Path, profile: str
) -> InventoryError | None:
    """Remove an invocation-owned File API client without hiding retained state."""
    last_denial: PermissionError | None = None
    for attempt in range(_QUERY_CLEANUP_ATTEMPTS):
        query_absent_or_removed = False
        try:
            query_path.unlink(missing_ok=True)
        except FileNotFoundError:
            query_absent_or_removed = True
        except PermissionError as error:
            last_denial = error
        except OSError as error:
            return InventoryError(
                f"{profile}: cannot remove owned File API query file {query_path}: {error}"
            )
        else:
            query_absent_or_removed = True

        if query_absent_or_removed:
            try:
                client_directory.rmdir()
            except FileNotFoundError:
                return None
            except PermissionError as error:
                last_denial = error
            except OSError as error:
                return InventoryError(
                    f"{profile}: cannot remove owned File API client directory "
                    f"{client_directory}: {error}"
                )
            else:
                return None

        if attempt + 1 < _QUERY_CLEANUP_ATTEMPTS:
            time.sleep(_QUERY_CLEANUP_RETRY_DELAY_SECONDS)

    try:
        retained = [str(path) for path in (query_path, client_directory) if path.exists()]
    except OSError as error:
        return InventoryError(f"{profile}: cannot verify owned File API query cleanup: {error}")
    if not retained:
        return None
    detail = f": {last_denial}" if last_denial is not None else ""
    return InventoryError(
        f"{profile}: cannot remove owned File API query client after "
        f"{_QUERY_CLEANUP_ATTEMPTS} attempts; retained {', '.join(retained)}{detail}"
    )


def _resolve_cmake_executable(value: str | Path) -> Path:
    text = os.fspath(value)
    located = shutil.which(text) if not Path(text).is_absolute() else text
    if not located:
        raise InventoryError(f"CMake executable {text!r} was not found")
    path = Path(located).resolve(strict=True)
    if _validate_directory_chain(path.parent, "CMake executable parent") is None:
        raise InventoryError("CMake executable parent does not exist")
    metadata = os.lstat(path)
    _validate_regular_file(metadata, "CMake executable")
    return path


def _executable_identity(path: Path) -> dict[str, Any]:
    maximum = 256 * 1024 * 1024
    metadata = os.lstat(path)
    if metadata.st_size > maximum:
        raise InventoryError("CMake executable exceeds the 256 MiB identity bound")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        _validate_regular_file(opened, "opened CMake executable")
        if _identity_token(opened) != _identity_token(metadata):
            raise InventoryError("CMake executable changed while it was opened")
        digest = hashlib.sha256()
        remaining = opened.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise InventoryError("CMake executable was truncated while hashing")
            digest.update(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise InventoryError("CMake executable grew while hashing")
    finally:
        os.close(descriptor)
    return {"bytes": int(metadata.st_size), "sha256": digest.hexdigest()}


def _artifact_identity(path: Path, build_directory: Path, label: str) -> dict[str, Any]:
    """Hash one built artifact through a stable, contained regular-file handle."""
    build_root = _absolute_directory(build_directory)
    artifact = _absolute_directory(path)
    if _validate_directory_chain(build_root, f"{label} build directory") is None:
        raise InventoryError(f"{label} build directory does not exist")
    try:
        common = os.path.commonpath((os.fspath(build_root), os.fspath(artifact)))
    except ValueError as error:
        raise InventoryError(f"{label} is on another volume from its build directory") from error
    if os.path.normcase(common) != os.path.normcase(os.fspath(build_root)):
        raise InventoryError(f"{label} escapes its build directory")
    if _validate_directory_chain(artifact.parent, f"{label} parent") is None:
        raise InventoryError(f"{label} parent does not exist")
    try:
        metadata = os.lstat(artifact)
    except OSError as error:
        raise InventoryError(f"cannot inspect {label}: {error}") from error
    _validate_regular_file(metadata, label)
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(artifact, flags)
    except OSError as error:
        raise InventoryError(f"cannot open {label} without following links: {error}") from error
    try:
        opened = os.fstat(descriptor)
        _validate_regular_file(opened, f"opened {label}")
        if _opened_link_count(descriptor, opened) != 1:
            raise InventoryError(f"opened {label} has multiple hard links")
        if _identity_token(opened) != _identity_token(metadata):
            raise InventoryError(f"{label} changed while it was opened")
        opened_token = _stat_token(opened)
        digest = hashlib.sha256()
        remaining = opened.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise InventoryError(f"{label} was truncated while hashing")
            digest.update(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise InventoryError(f"{label} grew while hashing")
        if _stat_token(os.fstat(descriptor)) != opened_token:
            raise InventoryError(f"{label} changed while hashing")
    finally:
        os.close(descriptor)
    try:
        after = os.lstat(artifact)
    except OSError as error:
        raise InventoryError(f"cannot re-check {label}: {error}") from error
    if _stat_token(after) != _stat_token(metadata):
        raise InventoryError(f"{label} changed after hashing")
    return {
        "path": _normalize_directory(str(artifact)),
        "bytes": int(metadata.st_size),
        "sha256": digest.hexdigest(),
    }


def _capture_artifact_manifest(evidence: dict[str, Any], build_directory: Path) -> list[dict[str, Any]]:
    """Capture immutable post-build identities for every configured target artifact."""
    manifest: list[dict[str, Any]] = []
    for target in evidence.get("targets", []):
        if not isinstance(target, dict):
            raise InventoryError("configured target evidence is malformed")
        target_id = target.get("id")
        target_name = target.get("target")
        configuration = target.get("configuration")
        artifacts = target.get("artifacts")
        if (
            not isinstance(target_id, str)
            or not target_id
            or not isinstance(target_name, str)
            or not target_name
            or not isinstance(configuration, str)
            or not configuration
            or not isinstance(artifacts, list)
            or not artifacts
        ):
            raise InventoryError("configured target lacks a stable artifact identity")
        identities = [
            _artifact_identity(Path(item), build_directory, f"{target_name} artifact {offset}")
            for offset, item in enumerate(artifacts)
            if isinstance(item, str) and item
        ]
        if len(identities) != len(artifacts):
            raise InventoryError(f"configured target {target_name!r} has an invalid artifact path")
        manifest.append(
            {
                "id": target_id,
                "target": target_name,
                "configuration": configuration,
                "artifactIdentities": identities,
            }
        )
    return sorted(manifest, key=lambda item: (item["id"], item["configuration"], item["target"]))


def _capture_plan(
    build_dir: Path, profile: str, cmake_executable: Path
) -> tuple[dict[str, Any], Path, Path, list[str]]:
    profile_data = load_stable_profile()
    config = next(
        (item for item in profile_data["buildConfigurations"] if item["id"] == profile),
        None,
    )
    if config is None:
        raise InventoryError(f"unknown capture profile {profile!r}")
    source_dir = _absolute_directory(REPO_ROOT)
    supplied_build = _absolute_directory(build_dir)
    preset_name = str(config.get("preset", ""))
    if preset_name:
        preset = resolve_configure_preset(extract_cmake_presets(), preset_name)
        binary = str(preset.get("resolvedBinaryDir", ""))
        if not binary:
            raise InventoryError(f"capture preset {preset_name!r} has no binaryDir")
        expected_build = _absolute_directory(Path(binary.replace("${sourceDir}", str(source_dir))))
        argv = [str(cmake_executable), "--preset", preset_name]
    else:
        source_dir = _absolute_directory(REPO_ROOT / str(config.get("sourceDirectory", "")))
        expected_build = _absolute_directory(REPO_ROOT / str(config.get("buildDirectory", "")))
        argv = [str(cmake_executable), "-S", str(source_dir), "-B", str(expected_build)]
    if os.path.normcase(os.fspath(expected_build)) != os.path.normcase(os.fspath(supplied_build)):
        raise InventoryError(
            f"{profile}: capture build directory is {supplied_build}, expected {expected_build}"
        )
    return config, source_dir, expected_build, argv


def _existing_index_identities(build_dir: Path, profile: str) -> dict[str, str]:
    snapshot = _snapshot_reply_directory(build_dir, profile)
    if snapshot is None:
        return {}
    try:
        indices = sorted(
            name
            for name in snapshot.entry_names
            if name.startswith("index-") and name.endswith(".json")
        )
        if len(indices) > _MAX_REPLY_INDICES:
            raise ReplyValidationError(
                f"{profile}: CMake File API reply has {len(indices)} indices, "
                f"above the {_MAX_REPLY_INDICES} bound"
            )
        result: dict[str, str] = {}
        for name in indices:
            payload = snapshot.read_bytes(name, _MAX_INDEX_BYTES, f"{profile} existing index")
            result[name] = hashlib.sha256(payload).hexdigest()
        snapshot.assert_stable()
        return result
    finally:
        snapshot.close()


def capture_codemodel_transaction(
    build_dir: Path,
    profile: str,
    *,
    cmake_executable: str | Path = "cmake",
    build: bool = False,
) -> Path:
    """Own one CI query/configure/(optional) build/reply transaction."""
    executable = _resolve_cmake_executable(cmake_executable)
    config, source_dir, build_directory, argv = _capture_plan(build_dir, profile, executable)
    _validate_directory_chain(source_dir, f"{profile} source directory")
    repository_before = _repository_provenance(REPO_ROOT)
    if repository_before["clean"] is not True:
        raise InventoryError(f"{profile}: source repository must be exactly clean before configure")
    ci = _github_actions_context()
    if ci is None:
        raise InventoryError(
            "CI-120 structural capture is only supported in its GitHub Actions producer job"
        )
    if repository_before["commit"].lower() != ci["sourceCommit"]:
        raise InventoryError(
            f"{profile}: GitHub SHA does not match the checked-out source commit"
        )

    build_directory = _ensure_safe_directory(
        build_directory, REPO_ROOT, f"{profile} build directory"
    )
    existing_indices = _existing_index_identities(build_directory, profile)
    run_id = uuid.uuid4().hex
    client_name = _CAPTURE_CLIENT_PREFIX + run_id
    query = _capture_query(profile, run_id)
    query_payload = (json.dumps(query, separators=(",", ":"), sort_keys=True) + "\n").encode("utf-8")
    query_root = _ensure_safe_directory(
        build_directory / ".cmake" / "api" / "v1" / "query",
        build_directory,
        f"{profile} query root",
    )
    client_directory = query_root / client_name
    try:
        os.mkdir(client_directory)
    except FileExistsError as error:
        raise InventoryError(f"{profile}: unique File API client already exists") from error
    _validate_directory_chain(client_directory, f"{profile} File API client")
    query_path = client_directory / "query.json"
    reply_snapshot: _ReplySnapshot | None = None
    try:
        _write_atomic(query_path, query_payload)
        executable_identity = _executable_identity(executable)
        version_result = subprocess.run(
            [str(executable), "--version"],
            cwd=source_dir,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=30,
        )
        version_line = (version_result.stdout or "").splitlines()
        if version_result.returncode or not version_line or not version_line[0].startswith("cmake version "):
            raise InventoryError("cannot bind the invoked CMake version")
        cmake_version = version_line[0].removeprefix("cmake version ").strip()
        if _executable_identity(executable) != executable_identity:
            raise InventoryError(f"{profile}: CMake executable changed while querying its version")
        completed = subprocess.run(argv, cwd=source_dir, check=False)
        if completed.returncode:
            raise InventoryError(f"{profile}: CMake configure failed with exit {completed.returncode}")
        if _executable_identity(executable) != executable_identity:
            raise InventoryError(f"{profile}: CMake executable changed across configure")
        core = _extract_reply_core(
            build_directory,
            profile,
            client_name=client_name,
            query=query,
        )
        if core is None:
            raise InventoryError(f"{profile}: CMake produced no File API reply")
        evidence, reply_snapshot, records = core
        if evidence["replyIndex"] in existing_indices:
            raise InventoryError(
                f"{profile}: selected File API index was not newly generated by this configure"
            )
        missing = _capture_material_errors(evidence, profile)
        if missing:
            raise InventoryError(
                f"{profile}: configure reply lacks material fields: {', '.join(missing)}"
            )
        producer = evidence.get("cmakeProducer", {})
        if (
            _normalize_directory(producer.get("executable")) != _normalize_directory(str(executable))
            or producer.get("version") != cmake_version
        ):
            raise InventoryError(f"{profile}: selected index came from another CMake executable/version")
        if build:
            # A build changes the build-tree directory timestamps by design.  Seal
            # the File API reply before it, then reopen the exact owned index after
            # it; this detects any reply substitution without mistaking products
            # appearing under bin/ for a reply mutation.
            pre_build_records = records
            reply_snapshot.assert_stable()
            reply_snapshot.close()
            reply_snapshot = None
            build_argv = [
                _normalize_directory(str(executable)), "--build", evidence["buildDirectory"],
                "--config", str(config.get("configuration", "")), "--parallel",
            ]
            built = subprocess.run(build_argv, cwd=source_dir, check=False)
            if built.returncode:
                raise InventoryError(f"{profile}: CMake build failed with exit {built.returncode}")
            if _executable_identity(executable) != executable_identity:
                raise InventoryError(f"{profile}: CMake executable changed across build")
            refreshed = _extract_reply_core(
                build_directory,
                profile,
                selected_index=evidence["replyIndex"],
                client_name=client_name,
                query=query,
            )
            if refreshed is None:
                raise InventoryError(f"{profile}: owned File API reply disappeared during build")
            evidence, reply_snapshot, records = refreshed
            if records != pre_build_records:
                raise InventoryError(f"{profile}: owned File API reply changed across build")
            refreshed_producer = evidence.get("cmakeProducer", {})
            if (
                _normalize_directory(refreshed_producer.get("executable"))
                != _normalize_directory(str(executable))
                or refreshed_producer.get("version") != cmake_version
            ):
                raise InventoryError(f"{profile}: File API producer changed across build")
            producer = refreshed_producer
            artifact_record: dict[str, Any] = {
                "state": "locally-observed-post-build",
                "build": {"argv": build_argv, "exitCode": 0},
                "targets": _capture_artifact_manifest(evidence, build_directory),
            }
        else:
            artifact_record = {"state": "declared-not-built", "build": None, "targets": []}
        repository_after = _repository_provenance(REPO_ROOT)
        if repository_after != repository_before:
            raise InventoryError(f"{profile}: repository state changed across configure/build")

        observed = {
            "sourceDirectory": evidence["sourceDirectory"],
            "buildDirectory": evidence["buildDirectory"],
            "preset": str(config.get("preset", "")),
            "configuration": str(config.get("configuration", "")),
            "generator": evidence["generator"],
            "architecture": evidence["architecture"],
            "toolset": evidence["toolset"],
            "cacheVariables": evidence["cacheVariables"],
            "cmakeProducer": producer,
        }
        record = {
            "schemaVersion": _PROVENANCE_SCHEMA,
            "producer": _PROVENANCE_PRODUCER,
            "profile": profile,
            "evidenceDirectory": evidence["evidenceDirectory"],
            "ci": ci,
            "transaction": {
                "runId": run_id,
                "queryClient": client_name,
                "querySha256": hashlib.sha256(query_payload).hexdigest(),
                "query": query,
                "profile": profile,
                "preset": str(config.get("preset", "")),
                "configuration": str(config.get("configuration", "")),
                "sourceDirectory": evidence["sourceDirectory"],
                "buildDirectory": evidence["buildDirectory"],
                "configure": {
                    "executable": _normalize_directory(str(executable)),
                    "executableIdentity": executable_identity,
                    "version": cmake_version,
                    "argv": [_normalize_directory(str(executable)), *argv[1:]],
                    "cwd": _normalize_directory(str(source_dir)),
                    "exitCode": 0,
                },
                "repositoryBefore": repository_before,
                "repositoryAfter": repository_after,
            },
            "observed": observed,
            "artifacts": artifact_record,
            "reply": {
                "index": evidence["replyIndex"],
                "files": records,
                "digest": _reply_records_digest(records),
            },
        }
        payload = (json.dumps(record, indent=2, sort_keys=False) + "\n").encode("utf-8")
        if len(payload) > _MAX_PROVENANCE_BYTES:
            raise InventoryError("generated CI-120 provenance record exceeds its size bound")
        # Complete the read transaction before making our own metadata change
        # beneath .cmake/api/v1.
        reply_snapshot.assert_stable()
        reply_snapshot.close()
        provenance_parent = _ensure_safe_directory(
            _provenance_path(build_directory, profile).parent,
            build_directory,
            f"{profile} provenance directory",
        )
        destination = provenance_parent / _provenance_path(build_directory, profile).name
        _write_atomic(destination, payload)
        # Re-read through the same strict path before reporting success.
        _load_provenance_document(build_directory, profile)
        return destination
    finally:
        if reply_snapshot is not None:
            reply_snapshot.close()
        # This exact random client directory is owned by this invocation.
        cleanup_failure = _cleanup_owned_query_client(query_path, client_directory, profile)
        if cleanup_failure is not None and sys.exc_info()[0] is None:
            raise cleanup_failure


def capture_codemodel_provenance(build_dir: Path, profile: str) -> Path:
    """Removed post-hoc signer retained only as an explicit fail-closed API."""
    del build_dir, profile
    raise InventoryError(
        "post-hoc provenance capture is forbidden; use capture_codemodel_transaction() "
        "so this tool owns the File API query and CMake configure"
    )


def _repository_provenance(root: Path = REPO_ROOT) -> dict[str, Any]:
    """The commit and cleanliness the inventory is being generated against."""

    repository_root = _absolute_directory(root)

    def git_text(*arguments: str) -> tuple[int, str]:
        completed = subprocess.run(
            ["git", "-C", str(repository_root), *arguments],
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        return completed.returncode, completed.stdout.strip()

    head_code, head = git_text("rev-parse", "HEAD")
    status_result = subprocess.run(
        [
            "git", "-C", str(repository_root), "status", "--porcelain=v1", "-z",
            "--untracked-files=all",
        ],
        capture_output=True,
    )
    status_code = status_result.returncode
    status = status_result.stdout
    if head_code != 0 or status_code != 0:
        raise InventoryError("git provenance is unavailable; evidence cannot be bound to a commit")
    return {
        "root": _normalize_directory(str(repository_root)),
        "commit": head,
        "clean": not status,
        "untrackedPolicy": "all-nonignored",
        "statusSha256": hashlib.sha256(status).hexdigest(),
    }


def build_inventory(
    codemodels: dict[str, Path] | None = None,
) -> dict[str, Any]:
    profile = load_stable_profile()
    option_declarations = extract_cmake_options()
    all_option_declarations = extract_all_cmake_options()
    target_declarations = extract_cmake_targets()
    requested = codemodels or {}
    known_profiles = {item["id"] for item in profile["buildConfigurations"]}
    unknown_codemodels = sorted(set(requested) - known_profiles)
    if unknown_codemodels:
        raise InventoryError(f"codemodel evidence names unknown profiles: {unknown_codemodels}")
    codemodel_evidence = []
    for configuration in profile["buildConfigurations"]:
        identifier = configuration["id"]
        if identifier in requested:
            codemodel_evidence.append(extract_codemodel_targets(requested[identifier], identifier))
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
    parent_state = _validate_directory_chain(path.parent, f"output parent for {path.name}")
    if parent_state is None:
        raise InventoryError(f"output parent for {path.name} does not exist")
    parent, _ = parent_state
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=parent
    )
    temporary = Path(temporary_name)
    temporary_identity: tuple[int, ...] | None = None
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())

        temporary_metadata = os.lstat(temporary)
        _validate_regular_file(temporary_metadata, f"temporary output for {path.name}")
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        verify_descriptor = os.open(temporary, flags)
        try:
            opened = os.fstat(verify_descriptor)
            _validate_regular_file(opened, f"opened temporary output for {path.name}")
            if _opened_link_count(verify_descriptor, opened) != 1:
                raise InventoryError(f"temporary output for {path.name} has multiple hard links")
            if _identity_token(opened) != _identity_token(temporary_metadata):
                raise InventoryError(f"temporary output for {path.name} changed before publication")
            temporary_identity = _identity_token(opened)
            opened_token = _stat_token(opened)
            observed = b""
            while len(observed) < len(payload):
                chunk = os.read(verify_descriptor, len(payload) - len(observed))
                if not chunk:
                    break
                observed += chunk
            if observed != payload or os.read(verify_descriptor, 1):
                raise InventoryError(f"temporary output for {path.name} has unexpected content")
            if _stat_token(os.fstat(verify_descriptor)) != opened_token:
                raise InventoryError(f"temporary output for {path.name} changed during verification")
        finally:
            os.close(verify_descriptor)

        parent_token = _directory_token(parent, f"output parent for {path.name}")
        os.replace(temporary, path)

        published_metadata = os.lstat(path)
        _validate_regular_file(published_metadata, f"published output {path.name}")
        published_descriptor = os.open(path, flags)
        try:
            opened = os.fstat(published_descriptor)
            _validate_regular_file(opened, f"opened published output {path.name}")
            if _opened_link_count(published_descriptor, opened) != 1:
                raise InventoryError(f"published output {path.name} has multiple hard links")
            if _identity_token(opened) != _identity_token(published_metadata):
                raise InventoryError(f"published output {path.name} changed while reopening")
            if temporary_identity is None or _identity_token(opened) != temporary_identity:
                raise InventoryError(
                    f"published output {path.name} is not the verified temporary file"
                )
            opened_token = _stat_token(opened)
            observed = b""
            while len(observed) < len(payload):
                chunk = os.read(published_descriptor, len(payload) - len(observed))
                if not chunk:
                    break
                observed += chunk
            if observed != payload or os.read(published_descriptor, 1):
                raise InventoryError(f"published output {path.name} does not match generated bytes")
            if _stat_token(os.fstat(published_descriptor)) != opened_token:
                raise InventoryError(f"published output {path.name} changed during verification")
        finally:
            os.close(published_descriptor)
        if _directory_token(parent, f"output parent for {path.name}") != parent_token:
            # Replacing one child changes the directory mutation timestamp.  A
            # different identity, rather than the timestamp component, is the
            # unsafe event after publication.
            after = os.lstat(parent)
            if _directory_stat_token(after)[:3] != parent_token[:3]:
                raise InventoryError(f"output parent for {path.name} was replaced during publication")
    finally:
        temporary.unlink(missing_ok=True)


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
