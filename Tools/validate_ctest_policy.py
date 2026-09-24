#!/usr/bin/env python3
"""CI-110 CTest registration policy: every test must be able to fail by timing out.

CTest applies no timeout at all to a test without a TIMEOUT property unless the
project includes CTest.cmake (SparkEngine only calls enable_testing()). A test
that hangs therefore stalls the whole ctest run until the hosted job's
timeout-minutes kills it, and the job reports a cancellation instead of the
test that hung. ``TIMEOUT 0`` is worse: CTest reads it as "no timeout".

Two views of the same rule:

* static (default): parse every first-party CMakeLists.txt and ``*.cmake``
  file that calls ``add_test`` (git-tracked, outside ThirdParty/) and require
  every ``add_test`` name to be covered by a ``set_tests_properties`` call in the
  same file that sets a positive TIMEOUT and non-empty LABELS. CTest test
  properties are directory-scoped, so a property set in another file cannot
  cover the registration. Runs with no configure or build.
* ``--ctest-json FILE``: validate ``ctest --show-only=json-v1`` output from a
  configured tree. This sees tests registered from subdirectories, loops and
  functions after CMake evaluated them, and rejects an empty inventory so a
  tree that registered nothing cannot pass.

Exit status: 0 when the policy holds, 1 on any violation, 2 on unreadable input.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]

# Vendored trees follow their upstream's test conventions and are never
# registered by SparkEngine's configure (their tests stay off).
EXCLUDED_TOP_LEVEL_DIRS = frozenset({"ThirdParty"})

# Longest single-test budget the policy accepts. SparkInstalledTemplates builds
# every template against an installed SDK and is the current ceiling.
MAX_TIMEOUT_SECONDS = 3600

_COMMAND_RE = re.compile(r"(?i)(?<![A-Za-z0-9_])(add_test|set_tests_properties)\s*\(")
_VARIABLE_REF_RE = re.compile(r"^\$\{[A-Za-z0-9_]+\}$")
_INTEGER_RE = re.compile(r"^-?[0-9]+$")


@dataclass
class TestPolicy:
    timeouts: list[str] = field(default_factory=list)
    labels: list[str] = field(default_factory=list)


def strip_comments(text: str) -> str:
    """Remove CMake line comments while keeping ``#`` inside quoted arguments."""
    out: list[str] = []
    for line in text.splitlines():
        in_quote = False
        escaped = False
        cut = len(line)
        for index, char in enumerate(line):
            if escaped:
                escaped = False
                continue
            if char == "\\":
                escaped = True
            elif char == '"':
                in_quote = not in_quote
            elif char == "#" and not in_quote:
                cut = index
                break
        out.append(line[:cut])
    return "\n".join(out)


def _balanced_body(text: str, open_index: int) -> tuple[str, int]:
    depth = 0
    in_quote = False
    escaped = False
    for index in range(open_index, len(text)):
        char = text[index]
        if escaped:
            escaped = False
            continue
        if char == "\\":
            escaped = True
        elif char == '"':
            in_quote = not in_quote
        elif not in_quote and char == "(":
            depth += 1
        elif not in_quote and char == ")":
            depth -= 1
            if depth == 0:
                return text[open_index + 1 : index], index + 1
    raise ValueError("unbalanced parentheses in CMake command")


def _tokens(body: str) -> list[str]:
    tokens: list[str] = []
    for match in re.finditer(r'"((?:[^"\\]|\\.)*)"|[^\s"]+', body):
        tokens.append(match.group(1) if match.group(1) is not None else match.group(0))
    return tokens


def parse_cmake(text: str) -> tuple[list[str], dict[str, TestPolicy]]:
    """Return (registered test names in order, policy properties per name)."""
    source = strip_comments(text)
    names: list[str] = []
    policies: dict[str, TestPolicy] = {}
    position = 0
    while True:
        match = _COMMAND_RE.search(source, position)
        if not match:
            break
        body, position = _balanced_body(source, match.end() - 1)
        tokens = _tokens(body)
        command = match.group(1).lower()
        if command == "add_test":
            if len(tokens) >= 2 and tokens[0] == "NAME":
                names.append(tokens[1])
            elif tokens:
                names.append(tokens[0])
            continue
        if "PROPERTIES" not in tokens:
            continue
        split = tokens.index("PROPERTIES")
        targets, props = tokens[:split], tokens[split + 1 :]
        for key_index in range(0, len(props) - 1, 2):
            key, value = props[key_index], props[key_index + 1]
            for target in targets:
                policy = policies.setdefault(target, TestPolicy())
                if key == "TIMEOUT":
                    policy.timeouts.append(value)
                elif key == "LABELS":
                    policy.labels.append(value)
    return names, policies


def _is_cmake_source(relative: str) -> bool:
    name = relative.rsplit("/", 1)[-1]
    return name == "CMakeLists.txt" or name.endswith(".cmake")


def _git_tracked_cmake_sources(root: Path) -> list[str] | None:
    """Tracked CMake files when ``root`` is itself a git work-tree top level.

    Returns None (use the filesystem walk) when git is unavailable or ``root``
    is merely nested inside some other repository, whose ``ls-files`` would
    report nothing for the untracked tree and silently hide every registration.
    """
    try:
        toplevel = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--show-toplevel"],
            capture_output=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    if Path(toplevel.stdout.decode("utf-8").strip()).resolve() != root.resolve():
        return None
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", "CMakeLists.txt", "*.cmake", "*/CMakeLists.txt"],
            capture_output=True,
            check=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return [entry for entry in result.stdout.decode("utf-8").split("\0") if entry]


def _walked_cmake_sources(root: Path) -> list[str]:
    """Filesystem fallback for a source tree without git metadata (an archive)."""
    found: list[str] = []
    for directory, subdirectories, files in os.walk(root):
        # Build trees (anything holding a CMakeCache.txt) carry generated and
        # fetched CMake files that are not first-party registrations.
        if "CMakeCache.txt" in files:
            subdirectories[:] = []
            continue
        at_root = Path(directory) == root
        subdirectories[:] = sorted(
            entry
            for entry in subdirectories
            if not entry.startswith(".") and not (at_root and entry in EXCLUDED_TOP_LEVEL_DIRS)
        )
        for name in files:
            relative = Path(directory, name).relative_to(root).as_posix()
            if _is_cmake_source(relative):
                found.append(relative)
    return found


def discover_cmake_sources(root: Path = REPO_ROOT) -> list[Path]:
    """First-party CMake files that call add_test, in stable path order.

    A file only qualifies once comment stripping still finds an ``add_test``
    call, so a commented-out registration neither adds a file nor hides one.
    """
    candidates = _git_tracked_cmake_sources(root)
    if candidates is None:
        candidates = _walked_cmake_sources(root)
    sources: list[Path] = []
    for relative in sorted(set(candidates)):
        if relative.split("/", 1)[0] in EXCLUDED_TOP_LEVEL_DIRS or not _is_cmake_source(relative):
            continue
        path = root / relative
        if not path.is_file():
            continue
        if parse_cmake(path.read_text(encoding="utf-8"))[0]:
            sources.append(path)
    return sources


def _timeout_error(name: str, value: str) -> str | None:
    if _VARIABLE_REF_RE.match(value):
        return None
    if not _INTEGER_RE.match(value):
        return f"{name}: TIMEOUT '{value}' is not a positive integer or a single ${{VAR}} reference"
    seconds = int(value)
    if seconds <= 0:
        return f"{name}: TIMEOUT {seconds} disables the CTest timeout; use a positive budget"
    if seconds > MAX_TIMEOUT_SECONDS:
        return f"{name}: TIMEOUT {seconds} exceeds the {MAX_TIMEOUT_SECONDS}s policy ceiling"
    return None


def check_cmake_text(text: str, origin: str) -> list[str]:
    names, policies = parse_cmake(text)
    errors: list[str] = []
    if not names:
        errors.append(f"{origin}: registers no tests; an empty inventory cannot satisfy the policy")
    for name in names:
        policy = policies.get(name)
        if policy is None or not policy.timeouts:
            errors.append(
                f"{origin}: {name}: no TIMEOUT; a hang would stall ctest instead of failing this test"
            )
        else:
            for value in policy.timeouts:
                problem = _timeout_error(name, value)
                if problem:
                    errors.append(f"{origin}: {problem}")
        if policy is None or not any(label.strip(" ;") for label in policy.labels):
            errors.append(f"{origin}: {name}: no LABELS; label-selected lanes (-L) can never run it")
    return errors


def check_ctest_json(document: object, origin: str) -> list[str]:
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
        return [f"{origin}: not ctest --show-only=json-v1 output (missing 'tests' list)"]
    tests = document["tests"]
    if not tests:
        return [f"{origin}: ctest inventory is empty; a tree that registers 0 tests cannot pass"]
    errors: list[str] = []
    for entry in tests:
        name = entry.get("name", "<unnamed>") if isinstance(entry, dict) else "<malformed>"
        properties = entry.get("properties", []) if isinstance(entry, dict) else []
        by_name = {
            prop.get("name"): prop.get("value")
            for prop in properties
            if isinstance(prop, dict)
        }
        timeout = by_name.get("TIMEOUT")
        if timeout is None:
            errors.append(f"{origin}: {name}: no TIMEOUT property")
        elif not isinstance(timeout, (int, float)) or isinstance(timeout, bool) or timeout <= 0:
            errors.append(f"{origin}: {name}: TIMEOUT {timeout!r} does not bound the test")
        elif timeout > MAX_TIMEOUT_SECONDS:
            errors.append(f"{origin}: {name}: TIMEOUT {timeout} exceeds the policy ceiling")
        labels = by_name.get("LABELS")
        if not isinstance(labels, list) or not any(str(label).strip() for label in labels):
            errors.append(f"{origin}: {name}: no LABELS property")
    return errors


def _display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--cmake-lists",
        action="append",
        type=Path,
        help="CMake file to check statically (default: every first-party file that calls add_test)",
    )
    parser.add_argument(
        "--ctest-json",
        action="append",
        type=Path,
        help="output of 'ctest --show-only=json-v1' to check",
    )
    args = parser.parse_args(argv)

    errors: list[str] = []
    checked = 0
    try:
        if args.cmake_lists:
            cmake_lists = args.cmake_lists
        elif args.ctest_json:
            cmake_lists = []
        else:
            cmake_lists = discover_cmake_sources()
            if not cmake_lists:
                errors.append(f"{REPO_ROOT}: no first-party CMake file registers a test; discovery found nothing")
        for path in cmake_lists:
            text = path.read_text(encoding="utf-8")
            errors.extend(check_cmake_text(text, _display_path(path)))
            checked += len(parse_cmake(text)[0])
        for path in args.ctest_json or []:
            document = json.loads(path.read_text(encoding="utf-8"))
            errors.extend(check_ctest_json(document, str(path)))
            if isinstance(document, dict) and isinstance(document.get("tests"), list):
                checked += len(document["tests"])
    except (OSError, ValueError) as exc:
        print(f"validate_ctest_policy: error: {exc}", file=sys.stderr)
        return 2

    for error in errors:
        print(f"validate_ctest_policy: error: {error}", file=sys.stderr)
    if errors:
        print(f"validate_ctest_policy: FAIL ({len(errors)} violation(s) across {checked} test registration(s))")
        return 1
    print(f"validate_ctest_policy: OK ({checked} test registration(s) carry TIMEOUT and LABELS)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
