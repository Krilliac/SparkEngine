#!/usr/bin/env python3
"""Fail-closed check that every asset a game module's source names exists (MOD-360).

Every C/C++ string literal in ``GameModules/<Module>/Source`` is scanned, outside
comments and preprocessor lines, with the same tokenizer the package-profile
closure uses (``tools/asset-integrity/package_closure.py``). A literal is an asset
reference when it starts with ``Assets/`` or when its first path component names
a top-level directory of ``Assets/`` (``Audio/Music/RTS/x.ogg``); both resolve
below the repository ``Assets/`` root and must exist with their exact case.

A literal that starts with ``Assets/`` but does not name a file on its own
(``"Assets/OpenWorld/"``, ``"Assets/ui/icon_%s.png"``) is not a complete path: the
runtime builds the real path by concatenation or formatting, so no static check
can prove it. In an enforced module that is a failure, not a skip.

Enforced modules also carry ``GameModules/<Module>/asset-references.json``, the
module's reference record (the shared ``Assets/`` files it names live outside the
module directory, so this is not a ``<Module>/Assets`` package manifest): every referenced path with its sha256, kind and the
``tools/asset-integrity/provenance.json`` rule that licenses it. The check
requires the record to list exactly the referenced set, each digest to match the
file on disk, and the repository integrity manifest ``Assets/assets.integrity.json``
to declare the same digest under the same provenance rule.

Only ``ENFORCED_MODULES`` fail the check. Every other module is reported so its
gaps stay visible, without claiming a gate for content it does not yet have.

Usage:
    python3 tools/check-module-asset-refs.py                        # all modules
    python3 tools/check-module-asset-refs.py --module SparkGameOpenWorld
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[1]
ASSET_ROOT_NAME = "Assets"
ASSET_PREFIX = "assets/"
MODULE_MANIFEST_RELATIVE = Path("asset-references.json")
MODULE_MANIFEST_VERSION = 1
MODULE_MANIFEST_KEYS = frozenset({"manifestVersion", "module", "description", "references"})
REFERENCE_KEYS = frozenset({"path", "sha256", "kind", "provenanceRule"})
INTEGRITY_MANIFEST_RELATIVE = Path("Assets/assets.integrity.json")
PROVENANCE_POLICY_RELATIVE = Path("tools/asset-integrity/provenance.json")
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
# A file-naming literal: path components of ordinary characters and a final component with a suffix.
FILE_LITERAL_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_ .()+-]*(?:/[A-Za-z0-9_][A-Za-z0-9_ .()+-]*)*\.[A-Za-z0-9]+\Z")
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_ASSET_BYTES = 256 * 1024 * 1024

# Modules whose references gate the check. Adding a module here is a promise that
# every asset its source names exists and is recorded in its module manifest.
ENFORCED_MODULES = frozenset({"SparkGameOpenWorld"})


def _load_closure_module() -> Any:
    """Import the package-closure tokenizer so both tools read C++ literals identically."""
    path = Path(__file__).resolve().parent / "asset-integrity" / "package_closure.py"
    spec = importlib.util.spec_from_file_location("spark_package_closure", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_CLOSURE = _load_closure_module()


@dataclass
class ModuleReport:
    """Everything one module's scan found."""

    name: str
    enforced: bool
    references: dict[str, list[str]] = field(default_factory=dict)
    problems: list[str] = field(default_factory=list)


def _exists_exact(repo_root: Path, relative: str) -> bool:
    """Return True when ``relative`` names a regular file whose every component matches on-disk case."""
    current = repo_root
    parts = relative.split("/")
    for index, part in enumerate(parts):
        if part in ("", ".", ".."):
            return False
        try:
            names = set(os.listdir(current))
        except OSError:
            return False
        if part not in names:
            return False
        current = current / part
        if current.is_symlink():
            return False
        if index < len(parts) - 1 and not current.is_dir():
            return False
    return current.is_file()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        remaining = MAX_ASSET_BYTES
        while chunk := stream.read(1 << 20):
            remaining -= len(chunk)
            if remaining < 0:
                raise OSError(f"{path} exceeds {MAX_ASSET_BYTES} bytes")
            digest.update(chunk)
    return digest.hexdigest()


def _read_json(path: Path) -> Any:
    size = path.stat().st_size
    if size > MAX_MANIFEST_BYTES:
        raise ValueError(f"{size} bytes exceeds the {MAX_MANIFEST_BYTES}-byte limit")
    return json.loads(path.read_text(encoding="utf-8"))


def _top_level_asset_directories(repo_root: Path) -> frozenset[str]:
    asset_root = repo_root / ASSET_ROOT_NAME
    if not asset_root.is_dir():
        return frozenset()
    return frozenset(entry.name for entry in asset_root.iterdir() if entry.is_dir())


def classify_literal(value: str, top_level: frozenset[str]) -> tuple[str, str] | None:
    """Classify one C++ literal.

    Returns ``("file", "Assets/<path>")`` for a literal that names an asset file,
    ``("prefix", value)`` for an ``Assets/`` literal that cannot name a file on its
    own (a directory prefix or a format string), and ``None`` for a literal that is not an asset path.
    """
    candidate = _CLOSURE.BACKSLASHES_RE.sub("/", value)
    while candidate.startswith("./"):
        candidate = candidate[2:]
    if candidate.casefold().startswith(ASSET_PREFIX):
        rest = candidate[len(ASSET_PREFIX):]
        if FILE_LITERAL_RE.match(rest) and not any(part in (".", "..") for part in rest.split("/")):
            return "file", f"{ASSET_ROOT_NAME}/{rest}"
        return "prefix", value
    reference = _CLOSURE.asset_reference(candidate, top_level)
    if reference is not None:
        return "file", f"{ASSET_ROOT_NAME}/{reference}"
    return None


def scan_module(repo_root: Path, module_dir: Path, top_level: frozenset[str]) -> ModuleReport:
    """Collect every asset reference in one module's sources and check each one exists."""
    report = ModuleReport(name=module_dir.name, enforced=module_dir.name in ENFORCED_MODULES)
    source_root = module_dir / "Source"
    sources = sorted(
        path for path in source_root.rglob("*")
        if path.suffix.lower() in _CLOSURE.SOURCE_SUFFIXES and path.is_file() and not path.is_symlink()
    ) if source_root.is_dir() else []
    for source in sources:
        location = source.relative_to(repo_root).as_posix()
        try:
            text = _CLOSURE._bounded_text(source, _CLOSURE.MAX_SOURCE_BYTES)
        except _CLOSURE.ClosureError as exc:
            report.problems.append(str(exc))
            continue
        for line, value in _CLOSURE.cpp_string_literals(text):
            classified = classify_literal(value, top_level)
            if classified is None:
                continue
            kind, path = classified
            where = f"{location}:{line}"
            if kind == "prefix":
                report.problems.append(
                    f"{where}: {value!r} is not a complete asset path; the path is composed at run time and "
                    "cannot be verified -- name each asset as a complete literal")
                continue
            report.references.setdefault(path, []).append(where)
    for path, sites in sorted(report.references.items()):
        if not _exists_exact(repo_root, path):
            report.problems.append(f"{sites[0]}: {path} does not exist (exact case)")
    return report


def _integrity_entries(repo_root: Path) -> tuple[str, dict[str, dict[str, Any]]]:
    document = _read_json(repo_root / INTEGRITY_MANIFEST_RELATIVE)
    if not isinstance(document, dict) or not isinstance(document.get("entries"), list) or \
            not isinstance(document.get("root"), str):
        raise ValueError("integrity manifest must contain root and entries")
    entries = {
        f"{document['root']}/{entry['path']}": entry for entry in document["entries"]
        if isinstance(entry, dict) and isinstance(entry.get("path"), str)
    }
    return document["root"], entries


def _provenance_rule_ids(repo_root: Path) -> set[str]:
    document = _read_json(repo_root / PROVENANCE_POLICY_RELATIVE)
    rules = document.get("rules") if isinstance(document, dict) else None
    if not isinstance(rules, list):
        raise ValueError("provenance policy must contain a rules list")
    return {rule["id"] for rule in rules if isinstance(rule, dict) and isinstance(rule.get("id"), str)}


def verify_module_manifest(repo_root: Path, module_dir: Path, report: ModuleReport) -> None:
    """Require the module's reference record to match its references, the files and the integrity manifest."""
    manifest_path = module_dir / MODULE_MANIFEST_RELATIVE
    location = manifest_path.relative_to(repo_root).as_posix()
    if not manifest_path.is_file():
        report.problems.append(f"{location}: missing; an enforced module must record its asset references")
        return
    try:
        document = _read_json(manifest_path)
        _, integrity = _integrity_entries(repo_root)
        rule_ids = _provenance_rule_ids(repo_root)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        report.problems.append(f"{location}: cannot verify: {exc}")
        return
    if not isinstance(document, dict) or set(document) != MODULE_MANIFEST_KEYS:
        report.problems.append(f"{location}: must contain exactly {sorted(MODULE_MANIFEST_KEYS)}")
        return
    if document["manifestVersion"] != MODULE_MANIFEST_VERSION or document["module"] != report.name:
        report.problems.append(
            f"{location}: manifestVersion must be {MODULE_MANIFEST_VERSION} and module must be {report.name!r}")
        return
    references = document["references"]
    if not isinstance(references, list):
        report.problems.append(f"{location}: references must be a list")
        return
    recorded: dict[str, dict[str, Any]] = {}
    for index, entry in enumerate(references):
        where = f"{location}: references[{index}]"
        if not isinstance(entry, dict) or set(entry) != REFERENCE_KEYS or \
                not all(isinstance(entry[key], str) and entry[key] for key in REFERENCE_KEYS):
            report.problems.append(f"{where} must contain exactly non-empty strings {sorted(REFERENCE_KEYS)}")
            continue
        path = entry["path"]
        if path in recorded:
            report.problems.append(f"{where}: duplicate path {path}")
            continue
        recorded[path] = entry
        if not SHA256_RE.match(entry["sha256"]):
            report.problems.append(f"{where}: sha256 must be 64 lowercase hex digits")
            continue
        if entry["provenanceRule"] not in rule_ids:
            report.problems.append(f"{where}: provenance rule {entry['provenanceRule']!r} is not defined in "
                                   f"{PROVENANCE_POLICY_RELATIVE.as_posix()}")
        if not _exists_exact(repo_root, path):
            report.problems.append(f"{where}: {path} does not exist (exact case)")
            continue
        actual = _sha256(repo_root / path)
        if actual != entry["sha256"]:
            report.problems.append(f"{where}: {path} sha256 is {actual}, manifest records {entry['sha256']}")
        declared = integrity.get(path)
        if declared is None:
            report.problems.append(f"{where}: {path} is not declared in {INTEGRITY_MANIFEST_RELATIVE.as_posix()}")
        elif declared.get("sha256") != actual:
            report.problems.append(f"{where}: {path} digest differs from {INTEGRITY_MANIFEST_RELATIVE.as_posix()}")
        elif not str(declared.get("provenance", "")).endswith(f"[{entry['provenanceRule']}]"):
            report.problems.append(
                f"{where}: {path} is attributed to a different provenance rule in "
                f"{INTEGRITY_MANIFEST_RELATIVE.as_posix()}")
    for path in sorted(set(report.references) - set(recorded)):
        report.problems.append(f"{location}: does not record referenced asset {path} "
                               f"(referenced at {report.references[path][0]})")
    for path in sorted(set(recorded) - set(report.references)):
        report.problems.append(f"{location}: records {path}, which no module source references")


def check(repo_root: Path, modules: list[str] | None) -> tuple[list[ModuleReport], list[str]]:
    """Scan the requested modules (all when ``modules`` is None); return reports and usage errors."""
    modules_root = repo_root / "GameModules"
    available = sorted(
        (path for path in modules_root.iterdir() if path.is_dir() and (path / "Source").is_dir()),
        key=lambda path: path.name.casefold(),
    ) if modules_root.is_dir() else []
    by_name = {path.name: path for path in available}
    errors = [f"unknown module: {name}" for name in (modules or []) if name not in by_name]
    selected = [by_name[name] for name in modules if name in by_name] if modules else available
    top_level = _top_level_asset_directories(repo_root)
    reports = []
    for module_dir in selected:
        report = scan_module(repo_root, module_dir, top_level)
        if report.enforced:
            verify_module_manifest(repo_root, module_dir, report)
        reports.append(report)
    return reports, errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--module", action="append", dest="modules", metavar="NAME",
                        help="Check only this module (repeatable); default checks every module")
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    reports, errors = check(args.repo_root.resolve(), args.modules)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    if errors:
        return 2

    failed = False
    for report in reports:
        mode = "enforced" if report.enforced else "report-only"
        status = "FAIL" if report.problems and report.enforced else ("WARN" if report.problems else "OK")
        print(f"{status}: {report.name} [{mode}] {len(report.references)} asset reference(s), "
              f"{len(report.problems)} problem(s)")
        stream = sys.stderr if report.enforced and report.problems else sys.stdout
        for problem in report.problems:
            print(f"  {problem}", file=stream)
        failed = failed or (report.enforced and bool(report.problems))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
