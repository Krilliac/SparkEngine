#!/usr/bin/env python3
"""Check that documented CMake/CTest commands agree with CMakePresets.json (CI-120).

Quick starts drift quietly: a README tells readers to run ``cmake --preset
windows-release`` and then ``cmake --build build``, but the preset writes
``build/windows-release``; a wiki page builds a Visual Studio tree without
``--config`` and gets Debug binaries out of a Release preset. Nothing fails until
a reader copies the command. This checker resolves every cmake/ctest/cpack
invocation inside the shell code blocks of the documentation surfaces below
against the same preset index the work-item contract uses
(``contract_selectors.cmake_preset_index``), so the two cannot disagree.

Rules, per documented invocation:

* ``--preset NAME`` must name a visible preset of the right family.
* A build tree handed to ``cmake --build``/``--install``, ``ctest`` or ``cpack``
  (explicit, through ``cd``, or as the directory of ``cpack --config
  <tree>/CPackConfig.cmake``) must be a configure preset's ``binaryDir`` or a
  tree the same document configured ad hoc earlier, with ``-B`` or through one
  of the repository's configure scripts (``generate.sh``/``generate.bat``/
  ``build.sh``/``build.ps1`` all configure ``build``). A code block that
  configures presets (and nothing ad hoc) may only use those presets' trees.
* An ad-hoc ``-B`` configure must not write into a preset's ``binaryDir``. An
  ad-hoc configure with a generator the presets use must state the ``-A``/``-T``
  values those presets pin (only the toolset version before the first comma is
  compared); a generator no preset uses carries no pin to compare.
* A preset tree built with a multi-config generator must name its preset's
  configuration (``--config`` / ``-C``). ``cmake --install`` and ``cpack`` fall
  back to Release there, so they may omit it only for a Release preset. A stated
  configuration must always match the preset's.

Tree paths that are placeholders (``$VAR``, ``<dir>``, ``%DIR%``, ``~``) or absolute
are not resolvable and are skipped rather than guessed.
"""

from __future__ import annotations

import argparse
import re
import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path

from common import REPO_ROOT, SiteDataError, read_bytes_stable
from contract_selectors import CMakePresetIndex, cmake_preset_index

# Hand-written Markdown that documents how to build, test, or package the engine.
DOCUMENT_FILES = (
    "README.md",
    "CLAUDE.md",
    ".github/copilot-instructions.md",
)
DOCUMENT_ROOTS = (
    "wiki",
    "docs",
    ".github/prompts",
)
# Excluded with a reason: generated pages are checked at their source, and dated
# implementation plans are historical records of what was run at the time.
EXCLUDED_PREFIXES = (
    "docs/api/",  # generated from headers
    "wiki/reference/",  # symbol, file-tree and class indexes generated from sources
    "docs/readiness/ENGINE_READINESS_HANDOFF.md",  # rendered from work items; validate.py checks their commands
    "docs/superpowers/",  # dated implementation plans (historical)
)
MAX_DOCUMENT_BYTES = 4 * 1024 * 1024

# Code-block info strings that hold shell commands; anything else (cmake, cpp,
# json, ...) is source text, not something a reader runs.
SHELL_LANGUAGES = {
    "", "bash", "sh", "shell", "zsh", "console", "shell-session", "sh-session", "terminal",
    "powershell", "pwsh", "ps1", "ps", "cmd", "bat", "batch", "bash session",
}
# List-nested fences are indented past CommonMark's three columns in practice.
_FENCE = re.compile(r"^\s*(`{3,}|~{3,})[ \t]*([^`\s]*)")
_INLINE_COMMAND = re.compile(r"(?<!`)`((?:cmake|ctest|cpack) [^`]+)`(?!`)")
# Shells whose paths use backslashes; POSIX lexing would read them as escapes.
WINDOWS_LANGUAGES = {"powershell", "pwsh", "ps1", "ps", "cmd", "bat", "batch"}
_PROMPT = re.compile(r"^\s*(?:\$|>|PS[^>]*>)\s+")
_TOOL = re.compile(r"^(?:.*[/\\])?(cmake|ctest|cpack)(?:\.exe)?$", re.IGNORECASE)
_SEPARATOR = re.compile(r"^[;&|]+$")
_PLACEHOLDER = re.compile(r"[$<>{}%~*\[\]]|\.\.\.")
# Repository scripts that configure the ad-hoc tree ``build`` (mkdir build; cd build; cmake ..).
CONFIGURE_SCRIPTS = {"generate.sh", "generate.bat", "build.sh", "build.ps1"}
_INTERPRETERS = {"bash", "sh", "zsh", "pwsh", "powershell", "call", "&", "cmd", "/c", "-file"}
# Without a configuration, a multi-config install or package step uses this one.
INSTALL_DEFAULT_CONFIGURATION = "Release"


@dataclass(frozen=True)
class Finding:
    """One documented command that disagrees with the presets."""

    path: str
    line: int
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


@dataclass
class _DocumentState:
    """Trees configured so far: document-wide ad-hoc trees, block-local preset trees."""

    adhoc_trees: set[str] = field(default_factory=set)
    block_presets: list[str] = field(default_factory=list)
    block_adhoc: bool = False
    cwd: str | None = None


def documented_markdown(root: Path = REPO_ROOT) -> list[Path]:
    """The Markdown documents whose build commands are checked, in stable order."""
    paths: set[Path] = set()
    for relative in DOCUMENT_FILES:
        candidate = root / relative
        if candidate.is_file() and not candidate.is_symlink():
            paths.add(candidate)
    for relative in DOCUMENT_ROOTS:
        base = root / relative
        if not base.is_dir():
            continue
        for candidate in base.rglob("*.md"):
            if candidate.is_file() and not candidate.is_symlink():
                paths.add(candidate)
    selected = []
    for path in paths:
        posix = path.relative_to(root).as_posix()
        if any(posix == prefix or posix.startswith(prefix) for prefix in EXCLUDED_PREFIXES):
            continue
        selected.append(path)
    return sorted(selected, key=lambda path: path.relative_to(root).as_posix())


@dataclass(frozen=True)
class CommandBlock:
    """One unit of documented commands: a fenced shell block or an inline code span.

    ``commands`` holds (1-based line, logical command) pairs; continuation lines
    (``\\``, PowerShell backtick, cmd ``^``) are already joined.
    """

    language: str
    commands: tuple[tuple[int, str], ...]


def _join_continuations(body: list[tuple[int, str]]) -> tuple[tuple[int, str], ...]:
    commands: list[tuple[int, str]] = []
    pending = ""
    pending_start = 0
    for number, raw in body:
        line = raw if pending else _PROMPT.sub("", raw)
        if not pending:
            pending_start = number
        stripped = line.rstrip()
        if stripped.endswith(("\\", "`", " ^")):
            pending += stripped[:-1] + " "
            continue
        commands.append((pending_start, pending + line))
        pending = ""
    if pending:
        commands.append((pending_start, pending))
    return tuple(commands)


def command_blocks(text: str) -> list[CommandBlock]:
    """Fenced shell blocks and inline ``cmake``/``ctest``/``cpack`` code spans, in document order."""
    blocks: list[CommandBlock] = []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        opening = _FENCE.match(lines[index])
        if not opening:
            for span in _INLINE_COMMAND.finditer(lines[index]):
                blocks.append(CommandBlock("", ((index + 1, span.group(1)),)))
            index += 1
            continue
        fence, language = opening.group(1), opening.group(2).lower()
        body: list[tuple[int, str]] = []
        index += 1
        while index < len(lines):
            closing = _FENCE.match(lines[index])
            if (
                closing
                and not closing.group(2)
                and closing.group(1)[0] == fence[0]
                and len(closing.group(1)) >= len(fence)
            ):
                break
            body.append((index + 1, lines[index]))
            index += 1
        index += 1
        if language in SHELL_LANGUAGES:
            commands = _join_continuations(body)
            if commands:
                blocks.append(CommandBlock(language, commands))
    return blocks


def _segments(line: str) -> list[list[str]]:
    """Split one logical command line into control-operator separated token lists."""
    lexer = shlex.shlex(line, posix=True, punctuation_chars=";&|")
    lexer.whitespace_split = True
    lexer.commenters = "#"
    try:
        tokens = list(lexer)
    except ValueError:
        tokens = line.split("#", 1)[0].split()
    segments: list[list[str]] = [[]]
    for token in tokens:
        if _SEPARATOR.match(token):
            segments.append([])
        else:
            segments[-1].append(token)
    return [segment for segment in segments if segment]


def _normalize_tree(value: str, cwd: str | None) -> str | None:
    """A repository-relative build tree, or None when it cannot be resolved."""
    if not value or _PLACEHOLDER.search(value):
        return None
    normalized = value.replace("\\", "/")
    if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        return None
    while normalized.startswith("./"):
        normalized = normalized[2:]
    normalized = normalized.rstrip("/")
    if normalized in {"", "."}:
        return cwd
    if normalized.split("/", 1)[0] == "..":
        return None
    if cwd:
        return f"{cwd}/{normalized}"
    return normalized


def _option_value(arguments: list[str], index: int, flag: str) -> tuple[str | None, int]:
    """The value of ``flag`` at ``arguments[index]`` (spaced, ``=``, or glued short form)."""
    argument = arguments[index]
    if argument == flag:
        return (arguments[index + 1], 2) if index + 1 < len(arguments) else (None, 1)
    if flag.startswith("--") and argument.startswith(flag + "="):
        return argument.split("=", 1)[1], 1
    if not flag.startswith("--") and argument.startswith(flag) and len(argument) > len(flag):
        return argument[len(flag):], 1
    return None, 0


def _parse(tool: str, arguments: list[str]) -> dict[str, object]:
    parsed: dict[str, object] = {"mode": "configure" if tool == "cmake" else tool, "presets": []}
    flags = {
        "cmake": (("--preset", "preset"), ("-B", "binary"), ("-G", "generator"), ("-A", "architecture"),
                  ("-T", "toolset"), ("--build", "build"),
                  ("--install", "install"), ("--config", "config")),
        "ctest": (("--preset", "preset"), ("--test-dir", "tree"), ("--build-config", "config"), ("-C", "config")),
        "cpack": (("--preset", "preset"), ("--config", "cpack_config"), ("-C", "config")),
    }[tool]
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--":
            break
        if tool == "cmake" and argument in {"-E", "-P", "--workflow", "--help", "--version", "--system-information"}:
            parsed["mode"] = "other"
            return parsed
        consumed = 0
        for flag, key in flags:
            value, consumed = _option_value(arguments, index, flag)
            if not consumed:
                continue
            if key == "preset":
                if value is not None:
                    parsed["presets"].append(value)
            elif key in {"build", "install"}:
                parsed["mode"] = key
                if value is not None and value != "--preset" and not value.startswith("--preset="):
                    parsed["tree"] = value
                else:
                    consumed = 1
            elif value is not None:
                parsed[key] = value
            break
        index += consumed or 1
    return parsed


class _Checker:
    def __init__(self, index: CMakePresetIndex, relative: str) -> None:
        self.index = index
        self.relative = relative
        self.findings: list[Finding] = []

    def report(self, line: int, message: str) -> None:
        self.findings.append(Finding(self.relative, line, message))

    def check_document(self, text: str) -> None:
        state = _DocumentState()
        for block in command_blocks(text):
            state.block_presets = []
            state.block_adhoc = False
            state.cwd = None
            for line_number, command in block.commands:
                if block.language in WINDOWS_LANGUAGES:
                    command = command.replace("\\", "/")
                for segment in _segments(command):
                    self.check_segment(line_number, segment, state)

    def check_segment(self, line: int, tokens: list[str], state: _DocumentState) -> None:
        tokens = [token for token in tokens if token]
        script = next((token for token in tokens if token.lower() not in _INTERPRETERS), None)
        if script and script.replace("\\", "/").rsplit("/", 1)[-1] in CONFIGURE_SCRIPTS:
            if state.cwd is None:
                state.adhoc_trees.add("build")
                state.block_adhoc = True
            return
        position = next((i for i, token in enumerate(tokens) if _TOOL.match(token) or token == "cd"), None)
        if position is None:
            return
        if tokens[position] == "cd":
            target = tokens[position + 1] if position + 1 < len(tokens) else ""
            tree = _normalize_tree(target, state.cwd) if target not in {"-", "~"} else None
            state.cwd = tree if tree and tree.split("/", 1)[0].startswith("build") else None
            return
        tool = _TOOL.match(tokens[position]).group(1).lower()
        parsed = _parse(tool, tokens[position + 1:])
        mode = parsed["mode"]
        if mode == "other":
            return
        family = {"configure": "configure", "build": "build", "install": "configure", "ctest": "test",
                  "cpack": "package"}[mode]
        parsed["presets"] = [preset for preset in parsed["presets"] if not _PLACEHOLDER.search(preset)]
        for preset in parsed["presets"]:
            if not self.index.exists(family, preset):
                self.report(line, f"`{tool} --preset {preset}` names no visible {family} preset in CMakePresets.json")
        if mode == "configure":
            self.check_configure(line, parsed, state)
        elif mode in {"build", "install"} and "tree" in parsed:
            raw = str(parsed["tree"])
            self.check_tree_use(line, f"cmake --{mode} {raw}", _normalize_tree(raw, state.cwd), parsed, state)
        elif mode == "ctest" and not parsed["presets"]:
            raw = str(parsed["tree"]) if "tree" in parsed else None
            tree = _normalize_tree(raw, state.cwd) if raw is not None else state.cwd
            described = f"ctest --test-dir {raw}" if raw is not None else f"ctest (run in {state.cwd})"
            self.check_tree_use(line, described, tree, parsed, state)
        elif mode == "cpack" and not parsed["presets"]:
            # cpack reads <tree>/CPackConfig.cmake, from --config or the working directory.
            raw = str(parsed["cpack_config"]) if "cpack_config" in parsed else None
            if raw is None:
                tree = state.cwd
                described = f"cpack (run in {state.cwd})"
            else:
                directory = raw.replace("\\", "/").rpartition("/")[0]
                tree = _normalize_tree(directory or ".", state.cwd)
                described = f"cpack --config {raw}"
            self.check_tree_use(line, described, tree, parsed, state)

    def check_configure(self, line: int, parsed: dict[str, object], state: _DocumentState) -> None:
        presets = [preset for preset in parsed["presets"] if self.index.exists("configure", preset)]
        binary = parsed.get("binary")
        tree = _normalize_tree(str(binary), state.cwd) if binary else None
        if presets:
            if tree:
                state.adhoc_trees.add(tree)
                state.block_adhoc = True
            else:
                state.block_presets.extend(presets)
            return
        if parsed["presets"]:
            return  # already reported as an unknown preset
        generator = str(parsed.get("generator") or "")
        pins = self.index.generator_pins(generator) if generator else set()
        toolset = parsed.get("toolset")
        # Only the toolset version is pinned; `-T v143,host=x64` states the same toolset.
        stated = (parsed.get("architecture"), str(toolset).split(",", 1)[0] if toolset else None)
        pins = {(architecture, pin.split(",", 1)[0] if pin else None) for architecture, pin in pins}
        if pins and stated not in pins:
            expected = " or ".join(
                sorted(" ".join(f"{flag} {value}" for flag, value in (("-A", a), ("-T", t)) if value) for a, t in pins)
            )
            self.report(
                line,
                f"ad-hoc `cmake -G \"{generator}\"` configure does not pin what the presets using that generator "
                f"pin ({expected}); add them or configure through the preset",
            )
        if tree is None:
            return
        owner = self.index.binary_dirs.get(tree)
        if owner:
            self.report(
                line,
                f"ad-hoc `cmake -B {tree}` writes into the binaryDir of preset {owner!r}; "
                f"use `cmake --preset {owner}` instead",
            )
            return
        state.adhoc_trees.add(tree)
        state.block_adhoc = True

    def check_tree_use(
        self, line: int, command: str, tree: str | None, parsed: dict[str, object], state: _DocumentState
    ) -> None:
        if tree is None:
            return
        config = parsed.get("config")
        owner = self.index.binary_dirs.get(tree)
        if state.block_presets and not state.block_adhoc and owner not in state.block_presets:
            expected = ", ".join(
                sorted({self.index.binary_dir_of(preset) or preset for preset in state.block_presets})
            )
            used = f" (preset {owner!r}'s tree)" if owner else ""
            self.report(
                line,
                f"`{command}`{used} does not use the tree the preset configured above writes ({expected})",
            )
            return
        if owner is None:
            if tree not in state.adhoc_trees:
                self.report(
                    line,
                    f"`{command}`: {tree!r} is neither a preset binaryDir in CMakePresets.json nor "
                    "configured with -B earlier in this document",
                )
            return
        expected = self.index.expected_configuration(owner)
        flag = "--config" if command.startswith("cmake") else "-C"
        # cmake_install.cmake (run by --install and cpack) defaults a multi-config tree to Release;
        # --build and ctest default it to Debug.
        installing = command.startswith(("cmake --install", "cpack"))
        default = INSTALL_DEFAULT_CONFIGURATION if installing else "Debug"
        if config is None:
            if self.index.is_multi_config(owner) and expected and expected != default:
                action = "installs or packages" if installing else "builds or tests"
                self.report(
                    line,
                    f"`{command}` omits `{flag} {expected}`: preset {owner!r} uses a multi-config "
                    f"generator, which otherwise {action} {default}",
                )
            return
        if expected and str(config) != expected:
            self.report(
                line,
                f"`{command} {flag} {config}` does not match preset {owner!r}, whose configuration "
                f"is {expected}",
            )


def check_text(relative: str, text: str, index: CMakePresetIndex | None = None) -> list[Finding]:
    """Findings for one document's text (used directly by the tests)."""
    checker = _Checker(index or cmake_preset_index(), relative)
    checker.check_document(text)
    return checker.findings


def check_documents(root: Path = REPO_ROOT, index: CMakePresetIndex | None = None) -> list[Finding]:
    """Findings across every documented build surface."""
    index = index or cmake_preset_index()
    findings: list[Finding] = []
    documents = documented_markdown(root)
    if not documents:
        raise SiteDataError("no documentation surface resolved for build-command checking")
    for path in documents:
        relative = path.relative_to(root).as_posix()
        text = read_bytes_stable(path, MAX_DOCUMENT_BYTES, relative).decode("utf-8", errors="replace")
        findings.extend(check_text(relative, text, index))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.parse_args(argv)
    try:
        findings = check_documents()
    except SiteDataError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    for finding in findings:
        print(finding)
    if findings:
        print(f"{len(findings)} documented build command(s) disagree with CMakePresets.json", file=sys.stderr)
        return 1
    print(f"documented build commands agree with CMakePresets.json ({len(documented_markdown())} documents)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
