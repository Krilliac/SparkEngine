#!/usr/bin/env python3
"""Prove documented CTest commands select and execute built tests (CI-110).

``validate.py`` already proves statically that every ``-L``/``-R`` filter in a
work-item command names a registered label or test. This tool resolves the same
commands against a real configured build tree. The documented commands are every
executable ``ctest`` segment in a work item's ``commands[]`` and every ``ctest``
line in a fenced block of ``wiki/advanced/Testing.md``.

A command applies to the tree when its ``--test-dir`` (or ``--preset``) resolves,
through ``CMakePresets.json``, to the preset whose ``build/<name>`` tree has the
same name as ``--build-dir`` and the tree's ``CMakeCache.txt`` holds every
``-D`` setting the same documented command configures it with. Every other
command is reported as not applicable to this tree and is never counted as a pass. For each applicable command the
tool runs ``ctest --show-only=json-v1`` with the command's own selection flags
and requires at least one selected test without the ``DISABLED`` property and,
for every enabled selected test, a ``command[0]`` that exists on disk (the test
executable or interpreter was built or installed). A command whose filter
selects nothing statically and is declared in its work item's
``plannedTestSelectors`` is listed as declared debt, not as a pass.

With ``--execute``, each applicable selection is also run through CTest. The
checker sets ``SPARK_TEST_LIMIT`` for the child process so SparkTests-based
registrations stay bounded; CTest's own timeout and no-tests checks remain
active. Expected-count pins are preserved even when larger than the requested
limit. Discovery-only commands never count as execution, and declared empty
selections fail in execute mode. ``SPARK_TEST_*`` environment selectors of ``SparkTests`` are not
documented CTest commands and are not parsed here.

Exit status: 0 every applicable command resolves (and passes with --execute), 1 at least one does not, 2
the tree has no ``CTestTestfile.cmake`` or no documented command applies to it
(so the check cannot stop checking and still pass).

Usage:
    python3 tools/site-data/check_documented_selectors.py --build-dir build/linux-gcc-release
    python3 tools/site-data/check_documented_selectors.py --build-dir build/windows-release --config Release
    python3 tools/site-data/check_documented_selectors.py --build-dir build \
        --documented-build-dir build/linux-gcc-release
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from common import REPO_ROOT, load_contract  # noqa: E402
from contract_selectors import (_CTEST_PLACEHOLDER, PresetReference, cmake_preset_index, command_tokens,  # noqa: E402
                                ctest_filter_errors)
from validate import _CTEST_COMMAND_TOKEN, executable_ctest_segments  # noqa: E402

TESTING_PAGE = Path("wiki/advanced/Testing.md")
# Selection flags forwarded to the show-only query, and whether each takes a value.
SELECTION_FLAGS = {
    "-L": True, "--label-regex": True, "-LE": True, "--label-exclude": True,
    "-R": True, "--tests-regex": True, "-E": True, "--exclude-regex": True,
    "-U": False, "--union": False,
}
FENCE = re.compile(r"^\s*(```|~~~)")
CTEST_TIMEOUT_SECONDS = 120
CTEST_EXECUTION_TIMEOUT_SECONDS = 900
DEFAULT_TEST_LIMIT = 50
CMAKE_TOOL = re.compile(r"^(?:.*[/\\])?cmake(?:\.exe)?$", re.IGNORECASE)
CACHE_ENTRY = re.compile(r"^([A-Za-z_][A-Za-z0-9_.+-]*)(?::[A-Z]+)?=(.*)$")


@dataclass
class DocumentedCommand:
    """One ctest invocation named by a work item or the testing page."""

    source: str
    text: str
    arguments: list[str]
    planned: list[str] = field(default_factory=list)
    # -D cache settings the same documented command configures the tree with.
    requires: dict[str, str] = field(default_factory=dict)


@dataclass
class Outcome:
    command: DocumentedCommand
    status: str  # pass | fail | debt | discovery | not-applicable
    detail: str


def configure_defines(command: str) -> dict[str, str]:
    """``-DNAME[:TYPE]=VALUE`` settings passed to a CMake configure step in the same command."""
    defines: dict[str, str] = {}
    for segment in re.split(r"[;&|\r\n]+", command):
        tokens = command_tokens(segment)
        start = next((index for index, token in enumerate(tokens) if CMAKE_TOOL.match(token)), None)
        if start is None or any(token in ("--build", "--install", "-E", "-P") for token in tokens[start + 1:]):
            continue
        arguments = tokens[start + 1:]
        for index, token in enumerate(arguments):
            value = token[2:] if token.startswith("-D") and len(token) > 2 else None
            if token == "-D" and index + 1 < len(arguments):
                value = arguments[index + 1]
            if value and "=" in value:
                name, setting = value.split("=", 1)
                defines[name.split(":", 1)[0]] = setting
    return defines


def _cmake_value(value: str) -> str:
    """Normalise CMake truth constants so ON/TRUE/1 and OFF/FALSE/0 compare equal."""
    upper = value.strip().upper()
    if upper in ("ON", "TRUE", "YES", "Y", "1"):
        return "ON"
    if upper in ("OFF", "FALSE", "NO", "N", "0", "") or upper.endswith("-NOTFOUND"):
        return "OFF"
    return value.strip()


def unmet_requirements(build_dir: Path, requires: dict[str, str]) -> list[str]:
    """The ``-D`` settings a documented command needs that this tree's CMakeCache.txt does not hold."""
    cache: dict[str, str] = {}
    cache_file = build_dir / "CMakeCache.txt"
    if cache_file.is_file():
        for line in cache_file.read_text(encoding="utf-8", errors="replace").splitlines():
            match = CACHE_ENTRY.match(line)
            if match:
                cache[match.group(1)] = match.group(2)
    return [f"-D{name}={value}" for name, value in sorted(requires.items())
            if name not in cache or _cmake_value(cache[name]) != _cmake_value(value)]


def _invocations(source: str, command: str, planned: list[str]) -> list[DocumentedCommand]:
    found = []
    requires = configure_defines(command)
    for segment in executable_ctest_segments(command):
        matches = list(_CTEST_COMMAND_TOKEN.finditer(segment))
        for index, match in enumerate(matches):
            end = matches[index + 1].start() if index + 1 < len(matches) else len(segment)
            arguments = command_tokens(segment[match.end():end])
            found.append(DocumentedCommand(source, segment[match.start():end].strip(), arguments, planned,
                                           requires))
    return found


def testing_page_commands(repo_root: Path) -> list[DocumentedCommand]:
    """Every ctest line inside a fenced block of the testing page (continuations joined)."""
    commands: list[DocumentedCommand] = []
    inside = False
    pending = ""
    lines = (repo_root / TESTING_PAGE).read_text(encoding="utf-8").splitlines()
    for number, line in enumerate(lines, start=1):
        if FENCE.match(line):
            inside, pending = not inside, ""
            continue
        if not inside:
            continue
        text = pending + line.strip()
        if text.endswith("\\"):
            pending = text[:-1] + " "
            continue
        pending = ""
        if text.startswith("#"):
            continue
        commands.extend(_invocations(f"{TESTING_PAGE.as_posix()}:{number}", text, []))
    return commands


def work_item_commands() -> list[DocumentedCommand]:
    commands: list[DocumentedCommand] = []
    for item in load_contract()["workItems"]:
        planned = [value for value in item.get("plannedTestSelectors") or [] if isinstance(value, str)]
        for index, command in enumerate(item.get("commands") or []):
            if isinstance(command, str):
                commands.extend(_invocations(f"{item['id']}.commands[{index}]", command, planned))
    return commands


def load_override(path: Path) -> list[DocumentedCommand]:
    """Commands from a JSON list of {"source", "command", "planned"?} objects (fixture input)."""
    commands: list[DocumentedCommand] = []
    for entry in json.loads(path.read_text(encoding="utf-8")):
        commands.extend(_invocations(entry["source"], entry["command"], list(entry.get("planned", []))))
    return commands


def _option_value(arguments: list[str], flag: str) -> str | None:
    for index, argument in enumerate(arguments):
        if argument == flag and index + 1 < len(arguments):
            return arguments[index + 1]
        if argument.startswith(flag + "="):
            return argument.split("=", 1)[1]
    return None


def command_tree(arguments: list[str]) -> str | None:
    """The ``build/<name>`` tree a command runs against, resolved through CMakePresets.json."""
    index = cmake_preset_index()
    preset = _option_value(arguments, "--preset")
    if preset:
        configure = index.configure_for(PresetReference("ctest", "test", preset))
        return index.binary_dir_of(configure) if configure else None
    test_dir = _option_value(arguments, "--test-dir")
    if not test_dir:
        return None
    normalized = test_dir.replace("\\", "/").removeprefix("./").rstrip("/")
    return normalized if normalized in index.binary_dirs else None


def selection_arguments(arguments: list[str]) -> list[str] | None:
    """The command's own selection flags, or None when a value is a shell or document placeholder."""
    selected: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        index += 1
        flag, has_inline, value = argument.partition("=")
        if not (has_inline and flag.startswith("--")):
            flag, value = argument, None
        takes_value = SELECTION_FLAGS.get(flag)
        if takes_value is None:
            continue
        if not takes_value:
            selected.append(flag)
            continue
        if value is None:
            value = arguments[index] if index < len(arguments) else ""
            index += 1
        if not value or _CTEST_PLACEHOLDER.search(value):
            return None
        selected += [flag, value]
    return selected


def show_only(ctest: str, build_dir: Path, config: str | None, selection: list[str]) -> list[dict]:
    argv = [ctest, "--test-dir", str(build_dir), "--show-only=json-v1"]
    if config:
        argv += ["-C", config]
    completed = subprocess.run(argv + selection, capture_output=True, text=True, timeout=CTEST_TIMEOUT_SECONDS,
                               check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"ctest exited {completed.returncode}: {completed.stderr.strip()[:300]}")
    document = json.loads(completed.stdout)
    tests = document.get("tests") if isinstance(document, dict) else None
    if not isinstance(tests, list):
        raise RuntimeError("ctest --show-only=json-v1 returned no tests array")
    return tests


def execute_selected(ctest: str, build_dir: Path, config: str | None, selection: list[str],
                     test_limit: int) -> None:
    """Run one documented selection and fail on CTest or timeout failure."""
    argv = [ctest, "--test-dir", str(build_dir)]
    if config:
        argv += ["-C", config]
    argv += selection + ["--output-on-failure", "--no-tests=error", "--timeout", str(CTEST_TIMEOUT_SECONDS)]
    environment = os.environ.copy()
    for variable in ("SPARK_TEST_FILE", "SPARK_TEST_NAME", "SPARK_TEST_NAME_PREFIX", "SPARK_TEST_EXCLUDE",
                     "SPARK_TEST_EXPECT_COUNT", "SPARK_TEST_LIMIT"):
        environment.pop(variable, None)
    environment["SPARK_TEST_LIMIT"] = str(test_limit)
    completed = subprocess.run(argv, capture_output=True, text=True, timeout=CTEST_EXECUTION_TIMEOUT_SECONDS,
                               check=False, env=environment)
    if completed.returncode != 0:
        detail = (completed.stdout.strip() + "\n" + completed.stderr.strip()).strip()
        raise RuntimeError(f"ctest exited {completed.returncode}: {detail[-600:]}")


def _disabled(test: dict) -> bool:
    return any(prop.get("name") == "DISABLED" and prop.get("value") in (True, "ON", "TRUE", "1")
               for prop in test.get("properties") or [] if isinstance(prop, dict))


def _expected_count_pin(test: dict) -> int | None:
    """Return one SparkTests expected-count pin, rejecting malformed values."""
    pins: list[int] = []
    for prop in test.get("properties") or []:
        if not isinstance(prop, dict) or prop.get("name") != "ENVIRONMENT":
            continue
        values = prop.get("value")
        if isinstance(values, str):
            values = [values]
        elif not isinstance(values, list):
            values = []
        for environment in values:
            if not isinstance(environment, str):
                continue
            for entry in environment.split(";"):
                if not entry.startswith("SPARK_TEST_EXPECT_COUNT="):
                    continue
                raw = entry.split("=", 1)[1]
                if not raw.isdigit() or int(raw) < 1:
                    raise ValueError(f"malformed SPARK_TEST_EXPECT_COUNT={raw!r}")
                pins.append(int(raw))
    return max(pins) if pins else None


def effective_test_limit(tests: list[dict], requested: int) -> int:
    """Keep every selected SparkTests family at or above its expected count."""
    pins = [_expected_count_pin(test) for test in tests]
    return max([requested, *(pin for pin in pins if pin is not None)])


def _discovery_only(arguments: list[str]) -> bool:
    """CTest introspection flags must never be reinterpreted as a test run."""
    return any(argument == "-N" or argument == "--show-only" or argument.startswith("--show-only=")
               for argument in arguments)


def _executable_exists(command: object) -> bool:
    if not isinstance(command, list) or not command or not isinstance(command[0], str) or not command[0]:
        return False
    executable = command[0]
    if os.path.isabs(executable):
        return os.path.isfile(executable)
    return shutil.which(executable) is not None


def evaluate(command: DocumentedCommand, ctest: str, build_dir: Path, config: str | None,
             execute: bool = False, test_limit: int = DEFAULT_TEST_LIMIT,
             documented_tree: str | None = None) -> Outcome:
    tree = command_tree(command.arguments)
    if tree is None:
        return Outcome(command, "not-applicable", "no --test-dir/--preset naming a preset build tree")
    expected_tree = documented_tree or f"build/{build_dir.name}"
    if tree != expected_tree:
        return Outcome(command, "not-applicable", f"targets {tree}")
    unmet = unmet_requirements(build_dir, command.requires)
    if unmet:
        return Outcome(command, "not-applicable", f"tree is not configured with {' '.join(unmet)}")
    selection = selection_arguments(command.arguments)
    if selection is None:
        return Outcome(command, "not-applicable", "selection value is a placeholder")
    if command.planned and ctest_filter_errors(selection) and not ctest_filter_errors(selection, command.planned):
        status = "fail" if execute else "debt"
        return Outcome(command, status, "selects nothing registered; declared in plannedTestSelectors")
    try:
        tests = show_only(ctest, build_dir, config, selection)
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        return Outcome(command, "fail", f"cannot query the tree: {error}")
    enabled = [test for test in tests if isinstance(test, dict) and not _disabled(test)]
    if not enabled:
        return Outcome(command, "fail", f"selects {len(tests)} test(s), none enabled")
    missing = sorted(str(test.get("name")) for test in enabled if not _executable_exists(test.get("command")))
    if missing:
        return Outcome(command, "fail", f"selected test(s) with no built executable: {', '.join(missing[:5])}")
    if execute:
        if _discovery_only(command.arguments):
            return Outcome(command, "discovery", f"selects {len(enabled)} enabled test(s); no tests executed")
        try:
            limit = effective_test_limit(enabled, test_limit)
            execute_selected(ctest, build_dir, config, selection, limit)
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            return Outcome(command, "fail", f"selection execution failed: {error}")
    return Outcome(command, "pass", f"selects {len(enabled)} enabled test(s)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--build-dir", type=Path, required=True, help="Configured CMake build tree")
    parser.add_argument("--documented-build-dir",
                        help="Documented preset tree mapped to --build-dir (for CI trees configured in build/)")
    parser.add_argument("--config", help="Configuration passed to ctest -C (multi-config trees)")
    parser.add_argument("--ctest", default=shutil.which("ctest"), help="ctest executable (default: from PATH)")
    parser.add_argument("--commands-json", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--execute", action="store_true", help="run every applicable documented selection")
    parser.add_argument("--test-limit", type=int, default=DEFAULT_TEST_LIMIT,
                        help=f"SPARK_TEST_LIMIT for executed SparkTests children (default: {DEFAULT_TEST_LIMIT})")
    args = parser.parse_args(argv)

    if args.test_limit < 1:
        parser.error("--test-limit must be positive")

    build_dir = args.build_dir.resolve()
    if not (build_dir / "CTestTestfile.cmake").is_file():
        print(f"ERROR: {build_dir} has no CTestTestfile.cmake; nothing was checked", file=sys.stderr)
        return 2
    if not args.ctest:
        print("ERROR: no ctest executable; pass --ctest", file=sys.stderr)
        return 2
    if args.commands_json:
        commands = load_override(args.commands_json)
    else:
        commands = work_item_commands() + testing_page_commands(REPO_ROOT)

    documented_tree = args.documented_build_dir.replace("\\", "/").removeprefix("./").rstrip("/") \
        if args.documented_build_dir else None
    outcomes = [evaluate(command, args.ctest, build_dir, args.config, args.execute, args.test_limit, documented_tree)
                for command in commands]
    counts = {status: sum(outcome.status == status for outcome in outcomes)
              for status in ("pass", "fail", "debt", "discovery", "not-applicable")}
    for outcome in outcomes:
        if outcome.status != "not-applicable":
            print(f"{outcome.status.upper()}: {outcome.command.source}: {outcome.command.text} -- {outcome.detail}")
    applicable = counts["pass"] + counts["fail"]
    print(f"tree {build_dir.name}: {applicable} applicable command(s): {counts['pass']} pass, "
          f"{counts['fail']} fail; {counts['debt']} declared debt; {counts['not-applicable']} not applicable "
          f"to this tree (not counted); {counts['discovery']} discovery-only (not counted)")
    if applicable == 0:
        print(f"ERROR: no documented command applies to {build_dir.name}; nothing was checked", file=sys.stderr)
        return 2
    return 1 if counts["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
