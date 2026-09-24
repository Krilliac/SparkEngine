#!/usr/bin/env python3
"""Resolve work-item CI jobs and test selectors against what exists.

A work item that names a required CI job and a test selector is claiming a gate
can produce evidence. Until those two reference classes are resolved, the claim
costs nothing to write and nothing to keep: the name of a job that was never
added validates exactly as well as the name of one that runs.
"""

from __future__ import annotations

import fnmatch
import functools
import re
import shlex
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from common import REPO_ROOT, SiteDataError, read_bytes_stable


WORKFLOW_ROOT = REPO_ROOT / ".github" / "workflows"
TEST_ROOT = REPO_ROOT / "Tests"
TEST_CMAKE = TEST_ROOT / "CMakeLists.txt"
# The build-matrix inventory owns CMakePresets.json parsing and inheritance
# resolution; work-item commands are resolved through the same code so the two
# contracts cannot disagree about what a preset means.
BUILDMATRIX_ROOT = REPO_ROOT / "Tools" / "buildmatrix"
MAX_WORKFLOW_BYTES = 2 * 1024 * 1024
MAX_TEST_SOURCE_BYTES = 8 * 1024 * 1024
GLOB_CHARACTERS = "*?["

_TOP_LEVEL_KEY = re.compile(r"^([A-Za-z_][A-Za-z0-9_-]*):")
_JOB_KEY = re.compile(r"^  ([A-Za-z_][A-Za-z0-9_.-]*):\s*(?:#.*)?$")
_TEST_DEFINITION = re.compile(
    r"^[ \t]*TEST(?:_F)?[ \t]*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:,\s*([A-Za-z_][A-Za-z0-9_]*)\s*)?\)",
    re.MULTILINE,
)
_CTEST_NAME = re.compile(r"\bNAME\s+([A-Za-z0-9_.$<>:\-]+)")
_CTEST_LABELS = re.compile(r"\bLABELS\s+\"([^\"]*)\"")


@functools.lru_cache(maxsize=1)
def workflow_job_ids() -> frozenset[str]:
    """Job identifiers defined across .github/workflows.

    Parsed structurally rather than with a YAML library because the site-data
    tooling is stdlib-only: a job key is a two-space-indented mapping key inside
    the top-level ``jobs:`` block.
    """
    if not WORKFLOW_ROOT.is_dir():
        raise SiteDataError("workflow directory .github/workflows does not exist")
    identifiers: set[str] = set()
    workflows = sorted(
        path
        for path in WORKFLOW_ROOT.iterdir()
        if path.is_file() and not path.is_symlink() and path.suffix in {".yml", ".yaml"}
    )
    if not workflows:
        raise SiteDataError("no workflow files define any CI job")
    for path in workflows:
        payload = read_bytes_stable(path, MAX_WORKFLOW_BYTES, f"workflow {path.name}")
        inside = False
        for line in payload.decode("utf-8", errors="replace").splitlines():
            top = _TOP_LEVEL_KEY.match(line)
            if top:
                inside = top.group(1) == "jobs"
                continue
            if not inside:
                continue
            job = _JOB_KEY.match(line)
            if job:
                identifiers.add(job.group(1))
    return frozenset(identifiers)


@functools.lru_cache(maxsize=1)
def test_selector_targets() -> frozenset[str]:
    """Everything a test selector may legitimately name.

    That is the registered CTest test names and labels plus the TEST/TEST_F
    identifiers the SparkTests harness selects through SPARK_TEST_NAME.
    """
    targets: set[str] = set()
    if TEST_CMAKE.is_file():
        cmake = read_bytes_stable(TEST_CMAKE, MAX_TEST_SOURCE_BYTES, "Tests/CMakeLists.txt").decode(
            "utf-8", errors="replace"
        )
        for match in _CTEST_NAME.finditer(cmake):
            targets.add(match.group(1))
        for match in _CTEST_LABELS.finditer(cmake):
            targets.update(label.strip() for label in match.group(1).split(";") if label.strip())
    for path in sorted(TEST_ROOT.rglob("*.cpp")):
        if path.is_symlink() or not path.is_file():
            continue
        source = read_bytes_stable(path, MAX_TEST_SOURCE_BYTES, f"test source {path.name}").decode(
            "utf-8", errors="replace"
        )
        for match in _TEST_DEFINITION.finditer(source):
            suite, name = match.group(1), match.group(2)
            targets.add(suite)
            if name:
                targets.add(name)
                targets.add(f"{suite}.{name}")
    if not targets:
        raise SiteDataError("no CTest test, label, or SparkTests definition could be resolved")
    return frozenset(targets)


def resolve_ci_job(value: str) -> bool:
    """A required CI job must name one workflow job exactly."""
    return value in workflow_job_ids()


@functools.lru_cache(maxsize=4096)
def resolve_test_selector(value: str) -> bool:
    """A test selector may be an exact name or a glob over selectable names.

    Cached per selector: a glob is compared against every selectable name, and
    the contract asks the same few hundred questions repeatedly.
    """
    targets = test_selector_targets()
    if value in targets:
        return True
    if not any(character in value for character in GLOB_CHARACTERS):
        return False
    return any(fnmatch.fnmatchcase(target, value) for target in targets)


_CMAKE_TOOL = re.compile(r"^(?:.*[/\\])?(cmake|ctest|cpack)(?:\.exe)?$", re.IGNORECASE)
_BUILD_TESTS_ON = re.compile(r"^-D\s*BUILD_TESTS(?::BOOL)?=(?:ON|TRUE|YES|Y|1)$", re.IGNORECASE)
# CMake's false constants (if() semantics); an unset BUILD_TESTS keeps the
# option's ON default from the root CMakeLists.txt.
_CMAKE_FALSE_VALUES = {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"}
_CMAKE_MODE_PRESET_KINDS = {"configure": "configure", "build": "build", "workflow": "workflow"}
_TOOL_PRESET_KINDS = {"ctest": "test", "cpack": "package"}


@dataclass(frozen=True)
class PresetReference:
    """One preset or preset build tree named by a cmake/ctest/cpack invocation.

    ``kind`` is the preset family the name must resolve in (``configure``,
    ``build``, ``test``, ``package``, ``workflow``) or ``binaryDir`` for a
    ``build/<dir>`` tree. ``enables_tests`` marks a configure invocation that
    forces ``-DBUILD_TESTS=ON`` over the preset's own value.
    """

    tool: str
    kind: str
    name: str
    enables_tests: bool = False


def _command_tokens(segment: str) -> list[str]:
    try:
        return shlex.split(segment, posix=True)
    except ValueError:
        return segment.split()


def _build_tree(value: str) -> str | None:
    """Return ``build/<dir>`` when ``value`` points into the repository build root."""
    normalized = value.replace("\\", "/")
    while normalized.startswith("./"):
        normalized = normalized[2:]
    parts = [part for part in normalized.split("/") if part]
    if len(parts) < 2 or parts[0] != "build":
        return None
    return f"build/{parts[1]}"


def preset_references(command: str) -> list[PresetReference]:
    """Every preset and ``build/<dir>`` tree a work-item command hands to CMake tools.

    Segments are split on shell control operators like the CTest fail-on-empty
    check, so a later invocation cannot hide behind an earlier valid one.
    """
    references: list[PresetReference] = []
    for segment in re.split(r"[;&|\r\n]+", command):
        tokens = [token.lstrip("$(!").rstrip(")") for token in _command_tokens(segment)]
        start = next((index for index, token in enumerate(tokens) if _CMAKE_TOOL.match(token)), None)
        if start is None:
            continue
        tool = _CMAKE_TOOL.match(tokens[start]).group(1).lower()
        arguments = [token for token in tokens[start + 1:] if token]
        if tool == "cmake":
            mode = "configure"
            for flag, flag_mode in (("--build", "build"), ("--install", "install"), ("--workflow", "workflow")):
                if any(argument == flag or argument.startswith(flag + "=") for argument in arguments):
                    mode = flag_mode
                    break
            preset_kind = _CMAKE_MODE_PRESET_KINDS.get(mode, "configure")
        else:
            mode = tool
            preset_kind = _TOOL_PRESET_KINDS.get(tool, "test")
        enables_tests = mode == "configure" and any(
            _BUILD_TESTS_ON.match(argument)
            or (argument == "-D" and index + 1 < len(arguments) and _BUILD_TESTS_ON.match("-D" + arguments[index + 1]))
            for index, argument in enumerate(arguments)
        )
        for index, argument in enumerate(arguments):
            preset_name = None
            if argument == "--preset" and index + 1 < len(arguments):
                preset_name = arguments[index + 1]
            elif argument.startswith("--preset="):
                preset_name = argument.split("=", 1)[1]
            if preset_name:
                references.append(PresetReference(tool, preset_kind, preset_name, enables_tests))
                continue
            value = argument
            if argument.startswith("--") and "=" in argument:
                value = argument.split("=", 1)[1]
            elif argument.startswith("-B") and len(argument) > 2:
                value = argument[2:]
            tree = _build_tree(value)
            if tree:
                references.append(PresetReference(tool, "binaryDir", tree, enables_tests))
    return references


def _import_inventory() -> Any:
    root = str(BUILDMATRIX_ROOT)
    if root not in sys.path:
        sys.path.insert(0, root)
    import inventory  # noqa: PLC0415 -- resolved from Tools/buildmatrix on demand

    return inventory


class CMakePresetIndex:
    """Visible CMake presets, their resolved build trees, and whether they build tests."""

    def __init__(self, presets: dict[str, Any]) -> None:
        inventory = _import_inventory()
        try:
            self.names = {
                kind: {preset["name"] for preset in presets.get(f"{kind}Presets", []) if not preset.get("hidden")}
                for kind in ("configure", "build", "test", "package", "workflow")
            }
            self._configure = {
                name: inventory.resolve_configure_preset(presets, name) for name in self.names["configure"]
            }
            self._test_configure = {
                name: inventory.resolve_dependent_preset(presets, "testPresets", name)["configurePreset"]
                for name in self.names["test"]
            }
        except inventory.InventoryError as error:
            raise SiteDataError(f"CMakePresets.json: {error}") from error
        self.binary_dirs: dict[str, str] = {}
        for name, resolved in sorted(self._configure.items()):
            binary_dir = resolved.get("resolvedBinaryDir")
            if isinstance(binary_dir, str) and binary_dir.startswith("${sourceDir}/"):
                tree = _build_tree(binary_dir[len("${sourceDir}/"):])
                if tree and tree == binary_dir[len("${sourceDir}/"):].rstrip("/"):
                    self.binary_dirs.setdefault(tree, name)

    def exists(self, kind: str, name: str) -> bool:
        return name in self.names.get(kind, set())

    def configure_for(self, reference: PresetReference) -> str | None:
        """The configure preset whose build tree a reference runs against."""
        if reference.kind == "binaryDir":
            return self.binary_dirs.get(reference.name)
        if reference.kind == "test":
            return self._test_configure.get(reference.name)
        if reference.kind == "configure" and reference.name in self._configure:
            return reference.name
        return None

    def builds_tests(self, configure_name: str) -> bool:
        value = self._configure[configure_name]["cacheVariables"].get("BUILD_TESTS")
        if isinstance(value, dict):
            value = value.get("value")
        if value is None:
            return True
        if isinstance(value, bool):
            return value
        return str(value).strip().upper() not in _CMAKE_FALSE_VALUES and not str(value).upper().endswith("-NOTFOUND")


@functools.lru_cache(maxsize=1)
def cmake_preset_index() -> CMakePresetIndex:
    """The repository's CMakePresets.json, resolved once per run."""
    inventory = _import_inventory()
    try:
        presets = inventory.extract_cmake_presets()
    except inventory.InventoryError as error:
        raise SiteDataError(f"CMakePresets.json: {error}") from error
    return CMakePresetIndex(presets)


def reset_caches() -> None:
    """Drop cached inventories so a test can point the resolvers at new content."""
    workflow_job_ids.cache_clear()
    test_selector_targets.cache_clear()
    resolve_test_selector.cache_clear()
    cmake_preset_index.cache_clear()


def _report() -> int:
    jobs = sorted(workflow_job_ids())
    targets = test_selector_targets()
    print(f"{len(jobs)} workflow jobs; {len(targets)} selectable test names and labels")
    return 0


if __name__ == "__main__":
    raise SystemExit(_report())
