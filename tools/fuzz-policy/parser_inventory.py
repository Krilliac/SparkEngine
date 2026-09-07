#!/usr/bin/env python3
"""Strict, bounded parser inventory and source-candidate scanner for SEC-120."""

from __future__ import annotations

import argparse
import json
import re
import stat
import sys
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Any

from policy_common import (
    FILE_ATTRIBUTE_REPARSE_POINT,
    Deadline,
    PolicyError,
    bounded_scandir,
    canonical_root,
    casefold_duplicates,
    confined_path,
    load_json_document,
    normalized_relative_path,
    read_confined_file,
    require_exact_int,
    require_exact_keys,
    require_iso_date,
    require_list,
    require_string,
    require_token,
)
import os


DEFAULT_INVENTORY = "tools/fuzz-policy/parser-inventory.json"
ALLOWED_SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm"}

ID_PATTERN = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*")
OWNER_PATTERN = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
CMAKE_NAME_PATTERN = re.compile(r"[A-Za-z][A-Za-z0-9_.+-]{2,63}")
SYMBOL_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]{2,}(?:::[A-Za-z_][A-Za-z0-9_]{2,})*")

# An exclusion is a reviewed, owned, expiring waiver — never free text. A ticket
# that is not on this list cannot hide a single source file.
APPROVED_EXCLUSION_TICKETS: dict[str, str] = {
    "NET-100": "net-100-protocol-campaign",
    "ENG-200": "eng-200-scripting-campaign",
}
DEFERRAL_TICKET = "SEC-120"
DEFERRAL_OWNER = "sec-120-parser-triage"

MAX_EXCLUSION_HIDDEN_CANDIDATES = 128
MAX_WAIVER_DAYS = 365
MAX_DEFERRED_CANDIDATES = 4_096

SOURCE_NAME_PATTERN = re.compile(
    r"(?:parser|serializer|deserializer|importer|loader|manifest|archive|reader|codec)",
    re.IGNORECASE,
)
CONTENT_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("json-parse", re.compile(r"\b(?:nlohmann\s*::\s*)?json\s*::\s*parse\s*\(")),
    ("json-parse-strict", re.compile(r"\bJson\s*::\s*Parse(?:Strict)?\s*\(")),
    ("json-string-scrape", re.compile(r"\bfindStringValue\b|\bExtract(?:Json|JSON)[A-Za-z0-9_]*\s*\(")),
    ("deserialize", re.compile(r"\b(?:De|Un)serialize[A-Za-z0-9_]*\s*\(")),
    (
        "parse-entry",
        re.compile(
            r"\b(?:Parse|Load)(?:Json|JSON|File|FromFile|FromBuffer|FromStream|Manifest"
            r"|Header|Scene|Save|Asset|Archive|Config)\s*\("
        ),
    ),
    (
        "load-format-entry",
        re.compile(
            r"\b(?:Load|Read|Decode|Parse)(?:WAV|Wav|BMP|Bmp|TGA|Tga|OBJ|Obj|NTEX|Ntex|Weights"
            r"|Skeleton|Animations|Terrain|Compressed|Precompiled|Sidecar|Templates|Baseline"
            r"|GoldenImage|ExecScript|Metadata|Heightmap|FromDisk|FromMemory|FromBytes)"
            r"[A-Za-z0-9_]*\s*\("
        ),
    ),
    ("image-codec", re.compile(r"\b(?:stbi_load|stbi_load_from_memory|LoadEXR|ParseEXRHeaderFromMemory)\s*\(")),
    (
        "os-image-codec",
        re.compile(
            r"\b(?:CreateDecoderFromFilename|CreateDecoderFromStream|WICDecodeMetadataCacheOn\w+"
            r"|CreateDDSTextureFrom\w+|CreateWICTextureFrom\w+)\b"
        ),
    ),
    ("model-codec", re.compile(r"\b(?:tinyobj\s*::\s*LoadObj|cgltf_parse|ufbx_load)\s*\(")),
    ("archive-codec", re.compile(r"\b(?:unzOpen|mz_zip_reader_init|ZSTD_decompress)\s*\(")),
    # Generic bounded binary readers. A hand-rolled byte cursor over file bytes
    # is a parser even when nothing in its name says so.
    ("binary-stream-read", re.compile(r"\.read\s*\(\s*reinterpret_cast\s*<")),
    ("stdio-binary-read", re.compile(r"\bfread\s*\(")),
    (
        "binary-magic-compare",
        re.compile(
            r"\bk[A-Z][A-Za-z0-9]*Magic\b"
            r"|\b(?:[A-Za-z_]\w*\.)?magic\s*(?:!=|==)"
            r"|\bmemcmp\s*\(\s*[A-Za-z_][\w.>\[\]-]*\s*,\s*\"[A-Za-z ]{4}\"\s*,\s*4\s*\)"
        ),
    ),
    (
        "byte-cursor",
        re.compile(
            r"\b(?:Freeze|Thaw|Unpack|Decode)(?:String|Bytes|Blob|Buffer|Raw|Tag|Header|Frame)\s*\("
            r"|\bm_readPos\b"
        ),
    ),
    ("scanf-parse", re.compile(r"\b(?:sscanf|sscanf_s|fscanf|swscanf|swscanf_s)\s*\(")),
    ("stream-slurp", re.compile(r"std::istreambuf_iterator\s*<\s*char\s*>")),
    ("getline-loop", re.compile(r"while\s*\(\s*std::getline\s*\(")),
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
class SubtreeExclusion:
    path: str
    reason: str
    ticket: str
    owner: str
    expires: date
    hidden_candidate_count: int


@dataclass(frozen=True)
class FileExclusion:
    path: str
    reason: str
    ticket: str
    owner: str
    expires: date
    sha256: str


@dataclass(frozen=True)
class Scope:
    roots: tuple[str, ...]
    excluded_subtrees: tuple[SubtreeExclusion, ...]
    excluded_files: tuple[FileExclusion, ...]
    extensions: frozenset[str]
    limits: ScanLimits


@dataclass(frozen=True)
class Deferral:
    source_file: str
    reason: str
    owner: str
    ticket: str
    expires: date


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
    deferred_candidates: tuple[Deferral, ...]


@dataclass(frozen=True)
class ScanResult:
    candidates: tuple[dict[str, Any], ...]
    hidden_by_subtree: dict[str, int]
    hidden_by_file: frozenset[str]
    scanned_files: int


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
    casefold_duplicates(result, field)
    return result


def _require_waiver(value: dict, field: str, *, as_of: date) -> tuple[str, str, date]:
    """Every waiver names an approved ticket, its approved owner, and an expiry."""
    ticket = require_token(value["ticket"], f"{field}.ticket", re.compile(r"[A-Z]+-[1-9][0-9]*"), maximum=32)
    if ticket not in APPROVED_EXCLUSION_TICKETS:
        raise PolicyError(
            f"{field}.ticket {ticket} is not an approved exclusion ticket "
            f"({', '.join(sorted(APPROVED_EXCLUSION_TICKETS))})"
        )
    owner = require_token(value["owner"], f"{field}.owner", OWNER_PATTERN, maximum=64)
    if owner != APPROVED_EXCLUSION_TICKETS[ticket]:
        raise PolicyError(f"{field}.owner must be {APPROVED_EXCLUSION_TICKETS[ticket]} for {ticket}")
    expires = require_iso_date(value["expires"], f"{field}.expires")
    if expires <= as_of:
        raise PolicyError(f"{field} expired on {expires.isoformat()}")
    if (expires - as_of).days > MAX_WAIVER_DAYS:
        raise PolicyError(f"{field}.expires is more than {MAX_WAIVER_DAYS} days out")
    require_string(value["reason"], f"{field}.reason", maximum=1024)
    return ticket, owner, expires


def _parse_subtree_exclusions(root: Path, value: Any, roots: tuple[str, ...], *, as_of: date) -> tuple[SubtreeExclusion, ...]:
    entries = require_list(value, "inventory.scope.excluded_subtrees", maximum=32)
    exclusions: list[SubtreeExclusion] = []
    for index, entry in enumerate(entries):
        field = f"inventory.scope.excluded_subtrees[{index}]"
        entry = require_exact_keys(
            entry, field, {"path", "reason", "ticket", "owner", "expires", "hidden_candidate_count"}
        )
        path = normalized_relative_path(entry["path"], f"{field}.path")
        # An exclusion may never swallow a whole scan root: that would silently
        # retire the root instead of waiving a reviewed subtree.
        inside = [scan_root for scan_root in roots if path.startswith(scan_root + "/")]
        if not inside:
            if any(path == scan_root or scan_root.startswith(path + "/") for scan_root in roots):
                raise PolicyError(f"{field}.path excludes an entire scan root: {path}")
            raise PolicyError(f"{field}.path is outside the declared scan roots: {path}")
        confined_path(root, path, f"{field}.path", expect="dir")
        ticket, owner, expires = _require_waiver(entry, field, as_of=as_of)
        hidden = require_exact_int(
            entry["hidden_candidate_count"],
            f"{field}.hidden_candidate_count",
            minimum=1,
            maximum=MAX_EXCLUSION_HIDDEN_CANDIDATES,
        )
        exclusions.append(SubtreeExclusion(path, entry["reason"], ticket, owner, expires, hidden))

    paths = [exclusion.path for exclusion in exclusions]
    if len(set(paths)) != len(paths):
        raise PolicyError("inventory.scope.excluded_subtrees contains duplicate paths")
    casefold_duplicates(paths, "inventory.scope.excluded_subtrees")
    for outer in paths:
        for inner in paths:
            if outer != inner and inner.startswith(outer + "/"):
                raise PolicyError(f"inventory.scope.excluded_subtrees overlap: {outer} contains {inner}")
    return tuple(exclusions)


def _parse_file_exclusions(root: Path, value: Any, roots: tuple[str, ...], *, as_of: date) -> tuple[FileExclusion, ...]:
    entries = require_list(value, "inventory.scope.excluded_files", maximum=64)
    exclusions: list[FileExclusion] = []
    for index, entry in enumerate(entries):
        field = f"inventory.scope.excluded_files[{index}]"
        entry = require_exact_keys(entry, field, {"path", "reason", "ticket", "owner", "expires", "sha256"})
        path = normalized_relative_path(entry["path"], f"{field}.path")
        if not any(path.startswith(scan_root + "/") for scan_root in roots):
            raise PolicyError(f"{field}.path is outside the declared scan roots: {path}")
        confined_path(root, path, f"{field}.path", expect="file")
        ticket, owner, expires = _require_waiver(entry, field, as_of=as_of)
        digest = require_token(entry["sha256"], f"{field}.sha256", SHA256_PATTERN, maximum=64)
        exclusions.append(FileExclusion(path, entry["reason"], ticket, owner, expires, digest))

    paths = [exclusion.path for exclusion in exclusions]
    if len(set(paths)) != len(paths):
        raise PolicyError("inventory.scope.excluded_files contains duplicate paths")
    casefold_duplicates(paths, "inventory.scope.excluded_files")
    return tuple(exclusions)


FIRST_PARTY_PREFIXES = ("Spark",)
FIRST_PARTY_EXTRA = ("GameModules",)
SOURCE_DIRECTORY_NAMES = ("Source", "src", "Include")


def _require_root_coverage(root: Path, roots: tuple[str, ...]) -> None:
    """Every first-party product source tree present in the repo must be scanned.

    Without this, the cheapest way to a green gate is to quietly drop a root:
    the report would still print ``scan_roots`` as though it were coverage.
    """
    covered = set(roots)
    missing: list[str] = []
    for entry in bounded_scandir(root, "inventory.scope.roots coverage"):
        if not entry.is_dir():
            continue
        name = entry.name
        if not (name.startswith(FIRST_PARTY_PREFIXES) or name in FIRST_PARTY_EXTRA):
            continue
        if name in covered:
            continue
        candidates = [
            f"{name}/{child}"
            for child in SOURCE_DIRECTORY_NAMES
            if (root / name / child).is_dir()
        ]
        if not candidates:
            continue
        if not any(candidate in covered for candidate in candidates):
            missing.append(" or ".join(candidates))
    if missing:
        raise PolicyError(f"inventory.scope.roots does not cover first-party source trees: {', '.join(sorted(missing))}")


def _parse_scope(root: Path, value: Any, *, as_of: date) -> Scope:
    value = require_exact_keys(
        value,
        "inventory.scope",
        {"roots", "excluded_subtrees", "excluded_files", "extensions", "limits"},
    )
    roots = _path_array(value["roots"], "inventory.scope.roots", maximum=32)
    for index, path in enumerate(roots):
        confined_path(root, path, f"inventory.scope.roots[{index}]", expect="dir")
    for outer in roots:
        for inner in roots:
            if outer != inner and inner.startswith(outer + "/"):
                raise PolicyError(f"inventory.scope.roots overlap: {outer} contains {inner}")

    excluded_subtrees = _parse_subtree_exclusions(root, value["excluded_subtrees"], roots, as_of=as_of)
    excluded_files = _parse_file_exclusions(root, value["excluded_files"], roots, as_of=as_of)
    for exclusion in excluded_files:
        for subtree in excluded_subtrees:
            if exclusion.path.startswith(subtree.path + "/"):
                raise PolicyError(f"inventory.scope.excluded_files {exclusion.path} is already inside {subtree.path}")

    _require_root_coverage(root, roots)

    extensions = _string_array(value["extensions"], "inventory.scope.extensions", maximum=32)
    extension_set = frozenset(extensions)
    if any(not item.startswith(".") or item.lower() != item for item in extension_set):
        raise PolicyError("inventory.scope.extensions must be lowercase dotted suffixes")
    # Shrinking the extension list is the cheapest way to make the gate look
    # clean, so the full set is mandatory rather than configurable.
    if extension_set != ALLOWED_SOURCE_EXTENSIONS:
        missing = sorted(ALLOWED_SOURCE_EXTENSIONS - extension_set)
        unknown = sorted(extension_set - ALLOWED_SOURCE_EXTENSIONS)
        raise PolicyError(
            f"inventory.scope.extensions must be the full supported set; missing={missing}, unsupported={unknown}"
        )

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
        timeout_seconds=require_exact_int(limits["timeout_seconds"], "inventory.scope.limits.timeout_seconds", minimum=1, maximum=600),
    )
    return Scope(roots, excluded_subtrees, excluded_files, extension_set, parsed_limits)


def _parse_target(root: Path, value: Any, field: str) -> dict[str, Any]:
    value = require_exact_keys(
        value,
        field,
        {"harness", "cmake_file", "cmake_target", "test_selector", "corpus_id", "entry_symbol"},
    )
    harness = normalized_relative_path(value["harness"], f"{field}.harness")
    cmake_file = normalized_relative_path(value["cmake_file"], f"{field}.cmake_file")
    confined_path(root, harness, f"{field}.harness", expect="file")
    confined_path(root, cmake_file, f"{field}.cmake_file", expect="file")
    return {
        "harness": harness,
        "cmake_file": cmake_file,
        "cmake_target": require_token(value["cmake_target"], f"{field}.cmake_target", CMAKE_NAME_PATTERN),
        "test_selector": require_token(value["test_selector"], f"{field}.test_selector", CMAKE_NAME_PATTERN),
        "corpus_id": require_token(value["corpus_id"], f"{field}.corpus_id", ID_PATTERN),
        "entry_symbol": require_token(value["entry_symbol"], f"{field}.entry_symbol", SYMBOL_PATTERN),
    }


def _parse_parser(root: Path, value: Any, index: int) -> ParserRecord:
    field = f"inventory.parsers[{index}]"
    value = require_exact_keys(
        value,
        field,
        {"id", "description", "trust_boundary", "source_files", "formats", "status"},
        {"blocker", "target"},
    )
    parser_id = require_token(value["id"], f"{field}.id", ID_PATTERN)
    description = require_string(value["description"], f"{field}.description", maximum=512)
    trust = require_token(value["trust_boundary"], f"{field}.trust_boundary", re.compile(r"untrusted-file"), maximum=64)
    source_files = _path_array(value["source_files"], f"{field}.source_files", maximum=64)
    for source_index, source_file in enumerate(source_files):
        confined_path(root, source_file, f"{field}.source_files[{source_index}]", expect="file")
    formats = _string_array(value["formats"], f"{field}.formats", maximum=64)
    status = require_token(value["status"], f"{field}.status", re.compile(r"blocked|fuzzed"), maximum=32)

    blocker_reason: str | None = None
    blocker_ticket: str | None = None
    target: dict[str, Any] | None = None
    if status == "blocked":
        if "target" in value:
            raise PolicyError(f"{field}.target is forbidden for blocked parser")
        blocker = require_exact_keys(value.get("blocker"), f"{field}.blocker", {"reason", "ticket"})
        blocker_reason = require_string(blocker["reason"], f"{field}.blocker.reason", maximum=1024)
        blocker_ticket = require_token(blocker["ticket"], f"{field}.blocker.ticket", re.compile(r"SEC-120"), maximum=64)
    else:
        if "blocker" in value:
            raise PolicyError(f"{field}.blocker is forbidden for fuzzed parser")
        target = _parse_target(root, value.get("target"), f"{field}.target")

    return ParserRecord(
        parser_id, description, trust, source_files, formats, status, blocker_reason, blocker_ticket, target
    )


def _parse_deferrals(root: Path, value: Any, *, as_of: date) -> tuple[Deferral, ...]:
    entries = require_list(value, "inventory.deferred_candidates", maximum=MAX_DEFERRED_CANDIDATES)
    deferrals: list[Deferral] = []
    for index, entry in enumerate(entries):
        field = f"inventory.deferred_candidates[{index}]"
        entry = require_exact_keys(entry, field, {"source_file", "reason", "owner", "ticket", "expires"})
        source_file = normalized_relative_path(entry["source_file"], f"{field}.source_file")
        confined_path(root, source_file, f"{field}.source_file", expect="file")
        ticket = require_token(entry["ticket"], f"{field}.ticket", re.compile(r"[A-Z]+-[1-9][0-9]*"), maximum=32)
        if ticket != DEFERRAL_TICKET:
            raise PolicyError(f"{field}.ticket must be {DEFERRAL_TICKET}")
        owner = require_token(entry["owner"], f"{field}.owner", OWNER_PATTERN, maximum=64)
        if owner != DEFERRAL_OWNER:
            raise PolicyError(f"{field}.owner must be {DEFERRAL_OWNER}")
        expires = require_iso_date(entry["expires"], f"{field}.expires")
        if expires <= as_of:
            raise PolicyError(f"{field} deferral expired on {expires.isoformat()}")
        if (expires - as_of).days > MAX_WAIVER_DAYS:
            raise PolicyError(f"{field}.expires is more than {MAX_WAIVER_DAYS} days out")
        reason = require_string(entry["reason"], f"{field}.reason", maximum=512)
        deferrals.append(Deferral(source_file, reason, owner, ticket, expires))

    paths = [deferral.source_file for deferral in deferrals]
    if len(set(paths)) != len(paths):
        raise PolicyError("inventory.deferred_candidates contains duplicate source files")
    casefold_duplicates(paths, "inventory.deferred_candidates")
    return tuple(deferrals)


def load_inventory(root: Path, manifest_path: str = DEFAULT_INVENTORY, *, as_of: date | None = None) -> Inventory:
    root = canonical_root(root)
    as_of = as_of or datetime.now(timezone.utc).date()
    document = load_json_document(root, manifest_path, "parser_inventory")
    document = require_exact_keys(
        document, "inventory", {"schema_version", "scope", "parsers", "deferred_candidates"}
    )
    schema_version = require_exact_int(document["schema_version"], "inventory.schema_version", minimum=1, maximum=1)
    scope = _parse_scope(root, document["scope"], as_of=as_of)
    parser_values = require_list(document["parsers"], "inventory.parsers", maximum=512)
    if not parser_values:
        raise PolicyError("inventory.parsers must not be empty")
    parsers = tuple(_parse_parser(root, item, index) for index, item in enumerate(parser_values))

    ids = [parser.parser_id for parser in parsers]
    if len(ids) != len(set(ids)):
        raise PolicyError("inventory.parsers contains duplicate ids")
    casefold_duplicates(ids, "inventory.parsers ids")
    owned_sources = [source for parser in parsers for source in parser.source_files]
    if len(owned_sources) != len(set(owned_sources)):
        raise PolicyError("inventory.parsers maps a source file to more than one parser")
    casefold_duplicates(owned_sources, "inventory.parsers source_files")
    # Two parsers sharing a target, test name, harness, or corpus would let one
    # real harness certify several unfuzzed parsers.
    for key in ("cmake_target", "test_selector", "harness", "corpus_id", "entry_symbol"):
        values = [parser.target[key] for parser in parsers if parser.target is not None]
        if len(values) != len(set(values)):
            raise PolicyError(f"inventory.parsers reuses target.{key} across parsers")
        casefold_duplicates(values, f"inventory.parsers target.{key}")

    deferred = _parse_deferrals(root, document["deferred_candidates"], as_of=as_of)
    overlap = set(owned_sources) & {deferral.source_file for deferral in deferred}
    if overlap:
        raise PolicyError(f"inventory sources cannot also be deferred: {sorted(overlap)}")
    return Inventory(schema_version, scope, parsers, deferred)


def _candidate_reasons(relative: str, text: str) -> list[str]:
    reasons = [name for name, pattern in CONTENT_PATTERNS if pattern.search(text)]
    if SOURCE_NAME_PATTERN.search(Path(relative).stem):
        reasons.append("parser-like-filename")
    return sorted(set(reasons))


def _covering_subtree(relative: str, exclusions: tuple[SubtreeExclusion, ...]) -> str | None:
    for exclusion in exclusions:
        if relative.startswith(exclusion.path + "/"):
            return exclusion.path
    return None


def scan_source_tree(root: Path, scope: Scope, *, deadline: Deadline | None = None) -> ScanResult:
    """Walk every declared root without following aliases or skipping read errors.

    Excluded subtrees are still walked: an exclusion waives *classification*, it
    does not blind the scanner, so the number of candidates each waiver hides is
    measured rather than assumed.
    """
    root = canonical_root(root)
    deadline = deadline or Deadline(scope.limits.timeout_seconds, "source scan")
    file_count = 0
    directory_count = 0
    total_bytes = 0
    candidates: list[dict[str, Any]] = []
    hidden_by_subtree: dict[str, int] = {exclusion.path: 0 for exclusion in scope.excluded_subtrees}
    file_exclusions = {exclusion.path: exclusion for exclusion in scope.excluded_files}
    hidden_by_file: set[str] = set()

    for scan_root in scope.roots:
        _, absolute_root, _ = confined_path(root, scan_root, f"scan root {scan_root}", expect="dir", root_is_canonical=True)
        stack: list[tuple[Path, int]] = [(absolute_root, 0)]
        while stack:
            directory, depth = stack.pop()
            deadline.check()
            if depth > scope.limits.max_depth:
                raise PolicyError(f"source scan exceeded depth {scope.limits.max_depth}: {directory}")
            directory_count += 1
            if directory_count > scope.limits.max_directories:
                raise PolicyError(f"source scan exceeded {scope.limits.max_directories} directories")
            entries = bounded_scandir(directory, "source scan")
            child_directories: list[Path] = []
            for entry in entries:
                deadline.check()
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
                if stat.S_ISDIR(identity.st_mode):
                    child_directories.append(Path(entry.path))
                    continue
                if not stat.S_ISREG(identity.st_mode):
                    raise PolicyError(f"source scan refuses non-regular file: {relative}")
                if identity.st_nlink != 1:
                    raise PolicyError(f"source scan refuses hard link: {relative}")
                if Path(entry.name).suffix.lower() not in scope.extensions:
                    continue
                file_count += 1
                if file_count > scope.limits.max_files:
                    raise PolicyError(f"source scan exceeded {scope.limits.max_files} files")
                if identity.st_size > scope.limits.max_file_bytes:
                    raise PolicyError(f"source file exceeds {scope.limits.max_file_bytes} bytes: {relative}")
                total_bytes += identity.st_size
                if total_bytes > scope.limits.max_total_bytes:
                    raise PolicyError(f"source scan exceeded {scope.limits.max_total_bytes} aggregate bytes")
                payload = read_confined_file(
                    root, relative, f"source file {relative}", max_bytes=scope.limits.max_file_bytes, root_is_canonical=True
                )
                try:
                    text = payload.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise PolicyError(f"source file is not strict UTF-8: {relative}") from exc
                reasons = _candidate_reasons(relative, text)
                if not reasons:
                    continue
                covering = _covering_subtree(relative, scope.excluded_subtrees)
                if covering is not None:
                    hidden_by_subtree[covering] += 1
                    if hidden_by_subtree[covering] > MAX_EXCLUSION_HIDDEN_CANDIDATES:
                        raise PolicyError(f"exclusion {covering} hides more than {MAX_EXCLUSION_HIDDEN_CANDIDATES} candidates")
                    continue
                exclusion = file_exclusions.get(relative)
                if exclusion is not None:
                    import hashlib

                    if hashlib.sha256(payload).hexdigest() != exclusion.sha256:
                        raise PolicyError(f"excluded file {relative} no longer matches its reviewed digest")
                    hidden_by_file.add(relative)
                    continue
                candidates.append({"source_file": relative, "reasons": reasons})
            for child in reversed(child_directories):
                stack.append((child, depth + 1))

    return ScanResult(
        tuple(sorted(candidates, key=lambda item: item["source_file"])),
        hidden_by_subtree,
        frozenset(hidden_by_file),
        file_count,
    )


def build_inventory_report(
    root: Path,
    manifest_path: str = DEFAULT_INVENTORY,
    *,
    as_of: date | None = None,
    deadline: Deadline | None = None,
) -> dict[str, Any]:
    inventory = load_inventory(root, manifest_path, as_of=as_of)
    scan = scan_source_tree(root, inventory.scope, deadline=deadline)
    candidate_files = {item["source_file"] for item in scan.candidates}
    owned_files = {source for parser in inventory.parsers for source in parser.source_files}
    deferred_files = {deferral.source_file for deferral in inventory.deferred_candidates}

    unexpected = sorted(candidate_files - owned_files - deferred_files)
    if unexpected:
        raise PolicyError(f"unclassified parser candidates: {', '.join(unexpected[:30])}")
    stale_deferred = sorted(deferred_files - candidate_files)
    if stale_deferred:
        raise PolicyError(f"stale deferred parser candidates: {', '.join(stale_deferred[:30])}")
    # Human review is more authoritative than the regexes, so an inventoried
    # source the scanner cannot see is not an error - it is a measured blind spot
    # in the detector, and reporting it as a real number keeps the coverage claim
    # honest instead of implying the scanner found everything.
    detector_blind_spots = sorted(owned_files - candidate_files)

    for exclusion in inventory.scope.excluded_subtrees:
        observed = scan.hidden_by_subtree.get(exclusion.path, 0)
        if observed == 0:
            raise PolicyError(f"exclusion {exclusion.path} hides no parser candidates and is stale")
        if observed != exclusion.hidden_candidate_count:
            raise PolicyError(
                f"exclusion {exclusion.path} hides {observed} candidates but declares "
                f"{exclusion.hidden_candidate_count}; re-review it"
            )
    unmatched_file_exclusions = sorted(
        {exclusion.path for exclusion in inventory.scope.excluded_files} - set(scan.hidden_by_file)
    )
    if unmatched_file_exclusions:
        raise PolicyError(f"stale excluded_files entries: {', '.join(unmatched_file_exclusions)}")

    return {
        "schema_version": inventory.schema_version,
        "policy_complete": not inventory.deferred_candidates
        and all(parser.status == "fuzzed" for parser in inventory.parsers),
        "scan_roots": list(inventory.scope.roots),
        "scanned_file_count": scan.scanned_files,
        "excluded_subtrees": [
            {
                "path": exclusion.path,
                "ticket": exclusion.ticket,
                "owner": exclusion.owner,
                "expires": exclusion.expires.isoformat(),
                "hidden_candidate_count": exclusion.hidden_candidate_count,
            }
            for exclusion in inventory.scope.excluded_subtrees
        ],
        "excluded_files": [
            {
                "path": exclusion.path,
                "ticket": exclusion.ticket,
                "owner": exclusion.owner,
                "expires": exclusion.expires.isoformat(),
            }
            for exclusion in inventory.scope.excluded_files
        ],
        "parser_count": len(inventory.parsers),
        "fuzzed_count": sum(parser.status == "fuzzed" for parser in inventory.parsers),
        "blocked_count": sum(parser.status == "blocked" for parser in inventory.parsers),
        "candidate_count": len(scan.candidates),
        "deferred_candidate_count": len(inventory.deferred_candidates),
        "unclassified_candidate_count": len(unexpected),
        "detector_blind_spot_count": len(detector_blind_spots),
        "detector_blind_spots": detector_blind_spots,
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
        "candidates": [dict(candidate) for candidate in scan.candidates],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", default=".")
    parser.add_argument("--manifest", default=DEFAULT_INVENTORY)
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
        report = build_inventory_report(Path(args.source_root), args.manifest, as_of=as_of)
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
