#!/usr/bin/env python3
"""Strict corpus metadata and build-binding validation for SEC-120."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from build_binding import verify_cmake_registration
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
CORPUS_ROOT = "Tests/fuzz-corpora"
CMAKE_FILE_NAMES = {"CMakeLists.txt"}
CMAKE_FILE_SUFFIXES = {".cmake"}
HARNESS_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}


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
class CorpusRecord:
    corpus_id: str
    parser_id: str
    corpus_dir: str
    last_verified: date
    content_digest: str
    budget: ResourceBudget
    seed_count: int
    seed_bytes: int


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


def _scan_corpus(
    root: Path,
    corpus_dir: str,
    budget: ResourceBudget,
    field: str,
    deadline: Deadline,
) -> tuple[int, int, str]:
    """Count, bound, and actually read every seed, returning a content digest.

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
    return entries, total_bytes, rollup.hexdigest()


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
        value, field, {"id", "parser_id", "corpus_dir", "last_verified", "content_digest", "budget"}
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
    seed_count, seed_bytes, digest = _scan_corpus(root, corpus_dir, budget, field, deadline)
    # last_verified only means something when it is pinned to exact content.
    if digest != declared_digest:
        raise PolicyError(
            f"{field} corpus content changed since {verified.isoformat()}; "
            "re-verify the seeds and update content_digest"
        )
    return CorpusRecord(corpus_id, parser_id, corpus_dir, verified, declared_digest, budget, seed_count, seed_bytes)


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
        entry_symbol=target["entry_symbol"],
        max_depth=budget.max_depth,
        max_input_bytes=budget.max_input_bytes,
        field=field,
    )
    assert_entry_symbol_in_sources(root, parser.source_files, target["entry_symbol"], field)


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
    return corpora


def build_corpus_report(
    root: Path,
    inventory: Inventory,
    corpus_path: str,
    *,
    as_of: date | None = None,
    deadline: Deadline | None = None,
) -> dict[str, Any]:
    corpora = load_corpora(root, inventory, corpus_path, as_of=as_of, deadline=deadline)
    return {
        "schema_version": 1,
        "corpus_count": len(corpora),
        "seed_count": sum(corpus.seed_count for corpus in corpora),
        "seed_bytes": sum(corpus.seed_bytes for corpus in corpora),
        "bound_target_count": len(corpora),
        "max_staleness_days": MAX_STALENESS_DAYS,
        "corpora": [
            {
                "id": corpus.corpus_id,
                "parser_id": corpus.parser_id,
                "seed_count": corpus.seed_count,
                "seed_bytes": corpus.seed_bytes,
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
