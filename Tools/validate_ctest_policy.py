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
  tree that registered nothing cannot pass. It also enforces "every shipped
  binary has a smoke or integration lane": each ``install(TARGETS ... RUNTIME)``
  executable that the tree's tests invoke must have one test that does more
  than print its help or version and carries an integration, smoke or process
  label (``KNOWN_BINARY_LANE_GAPS`` lists the documented exceptions).

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

# Shipped-binary rule (CI-110 "every shipped binary has a smoke or integration lane").
_TARGET_COMMAND_RE = re.compile(r"(?i)(?<![A-Za-z0-9_])(add_executable|install)\s*\(")
_INSTALL_KEYWORDS = frozenset(
    {"EXPORT", "RUNTIME", "LIBRARY", "ARCHIVE", "BUNDLE", "FRAMEWORK", "DESTINATION", "COMPONENT", "PERMISSIONS"}
)
SMOKE_LABELS = frozenset({"integration", "smoke", "process"})
HELP_VERSION_ARGS = frozenset({"--help", "-h", "--version", "-v"})
# cmake -P runners that only compare the target's --version output (the generated
# Verify<Target>Version.cmake scripts in the product CMakeLists).
VERSION_PROBE_DEFINES = frozenset({"SPARK_VERSION_EXECUTABLE"})
# Shipped binaries with no non-interactive mode a lane could drive. Each entry
# says where the behaviour is covered instead; the gap is printed on every run,
# and an entry whose binary gains a behavioural lane fails until it is removed.
KNOWN_BINARY_LANE_GAPS = {
    "SparkLauncher": "GUI-only project picker with no headless mode; its launch-request validation runs "
    "in-process in Tests/TestLauncherProcess.cpp, not through the shipped binary",
}


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


def shipped_executables(root: Path = REPO_ROOT) -> list[str]:
    """Executable targets that an ``install(TARGETS ... RUNTIME ...)`` rule ships, from the tracked CMake files."""
    candidates = _git_tracked_cmake_sources(root)
    if candidates is None:
        candidates = _walked_cmake_sources(root)
    executables: set[str] = set()
    installed: set[str] = set()
    for relative in candidates:
        if relative.split("/", 1)[0] in EXCLUDED_TOP_LEVEL_DIRS or not _is_cmake_source(relative):
            continue
        path = root / relative
        if not path.is_file():
            continue
        source = strip_comments(path.read_text(encoding="utf-8"))
        for match in _TARGET_COMMAND_RE.finditer(source):
            tokens = _tokens(_balanced_body(source, match.end() - 1)[0])
            if match.group(1).lower() == "add_executable":
                if tokens and "$" not in tokens[0]:
                    executables.add(tokens[0])
            elif tokens[:1] == ["TARGETS"] and "RUNTIME" in tokens:
                names = tokens[1:]
                end = next((index for index, token in enumerate(names) if token in _INSTALL_KEYWORDS), len(names))
                installed.update(name for name in names[:end] if "$" not in name)
    return sorted(executables & installed)


def _binary_name(value: str) -> str:
    name = re.split(r"[\\/]", value)[-1]
    return name[:-4] if name.lower().endswith(".exe") else name


def _is_version_probe(command: list[str], target: str) -> bool:
    """True when the only thing a test does with ``target`` is print its help or version."""
    if command and _binary_name(command[0]) == target:
        return bool(command[1:]) and set(command[1:]) <= HELP_VERSION_ARGS
    references = [arg for arg in command if "=" in arg and _binary_name(arg.split("=", 1)[1]) == target]
    return bool(references) and all(
        arg.startswith("-D") and arg[2:].split("=", 1)[0] in VERSION_PROBE_DEFINES for arg in references
    )


def check_shipped_binary_lanes(
    document: dict, shipped: list[str], origin: str, gaps: dict[str, str] = KNOWN_BINARY_LANE_GAPS
) -> tuple[list[str], list[str]]:
    """Every shipped executable this tree's tests invoke needs a behavioural smoke/integration/process lane.

    A target counts as built in the tree once any registered test names its
    file (as the command, an argument, a ``-D...=`` value, or an ENVIRONMENT
    value). Tests that only print its help or version do not count, and the
    behavioural test must carry an integration, smoke or process label.
    Returns (errors, notes); a known gap is a note, and a stale one an error.
    """
    covered: dict[str, bool] = {}
    for entry in document["tests"]:
        if not isinstance(entry, dict):
            continue
        command = [str(arg) for arg in entry.get("command") or []]
        properties = {
            prop.get("name"): prop.get("value") for prop in entry.get("properties", []) if isinstance(prop, dict)
        }
        environment = [str(value) for value in properties.get("ENVIRONMENT") or []]
        labels = {str(label) for label in properties.get("LABELS") or []}
        referenced = {_binary_name(arg.split("=", 1)[-1]) for arg in command + environment} & set(shipped)
        for target in referenced:
            behavioural = not _is_version_probe(command, target) and bool(labels & SMOKE_LABELS)
            covered[target] = covered.get(target, False) or behavioural
    errors: list[str] = []
    notes: list[str] = []
    for target in sorted(covered):
        if target in gaps and covered[target]:
            errors.append(f"{origin}: {target} now has a behavioural lane; remove it from KNOWN_BINARY_LANE_GAPS")
        elif target in gaps:
            notes.append(f"{origin}: known gap: shipped binary {target} has no behavioural lane ({gaps[target]})")
        elif not covered[target]:
            errors.append(
                f"{origin}: shipped binary {target} is only exercised through --help/--version or by tests without "
                f"an {'/'.join(sorted(SMOKE_LABELS))} label; add a behavioural lane"
            )
    return errors, notes


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
        shipped = shipped_executables() if args.ctest_json else []
        for path in args.ctest_json or []:
            document = json.loads(path.read_text(encoding="utf-8"))
            errors.extend(check_ctest_json(document, str(path)))
            if isinstance(document, dict) and isinstance(document.get("tests"), list):
                checked += len(document["tests"])
                lane_errors, lane_notes = check_shipped_binary_lanes(document, shipped, str(path))
                errors.extend(lane_errors)
                for note in lane_notes:
                    print(f"validate_ctest_policy: note: {note}")
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
