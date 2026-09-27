#!/usr/bin/env python3
"""INST-130: SparkInstaller's documented capabilities must match the binary.

Three views of the command line must agree exactly: the ``### Flags`` table in
SparkInstaller/README.md, the flags main.cpp accepts, and the flags its
PrintHelp() text names. A flag whose handling main.cpp compiles only under a
SparkInstaller CMake option that the published build (the release.yml
``build-installer`` job) configures OFF must be marked ``source-build only`` in
the README, and a flag marked that way must really be gated and really be
compiled out of the published build. The README exit-code table must equal the
literal exit codes the installer returns.

Exit status: 0 when every claim holds, 1 on any mismatch, 2 on unreadable input.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]

README = Path("SparkInstaller/README.md")
MAIN = Path("SparkInstaller/src/main.cpp")
INSTALLER_CMAKE = Path("SparkInstaller/CMakeLists.txt")
EXIT_CODE_SOURCES = (
    Path("SparkInstaller/src/main.cpp"),
    Path("SparkInstaller/src/Installer.cpp"),
    Path("SparkInstaller/src/InstallerPreflight.cpp"),
)
RELEASE_WORKFLOW = Path(".github/workflows/release.yml")
PUBLISHED_JOB = "build-installer"
SOURCE_BUILD_MARKER = "source-build only"

_FLAG = re.compile(r"--[a-z][a-z-]*")
_ACCEPTED_FLAG = re.compile(r'\barg\s*==\s*"(--[a-z][a-z-]*)"')
# `else if (arg == "--gui")\n    wantGui = true;` binds a flag to the variable it sets.
_FLAG_SETS_VARIABLE = re.compile(r'\barg\s*==\s*"(--[a-z][a-z-]*)"\s*\)\s*\{?\s*([A-Za-z_]\w*)\s*=\s*true\s*;')
# `if (wantGui)\n{\n#ifdef OPTION` gates everything the flag does on a compile option.
_VARIABLE_GATE = re.compile(r"\bif\s*\(\s*([A-Za-z_]\w*)\s*\)\s*\{\s*#\s*if(?:def\s+(\w+)|\s+defined\s*\(\s*(\w+)\s*\))")
_CMAKE_OPTION = re.compile(r"^\s*option\s*\(\s*(\w+)\b", re.MULTILINE)
_RETURN_CODE = re.compile(r"\b(?:return|std::exit\s*\()\s*(\d+)\s*\)?\s*;")
_JOB_KEY = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_.-]*):\s*(?:#.*)?$")
_TOP_LEVEL_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*:")


class ClaimsError(RuntimeError):
    """An input file is missing or has no recognizable structure."""


def read_text(root: Path, relative: Path) -> str:
    path = root / relative
    try:
        return path.read_text(encoding="utf-8").replace("\r\n", "\n")
    except OSError as error:
        raise ClaimsError(f"{relative.as_posix()}: {error.strerror or error}") from error


def markdown_table(text: str, heading: str, label: str) -> list[list[str]]:
    """Body rows of the first pipe table under ``heading`` (header and rule dropped)."""
    lines = text.splitlines()
    try:
        start = lines.index(heading)
    except ValueError as error:
        raise ClaimsError(f"{label}: README has no '{heading}' section") from error
    rows: list[list[str]] = []
    for line in lines[start + 1:]:
        if line.startswith("#"):
            break
        if line.startswith("|"):
            rows.append([cell.strip() for cell in line.strip().strip("|").split("|")])
        elif rows:
            break
    if len(rows) < 3:
        raise ClaimsError(f"{label}: '{heading}' has no table rows")
    return rows[2:]


def readme_flags(readme: str) -> dict[str, bool]:
    """README flag -> whether its row is marked source-build only."""
    flags: dict[str, bool] = {}
    for row in markdown_table(readme, "### Flags", "flags"):
        names = _FLAG.findall(" ".join(re.findall(r"`([^`]*)`", row[0])))
        if not names:
            raise ClaimsError(f"flags: README row '{row[0]}' names no flag")
        marked = SOURCE_BUILD_MARKER in " ".join(row[1:]).lower()
        for name in names:
            flags[name] = marked
    return flags


def help_text_flags(main: str) -> set[str]:
    """Flags named anywhere in PrintHelp()'s string literals."""
    match = re.search(r"\bvoid\s+PrintHelp\s*\(\s*\)\s*\{(.*?)\n    \}", main, re.DOTALL)
    if not match:
        raise ClaimsError("help: main.cpp defines no PrintHelp()")
    literals = re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1))
    return set(_FLAG.findall(" ".join(literals)))


def compile_gates(main: str, options: set[str]) -> dict[str, str]:
    """Flag -> the SparkInstaller CMake option its handling is compiled under."""
    variable_of = {flag: variable for flag, variable in _FLAG_SETS_VARIABLE.findall(main)}
    gate_of: dict[str, str] = {}
    for variable, ifdef_option, defined_option in _VARIABLE_GATE.findall(main):
        option = ifdef_option or defined_option
        if option in options:
            gate_of[variable] = option
    return {flag: gate_of[variable] for flag, variable in variable_of.items() if variable in gate_of}


def published_option_values(workflow: str) -> dict[str, str]:
    """``-D<NAME>=<VALUE>`` settings in the published installer job."""
    lines = workflow.splitlines()
    header = f"  {PUBLISHED_JOB}:"
    start = next((index for index, line in enumerate(lines) if line.split("#", 1)[0].rstrip() == header), None)
    if start is None:
        raise ClaimsError(f"published build: release.yml defines no {PUBLISHED_JOB} job")
    body: list[str] = []
    for line in lines[start + 1:]:
        if _JOB_KEY.match(line) or _TOP_LEVEL_KEY.match(line):
            break
        body.append(line)
    return dict(re.findall(r"-D([A-Z][A-Z0-9_]*)(?::BOOL)?=([A-Za-z0-9]+)", "\n".join(body)))


def readme_exit_codes(readme: str) -> set[int]:
    codes: set[int] = set()
    for row in markdown_table(readme, "### Exit codes", "exit codes"):
        if not row[0].isdigit():
            raise ClaimsError(f"exit codes: README row '{row[0]}' is not a numeric code")
        codes.add(int(row[0]))
    return codes


def source_exit_codes(sources: list[str]) -> set[int]:
    return {int(code) for text in sources for code in _RETURN_CODE.findall(text)}


def check(root: Path) -> list[str]:
    readme = read_text(root, README)
    main = read_text(root, MAIN)
    options = set(_CMAKE_OPTION.findall(read_text(root, INSTALLER_CMAKE)))
    published = published_option_values(read_text(root, RELEASE_WORKFLOW))

    errors: list[str] = []
    documented = readme_flags(readme)
    accepted = set(_ACCEPTED_FLAG.findall(main))
    helped = help_text_flags(main)
    if not accepted:
        raise ClaimsError("flags: main.cpp accepts no recognizable flag")
    for flag in sorted(set(documented) - accepted):
        errors.append(f"flags: README documents {flag}, which main.cpp does not accept")
    for flag in sorted(accepted - set(documented)):
        errors.append(f"flags: main.cpp accepts {flag}, which the README does not document")
    for flag in sorted(accepted - helped):
        errors.append(f"flags: main.cpp accepts {flag}, which --help does not mention")
    for flag in sorted(helped - accepted):
        errors.append(f"flags: --help mentions {flag}, which main.cpp does not accept")

    gates = compile_gates(main, options)
    for flag in sorted(accepted):
        option = gates.get(flag)
        compiled_out = option is not None and published.get(option, "ON").upper() in {"OFF", "0", "FALSE", "NO"}
        marked = documented.get(flag, False)
        if compiled_out and not marked:
            errors.append(
                f"flags: {flag} is compiled only with {option}, which the published {PUBLISHED_JOB} build "
                f"sets OFF, but the README does not mark it '{SOURCE_BUILD_MARKER}'"
            )
        if marked and option is None:
            errors.append(f"flags: README marks {flag} '{SOURCE_BUILD_MARKER}', but main.cpp does not gate it")
        elif marked and not compiled_out:
            errors.append(
                f"flags: README marks {flag} '{SOURCE_BUILD_MARKER}', but the published {PUBLISHED_JOB} "
                f"build does not set {option} OFF"
            )
        if marked and option is not None and option not in readme:
            errors.append(f"flags: README marks {flag} '{SOURCE_BUILD_MARKER}' without naming {option}")

    documented_codes = readme_exit_codes(readme)
    returned_codes = source_exit_codes([read_text(root, path) for path in EXIT_CODE_SOURCES])
    for code in sorted(returned_codes - documented_codes):
        errors.append(f"exit codes: the installer returns {code}, which the README does not document")
    for code in sorted(documented_codes - returned_codes):
        errors.append(f"exit codes: the README documents {code}, which the installer never returns")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root to check")
    arguments = parser.parse_args(argv)
    try:
        errors = check(arguments.root)
    except ClaimsError as error:
        print(f"check_installer_claims: {error}", file=sys.stderr)
        return 2
    for error in errors:
        print(f"check_installer_claims: {error}", file=sys.stderr)
    if errors:
        return 1
    print("check_installer_claims: OK (README flags, --help, main.cpp, published build and exit codes agree)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
