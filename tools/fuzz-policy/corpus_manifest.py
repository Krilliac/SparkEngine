#!/usr/bin/env python3
"""Strict corpus metadata and build-binding validation for SEC-120."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from build_binding import commands_named, parse_cmake, verify_cmake_registration
from harness_shape import assert_entry_symbol_in_sources, verify_harness
from parser_inventory import (
    ALLOWED_SOURCE_EXTENSIONS,
    DEFAULT_INVENTORY,
    ID_PATTERN,
    SHA256_PATTERN,
    Inventory,
    ParserRecord,
    load_inventory,
)
from policy_common import (
    FILE_ATTRIBUTE_REPARSE_POINT,
    Deadline,
    PolicyError,
    bounded_scandir,
    canonical_root,
    casefold_duplicates,
    confined_path,
    load_json_document,
    read_confined_file,
    require_exact_int,
    require_exact_keys,
    require_iso_date,
    require_list,
    require_string,
    require_token,
)


DEFAULT_CORPUS_MANIFEST = "tools/fuzz-policy/corpus-manifest.json"
MAX_CORPORA = 512
MAX_STALENESS_DAYS = 90
MAX_CORPUS_DIRECTORIES = 10_000
MAX_CORPUS_WALK_DEPTH = 64
CORPUS_SCAN_SECONDS = 60

# Seeds live in one reviewed tree. A corpus that pointed at a source root would
# count production .cpp files as fuzz seeds.
CORPUS_ROOT = "FuzzerTests/corpora"
CMAKE_FILE_NAMES = {"CMakeLists.txt"}
CMAKE_FILE_SUFFIXES = {".cmake"}
HARNESS_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}

# Every found issue lands as a minimized seed named regression-<slug>.<ext> and
# a record naming the finding and the test that fails without its fix.
REGRESSION_PREFIX = "regression-"
REGRESSION_FILE_PATTERN = re.compile(r"^regression-[a-z0-9]+(?:-[a-z0-9]+)*\.[a-z0-9]+$")
REGRESSION_FOUND_BY = frozenset({"campaign", "smoke", "review", "report"})
REGRESSION_PLACEHOLDER = "TODO"
MAX_REGRESSIONS_PER_CORPUS = 1024
MAX_FINDING_CHARS = 300
GUARD_TEST_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_.-]*$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")
REPLAY_RUNS_FLAG = "-runs="
# libFuzzer flags that make a replay depend on wall-clock time or on parallel
# worker scheduling. A blocking smoke is a deterministic replay of the reviewed
# seeds, so none of these may appear on its add_test.
NONDETERMINISTIC_REPLAY_FLAGS = ("-max_total_time=", "-jobs=", "-workers=", "-fork=")

# Guard tests resolve against first-party registrations: CTest names in any
# CMake listfile and TEST/TEST_F cases under Tests/. Vendored and build trees
# never count, and the walk is bounded like every other scan in this gate.
GUARD_SCAN_SKIP_DIRS = frozenset({"ThirdParty", "Assets", "Art", "node_modules"})
GUARD_SCAN_MAX_FILE_BYTES = 8 * 1024 * 1024
GUARD_SCAN_MAX_FILES = 20_000
_CTEST_REGISTRATION = re.compile(r"\badd_test\s*\(\s*NAME\s+([A-Za-z0-9_.\-]+)")
_SPARK_TEST_DEFINITION = re.compile(
    r"^[ \t]*TEST(?:_F)?[ \t]*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:,\s*([A-Za-z_][A-Za-z0-9_]*)\s*)?\)",
    re.MULTILINE,
)


@dataclass(frozen=True)
class ResourceBudget:
    max_input_bytes: int
    max_parse_time_ms: int
    max_memory_mb: int
    max_depth: int
    max_corpus_entries: int
    max_corpus_bytes: int
    smoke_seconds: int


@dataclass(frozen=True)
class RegressionRecord:
    file: str
    finding: str
    found_by: str
    guard_test: str
    fixed_commit: str


@dataclass(frozen=True)
class CorpusRecord:
    corpus_id: str
    parser_id: str
    corpus_dir: str
    last_verified: date
    content_digest: str
    budget: ResourceBudget
    seed_count: int
    seed_bytes: int
    regressions: tuple[RegressionRecord, ...]


def _parse_budget(value: Any, field: str) -> ResourceBudget:
    value = require_exact_keys(
        value,
        field,
        {
            "max_input_bytes",
            "max_parse_time_ms",
            "max_memory_mb",
            "max_depth",
            "max_corpus_entries",
            "max_corpus_bytes",
            "smoke_seconds",
        },
    )
    return ResourceBudget(
        max_input_bytes=require_exact_int(value["max_input_bytes"], f"{field}.max_input_bytes", minimum=1, maximum=100 * 1024 * 1024),
        max_parse_time_ms=require_exact_int(value["max_parse_time_ms"], f"{field}.max_parse_time_ms", minimum=1, maximum=60_000),
        max_memory_mb=require_exact_int(value["max_memory_mb"], f"{field}.max_memory_mb", minimum=16, maximum=4096),
        max_depth=require_exact_int(value["max_depth"], f"{field}.max_depth", minimum=1, maximum=1024),
        max_corpus_entries=require_exact_int(value["max_corpus_entries"], f"{field}.max_corpus_entries", minimum=1, maximum=100_000),
        max_corpus_bytes=require_exact_int(value["max_corpus_bytes"], f"{field}.max_corpus_bytes", minimum=1, maximum=10 * 1024 * 1024 * 1024),
        smoke_seconds=require_exact_int(value["smoke_seconds"], f"{field}.smoke_seconds", minimum=1, maximum=600),
    )


def scan_corpus(
    root: Path,
    corpus_dir: str,
    budget: ResourceBudget,
    field: str,
    deadline: Deadline,
) -> tuple[int, int, str, tuple[str, ...]]:
    """Count, bound, and actually read every seed.

    Returns the seed count, total bytes, content digest and each seed's path
    relative to ``corpus_dir``.

    stat() alone proves a seed exists, not that it can be read. Each seed is
    opened through the confinement helper so an unreadable or aliased seed is a
    hard failure instead of a silently counted one.
    """
    _, directory, _ = confined_path(root, corpus_dir, f"{field}.corpus_dir", expect="dir")
    entries = 0
    total_bytes = 0
    directory_count = 0
    digests: list[tuple[str, str]] = []
    stack: list[tuple[Path, int]] = [(directory, 0)]
    while stack:
        deadline.check()
        current, depth = stack.pop()
        if depth > MAX_CORPUS_WALK_DEPTH:
            raise PolicyError(f"{field}.corpus_dir exceeds depth {MAX_CORPUS_WALK_DEPTH}")
        directory_count += 1
        if directory_count > MAX_CORPUS_DIRECTORIES:
            raise PolicyError(f"{field}.corpus_dir exceeds {MAX_CORPUS_DIRECTORIES} directories")
        children = bounded_scandir(current, f"{field}.corpus_dir")
        child_dirs: list[Path] = []
        for child in children:
            deadline.check()
            relative = Path(child.path).relative_to(root).as_posix()
            try:
                # See parser_inventory.scan_source_tree: DirEntry.stat() can
                # synthesize st_nlink=0 on Windows, so use path lstat here.
                identity = os.lstat(child.path)
            except OSError as exc:
                raise PolicyError(f"{field} corpus entry is unreadable: {relative}: {exc}") from exc
            if child.is_symlink() or getattr(identity, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT:
                raise PolicyError(f"{field} corpus refuses symlink/reparse point: {relative}")
            if stat.S_ISDIR(identity.st_mode):
                child_dirs.append(Path(child.path))
                continue
            if not stat.S_ISREG(identity.st_mode):
                raise PolicyError(f"{field} corpus refuses non-regular file: {relative}")
            if identity.st_nlink != 1:
                raise PolicyError(f"{field} corpus refuses hard link: {relative}")
            if identity.st_size <= 0:
                raise PolicyError(f"{field} corpus seed is empty: {relative}")
            if identity.st_size > budget.max_input_bytes:
                raise PolicyError(f"{field} corpus seed exceeds max_input_bytes: {relative}")
            entries += 1
            total_bytes += identity.st_size
            if entries > budget.max_corpus_entries:
                raise PolicyError(f"{field} exceeds max_corpus_entries")
            if total_bytes > budget.max_corpus_bytes:
                raise PolicyError(f"{field} exceeds max_corpus_bytes")
            payload = read_confined_file(
                root, relative, f"{field} corpus seed", max_bytes=budget.max_input_bytes, root_is_canonical=True
            )
            digests.append((relative, hashlib.sha256(payload).hexdigest()))
        for child_dir in reversed(child_dirs):
            stack.append((child_dir, depth + 1))
    if entries == 0:
        raise PolicyError(f"{field}.corpus_dir contains no seeds")
    rollup = hashlib.sha256()
    for relative, digest in sorted(digests):
        rollup.update(relative.encode("utf-8"))
        rollup.update(b"\0")
        rollup.update(digest.encode("ascii"))
        rollup.update(b"\0")
    seeds = tuple(sorted(relative[len(corpus_dir) + 1:] for relative, _ in digests))
    return entries, total_bytes, rollup.hexdigest(), seeds


def _require_not_placeholder(value: str, field: str) -> str:
    if value.strip().upper() == REGRESSION_PLACEHOLDER:
        raise PolicyError(f"{field} is still the import placeholder {REGRESSION_PLACEHOLDER!r}; record the real value")
    return value


def _parse_regressions(value: Any, field: str) -> tuple[RegressionRecord, ...]:
    records: list[RegressionRecord] = []
    for index, item in enumerate(require_list(value, field, maximum=MAX_REGRESSIONS_PER_CORPUS)):
        item_field = f"{field}[{index}]"
        item = require_exact_keys(item, item_field, {"file", "finding", "found_by", "guard_test", "fixed_commit"})
        file_name = require_token(item["file"], f"{item_field}.file", REGRESSION_FILE_PATTERN)
        finding = _require_not_placeholder(
            require_string(item["finding"], f"{item_field}.finding", maximum=MAX_FINDING_CHARS),
            f"{item_field}.finding",
        )
        if "\n" in finding or "\r" in finding:
            raise PolicyError(f"{item_field}.finding must be one line")
        found_by = require_string(item["found_by"], f"{item_field}.found_by", maximum=32)
        if found_by not in REGRESSION_FOUND_BY:
            raise PolicyError(f"{item_field}.found_by must be one of {', '.join(sorted(REGRESSION_FOUND_BY))}")
        guard_test = _require_not_placeholder(
            require_token(item["guard_test"], f"{item_field}.guard_test", GUARD_TEST_PATTERN),
            f"{item_field}.guard_test",
        )
        fixed_commit = require_token(item["fixed_commit"], f"{item_field}.fixed_commit", COMMIT_PATTERN, maximum=40)
        records.append(RegressionRecord(file_name, finding, found_by, guard_test, fixed_commit))
    files = [record.file for record in records]
    if len(files) != len(set(files)):
        raise PolicyError(f"{field} declares a regression file twice")
    return tuple(records)


def is_shallow_checkout(root: Path) -> bool:
    """True when ``root`` is a shallow clone, whose history cannot show a recorded fix commit."""
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--is-shallow-repository"],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0 and result.stdout.strip() == "true"


def validate_regression_fix_commits(root: Path, inventory: Inventory, corpora: tuple[CorpusRecord, ...]) -> None:
    """A recorded fix must exist in this checkout and change the parser it guards."""
    sources = {parser.parser_id: set(parser.source_files) for parser in inventory.parsers}
    changed_by_commit: dict[str, set[str]] = {}
    for corpus in corpora:
        for record in corpus.regressions:
            commit = record.fixed_commit
            if commit not in changed_by_commit:
                try:
                    kind = subprocess.run(
                        ["git", "-C", str(root), "cat-file", "-t", commit],
                        capture_output=True, text=True, timeout=30, check=False,
                    )
                    if kind.returncode != 0 or kind.stdout.strip() != "commit":
                        raise PolicyError(f"{corpus.corpus_id} regression {record.file} fixed_commit is not a commit: {commit}")
                    diff = subprocess.run(
                        ["git", "-C", str(root), "diff-tree", "--root", "--no-commit-id", "--name-only", "-r", commit],
                        capture_output=True, text=True, timeout=30, check=False,
                    )
                except (OSError, subprocess.TimeoutExpired) as exc:
                    raise PolicyError(f"cannot verify fixed_commit {commit}: {exc}") from exc
                if diff.returncode != 0:
                    raise PolicyError(f"cannot inspect fixed_commit {commit}: {diff.stderr.strip()}")
                changed_by_commit[commit] = set(diff.stdout.splitlines())
            if not (sources[corpus.parser_id] & changed_by_commit[commit]):
                raise PolicyError(
                    f"{corpus.corpus_id} regression {record.file} fixed_commit {commit} "
                    "does not touch an inventoried parser source"
                )


def _require_declared_regressions(
    regressions: tuple[RegressionRecord, ...], seeds: tuple[str, ...], field: str
) -> None:
    """Every regression-* seed is declared and every declared fixture is a seed."""
    present = {seed for seed in seeds if PurePosixPath(seed).name.startswith(REGRESSION_PREFIX)}
    declared = {record.file for record in regressions}
    undeclared = sorted(present - declared)
    if undeclared:
        raise PolicyError(f"{field} has undeclared regression fixtures (add a regressions record): {undeclared}")
    missing = sorted(declared - set(seeds))
    if missing:
        raise PolicyError(f"{field}.regressions names fixtures that are not corpus seeds: {missing}")


def registered_test_names(root: Path, deadline: Deadline) -> frozenset[str]:
    """CTest names from every first-party CMake listfile plus SparkTests TEST/TEST_F names."""
    names: set[str] = set()
    scanned = 0

    def read(path: Path) -> str | None:
        nonlocal scanned
        deadline.check()
        scanned += 1
        if scanned > GUARD_SCAN_MAX_FILES:
            raise PolicyError(f"guard-test scan exceeds {GUARD_SCAN_MAX_FILES} files")
        if path.is_symlink() or not path.is_file() or path.stat().st_size > GUARD_SCAN_MAX_FILE_BYTES:
            return None
        return path.read_bytes().decode("utf-8", errors="replace")

    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = sorted(
            name
            for name in subdirectories
            if not name.startswith(".")
            and not name.startswith("build")
            and name not in GUARD_SCAN_SKIP_DIRS
            and not os.path.islink(os.path.join(directory, name))
        )
        current = Path(directory)
        for name in sorted(files):
            if name in CMAKE_FILE_NAMES or (current.name == "cmake" and PurePosixPath(name).suffix in CMAKE_FILE_SUFFIXES):
                text = read(current / name)
                if text is not None:
                    names.update(match.group(1) for match in _CTEST_REGISTRATION.finditer(text))
    tests_root = root / "Tests"
    if tests_root.is_dir():
        for path in sorted(tests_root.rglob("*.cpp")):
            text = read(path)
            if text is None:
                continue
            for match in _SPARK_TEST_DEFINITION.finditer(text):
                names.add(match.group(1))
                if match.group(2):
                    names.add(f"{match.group(1)}.{match.group(2)}")
    return frozenset(names)


def _require_corpus_dir(value: Any, field: str, roots: tuple[str, ...]) -> str:
    from policy_common import normalized_relative_path

    corpus_dir = normalized_relative_path(value, f"{field}.corpus_dir")
    if not corpus_dir.startswith(CORPUS_ROOT + "/"):
        raise PolicyError(f"{field}.corpus_dir must live under {CORPUS_ROOT}: {corpus_dir}")
    for scan_root in roots:
        if corpus_dir == scan_root or corpus_dir.startswith(scan_root + "/") or scan_root.startswith(corpus_dir + "/"):
            raise PolicyError(f"{field}.corpus_dir intersects the source scan root {scan_root}")
    return corpus_dir


def _parse_corpus(
    root: Path,
    value: Any,
    index: int,
    inventory: Inventory,
    *,
    as_of: date,
    deadline: Deadline,
) -> CorpusRecord:
    field = f"corpus_manifest.corpora[{index}]"
    value = require_exact_keys(
        value, field, {"id", "parser_id", "corpus_dir", "last_verified", "content_digest", "budget", "regressions"}
    )
    corpus_id = require_token(value["id"], f"{field}.id", ID_PATTERN)
    parser_id = require_token(value["parser_id"], f"{field}.parser_id", ID_PATTERN)
    corpus_dir = _require_corpus_dir(value["corpus_dir"], field, inventory.scope.roots)
    verified = require_iso_date(value["last_verified"], f"{field}.last_verified")
    if verified > as_of:
        raise PolicyError(f"{field}.last_verified must not be in the future")
    age = (as_of - verified).days
    if age > MAX_STALENESS_DAYS:
        raise PolicyError(f"{field} corpus is stale ({age} days; maximum {MAX_STALENESS_DAYS})")
    declared_digest = require_token(value["content_digest"], f"{field}.content_digest", SHA256_PATTERN, maximum=64)
    budget = _parse_budget(value["budget"], f"{field}.budget")
    regressions = _parse_regressions(value["regressions"], f"{field}.regressions")
    seed_count, seed_bytes, digest, seeds = scan_corpus(root, corpus_dir, budget, field, deadline)
    _require_declared_regressions(regressions, seeds, field)
    # last_verified only means something when it is pinned to exact content.
    if digest != declared_digest:
        raise PolicyError(
            f"{field} corpus content changed since {verified.isoformat()}; "
            "re-verify the seeds and update content_digest"
        )
    return CorpusRecord(
        corpus_id, parser_id, corpus_dir, verified, declared_digest, budget, seed_count, seed_bytes, regressions
    )


def _require_artifact_shapes(parser: ParserRecord, owned_sources: set[str]) -> None:
    assert parser.target is not None
    field = f"{parser.parser_id}.target"
    harness = parser.target["harness"]
    cmake_file = parser.target["cmake_file"]
    if PurePosixPath(harness).suffix.lower() not in HARNESS_SUFFIXES:
        raise PolicyError(f"{field}.harness is not a C/C++ source file: {harness}")
    name = PurePosixPath(cmake_file).name
    if name not in CMAKE_FILE_NAMES and PurePosixPath(cmake_file).suffix.lower() not in CMAKE_FILE_SUFFIXES:
        raise PolicyError(f"{field}.cmake_file is not a CMake listfile: {cmake_file}")
    # A production source may not audit itself: harness and registration must be
    # separate artifacts from the parser they cover.
    for role, path in (("harness", harness), ("cmake_file", cmake_file)):
        if path in owned_sources:
            raise PolicyError(f"{field}.{role} is also an inventoried parser source: {path}")
    binding_source = parser.target.get("binding_source")
    if binding_source is not None:
        if PurePosixPath(binding_source).suffix.lower() not in HARNESS_SUFFIXES:
            raise PolicyError(f"{field}.binding_source is not a C/C++ source file: {binding_source}")
        if binding_source in owned_sources:
            raise PolicyError(f"{field}.binding_source is also an inventoried parser source: {binding_source}")


def _validate_target_binding(root: Path, parser: ParserRecord, corpus: CorpusRecord, owned_sources: set[str]) -> None:
    assert parser.target is not None
    target = parser.target
    field = parser.parser_id
    _require_artifact_shapes(parser, owned_sources)
    budget = corpus.budget
    verify_cmake_registration(
        root,
        cmake_file=target["cmake_file"],
        cmake_target=target["cmake_target"],
        test_selector=target["test_selector"],
        harness=target["harness"],
        corpus_dir=corpus.corpus_dir,
        max_input_bytes=budget.max_input_bytes,
        timeout_seconds=max(1, (budget.max_parse_time_ms + 999) // 1000),
        max_memory_mb=budget.max_memory_mb,
        smoke_seconds=budget.smoke_seconds,
        field=field,
    )
    verify_harness(
        root,
        harness=target["harness"],
        entry_symbol=target["harness_entry_symbol"],
        max_depth=budget.max_depth,
        max_input_bytes=budget.max_input_bytes,
        field=field,
    )
    _verify_replay_runs(root, target["cmake_file"], target["test_selector"], corpus.seed_count, field)
    assert_entry_symbol_in_sources(root, parser.source_files, target["entry_symbol"], field)
    if target["binding_source"] is not None:
        assert_entry_symbol_in_sources(
            root,
            (target["binding_source"],),
            target["entry_symbol"],
            f"{field}.target.binding_source",
        )


def _verify_replay_runs(root: Path, cmake_file: str, test_selector: str, seed_count: int, field: str) -> None:
    """The blocking smoke must replay every seed, regression fixtures included, exactly once."""
    payload = read_confined_file(root, cmake_file, f"{field}.cmake_file", max_bytes=1024 * 1024)
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PolicyError(f"{field}.cmake_file must be strict UTF-8") from exc
    for command in commands_named(parse_cmake(text, f"{field}.cmake_file"), "add_test"):
        if len(command.arguments) > 1 and command.arguments[0].upper() == "NAME" and command.arguments[1] == test_selector:
            nondeterministic = [
                argument for argument in command.arguments if argument.startswith(NONDETERMINISTIC_REPLAY_FLAGS)
            ]
            if nondeterministic:
                raise PolicyError(
                    f"{field} add_test {test_selector!r} passes {nondeterministic}; the blocking smoke must be a "
                    "deterministic replay of the reviewed seeds, not a time- or worker-bounded campaign"
                )
            runs = [argument for argument in command.arguments if argument.startswith(REPLAY_RUNS_FLAG)]
            expected = f"{REPLAY_RUNS_FLAG}{seed_count}"
            if runs != [expected]:
                raise PolicyError(
                    f"{field} add_test {test_selector!r} passes {runs or 'no -runs'}; the corpus holds {seed_count} "
                    f"seeds, so the replay must pass exactly {expected}"
                )
            return
    raise PolicyError(f"{field} has no add_test registering {test_selector!r}")


def load_corpora(
    root: Path,
    inventory: Inventory,
    manifest_path: str = DEFAULT_CORPUS_MANIFEST,
    *,
    as_of: date | None = None,
    deadline: Deadline | None = None,
) -> tuple[CorpusRecord, ...]:
    root = canonical_root(root)
    as_of = as_of or datetime.now(timezone.utc).date()
    deadline = deadline or Deadline(CORPUS_SCAN_SECONDS, "corpus scan")
    document = load_json_document(root, manifest_path, "corpus_manifest")
    document = require_exact_keys(document, "corpus_manifest", {"schema_version", "corpora"})
    require_exact_int(document["schema_version"], "corpus_manifest.schema_version", minimum=1, maximum=1)
    values = require_list(document["corpora"], "corpus_manifest.corpora", maximum=MAX_CORPORA)
    corpora = tuple(
        _parse_corpus(root, item, index, inventory, as_of=as_of, deadline=deadline)
        for index, item in enumerate(values)
    )

    for label, key in (("corpus ids", "corpus_id"), ("parser ids", "parser_id"), ("corpus directories", "corpus_dir")):
        seen = [getattr(corpus, key) for corpus in corpora]
        if len(seen) != len(set(seen)):
            raise PolicyError(f"corpus_manifest reuses {label} across corpora")
        casefold_duplicates(seen, f"corpus_manifest {label}")

    fuzzed = {parser.parser_id: parser for parser in inventory.parsers if parser.status == "fuzzed"}
    corpus_by_parser = {corpus.parser_id: corpus for corpus in corpora}
    if set(corpus_by_parser) != set(fuzzed):
        missing = sorted(set(fuzzed) - set(corpus_by_parser))
        extra = sorted(set(corpus_by_parser) - set(fuzzed))
        raise PolicyError(f"corpus/parser mismatch; missing={missing}, extra={extra}")

    owned_sources = {source for parser in inventory.parsers for source in parser.source_files}
    for parser_id, parser in fuzzed.items():
        corpus = corpus_by_parser[parser_id]
        assert parser.target is not None
        if parser.target["corpus_id"] != corpus.corpus_id:
            raise PolicyError(f"{parser_id}.target.corpus_id does not name its corpus")
        _validate_target_binding(root, parser, corpus, owned_sources)

    if any(corpus.regressions for corpus in corpora):
        # A guard that names no registered test proves nothing about the fix.
        registered = registered_test_names(root, deadline)
        for corpus in corpora:
            for record in corpus.regressions:
                if record.guard_test not in registered:
                    raise PolicyError(
                        f"{corpus.corpus_id} regression {record.file} names guard_test {record.guard_test!r}, "
                        "which is neither a registered CTest nor a Tests/ TEST case"
                    )
    return corpora


def build_corpus_report(
    root: Path,
    inventory: Inventory,
    corpus_path: str,
    *,
    as_of: date | None = None,
    deadline: Deadline | None = None,
    verify_fix_commits: bool = True,
) -> dict[str, Any]:
    corpora = load_corpora(root, inventory, corpus_path, as_of=as_of, deadline=deadline)
    if verify_fix_commits:
        validate_regression_fix_commits(root, inventory, corpora)
    return {
        "schema_version": 1,
        "corpus_count": len(corpora),
        "seed_count": sum(corpus.seed_count for corpus in corpora),
        "seed_bytes": sum(corpus.seed_bytes for corpus in corpora),
        "regression_count": sum(len(corpus.regressions) for corpus in corpora),
        "bound_target_count": len(corpora),
        "max_staleness_days": MAX_STALENESS_DAYS,
        "corpora": [
            {
                "id": corpus.corpus_id,
                "parser_id": corpus.parser_id,
                "seed_count": corpus.seed_count,
                "seed_bytes": corpus.seed_bytes,
                "regression_count": len(corpus.regressions),
                "last_verified": corpus.last_verified.isoformat(),
            }
            for corpus in corpora
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=".")
    parser.add_argument("--inventory", default=DEFAULT_INVENTORY)
    parser.add_argument("--manifest", default=DEFAULT_CORPUS_MANIFEST)
    parser.add_argument("--emit-json", action="store_true")
    parser.add_argument("--as-of-date", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    as_of: date | None = None
    if args.as_of_date:
        try:
            as_of = date.fromisoformat(args.as_of_date)
        except ValueError:
            print("invalid --as-of-date", file=sys.stderr)
            return 2
    try:
        root = Path(args.source_root)
        inventory = load_inventory(root, args.inventory, as_of=as_of)
        report = build_corpus_report(root, inventory, args.manifest, as_of=as_of)
    except (OSError, PolicyError) as exc:
        if args.emit_json:
            print(json.dumps({"passed": False, "error": str(exc)}, indent=2, sort_keys=True))
        else:
            print(f"corpus manifest: FAIL: {exc}", file=sys.stderr)
        return 1
    if args.emit_json:
        print(json.dumps({"passed": True, **report}, indent=2, sort_keys=True))
    else:
        print(f"corpus manifest: PASS ({report['corpus_count']} corpora, {report['seed_count']} seeds)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
