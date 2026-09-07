#!/usr/bin/env python3
"""Strict, bounded parser inventory and source-candidate scanner for SEC-120."""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from policy_common import (
    FILE_ATTRIBUTE_REPARSE_POINT,
    PolicyError,
    canonical_root,
    confined_path,
    load_json_document,
    normalized_relative_path,
    read_confined_file,
    require_exact_int,
    require_exact_keys,
    require_list,
    require_string,
)


DEFAULT_INVENTORY = "tools/fuzz-policy/parser-inventory.json"
ALLOWED_SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm"}
ID_PATTERN = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
TICKET_PATTERN = re.compile(r"^[A-Z]+-[1-9][0-9]*$")
SOURCE_NAME_PATTERN = re.compile(
    r"(?:parser|serializer|deserializer|importer|loader|manifest|archive|reader|codec)",
    re.IGNORECASE,
)
CONTENT_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("json-parse", re.compile(r"\b(?:nlohmann\s*::\s*)?json\s*::\s*parse\s*\(")),
    ("deserialize", re.compile(r"\b(?:De|Un)serialize[A-Za-z0-9_]*\s*\(")),
    ("parse-entry", re.compile(r"\b(?:Parse|Load)(?:Json|JSON|File|FromFile|FromBuffer|FromStream|Manifest|Header|Scene|Save|Asset|Archive|Config)\s*\(")),
    ("image-codec", re.compile(r"\b(?:stbi_load|LoadEXR|ParseEXRHeaderFromMemory)\s*\(")),
    ("model-codec", re.compile(r"\b(?:tinyobj\s*::\s*LoadObj|cgltf_parse|ufbx_load)\s*\(")),
    ("archive-codec", re.compile(r"\b(?:unzOpen|mz_zip_reader_init|ZSTD_decompress)\s*\(")),
)


@dataclass(frozen=True)
class ScanLimits:
    max_files: int
    max_directories: int
    max_file_bytes: int
    max_total_bytes: int
    max_depth: int
    timeout_seconds: int


@dataclass(frozen=True)
class Scope:
    roots: tuple[str, ...]
    excluded_subtrees: tuple[str, ...]
    extensions: frozenset[str]
    limits: ScanLimits


@dataclass(frozen=True)
class ParserRecord:
    parser_id: str
    description: str
    trust_boundary: str
    source_files: tuple[str, ...]
    formats: tuple[str, ...]
    status: str
    blocker_reason: str | None
    blocker_ticket: str | None
    target: dict[str, Any] | None


@dataclass(frozen=True)
class Inventory:
    schema_version: int
    scope: Scope
    parsers: tuple[ParserRecord, ...]
    deferred_candidates: tuple[str, ...]
    deferred_reason: str
    deferred_ticket: str


def _string_array(value: Any, field: str, *, maximum: int) -> tuple[str, ...]:
    values = require_list(value, field, maximum=maximum)
    if not values:
        raise PolicyError(f"{field} must not be empty")
    result = tuple(require_string(item, f"{field}[{index}]", maximum=1024) for index, item in enumerate(values))
    if len(set(result)) != len(result):
        raise PolicyError(f"{field} contains duplicate values")
    return result


def _path_array(value: Any, field: str, *, maximum: int) -> tuple[str, ...]:
    values = _string_array(value, field, maximum=maximum)
    result = tuple(normalized_relative_path(item, f"{field}[{index}]") for index, item in enumerate(values))
    if len(set(result)) != len(result):
        raise PolicyError(f"{field} contains duplicate normalized paths")
    return result


def _parse_scope(root: Path, value: Any) -> Scope:
    value = require_exact_keys(
        value,
        "inventory.scope",
        {"roots", "excluded_subtrees", "extensions", "limits"},
    )
    roots = _path_array(value["roots"], "inventory.scope.roots", maximum=32)
    for index, path in enumerate(roots):
        confined_path(root, path, f"inventory.scope.roots[{index}]", expect="dir")

    exclusions = require_list(value["excluded_subtrees"], "inventory.scope.excluded_subtrees", maximum=32)
    excluded_paths: list[str] = []
    for index, exclusion in enumerate(exclusions):
        field = f"inventory.scope.excluded_subtrees[{index}]"
        exclusion = require_exact_keys(exclusion, field, {"path", "reason", "ticket"})
        path = normalized_relative_path(exclusion["path"], f"{field}.path")
        require_string(exclusion["reason"], f"{field}.reason", maximum=1024)
        ticket = require_string(exclusion["ticket"], f"{field}.ticket", maximum=64)
        if not TICKET_PATTERN.fullmatch(ticket):
            raise PolicyError(f"{field}.ticket is not a tracking ticket")
        if not any(path == scan_root or path.startswith(scan_root + "/") for scan_root in roots):
            raise PolicyError(f"{field}.path is outside the declared scan roots")
        confined_path(root, path, f"{field}.path", expect="dir")
        excluded_paths.append(path)
    if len(set(excluded_paths)) != len(excluded_paths):
        raise PolicyError("inventory.scope.excluded_subtrees contains duplicate paths")

    extensions = _string_array(value["extensions"], "inventory.scope.extensions", maximum=32)
    extension_set = frozenset(extensions)
    if any(not item.startswith(".") or item.lower() != item for item in extension_set):
        raise PolicyError("inventory.scope.extensions must be lowercase dotted suffixes")
    unknown_extensions = extension_set - ALLOWED_SOURCE_EXTENSIONS
    if unknown_extensions:
        raise PolicyError(f"inventory.scope.extensions has unsupported values: {sorted(unknown_extensions)}")

    limits = require_exact_keys(
        value["limits"],
        "inventory.scope.limits",
        {"max_files", "max_directories", "max_file_bytes", "max_total_bytes", "max_depth", "timeout_seconds"},
    )
    parsed_limits = ScanLimits(
        max_files=require_exact_int(limits["max_files"], "inventory.scope.limits.max_files", minimum=1, maximum=100_000),
        max_directories=require_exact_int(limits["max_directories"], "inventory.scope.limits.max_directories", minimum=1, maximum=20_000),
        max_file_bytes=require_exact_int(limits["max_file_bytes"], "inventory.scope.limits.max_file_bytes", minimum=1, maximum=16 * 1024 * 1024),
        max_total_bytes=require_exact_int(limits["max_total_bytes"], "inventory.scope.limits.max_total_bytes", minimum=1, maximum=1024 * 1024 * 1024),
        max_depth=require_exact_int(limits["max_depth"], "inventory.scope.limits.max_depth", minimum=1, maximum=64),
        timeout_seconds=require_exact_int(limits["timeout_seconds"], "inventory.scope.limits.timeout_seconds", minimum=1, maximum=300),
    )
    return Scope(roots, tuple(excluded_paths), extension_set, parsed_limits)


def _parse_target(root: Path, value: Any, field: str) -> dict[str, Any]:
    value = require_exact_keys(
        value,
        field,
        {"harness", "cmake_file", "cmake_target", "test_selector", "corpus_id"},
    )
    harness = normalized_relative_path(value["harness"], f"{field}.harness")
    cmake_file = normalized_relative_path(value["cmake_file"], f"{field}.cmake_file")
    confined_path(root, harness, f"{field}.harness", expect="file")
    confined_path(root, cmake_file, f"{field}.cmake_file", expect="file")
    return {
        "harness": harness,
        "cmake_file": cmake_file,
        "cmake_target": require_string(value["cmake_target"], f"{field}.cmake_target", maximum=128),
        "test_selector": require_string(value["test_selector"], f"{field}.test_selector", maximum=128),
        "corpus_id": require_string(value["corpus_id"], f"{field}.corpus_id", maximum=128),
    }


def _parse_parser(root: Path, value: Any, index: int) -> ParserRecord:
    field = f"inventory.parsers[{index}]"
    value = require_exact_keys(
        value,
        field,
        {"id", "description", "trust_boundary", "source_files", "formats", "status"},
        {"blocker", "target"},
    )
    parser_id = require_string(value["id"], f"{field}.id", maximum=128)
    if not ID_PATTERN.fullmatch(parser_id):
        raise PolicyError(f"{field}.id must be lowercase kebab-case")
    description = require_string(value["description"], f"{field}.description", maximum=512)
    trust = require_string(value["trust_boundary"], f"{field}.trust_boundary", maximum=64)
    if trust != "untrusted-file":
        raise PolicyError(f"{field}.trust_boundary must be untrusted-file in SEC-120 scope")
    source_files = _path_array(value["source_files"], f"{field}.source_files", maximum=64)
    for source_index, source_file in enumerate(source_files):
        confined_path(root, source_file, f"{field}.source_files[{source_index}]", expect="file")
    formats = _string_array(value["formats"], f"{field}.formats", maximum=64)
    status = require_string(value["status"], f"{field}.status", maximum=32)
    if status not in {"blocked", "fuzzed"}:
        raise PolicyError(f"{field}.status must be blocked or fuzzed")

    blocker_reason: str | None = None
    blocker_ticket: str | None = None
    target: dict[str, Any] | None = None
    if status == "blocked":
        if "target" in value:
            raise PolicyError(f"{field}.target is forbidden for blocked parser")
        blocker = require_exact_keys(value.get("blocker"), f"{field}.blocker", {"reason", "ticket"})
        blocker_reason = require_string(blocker["reason"], f"{field}.blocker.reason", maximum=1024)
        blocker_ticket = require_string(blocker["ticket"], f"{field}.blocker.ticket", maximum=64)
        if blocker_ticket != "SEC-120":
            raise PolicyError(f"{field}.blocker.ticket must be SEC-120")
    else:
        if "blocker" in value:
            raise PolicyError(f"{field}.blocker is forbidden for fuzzed parser")
        target = _parse_target(root, value.get("target"), f"{field}.target")

    return ParserRecord(
        parser_id,
        description,
        trust,
        source_files,
        formats,
        status,
        blocker_reason,
        blocker_ticket,
        target,
    )


def load_inventory(root: Path, manifest_path: str = DEFAULT_INVENTORY) -> Inventory:
    root = canonical_root(root)
    document = load_json_document(root, manifest_path, "parser_inventory")
    document = require_exact_keys(
        document,
        "inventory",
        {"schema_version", "scope", "parsers", "deferred_candidates"},
    )
    schema_version = require_exact_int(document["schema_version"], "inventory.schema_version", minimum=1, maximum=1)
    scope = _parse_scope(root, document["scope"])
    parser_values = require_list(document["parsers"], "inventory.parsers", maximum=512)
    if not parser_values:
        raise PolicyError("inventory.parsers must not be empty")
    parsers = tuple(_parse_parser(root, item, index) for index, item in enumerate(parser_values))
    ids = [parser.parser_id for parser in parsers]
    if len(ids) != len(set(ids)):
        raise PolicyError("inventory.parsers contains duplicate ids")
    owned_sources = [source for parser in parsers for source in parser.source_files]
    if len(owned_sources) != len(set(owned_sources)):
        raise PolicyError("inventory.parsers maps a source file to more than one parser")

    deferred = require_exact_keys(
        document["deferred_candidates"],
        "inventory.deferred_candidates",
        {"reason", "ticket", "source_files"},
    )
    deferred_reason = require_string(deferred["reason"], "inventory.deferred_candidates.reason", maximum=1024)
    deferred_ticket = require_string(deferred["ticket"], "inventory.deferred_candidates.ticket", maximum=64)
    if deferred_ticket != "SEC-120":
        raise PolicyError("inventory.deferred_candidates.ticket must be SEC-120")
    deferred_values = require_list(
        deferred["source_files"],
        "inventory.deferred_candidates.source_files",
        maximum=20_000,
    )
    deferred_files = tuple(
        normalized_relative_path(item, f"inventory.deferred_candidates.source_files[{index}]")
        for index, item in enumerate(deferred_values)
    )
    if len(set(deferred_files)) != len(deferred_files):
        raise PolicyError("inventory.deferred_candidates.source_files contains duplicate paths")
    for index, source_file in enumerate(deferred_files):
        confined_path(root, source_file, f"inventory.deferred_candidates.source_files[{index}]", expect="file")
    overlap = set(owned_sources) & set(deferred_files)
    if overlap:
        raise PolicyError(f"inventory sources cannot also be deferred: {sorted(overlap)}")
    return Inventory(schema_version, scope, parsers, deferred_files, deferred_reason, deferred_ticket)


def _excluded(relative: str, exclusions: tuple[str, ...]) -> bool:
    return any(relative == item or relative.startswith(item + "/") for item in exclusions)


def _candidate_reasons(relative: str, text: str) -> list[str]:
    reasons = [name for name, pattern in CONTENT_PATTERNS if pattern.search(text)]
    if SOURCE_NAME_PATTERN.search(Path(relative).stem):
        reasons.append("parser-like-filename")
    return sorted(set(reasons))


def scan_source_tree(root: Path, scope: Scope) -> list[dict[str, Any]]:
    """Scan deterministic first-party roots without following aliases or skipping read errors."""
    root = canonical_root(root)
    started = time.monotonic()
    file_count = 0
    directory_count = 0
    total_bytes = 0
    candidates: list[dict[str, Any]] = []

    def check_time() -> None:
        if time.monotonic() - started > scope.limits.timeout_seconds:
            raise PolicyError(f"source scan exceeded {scope.limits.timeout_seconds} seconds")

    for scan_root in scope.roots:
        _, absolute_root, _ = confined_path(root, scan_root, f"scan root {scan_root}", expect="dir")
        stack: list[tuple[Path, int]] = [(absolute_root, 0)]
        while stack:
            directory, depth = stack.pop()
            check_time()
            if depth > scope.limits.max_depth:
                raise PolicyError(f"source scan exceeded depth {scope.limits.max_depth}: {directory}")
            directory_count += 1
            if directory_count > scope.limits.max_directories:
                raise PolicyError(f"source scan exceeded {scope.limits.max_directories} directories")
            try:
                entries = sorted(os.scandir(directory), key=lambda entry: entry.name.casefold())
            except OSError as exc:
                raise PolicyError(f"source directory is unreadable: {directory}: {exc}") from exc
            child_directories: list[Path] = []
            for entry in entries:
                check_time()
                relative = Path(entry.path).relative_to(root).as_posix()
                try:
                    # DirEntry.stat() reports st_nlink=0 on some Windows Python
                    # builds. lstat(path) returns the real link count used by the
                    # hard-link policy and still refuses to follow aliases.
                    identity = os.lstat(entry.path)
                except OSError as exc:
                    raise PolicyError(f"source entry is unreadable: {relative}: {exc}") from exc
                if entry.is_symlink() or getattr(identity, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT:
                    raise PolicyError(f"source scan refuses symlink/reparse point: {relative}")
                if _excluded(relative, scope.excluded_subtrees):
                    continue
                if stat.S_ISDIR(identity.st_mode):
                    child_directories.append(Path(entry.path))
                    continue
                if Path(entry.name).suffix.lower() not in scope.extensions:
                    continue
                if not stat.S_ISREG(identity.st_mode):
                    raise PolicyError(f"source scan refuses non-regular file: {relative}")
                if identity.st_nlink != 1:
                    raise PolicyError(f"source scan refuses hard link: {relative}")
                file_count += 1
                if file_count > scope.limits.max_files:
                    raise PolicyError(f"source scan exceeded {scope.limits.max_files} files")
                if identity.st_size > scope.limits.max_file_bytes:
                    raise PolicyError(f"source file exceeds {scope.limits.max_file_bytes} bytes: {relative}")
                total_bytes += identity.st_size
                if total_bytes > scope.limits.max_total_bytes:
                    raise PolicyError(f"source scan exceeded {scope.limits.max_total_bytes} aggregate bytes")
                payload = read_confined_file(root, relative, f"source file {relative}", max_bytes=scope.limits.max_file_bytes)
                try:
                    text = payload.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise PolicyError(f"source file is not strict UTF-8: {relative}") from exc
                reasons = _candidate_reasons(relative, text)
                if reasons:
                    candidates.append({"source_file": relative, "reasons": reasons})
            for child in reversed(child_directories):
                stack.append((child, depth + 1))

    return sorted(candidates, key=lambda item: item["source_file"])


def build_inventory_report(root: Path, manifest_path: str = DEFAULT_INVENTORY) -> dict[str, Any]:
    inventory = load_inventory(root, manifest_path)
    candidates = scan_source_tree(root, inventory.scope)
    candidate_files = {item["source_file"] for item in candidates}
    owned_files = {source for parser in inventory.parsers for source in parser.source_files}
    deferred_files = set(inventory.deferred_candidates)
    unexpected = sorted(candidate_files - owned_files - deferred_files)
    stale_deferred = sorted(deferred_files - candidate_files)
    if unexpected:
        raise PolicyError(f"unclassified parser candidates: {', '.join(unexpected[:30])}")
    if stale_deferred:
        raise PolicyError(f"stale deferred parser candidates: {', '.join(stale_deferred[:30])}")

    return {
        "schema_version": inventory.schema_version,
        "policy_complete": not inventory.deferred_candidates and all(parser.status == "fuzzed" for parser in inventory.parsers),
        "scan_roots": list(inventory.scope.roots),
        "excluded_subtrees": list(inventory.scope.excluded_subtrees),
        "parser_count": len(inventory.parsers),
        "fuzzed_count": sum(parser.status == "fuzzed" for parser in inventory.parsers),
        "blocked_count": sum(parser.status == "blocked" for parser in inventory.parsers),
        "candidate_count": len(candidates),
        "deferred_candidate_count": len(inventory.deferred_candidates),
        "unclassified_candidate_count": 0,
        "parsers": [
            {
                "id": parser.parser_id,
                "status": parser.status,
                "source_files": list(parser.source_files),
                "formats": list(parser.formats),
                "target": parser.target,
            }
            for parser in inventory.parsers
        ],
        "candidates": candidates,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=".")
    parser.add_argument("--manifest", default=DEFAULT_INVENTORY)
    parser.add_argument("--emit-json", action="store_true")
    args = parser.parse_args(argv)
    try:
        report = build_inventory_report(Path(args.source_root), args.manifest)
    except (OSError, PolicyError) as exc:
        if args.emit_json:
            print(json.dumps({"passed": False, "error": str(exc)}, indent=2, sort_keys=True))
        else:
            print(f"parser inventory: FAIL: {exc}", file=sys.stderr)
        return 1
    if args.emit_json:
        print(json.dumps({"passed": True, **report}, indent=2, sort_keys=True))
    else:
        print(
            "parser inventory: PASS "
            f"({report['parser_count']} parsers, {report['candidate_count']} candidates, "
            f"{report['deferred_candidate_count']} deferred)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
