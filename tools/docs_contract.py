#!/usr/bin/env python3
"""Deterministic first-party documentation generation and validation."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import posixpath
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = REPO_ROOT / "docs" / "generated-docs-manifest.json"
MAX_SOURCE_FILES = 6000
MAX_SOURCE_BYTES = 512 * 1024 * 1024
MAX_SOURCE_FILE_BYTES = 8 * 1024 * 1024
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
HEADER_GUARD_SUFFIXES = ("_H", "_HPP", "_HH", "_HXX", "_GUARD", "_INCLUDED")
TICK = chr(96)


class ContractError(RuntimeError):
    pass


@dataclass(frozen=True, order=True)
class Symbol:
    path: str
    line: int
    kind: str
    name: str
    brief: str = ""

    def tsv_row(self) -> tuple[str, str, str, str, str]:
        clean = self.brief.replace("\t", " ").replace("\r", " ").replace("\n", " ")
        return self.kind, self.name, self.path, str(self.line), clean


def atomic_write_bytes(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, path)
    except BaseException:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


def atomic_write_text(path: Path, payload: str) -> None:
    atomic_write_bytes(path, payload.encode("utf-8"))


def safe_relative(raw: str) -> PurePosixPath:
    path = PurePosixPath(raw)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise ContractError(f"unsafe repository-relative path: {raw!r}")
    if "\\" in raw or "\x00" in raw:
        raise ContractError(f"non-canonical repository-relative path: {raw!r}")
    return path


def load_contract() -> dict:
    try:
        contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot load {CONTRACT_PATH}: {exc}") from exc
    if contract.get("schemaVersion") != 1:
        raise ContractError("generated-docs manifest schemaVersion must be 1")
    source = contract.get("sourceContract")
    if not isinstance(source, dict):
        raise ContractError("sourceContract must be an object")
    for key in ("includeRoots", "extensions", "headerExtensions", "excludePrefixes"):
        values = source.get(key)
        if not isinstance(values, list) or not values or not all(isinstance(value, str) and value for value in values):
            raise ContractError(f"sourceContract.{key} must be a non-empty string array")
        if len(values) != len(set(values)):
            raise ContractError(f"sourceContract.{key} contains duplicates")
    generators = contract.get("generators")
    if not isinstance(generators, list) or not generators:
        raise ContractError("generators must be a non-empty array")
    ids = [entry.get("id") for entry in generators if isinstance(entry, dict)]
    scripts = [entry.get("script") for entry in generators if isinstance(entry, dict)]
    if len(ids) != len(generators) or any(not isinstance(value, str) or not value for value in ids):
        raise ContractError("each generator must have a non-empty string id")
    if len(ids) != len(set(ids)) or len(scripts) != len(set(scripts)):
        raise ContractError("generator ids and scripts must each be unique")
    return contract


def tracked_paths() -> set[str] | None:
    external = os.environ.get("SPARK_DOC_TRACKED_PATHS")
    if external:
        try:
            raw = Path(external).read_bytes()
        except OSError as exc:
            raise ContractError(f"cannot read tracked-path manifest: {exc}") from exc
        result: set[str] = set()
        for item in raw.split(b"\0"):
            if not item:
                continue
            try:
                value = item.decode("utf-8")
            except UnicodeDecodeError as exc:
                raise ContractError("tracked-path manifest has a non-UTF-8 path") from exc
            result.add(safe_relative(value).as_posix())
        return result
    try:
        process = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "ls-files", "-z", "--cached"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    result: set[str] = set()
    for item in process.stdout.split(b"\0"):
        if item:
            result.add(safe_relative(item.decode("utf-8")).as_posix())
    return result


def source_inventory() -> list[PurePosixPath]:
    source = load_contract()["sourceContract"]
    roots = tuple(safe_relative(value) for value in source["includeRoots"])
    extensions = {value.lower() for value in source["extensions"]}
    excluded = tuple(safe_relative(value.rstrip("/")).as_posix() + "/" for value in source["excludePrefixes"])
    tracked = tracked_paths()
    candidates: list[str] = []
    if tracked is not None:
        candidates.extend(sorted(tracked))
    else:
        for root in roots:
            full = REPO_ROOT.joinpath(*root.parts)
            if full.is_symlink():
                raise ContractError(f"source root is a symlink or reparse point: {root.as_posix()}")
            if not full.exists():
                continue
            for directory, dirnames, filenames in os.walk(full, followlinks=False):
                base = Path(directory)
                for name in dirnames:
                    if (base / name).is_symlink():
                        raise ContractError(f"source directory is a symlink or reparse point: {base / name}")
                candidates.extend((base / name).relative_to(REPO_ROOT).as_posix() for name in filenames)

    paths: set[PurePosixPath] = set()
    for raw in candidates:
        rel = safe_relative(raw)
        posix = rel.as_posix()
        if not any(rel == root or root in rel.parents for root in roots):
            continue
        if any(posix.startswith(prefix) for prefix in excluded):
            continue
        if rel.suffix.lower() not in extensions:
            continue
        full = REPO_ROOT.joinpath(*rel.parts)
        if full.is_symlink() or not full.is_file():
            raise ContractError(f"tracked source is missing or unsafe: {posix}")
        paths.add(rel)

    ordered = sorted(paths, key=lambda value: value.as_posix())
    if not ordered or len(ordered) > MAX_SOURCE_FILES:
        raise ContractError(f"source inventory size {len(ordered)} is outside 1..{MAX_SOURCE_FILES}")
    total = 0
    for rel in ordered:
        size = REPO_ROOT.joinpath(*rel.parts).stat().st_size
        if size > MAX_SOURCE_FILE_BYTES:
            raise ContractError(f"source file exceeds byte limit: {rel.as_posix()}")
        total += size
        if total > MAX_SOURCE_BYTES:
            raise ContractError("source inventory exceeds total-byte limit")
    return ordered


def read_source(rel: PurePosixPath) -> str:
    try:
        return REPO_ROOT.joinpath(*rel.parts).read_text(encoding="utf-8-sig")
    except (OSError, UnicodeDecodeError) as exc:
        raise ContractError(f"cannot read UTF-8 source {rel.as_posix()}: {exc}") from exc


def strip_cpp_comments(text: str) -> str:
    out: list[str] = []
    state = "normal"
    index = 0
    while index < len(text):
        ch = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if state == "normal":
            if ch == "/" and nxt == "/":
                out.extend((" ", " "))
                index += 2
                state = "line"
                continue
            if ch == "/" and nxt == "*":
                out.extend((" ", " "))
                index += 2
                state = "block"
                continue
            if ch == '"':
                out.append(" ")
                state = "string"
            elif ch == "'":
                out.append(" ")
                state = "char"
            else:
                out.append(ch)
            index += 1
            continue
        if state == "line":
            out.append("\n" if ch == "\n" else " ")
            if ch == "\n":
                state = "normal"
            index += 1
            continue
        if state == "block":
            if ch == "*" and nxt == "/":
                out.extend((" ", " "))
                index += 2
                state = "normal"
                continue
            out.append("\n" if ch == "\n" else " ")
            index += 1
            continue
        quote = '"' if state == "string" else "'"
        if ch == "\\" and nxt:
            out.extend((" ", "\n" if nxt == "\n" else " "))
            index += 2
        else:
            out.append("\n" if ch == "\n" else " ")
            if ch == quote:
                state = "normal"
            index += 1
    if state == "block":
        raise ContractError("unterminated block comment in first-party source")
    return "".join(out)


def brief_for_line(lines: Sequence[str], line_index: int) -> str:
    window = "\n".join(lines[max(0, line_index - 8):line_index + 1])
    matches = list(re.finditer(r"@brief\s+([^\r\n*]+)", window))
    return matches[-1].group(1).strip() if matches else ""


def class_name(fragment: str) -> str | None:
    prefix = re.split(r"[:{;]", fragment, maxsplit=1)[0]
    prefix = re.sub(r"\bfinal\b", " ", prefix)
    prefix = re.sub(r"\b(?:alignas|__declspec)\s*\([^)]*\)", " ", prefix)
    prefix = re.sub(r"\[\[[^\]]+\]\]", " ", prefix)
    names = IDENT_RE.findall(prefix)
    return names[-1] if names else None


def extract_symbols(rel: PurePosixPath, text: str) -> list[Symbol]:
    stripped = strip_cpp_comments(text)
    original = text.splitlines()
    symbols: list[Symbol] = []
    for line_no, line in enumerate(stripped.splitlines(), start=1):
        brief = brief_for_line(original, line_no - 1)
        macro = re.match(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)", line)
        if macro:
            name = macro.group(1)
            if not name.endswith(HEADER_GUARD_SUFFIXES):
                symbols.append(Symbol(rel.as_posix(), line_no, "macro", name, brief))
            continue
        declaration = re.match(r"^\s*(class|struct)\s+(.+)", line)
        if declaration:
            name = class_name(declaration.group(2))
            if name and name not in {"final", "public", "private", "protected"}:
                symbols.append(Symbol(rel.as_posix(), line_no, declaration.group(1), name, brief))
            continue
        enum = re.match(r"^\s*enum(?:\s+class)?\s+([A-Za-z_][A-Za-z0-9_]*)", line)
        if enum:
            symbols.append(Symbol(rel.as_posix(), line_no, "enum", enum.group(1), brief))
            continue
        using = re.match(r"^\s*using\s+([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        if using:
            symbols.append(Symbol(rel.as_posix(), line_no, "alias", using.group(1), brief))
            continue
        typedef = re.match(r"^\s*typedef\b.+\b([A-Za-z_][A-Za-z0-9_]*)\s*;", line)
        if typedef:
            symbols.append(Symbol(rel.as_posix(), line_no, "alias", typedef.group(1), brief))
            continue
        method = re.match(
            r"^\s*(?:template\s*<[^>]+>\s*)?(?:[A-Za-z_][A-Za-z0-9_:<>,*&\s]*\s+)"
            r"([A-Za-z_][A-Za-z0-9_:]*::[A-Za-z_~][A-Za-z0-9_]*)\s*\(",
            line,
        )
        if method:
            symbols.append(Symbol(rel.as_posix(), line_no, "method", method.group(1), brief))
            continue
        function = re.match(
            r"^\s*(?:template\s*<[^>]+>\s*)?(?:static\s+|inline\s+|constexpr\s+|virtual\s+|extern\s+)*"
            r"[A-Za-z_][A-Za-z0-9_:<>,*&\s]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(",
            line,
        )
        if function and function.group(1) not in {
            "if", "for", "while", "switch", "return", "sizeof", "alignof",
            "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast",
        }:
            symbols.append(Symbol(rel.as_posix(), line_no, "function", function.group(1), brief))
    unique = {(value.kind, value.name, value.path, value.line): value for value in symbols}
    return sorted(unique.values())


def scan_sources() -> tuple[list[PurePosixPath], list[Symbol], dict[str, str]]:
    paths = source_inventory()
    texts: dict[str, str] = {}
    symbols: list[Symbol] = []
    for rel in paths:
        text = read_source(rel)
        texts[rel.as_posix()] = text
        symbols.extend(extract_symbols(rel, text))
    return paths, sorted(symbols), texts


def source_identity() -> tuple[str, str]:
    sha = os.environ.get("SPARKENGINE_DOC_SOURCE_SHA", "")
    committed_at = os.environ.get("SPARKENGINE_DOC_SOURCE_COMMITTED_AT", "")
    if not sha:
        try:
            sha = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, timeout=15,
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise ContractError("SPARKENGINE_DOC_SOURCE_SHA is required without Git") from exc
    if not SHA_RE.fullmatch(sha):
        raise ContractError("source SHA must be an exact 40-character lowercase Git SHA")
    if not committed_at:
        try:
            committed_at = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "show", "-s", "--format=%cI", sha],
                check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, timeout=15,
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise ContractError("SPARKENGINE_DOC_SOURCE_COMMITTED_AT is required without Git") from exc
    try:
        datetime.fromisoformat(committed_at.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ContractError("source committed-at value must be RFC 3339") from exc
    return sha, committed_at


def relative_link(from_file: PurePosixPath, target: PurePosixPath) -> str:
    return posixpath.relpath(target.as_posix(), start=from_file.parent.as_posix())


def write_api_manifest(output: Path, source_sha: str) -> None:
    files: list[dict[str, object]] = []
    total = 0
    for path in sorted(output.rglob("*")):
        if path.is_symlink():
            raise ContractError(f"generated API tree contains symlink: {path}")
        if not path.is_file() or path.name == ".manifest.json":
            continue
        payload = path.read_bytes()
        total += len(payload)
        if len(files) >= 2500 or total > 128 * 1024 * 1024:
            raise ContractError("generated API manifest exceeds resource bounds")
        files.append({
            "path": path.relative_to(output).as_posix(),
            "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        })
    manifest = {
        "schemaVersion": 1,
        "sourceCommit": source_sha,
        "files": files,
        "fileCount": len(files),
        "totalBytes": total,
    }
    atomic_write_text(output / ".manifest.json", json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def validate_api_manifest(output: Path, expected_sha: str | None = None) -> list[str]:
    errors: list[str] = []
    try:
        manifest = json.loads((output / ".manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot read generated API manifest: {exc}"]
    if manifest.get("schemaVersion") != 1:
        errors.append("generated API manifest schemaVersion must be 1")
    source_sha = manifest.get("sourceCommit")
    if not isinstance(source_sha, str) or not SHA_RE.fullmatch(source_sha):
        errors.append("generated API manifest sourceCommit must be an exact SHA")
    elif expected_sha and source_sha != expected_sha:
        errors.append(f"generated API sourceCommit {source_sha} does not match {expected_sha}")
    rows = manifest.get("files")
    if not isinstance(rows, list) or not rows:
        return errors + ["generated API manifest files must be a non-empty array"]
    seen: set[str] = set()
    total = 0
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("path"), str):
            errors.append("generated API manifest row is malformed")
            continue
        try:
            rel = safe_relative(row["path"])
        except ContractError as exc:
            errors.append(str(exc))
            continue
        canonical = rel.as_posix()
        if canonical in seen:
            errors.append(f"duplicate API manifest path: {canonical}")
            continue
        seen.add(canonical)
        full = output.joinpath(*rel.parts)
        if full.is_symlink() or not full.is_file():
            errors.append(f"API manifest target is missing or unsafe: {canonical}")
            continue
        payload = full.read_bytes()
        total += len(payload)
        if row.get("bytes") != len(payload):
            errors.append(f"API manifest byte mismatch: {canonical}")
        if row.get("sha256") != hashlib.sha256(payload).hexdigest():
            errors.append(f"API manifest digest mismatch: {canonical}")
    actual = {
        path.relative_to(output).as_posix()
        for path in output.rglob("*")
        if path.is_file() and path.name != ".manifest.json"
    }
    if actual != seen:
        errors.append("API manifest file set does not exactly match generated tree")
    if manifest.get("fileCount") != len(seen) or manifest.get("totalBytes") != total:
        errors.append("API manifest aggregate counts are inconsistent")
    return errors


def generate_api(output: Path) -> None:
    sha, committed_at = source_identity()
    paths, symbols, _ = scan_sources()
    header_extensions = {value.lower() for value in load_contract()["sourceContract"]["headerExtensions"]}
    if output.exists():
        if output.is_symlink():
            raise ContractError("refusing symlink API output")
        for child in sorted(output.rglob("*"), key=lambda value: len(value.parts), reverse=True):
            if child.is_symlink():
                raise ContractError(f"refusing symlink API entry: {child}")
            if child.is_file():
                child.unlink()
            elif child.is_dir():
                child.rmdir()
    output.mkdir(parents=True, exist_ok=True)
    headers = [rel for rel in paths if rel.suffix.lower() in header_extensions]
    by_path: dict[str, list[Symbol]] = {}
    for symbol in symbols:
        by_path.setdefault(symbol.path, []).append(symbol)
    modules: dict[str, int] = {}
    for rel in headers:
        modules[rel.parts[0]] = modules.get(rel.parts[0], 0) + 1
        output_rel = PurePosixPath("docs/api").joinpath(rel).with_suffix(".md")
        source_link = relative_link(output_rel, rel)
        api_link = relative_link(output_rel, PurePosixPath("docs/api/README.md"))
        lines = [
            f"# {TICK}{rel.as_posix()}{TICK}",
            "",
            f"[Back to API Reference]({api_link}) - [View source]({source_link})",
            "",
            "## Declarations",
            "",
        ]
        selected = by_path.get(rel.as_posix(), [])
        if selected:
            lines.extend(["| Symbol | Kind | Source | Brief |", "|--------|------|--------|-------|"])
            for value in selected:
                brief = value.brief.replace("|", "\\|")
                lines.append(
                    f"| {TICK}{value.name}{TICK} | {value.kind} | "
                    f"[L{value.line}]({source_link}#L{value.line}) | {brief} |"
                )
        else:
            lines.append("_No indexed declarations in this header._")
        lines.append("")
        atomic_write_text(output.joinpath(*rel.parts).with_suffix(".md"), "\n".join(lines))

    readme = [
        "# SparkEngine API Reference",
        "",
        f"> Deterministically generated from exact source commit {TICK}{sha}{TICK}",
        f"> committed at {TICK}{committed_at}{TICK}.",
        "",
        f"**Coverage:** {len(paths)} first-party source files, {len(headers)} headers, {len(symbols)} symbol rows.",
        "",
        "## Modules",
        "",
        "| Module | Header pages |",
        "|--------|-------------:|",
    ]
    readme.extend(f"| {TICK}{module}{TICK} | {count} |" for module, count in sorted(modules.items()))
    readme.append("")
    atomic_write_text(output / "README.md", "\n".join(readme))
    rows = ["\t".join(value.tsv_row()) for value in symbols]
    atomic_write_text(output / ".symbols.tsv", ("\n".join(rows) + "\n") if rows else "")
    for filename, predicate, title in (
        ("ComponentIndex.md", lambda value: "Component" in value.name, "Component Index"),
        ("SystemIndex.md", lambda value: "System" in value.name, "System Index"),
    ):
        selected = [value for value in symbols if predicate(value)]
        lines = [f"# {title}", "", "| Symbol | Kind | Source |", "|--------|------|--------|"]
        lines.extend(
            f"| {TICK}{value.name}{TICK} | {value.kind} | "
            f"[{PurePosixPath(value.path).name}:L{value.line}](../../{value.path}#L{value.line}) |"
            for value in selected
        )
        lines.append("")
        atomic_write_text(output / filename, "\n".join(lines))
    metadata = {
        "schemaVersion": 1,
        "sourceCommit": sha,
        "sourceCommittedAt": committed_at,
        "sourcesScanned": len(paths),
        "headersScanned": len(headers),
        "cppFilesScanned": sum(rel.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".mm"} for rel in paths),
        "markdownPages": len(headers) + 3,
        "symbolRecords": len(symbols),
    }
    atomic_write_text(output / ".generation.json", json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    write_api_manifest(output, sha)


def load_symbols(path: Path) -> list[Symbol]:
    rows: list[Symbol] = []
    try:
        with path.open(encoding="utf-8", newline="") as stream:
            for line_no, row in enumerate(csv.reader(stream, delimiter="\t"), start=1):
                if len(row) != 5:
                    raise ContractError(f"{path}:{line_no}: expected five TSV fields")
                kind, name, raw_path, raw_line, brief = row
                safe_relative(raw_path)
                try:
                    source_line = int(raw_line)
                except ValueError as exc:
                    raise ContractError(f"{path}:{line_no}: invalid source line") from exc
                if source_line <= 0:
                    raise ContractError(f"{path}:{line_no}: source line must be positive")
                rows.append(Symbol(raw_path, source_line, kind, name, brief))
    except OSError as exc:
        raise ContractError(f"cannot read symbol TSV: {exc}") from exc
    if len(rows) != len(set(rows)):
        raise ContractError("symbol TSV contains duplicate rows")
    return sorted(rows)


INDEX_SPECS = (
    ("Symbol-Index.md", "Symbol Index", "Every indexed first-party declaration.", None),
    ("Function-Index.md", "Function Index", "Free functions and out-of-line methods.", {"function", "method"}),
    ("Class-Index.md", "Class and Struct Index", "Every declared class and struct.", {"class", "struct"}),
    ("Enum-Index.md", "Enum Index", "Every declared enum.", {"enum"}),
    ("Macro-Index.md", "Macro and Alias Index", "Every indexed macro and type alias.", {"macro", "alias"}),
)


def render_index(title: str, description: str, symbols: Iterable[Symbol]) -> str:
    selected = sorted(symbols, key=lambda value: (value.name.casefold(), value.path, value.line, value.kind))
    lines = [
        f"# {title}",
        "",
        f"> {description}",
        ">",
        f"> **Total:** {len(selected)} symbols. Auto-generated by {TICK}docs/generate-symbol-index.sh{TICK}.",
        "",
        "| Symbol | Kind | Module | Source | Brief |",
        "|--------|------|--------|--------|-------|",
    ]
    for value in selected:
        module = PurePosixPath(value.path).parts[0]
        brief = value.brief.replace("|", "\\|")
        lines.append(
            f"| {TICK}{value.name}{TICK} | {value.kind} | {module} | "
            f"[{PurePosixPath(value.path).name}:L{value.line}](../../{value.path}#L{value.line}) | {brief} |"
        )
    lines.append("")
    return "\n".join(lines)


def generate_indexes(api_dir: Path, output_root: Path) -> None:
    actual = load_symbols(api_dir / ".symbols.tsv")
    _, expected, _ = scan_sources()
    if actual != expected:
        raise ContractError("symbol TSV is not an exact path, line, kind, name, and brief projection of source")
    reference = output_root / "reference"
    for filename, title, description, kinds in INDEX_SPECS:
        selected = actual if kinds is None else [value for value in actual if value.kind in kinds]
        atomic_write_text(reference / filename, render_index(title, description, selected))


def generate_file_tree(output: Path) -> None:
    paths, _, texts = scan_sources()
    total_loc = 0
    lines = [
        "# File Tree",
        "",
        "> Every tracked first-party native source file declared by the generated-docs manifest.",
        "",
        f"Auto-generated by {TICK}docs/generate-file-tree.sh{TICK}.",
        "",
        "## Hierarchy",
        "",
    ]
    current = None
    for rel in paths:
        directory = rel.parent.as_posix()
        if directory != current:
            lines.extend([f"### {TICK}{directory}/{TICK}", ""])
            current = directory
        loc = len(texts[rel.as_posix()].splitlines())
        total_loc += loc
        lines.append(f"- [{TICK}{rel.name}{TICK}](../../{rel.as_posix()}) - {loc} LOC")
    lines.extend([
        "",
        "---",
        "",
        "## Totals",
        "",
        "| Metric | Count |",
        "|--------|------:|",
        f"| Source files scanned | {len(paths)} |",
        f"| Total logical lines | {total_loc} |",
        "",
    ])
    atomic_write_text(output, "\n".join(lines))


def generate_hierarchy(output: Path) -> None:
    paths, _, texts = scan_sources()
    edges: set[tuple[str, str, str, str, int]] = set()
    for rel in paths:
        for line_no, line in enumerate(strip_cpp_comments(texts[rel.as_posix()]).splitlines(), start=1):
            declaration = re.match(
                r"^\s*(?:class|struct)\s+(.+?)\s*:\s*(?:public|protected|private)?\s*([A-Za-z_][A-Za-z0-9_:]*)",
                line,
            )
            if declaration:
                derived = class_name(declaration.group(1))
                if derived:
                    edges.add((rel.parts[0], derived, declaration.group(2).split("::")[-1], rel.as_posix(), line_no))
    lines = [
        "# Class Hierarchy",
        "",
        f"> **Total inheritance edges:** {len(edges)}. Derived from the exact first-party source contract.",
        "",
        f"Auto-generated by {TICK}docs/generate-class-hierarchy.sh{TICK}.",
        "",
    ]
    current = None
    for module, derived, base, _path, _line in sorted(edges):
        if module != current:
            if current is not None:
                lines.extend(["~~~", ""])
            current = module
            lines.extend([f"## {TICK}{module}{TICK}", "", "~~~mermaid", "classDiagram"])
        lines.append(f"    {base} <|-- {derived}")
    if current is not None:
        lines.extend(["~~~", ""])
    atomic_write_text(output, "\n".join(lines))


FILE_TREE_ROW = re.compile(r"^- \[[^]]+\]\(\.\./\.\./([^)]+)\) - ([0-9]+) LOC$")


def validate_source_contract(api_dir: Path, wiki_root: Path) -> list[str]:
    errors: list[str] = []
    paths, expected_symbols, texts = scan_sources()
    try:
        actual_symbols = load_symbols(api_dir / ".symbols.tsv")
    except ContractError as exc:
        errors.append(str(exc))
        actual_symbols = []
    if actual_symbols != expected_symbols:
        errors.append(
            f"symbol TSV differs from source: {len(set(expected_symbols) - set(actual_symbols))} missing, "
            f"{len(set(actual_symbols) - set(expected_symbols))} extra"
        )
    reference = wiki_root / "reference"
    for filename, title, description, kinds in INDEX_SPECS:
        selected = expected_symbols if kinds is None else [value for value in expected_symbols if value.kind in kinds]
        expected = render_index(title, description, selected).encode("utf-8")
        try:
            actual = (reference / filename).read_bytes()
        except OSError as exc:
            errors.append(f"cannot read {reference / filename}: {exc}")
            continue
        if actual != expected:
            errors.append(f"{filename} is not an exact source-derived index")

    rows: dict[str, int] = {}
    duplicates: set[str] = set()
    tree_path = reference / "File-Tree.md"
    try:
        for line in tree_path.read_text(encoding="utf-8").splitlines():
            match = FILE_TREE_ROW.match(line)
            if match:
                if match.group(1) in rows:
                    duplicates.add(match.group(1))
                rows[match.group(1)] = int(match.group(2))
    except OSError as exc:
        errors.append(f"cannot read File-Tree.md: {exc}")
    expected_rows = {rel.as_posix(): len(texts[rel.as_posix()].splitlines()) for rel in paths}
    missing = set(expected_rows) - set(rows)
    deleted = set(rows) - set(expected_rows)
    wrong = {path for path in set(rows) & set(expected_rows) if rows[path] != expected_rows[path]}
    if duplicates or missing or deleted or wrong:
        errors.append(
            f"File-Tree is not bidirectionally exact: {len(missing)} missing, "
            f"{len(deleted)} deleted, {len(wrong)} wrong LOC, {len(duplicates)} duplicate"
        )
    try:
        metadata = json.loads((api_dir / ".generation.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"cannot read API generation metadata: {exc}")
    else:
        header_extensions = {value.lower() for value in load_contract()["sourceContract"]["headerExtensions"]}
        expected_counts = {
            "sourcesScanned": len(paths),
            "headersScanned": sum(rel.suffix.lower() in header_extensions for rel in paths),
            "symbolRecords": len(expected_symbols),
        }
        for key, expected in expected_counts.items():
            if metadata.get(key) != expected:
                errors.append(f"API generation metadata {key} is incomplete or inconsistent")
    errors.extend(validate_api_manifest(api_dir))
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    api = subparsers.add_parser("generate-api")
    api.add_argument("--output", type=Path, required=True)
    indexes = subparsers.add_parser("generate-indexes")
    indexes.add_argument("--api-dir", type=Path, required=True)
    indexes.add_argument("--output-root", type=Path, required=True)
    tree = subparsers.add_parser("generate-file-tree")
    tree.add_argument("--output", type=Path, required=True)
    hierarchy = subparsers.add_parser("generate-hierarchy")
    hierarchy.add_argument("--output", type=Path, required=True)
    validate = subparsers.add_parser("validate")
    validate.add_argument("--api-dir", type=Path, required=True)
    validate.add_argument("--wiki-root", type=Path, required=True)
    api_manifest = subparsers.add_parser("validate-api-manifest")
    api_manifest.add_argument("--api-dir", type=Path, required=True)
    api_manifest.add_argument("--source-sha")
    args = parser.parse_args(argv)
    try:
        if args.command == "generate-api":
            generate_api(args.output.resolve())
        elif args.command == "generate-indexes":
            generate_indexes(args.api_dir.resolve(), args.output_root.resolve())
        elif args.command == "generate-file-tree":
            generate_file_tree(args.output.resolve())
        elif args.command == "generate-hierarchy":
            generate_hierarchy(args.output.resolve())
        elif args.command == "validate":
            errors = validate_source_contract(args.api_dir.resolve(), args.wiki_root.resolve())
            if errors:
                print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
                return 1
        elif args.command == "validate-api-manifest":
            errors = validate_api_manifest(args.api_dir.resolve(), args.source_sha)
            if errors:
                print("\n".join(f"ERROR: {error}" for error in errors), file=sys.stderr)
                return 1
    except ContractError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
