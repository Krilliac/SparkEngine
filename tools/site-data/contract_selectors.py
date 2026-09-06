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
from pathlib import Path

from common import REPO_ROOT, SiteDataError, read_bytes_stable


WORKFLOW_ROOT = REPO_ROOT / ".github" / "workflows"
TEST_ROOT = REPO_ROOT / "Tests"
TEST_CMAKE = TEST_ROOT / "CMakeLists.txt"
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


def reset_caches() -> None:
    """Drop cached inventories so a test can point the resolvers at new content."""
    workflow_job_ids.cache_clear()
    test_selector_targets.cache_clear()
    resolve_test_selector.cache_clear()


def _report() -> int:
    jobs = sorted(workflow_job_ids())
    targets = test_selector_targets()
    print(f"{len(jobs)} workflow jobs; {len(targets)} selectable test names and labels")
    return 0


if __name__ == "__main__":
    raise SystemExit(_report())
