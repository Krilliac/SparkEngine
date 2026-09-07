#!/usr/bin/env python3
"""Strict corpus metadata and static resource-binding validation for SEC-120."""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
import time
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Any

from parser_inventory import DEFAULT_INVENTORY, Inventory, ParserRecord, load_inventory
from policy_common import (
    FILE_ATTRIBUTE_REPARSE_POINT,
    PolicyError,
    canonical_root,
    confined_path,
    load_json_document,
    read_confined_file,
    require_exact_int,
    require_exact_keys,
    require_list,
    require_string,
)


DEFAULT_CORPUS_MANIFEST = "tools/fuzz-policy/corpus-manifest.json"
MAX_CORPORA = 512
MAX_STALENESS_DAYS = 90
MAX_CORPUS_DIRECTORIES = 10_000
MAX_CORPUS_WALK_DEPTH = 64
MAX_CORPUS_SCAN_SECONDS = 30


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


def _scan_corpus(root: Path, corpus_dir: str, budget: ResourceBudget, field: str) -> tuple[int, int]:
    _, directory, _ = confined_path(root, corpus_dir, f"{field}.corpus_dir", expect="dir")
    entries = 0
    total_bytes = 0
    directory_count = 0
    started = time.monotonic()
    stack: list[tuple[Path, int]] = [(directory, 0)]
    while stack:
        if time.monotonic() - started > MAX_CORPUS_SCAN_SECONDS:
            raise PolicyError(f"{field}.corpus_dir scan exceeded {MAX_CORPUS_SCAN_SECONDS} seconds")
        current, depth = stack.pop()
        if depth > MAX_CORPUS_WALK_DEPTH:
            raise PolicyError(f"{field}.corpus_dir exceeds depth {MAX_CORPUS_WALK_DEPTH}")
        directory_count += 1
        if directory_count > MAX_CORPUS_DIRECTORIES:
            raise PolicyError(f"{field}.corpus_dir exceeds {MAX_CORPUS_DIRECTORIES} directories")
        try:
            children = sorted(os.scandir(current), key=lambda child: child.name.casefold())
        except OSError as exc:
            raise PolicyError(f"{field}.corpus_dir is unreadable: {exc}") from exc
        child_dirs: list[Path] = []
        for child in children:
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
        for child_dir in reversed(child_dirs):
            stack.append((child_dir, depth + 1))
    if entries == 0:
        raise PolicyError(f"{field}.corpus_dir contains no seeds")
    return entries, total_bytes


def _parse_corpus(root: Path, value: Any, index: int, *, as_of: date) -> CorpusRecord:
    field = f"corpus_manifest.corpora[{index}]"
    value = require_exact_keys(value, field, {"id", "parser_id", "corpus_dir", "last_verified", "budget"})
    corpus_id = require_string(value["id"], f"{field}.id", maximum=128)
    parser_id = require_string(value["parser_id"], f"{field}.parser_id", maximum=128)
    corpus_dir = require_string(value["corpus_dir"], f"{field}.corpus_dir", maximum=1024).replace("\\", "/")
    verified_text = require_string(value["last_verified"], f"{field}.last_verified", maximum=10)
    try:
        verified = date.fromisoformat(verified_text)
    except ValueError as exc:
        raise PolicyError(f"{field}.last_verified must be YYYY-MM-DD") from exc
    if verified.isoformat() != verified_text:
        raise PolicyError(f"{field}.last_verified must use canonical YYYY-MM-DD")
    if verified > as_of:
        raise PolicyError(f"{field}.last_verified must not be in the future")
    age = (as_of - verified).days
    if age > MAX_STALENESS_DAYS:
        raise PolicyError(f"{field} corpus is stale ({age} days; maximum {MAX_STALENESS_DAYS})")
    budget = _parse_budget(value["budget"], f"{field}.budget")
    seed_count, seed_bytes = _scan_corpus(root, corpus_dir, budget, field)
    return CorpusRecord(corpus_id, parser_id, corpus_dir, verified, budget, seed_count, seed_bytes)


def _require_literal(text: str, literal: str, field: str) -> None:
    if literal not in text:
        raise PolicyError(f"{field} does not bind required literal {literal!r}")


def _validate_target_binding(root: Path, parser: ParserRecord, corpus: CorpusRecord) -> None:
    assert parser.target is not None
    target = parser.target
    cmake_payload = read_confined_file(root, target["cmake_file"], f"{parser.parser_id}.cmake_file", max_bytes=1024 * 1024)
    harness_payload = read_confined_file(root, target["harness"], f"{parser.parser_id}.harness", max_bytes=4 * 1024 * 1024)
    try:
        cmake_text = cmake_payload.decode("utf-8")
        harness_text = harness_payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PolicyError(f"{parser.parser_id} target files must be strict UTF-8") from exc

    budget = corpus.budget
    timeout_seconds = max(1, (budget.max_parse_time_ms + 999) // 1000)
    for literal in (
        target["cmake_target"],
        target["test_selector"],
        target["harness"],
        f"-max_len={budget.max_input_bytes}",
        f"-timeout={timeout_seconds}",
        f"-rss_limit_mb={budget.max_memory_mb}",
        f"TIMEOUT {budget.smoke_seconds}",
    ):
        _require_literal(cmake_text, literal, f"{parser.parser_id}.cmake_file")
    _require_literal(harness_text, "LLVMFuzzerTestOneInput", f"{parser.parser_id}.harness")
    depth_pattern = re.compile(rf"\bSPARK_FUZZ_MAX_DEPTH\s*=\s*{budget.max_depth}\b")
    if not depth_pattern.search(harness_text):
        raise PolicyError(f"{parser.parser_id}.harness does not bind max_depth={budget.max_depth}")


def load_corpora(
    root: Path,
    inventory: Inventory,
    manifest_path: str = DEFAULT_CORPUS_MANIFEST,
    *,
    as_of: date | None = None,
) -> tuple[CorpusRecord, ...]:
    root = canonical_root(root)
    as_of = as_of or datetime.now(timezone.utc).date()
    document = load_json_document(root, manifest_path, "corpus_manifest")
    document = require_exact_keys(document, "corpus_manifest", {"schema_version", "corpora"})
    require_exact_int(document["schema_version"], "corpus_manifest.schema_version", minimum=1, maximum=1)
    values = require_list(document["corpora"], "corpus_manifest.corpora", maximum=MAX_CORPORA)
    corpora = tuple(_parse_corpus(root, item, index, as_of=as_of) for index, item in enumerate(values))

    corpus_ids = [corpus.corpus_id for corpus in corpora]
    parser_ids = [corpus.parser_id for corpus in corpora]
    if len(corpus_ids) != len(set(corpus_ids)):
        raise PolicyError("corpus_manifest contains duplicate corpus ids")
    if len(parser_ids) != len(set(parser_ids)):
        raise PolicyError("corpus_manifest contains more than one corpus for a parser")

    fuzzed = {parser.parser_id: parser for parser in inventory.parsers if parser.status == "fuzzed"}
    corpus_by_parser = {corpus.parser_id: corpus for corpus in corpora}
    if set(corpus_by_parser) != set(fuzzed):
        missing = sorted(set(fuzzed) - set(corpus_by_parser))
        extra = sorted(set(corpus_by_parser) - set(fuzzed))
        raise PolicyError(f"corpus/parser mismatch; missing={missing}, extra={extra}")
    for parser_id, parser in fuzzed.items():
        corpus = corpus_by_parser[parser_id]
        assert parser.target is not None
        if parser.target["corpus_id"] != corpus.corpus_id:
            raise PolicyError(f"{parser_id}.target.corpus_id does not name its corpus")
        _validate_target_binding(root, parser, corpus)
    return corpora


def build_corpus_report(root: Path, inventory_path: str, corpus_path: str, *, as_of: date | None = None) -> dict[str, Any]:
    inventory = load_inventory(root, inventory_path)
    corpora = load_corpora(root, inventory, corpus_path, as_of=as_of)
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
        report = build_corpus_report(Path(args.source_root), args.inventory, args.manifest, as_of=as_of)
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
