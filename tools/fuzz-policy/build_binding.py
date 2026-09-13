#!/usr/bin/env python3
"""Authoritative CMake/CTest/harness structural verification for SEC-120.

Substring proofs are not proofs: a commented-out ``add_executable`` still
contains every literal a naive checker looks for. This module tokenises CMake
and C++ so that only a *declared* target, a *registered* CTest entry point, a
real ``LLVMFuzzerTestOneInput`` definition, an explicit corpus argument, and
literal runtime limits can satisfy the gate.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable, Sequence

from policy_common import PolicyError, read_confined_file

MAX_CMAKE_BYTES = 1024 * 1024
MAX_CMAKE_COMMANDS = 20_000
MAX_CMAKE_ARGUMENTS = 4_096
MAX_REACHABILITY_LISTFILES = 4_096
MAX_REACHABILITY_DEPTH = 32

ROOT_CMAKE = "CMakeLists.txt"
FUZZ_LABEL = "fuzz"
SANITIZER_LITERAL = "fsanitize=fuzzer"

_COMMAND_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_TARGET_FILE_EXPR = re.compile(r"^\$<TARGET_FILE:([A-Za-z0-9_.+-]+)>$")
_SOURCE_KEYWORDS = {"WIN32", "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL"}


@dataclass(frozen=True)
class CMakeCommand:
    name: str
    arguments: tuple[str, ...]
    line: int


def _bracket_open(text: str, index: int) -> tuple[int, str] | None:
    """Return (payload_start, closing_token) when a CMake bracket opens at index."""
    if index >= len(text) or text[index] != "[":
        return None
    cursor = index + 1
    equals = 0
    while cursor < len(text) and text[cursor] == "=":
        equals += 1
        cursor += 1
    if cursor >= len(text) or text[cursor] != "[":
        return None
    return cursor + 1, "]" + "=" * equals + "]"


def _skip_comment(text: str, index: int, line: int, field: str) -> tuple[int, int]:
    """Consume a line comment or bracket comment starting at ``index``."""
    bracket = _bracket_open(text, index + 1)
    if bracket is not None:
        start, closing = bracket
        end = text.find(closing, start)
        if end < 0:
            raise PolicyError(f"{field} has an unterminated bracket comment at line {line}")
        return end + len(closing), line + text.count("\n", index, end)
    end = text.find("\n", index)
    return (len(text), line) if end < 0 else (end, line)


def _parse_quoted(text: str, index: int, line: int, field: str) -> tuple[str, int, int]:
    chunks: list[str] = []
    length = len(text)
    while index < length:
        char = text[index]
        if char == "\\":
            if index + 1 >= length:
                break
            following = text[index + 1]
            if following == "\n":
                line += 1
            else:
                chunks.append(following)
            index += 2
            continue
        if char == '"':
            return "".join(chunks), index + 1, line
        if char == "\n":
            line += 1
        chunks.append(char)
        index += 1
    raise PolicyError(f"{field} has an unterminated quoted argument at line {line}")


def _parse_unquoted(text: str, index: int) -> tuple[str, int]:
    start = index
    length = len(text)
    while index < length:
        char = text[index]
        if char.isspace() or char in "()#" or char == '"':
            break
        if char == "\\" and index + 1 < length:
            index += 2
            continue
        index += 1
    return text[start:index].replace("\\", ""), index


def _parse_arguments(text: str, index: int, line: int, field: str, name: str) -> tuple[list[str], int, int]:
    arguments: list[str] = []
    length = len(text)
    depth = 0
    while index < length:
        char = text[index]
        if char == "\n":
            line += 1
            index += 1
            continue
        if char.isspace():
            index += 1
            continue
        if char == ")":
            if depth == 0:
                return arguments, index + 1, line
            depth -= 1
            value, index = ")", index + 1
        elif char == "(":
            depth += 1
            if depth > MAX_REACHABILITY_DEPTH:
                raise PolicyError(f"{field} command {name!r} exceeds nesting depth {MAX_REACHABILITY_DEPTH}")
            value, index = "(", index + 1
        elif char == "#":
            index, line = _skip_comment(text, index, line, field)
            continue
        elif char == '"':
            value, index, line = _parse_quoted(text, index + 1, line, field)
        else:
            bracket = _bracket_open(text, index)
            if bracket is not None:
                start, closing = bracket
                end = text.find(closing, start)
                if end < 0:
                    raise PolicyError(f"{field} has an unterminated bracket argument at line {line}")
                value = text[start:end]
                line += text.count("\n", index, end)
                index = end + len(closing)
            else:
                value, index = _parse_unquoted(text, index)
        if len(arguments) >= MAX_CMAKE_ARGUMENTS:
            raise PolicyError(f"{field} command {name!r} exceeds {MAX_CMAKE_ARGUMENTS} arguments")
        arguments.append(value)
    raise PolicyError(f"{field} command {name!r} at line {line} has an unterminated argument list")


def parse_cmake(text: str, field: str) -> tuple[CMakeCommand, ...]:
    """Tokenise a CMake listfile, discarding comments and refusing malformed input."""
    commands: list[CMakeCommand] = []
    index = 0
    line = 1
    length = len(text)
    while index < length:
        char = text[index]
        if char == "\n":
            line += 1
            index += 1
            continue
        if char.isspace():
            index += 1
            continue
        if char == "#":
            index, line = _skip_comment(text, index, line, field)
            continue
        match = _COMMAND_NAME.match(text, index)
        if match is None:
            raise PolicyError(f"{field} has an unparsable token at line {line}: {text[index]!r}")
        name = match.group(0).casefold()
        cursor = match.end()
        while cursor < length and text[cursor] in " \t\r\n":
            if text[cursor] == "\n":
                line += 1
            cursor += 1
        if cursor >= length or text[cursor] != "(":
            raise PolicyError(f"{field} command {name!r} at line {line} is missing its argument list")
        start_line = line
        arguments, cursor, line = _parse_arguments(text, cursor + 1, line, field, name)
        if len(commands) >= MAX_CMAKE_COMMANDS:
            raise PolicyError(f"{field} exceeds {MAX_CMAKE_COMMANDS} commands")
        commands.append(CMakeCommand(name, tuple(arguments), start_line))
        index = cursor
    return tuple(commands)


def _literal(value: str, field: str) -> str:
    """Reject arguments this checker cannot resolve to one concrete value."""
    if "${" in value or "$ENV{" in value or "@" in value:
        raise PolicyError(f"{field} uses an unresolvable CMake variable: {value!r}")
    return value


def commands_named(commands: Iterable[CMakeCommand], name: str) -> list[CMakeCommand]:
    return [command for command in commands if command.name == name]


def _read_cmake(root: Path, relative: str, field: str) -> tuple[CMakeCommand, ...]:
    payload = read_confined_file(root, relative, field, max_bytes=MAX_CMAKE_BYTES)
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PolicyError(f"{field} must be strict UTF-8") from exc
    return parse_cmake(text, field)


def _join(directory: str, relative: str, field: str) -> str:
    raw = relative.replace("\\", "/")
    candidate = PurePosixPath(f"{directory}/{raw}") if directory else PurePosixPath(raw)
    if candidate.is_absolute():
        raise PolicyError(f"{field} uses an absolute path: {relative!r}")
    parts: list[str] = []
    for part in candidate.parts:
        if part == "..":
            raise PolicyError(f"{field} escapes the repository: {relative!r}")
        if part in ("", "."):
            continue
        parts.append(part)
    if not parts:
        raise PolicyError(f"{field} resolves to an empty path: {relative!r}")
    return "/".join(parts)


def _listfile_directory(cmake_file: str) -> str:
    parent = str(PurePosixPath(cmake_file).parent)
    return "" if parent == "." else parent


SOURCE_DIR_VARIABLES = ("${CMAKE_SOURCE_DIR}/", "${CMAKE_CURRENT_LIST_DIR}/", "${CMAKE_CURRENT_SOURCE_DIR}/")


def _resolve_path(directory: str, value: str, field: str) -> str:
    """Resolve a CMake path argument to a repository-relative path.

    Only the two directory variables whose value this checker actually knows are
    honoured; anything else is unresolvable and therefore unprovable.
    """
    if value.startswith("${CMAKE_SOURCE_DIR}/"):
        return _join("", value[len("${CMAKE_SOURCE_DIR}/"):], field)
    for variable in ("${CMAKE_CURRENT_LIST_DIR}/", "${CMAKE_CURRENT_SOURCE_DIR}/"):
        if value.startswith(variable):
            return _join(directory, value[len(variable):], field)
    return _join(directory, _literal(value, field), field)


def assert_cmake_reachable(root: Path, cmake_file: str, field: str) -> tuple[str, ...]:
    """Prove the listfile is included by the root project instead of orphaned."""
    target = cmake_file.replace("\\", "/")
    visited: set[str] = set()
    queue: list[tuple[str, int, tuple[str, ...]]] = [(ROOT_CMAKE, 0, (ROOT_CMAKE,))]
    while queue:
        current, depth, chain = queue.pop()
        if current in visited:
            continue
        visited.add(current)
        if len(visited) > MAX_REACHABILITY_LISTFILES:
            raise PolicyError(f"{field} reachability walk exceeded {MAX_REACHABILITY_LISTFILES} listfiles")
        if depth > MAX_REACHABILITY_DEPTH:
            raise PolicyError(f"{field} reachability walk exceeded depth {MAX_REACHABILITY_DEPTH}")
        if current == target:
            return chain
        try:
            commands = _read_cmake(root, current, f"{field} listfile {current}")
        except PolicyError:
            # An unreadable or unparsable sibling listfile cannot vouch for the
            # target; keep walking the branches that are readable.
            continue
        directory = _listfile_directory(current)
        for command in commands_named(commands, "add_subdirectory"):
            if not command.arguments:
                continue
            child = command.arguments[0]
            if "${" in child or "@" in child:
                continue
            try:
                child_listfile = _join(directory, f"{child.rstrip('/')}/{ROOT_CMAKE}", field)
            except PolicyError:
                continue
            queue.append((child_listfile, depth + 1, chain + (child_listfile,)))
    raise PolicyError(f"{field} is not reachable from {ROOT_CMAKE} via add_subdirectory: {cmake_file}")


def _single(
    commands: Sequence[CMakeCommand],
    name: str,
    predicate: Callable[[CMakeCommand], bool],
    field: str,
    what: str,
) -> CMakeCommand:
    matches = [command for command in commands if command.name == name and predicate(command)]
    if not matches:
        raise PolicyError(f"{field} has no {what}")
    if len(matches) > 1:
        raise PolicyError(f"{field} has more than one {what}")
    return matches[0]


def _test_properties(arguments: Sequence[str], field: str) -> dict[str, str]:
    upper = [argument.upper() for argument in arguments]
    if "PROPERTIES" not in upper:
        raise PolicyError(f"{field} set_tests_properties has no PROPERTIES section")
    tail = arguments[upper.index("PROPERTIES") + 1:]
    if len(tail) % 2:
        raise PolicyError(f"{field} set_tests_properties has an unpaired property")
    return {tail[index].upper(): tail[index + 1] for index in range(0, len(tail), 2)}


def verify_cmake_registration(
    root: Path,
    *,
    cmake_file: str,
    cmake_target: str,
    test_selector: str,
    harness: str,
    corpus_dir: str,
    max_input_bytes: int,
    timeout_seconds: int,
    max_memory_mb: int,
    smoke_seconds: int,
    field: str,
) -> None:
    """Require a declared target, a registered test, an explicit corpus, and real limits."""
    assert_cmake_reachable(root, cmake_file, field)
    commands = _read_cmake(root, cmake_file, f"{field}.cmake_file")
    directory = _listfile_directory(cmake_file)

    executable = _single(
        commands,
        "add_executable",
        lambda command: bool(command.arguments) and _literal(command.arguments[0], field) == cmake_target,
        field,
        f"add_executable declaring target {cmake_target!r}",
    )
    sources = [
        _resolve_path(directory, argument, field)
        for argument in executable.arguments[1:]
        if argument.upper() not in _SOURCE_KEYWORDS
    ]
    if harness not in sources:
        raise PolicyError(f"{field} target {cmake_target!r} does not compile the declared harness {harness}")

    sanitized = any(
        command.arguments
        and _literal(command.arguments[0], field) == cmake_target
        and any(SANITIZER_LITERAL in argument for argument in command.arguments[1:])
        for command in commands
    )
    if not sanitized:
        raise PolicyError(f"{field} target {cmake_target!r} is not built with -{SANITIZER_LITERAL}")

    test = _single(
        commands,
        "add_test",
        lambda command: len(command.arguments) > 1
        and command.arguments[0].upper() == "NAME"
        and _literal(command.arguments[1], field) == test_selector,
        field,
        f"add_test registering {test_selector!r}",
    )
    upper = [argument.upper() for argument in test.arguments]
    if "COMMAND" not in upper:
        raise PolicyError(f"{field} add_test {test_selector!r} has no COMMAND")
    invocation = list(test.arguments[upper.index("COMMAND") + 1:])
    if not invocation:
        raise PolicyError(f"{field} add_test {test_selector!r} has an empty COMMAND")
    expression = _TARGET_FILE_EXPR.match(invocation[0])
    named = expression.group(1) if expression else _literal(invocation[0], field)
    if named != cmake_target:
        raise PolicyError(f"{field} add_test {test_selector!r} does not run target {cmake_target!r}")

    runtime = list(invocation[1:])
    flags = [_literal(argument, field) for argument in runtime if argument.startswith("-")]
    for flag, value in (
        ("-max_len", max_input_bytes),
        ("-timeout", timeout_seconds),
        ("-rss_limit_mb", max_memory_mb),
    ):
        if f"{flag}={value}" not in flags:
            raise PolicyError(f"{field} add_test {test_selector!r} does not pass {flag}={value}")

    inputs = {_resolve_path(directory, argument, field) for argument in runtime if not argument.startswith("-")}
    if corpus_dir not in inputs:
        raise PolicyError(f"{field} add_test {test_selector!r} does not pass the corpus directory {corpus_dir}")
    unreviewed = sorted(inputs - {corpus_dir})
    if unreviewed:
        raise PolicyError(f"{field} add_test {test_selector!r} passes unreviewed input paths: {unreviewed}")

    properties_command = _single(
        commands,
        "set_tests_properties",
        lambda command: bool(command.arguments) and _literal(command.arguments[0], field) == test_selector,
        field,
        f"set_tests_properties for {test_selector!r}",
    )
    properties = _test_properties(properties_command.arguments, field)
    if properties.get("TIMEOUT") != str(smoke_seconds):
        raise PolicyError(f"{field} test {test_selector!r} does not set TIMEOUT {smoke_seconds}")
    if FUZZ_LABEL not in [label for label in re.split(r"[;,\s]+", properties.get("LABELS", "")) if label]:
        raise PolicyError(f"{field} test {test_selector!r} is not labelled {FUZZ_LABEL!r}")
