#!/usr/bin/env python3
"""Fail-closed validation for documentation links, anchors, and routes."""

from __future__ import annotations

import argparse
from bisect import bisect_right
import hashlib
import html
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Sequence
from urllib.parse import unquote, urlsplit

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

from common import SITE_CONTRACT_ROOT, github_heading_slug, load_json
from docs_contract import validate_api_manifest

MAX_DOCUMENTS = 1500
MAX_DOCUMENT_BYTES = 8 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_LINKS = 150000
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
FENCE = chr(96) * 3
INLINE_CODE_RE = re.compile(
    re.escape(chr(96)) + r"[^" + re.escape(chr(96)) + r"]*" + re.escape(chr(96))
)
UNSAFE_SCHEMES = {"data", "file", "javascript", "vbscript"}
EXTERNAL_SCHEMES = {"http", "https", "mailto", "tel"}
HTML_LINK_RE = re.compile(
    r"<(?:a|area|link|img|source)\b[^>]*?\b(?:href|src)\s*=\s*([\"'])(.*?)\1",
    re.IGNORECASE,
)
INLINE_LINK_RE = re.compile(
    r"!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*(?:<([^>]+)>|([^\s)]+))(?:\s+[\"'][^\"']*[\"'])?\s*\)"
)
REFERENCE_DEF_RE = re.compile(r"^\s{0,3}\[([^\]]+)\]:\s*(?:<([^>]+)>|(\S+))(?:\s+.*)?$", re.MULTILINE)
REFERENCE_USE_RE = re.compile(r"!?\[([^\]]+)\]\[([^\]]*)\]")
AUTOLINK_RE = re.compile(r"<([^<>\s]+)>")
EXPLICIT_ANCHOR_RE = re.compile(r"<(?:a|[^>]+)\b(?:id|name)\s*=\s*[\"']([^\"']+)[\"']", re.IGNORECASE)
SOURCE_LINE_RE = re.compile(r"^L([1-9][0-9]*)(?:-L([1-9][0-9]*))?$")


class LinkContractError(RuntimeError):
    pass


@dataclass(frozen=True)
class Document:
    path: Path
    logical: str


def is_reparse(path: Path) -> bool:
    try:
        if path.is_symlink():
            return True
        junction = getattr(path, "is_junction", None)
        if callable(junction) and junction():
            return True
        return bool(
            getattr(path.lstat(), "st_file_attributes", 0)
            & FILE_ATTRIBUTE_REPARSE_POINT
        )
    except OSError:
        return False


def path_crosses_reparse(path: Path, root: Path) -> bool:
    try:
        relative = path.relative_to(root)
    except ValueError:
        return True
    current = root
    if is_reparse(current):
        return True
    for part in relative.parts:
        current /= part
        if is_reparse(current):
            return True
    return False


def safe_relative(raw: str) -> PurePosixPath:
    path = PurePosixPath(raw)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise LinkContractError(f"unsafe repository-relative path: {raw!r}")
    if "\\" in raw or "\x00" in raw:
        raise LinkContractError(f"non-canonical repository-relative path: {raw!r}")
    return path


def excluded(logical: str, prefixes: tuple[str, ...], paths: set[str]) -> bool:
    return logical in paths or any(logical.startswith(prefix) for prefix in prefixes)


def walk_markdown(root: Path, logical_root: PurePosixPath) -> Iterable[Document]:
    if is_reparse(root) or not root.is_dir():
        raise LinkContractError(f"Markdown root is missing or unsafe: {logical_root.as_posix()}")
    stack = [(root, logical_root)]
    while stack:
        directory, logical = stack.pop()
        try:
            entries = sorted(os.scandir(directory), key=lambda item: item.name.casefold(), reverse=True)
        except OSError as exc:
            raise LinkContractError(f"cannot enumerate {directory}: {exc}") from exc
        folded: dict[str, str] = {}
        for entry in entries:
            prior = folded.setdefault(entry.name.casefold(), entry.name)
            if prior != entry.name:
                raise LinkContractError(f"case-insensitive path collision in {directory}: {prior} vs {entry.name}")
            child_logical = logical / entry.name
            entry_path = Path(entry.path)
            if is_reparse(entry_path):
                raise LinkContractError(f"symlink or reparse entry in Markdown tree: {child_logical.as_posix()}")
            if entry.is_dir(follow_symlinks=False):
                stack.append((Path(entry.path), child_logical))
            elif entry.is_file(follow_symlinks=False) and entry.name.lower().endswith(".md"):
                yield Document(entry_path, child_logical.as_posix())


def collect_document_records(
    catalog: dict[str, Any],
    repo_root: Path = REPO_ROOT,
) -> list[Document]:
    include = catalog.get("include")
    if not isinstance(include, dict):
        raise LinkContractError("docs catalog include must be an object")
    prefixes_raw = catalog.get("excludePrefixes", [])
    paths_raw = catalog.get("excludePaths", [])
    if not isinstance(prefixes_raw, list) or not isinstance(paths_raw, list):
        raise LinkContractError("docs catalog exclusions must be arrays")
    prefixes = tuple(safe_relative(value.rstrip("/")).as_posix() + "/" for value in prefixes_raw)
    excluded_paths = {safe_relative(value).as_posix() for value in paths_raw}
    records: dict[str, Document] = {}

    for raw in include.get("rootDocuments", []):
        rel = safe_relative(raw)
        logical = rel.as_posix()
        path = repo_root.joinpath(*rel.parts)
        if is_reparse(path) or not path.is_file():
            raise LinkContractError(f"root document is missing or unsafe: {logical}")
        if not excluded(logical, prefixes, excluded_paths):
            records[logical] = Document(path, logical)

    for raw in include.get("recursiveMarkdownRoots", []):
        rel = safe_relative(raw)
        logical_root = rel.as_posix()
        root = repo_root.joinpath(*rel.parts)
        for document in walk_markdown(root, rel):
            if excluded(document.logical, prefixes, excluded_paths):
                continue
            if document.logical in records:
                raise LinkContractError(f"document discovered twice: {document.logical}")
            records[document.logical] = document

    ordered = [records[key] for key in sorted(records)]
    if not ordered or len(ordered) > MAX_DOCUMENTS:
        raise LinkContractError(f"document count {len(ordered)} is outside 1..{MAX_DOCUMENTS}")
    total = 0
    for document in ordered:
        size = document.path.stat().st_size
        if size > MAX_DOCUMENT_BYTES:
            raise LinkContractError(f"document exceeds byte limit: {document.logical}")
        total += size
        if total > MAX_TOTAL_BYTES:
            raise LinkContractError("documentation corpus exceeds total-byte limit")
    folded: dict[str, str] = {}
    for document in ordered:
        prior = folded.setdefault(document.logical.casefold(), document.logical)
        if prior != document.logical:
            raise LinkContractError(f"case-insensitive document collision: {prior} vs {document.logical}")
    return ordered


def collect_documents(catalog: dict[str, Any]) -> list[Path]:
    return [document.path for document in collect_document_records(catalog)]


def without_fenced_code(content: str) -> str:
    output: list[str] = []
    in_fence = False
    marker = ""
    for line in content.splitlines(keepends=True):
        stripped = line.lstrip()
        if stripped.startswith(FENCE) or stripped.startswith("~~~"):
            candidate = stripped[:3]
            if not in_fence:
                in_fence = True
                marker = candidate
            elif candidate == marker:
                in_fence = False
            output.append("\n" if line.endswith("\n") else "")
        elif in_fence:
            output.append("\n" if line.endswith("\n") else "")
        else:
            output.append(line)
    return "".join(output)


def without_inline_code(content: str) -> str:
    return INLINE_CODE_RE.sub(lambda match: " " * len(match.group(0)), content)


def heading_ids(content: str) -> set[str]:
    ids: set[str] = set()
    counts: dict[str, int] = {}
    visible = without_inline_code(without_fenced_code(content))
    for line in visible.splitlines():
        match = re.match(r"^(#{1,6})\s+(.+?)\s*#*\s*$", line)
        if match:
            label = re.sub(r"<[^>]+>|[" + re.escape(chr(96) + "*~") + r"]", "", match.group(2)).strip()
            base = github_heading_slug(label)
            count = counts.get(base, 0)
            counts[base] = count + 1
            ids.add(base if count == 0 else f"{base}-{count}")
    ids.update(html.unescape(value) for value in EXPLICIT_ANCHOR_RE.findall(visible))
    return ids


def normalize_reference(label: str) -> str:
    return " ".join(label.strip().casefold().split())


def extract_links(content: str, source_path: Path) -> list[dict[str, Any]]:
    visible = without_inline_code(without_fenced_code(content))
    source = source_path.relative_to(REPO_ROOT).as_posix()
    line_starts = [0, *(match.end() for match in re.finditer("\n", visible))]

    def source_line(offset: int) -> int:
        return bisect_right(line_starts, offset)

    definitions: dict[str, str] = {}
    for match in REFERENCE_DEF_RE.finditer(visible):
        label = normalize_reference(match.group(1))
        target = html.unescape(unquote((match.group(2) or match.group(3)).strip()))
        if label in definitions and definitions[label] != target:
            raise LinkContractError(f"{source}:{source_line(match.start())}: duplicate reference definition")
        definitions[label] = target

    found: list[dict[str, Any]] = []
    occupied: list[tuple[int, int]] = []

    def add(match: re.Match[str], target: str, kind: str = "markdown") -> None:
        target = html.unescape(unquote(target.strip()))
        if target:
            found.append({
                "target": target,
                "line": source_line(match.start()),
                "source": source,
                "kind": kind,
            })
        occupied.append(match.span())

    for match in INLINE_LINK_RE.finditer(visible):
        add(match, match.group(1) or match.group(2))
    for match in REFERENCE_USE_RE.finditer(visible):
        label = normalize_reference(match.group(2) or match.group(1))
        if label not in definitions:
            found.append({
                "target": f"[{label}]",
                "line": source_line(match.start()),
                "source": source,
                "kind": "reference",
                "error": "reference-style link has no definition",
            })
        else:
            add(match, definitions[label], "reference")
    for match in HTML_LINK_RE.finditer(visible):
        add(match, match.group(2), "html")
    for match in AUTOLINK_RE.finditer(visible):
        if any(start <= match.start() and match.end() <= end for start, end in occupied):
            continue
        candidate = match.group(1)
        if re.fullmatch(r"/?[A-Za-z][A-Za-z0-9:_-]*", candidate):
            continue
        parsed = urlsplit(candidate)
        markdown_path = parsed.path.casefold().endswith((".md", ".markdown"))
        uri = bool(re.fullmatch(r"[A-Za-z][A-Za-z0-9+.-]{1,31}:[^<>\s]*", candidate))
        email = bool(re.fullmatch(r"[^<>\s@]+@[^<>\s@]+\.[^<>\s@]+", candidate))
        if candidate.startswith("#") or markdown_path or uri or email:
            add(match, candidate, "autolink")
    if len(found) > MAX_LINKS:
        raise LinkContractError(f"link count exceeds {MAX_LINKS}")
    return found


def is_external(target: str) -> tuple[bool, str | None]:
    split = urlsplit(target)
    scheme = split.scheme.casefold()
    if scheme in UNSAFE_SCHEMES:
        return False, f"unsafe link scheme: {scheme}"
    if scheme and scheme not in EXTERNAL_SCHEMES:
        return False, f"unsupported link scheme: {scheme}"
    if scheme or target.startswith("//"):
        return True, None
    return False, None


def exact_case(path: Path, root: Path) -> bool:
    try:
        relative = path.relative_to(root)
    except ValueError:
        return False
    current = root
    for part in relative.parts:
        try:
            names = {entry.name for entry in os.scandir(current)}
        except OSError:
            return False
        if part not in names:
            return False
        current = current / part
    return True


def source_line_anchor(
    path: Path,
    anchor: str,
    line_count_cache: dict[str, int],
) -> str | None:
    match = SOURCE_LINE_RE.fullmatch(anchor)
    if not match:
        return "invalid source line anchor"
    cache_key = str(path)
    if cache_key not in line_count_cache:
        try:
            with path.open("rb") as stream:
                lines = 0
                last = b""
                while chunk := stream.read(1024 * 1024):
                    lines += chunk.count(b"\n")
                    last = chunk[-1:]
            if path.stat().st_size and last != b"\n":
                lines += 1
            line_count_cache[cache_key] = lines
        except OSError:
            return "cannot read source target"
    lines = line_count_cache[cache_key]
    start = int(match.group(1))
    end = int(match.group(2) or start)
    if start > end or end > lines:
        return f"source line anchor exceeds {lines} lines"
    return None


def resolve_link(
    source_path: Path,
    target: str,
    heading_cache: dict[str, set[str]],
    generated_paths: set[str],
    path_cache: dict[tuple[str, str], tuple[Path | None, str | None]],
    filesystem_cache: dict[str, str | None],
    line_count_cache: dict[str, int],
    repo_root: Path = REPO_ROOT,
) -> str | None:
    external, scheme_error = is_external(target)
    if scheme_error:
        return scheme_error
    if external:
        return None
    split = urlsplit(target)
    raw_path = split.path
    anchor = split.fragment
    if "\\" in raw_path:
        return "link path uses non-canonical backslash"
    path_key = (str(source_path), raw_path)
    normalized, cached_error = path_cache.get(path_key, (None, None))
    if path_key not in path_cache:
        if not raw_path:
            resolved = source_path
        elif raw_path.startswith("/"):
            try:
                rel = safe_relative(raw_path.lstrip("/"))
            except LinkContractError as exc:
                path_cache[path_key] = (None, str(exc))
                return str(exc)
            resolved = repo_root.joinpath(*rel.parts)
        else:
            decoded = PurePosixPath(raw_path)
            resolved = source_path.parent.joinpath(*decoded.parts)
        root_resolved = repo_root.resolve()
        try:
            lexical = Path(os.path.abspath(resolved))
            lexical.relative_to(root_resolved)
        except (OSError, ValueError):
            path_cache[path_key] = (None, "link escapes repository root")
            return "link escapes repository root"
        if path_crosses_reparse(lexical, root_resolved):
            normalized = lexical
            cached_error = "link crosses a symlink or reparse point"
        else:
            normalized = lexical.resolve(strict=False)
            try:
                normalized.relative_to(root_resolved)
            except ValueError:
                path_cache[path_key] = (None, "link escapes repository root")
                return "link escapes repository root"
            filesystem_key = str(normalized)
            if filesystem_key in filesystem_cache:
                cached_error = filesystem_cache[filesystem_key]
            elif not normalized.exists():
                cached_error = f"target does not exist: {raw_path}"
                filesystem_cache[filesystem_key] = cached_error
            elif not exact_case(normalized, root_resolved):
                cached_error = f"target path casing is not exact: {raw_path}"
                filesystem_cache[filesystem_key] = cached_error
            else:
                cached_error = None
                filesystem_cache[filesystem_key] = None
        path_cache[path_key] = (normalized, cached_error)
    if normalized is not None:
        logical = normalized.relative_to(repo_root.resolve()).as_posix()
        if logical.startswith("docs/api/") and logical not in generated_paths:
            return "generated API target is absent from the exact manifest"
    if cached_error:
        return cached_error
    assert normalized is not None
    logical = normalized.relative_to(repo_root.resolve()).as_posix()
    if logical.startswith("docs/api/") and logical not in generated_paths:
        return "generated API target is absent from the exact manifest"
    if anchor:
        if SOURCE_LINE_RE.fullmatch(anchor):
            return source_line_anchor(normalized, anchor, line_count_cache)
        if normalized.suffix.lower() != ".md":
            return f"non-Markdown target has unsupported anchor #{anchor}"
        key = logical
        if key not in heading_cache:
            try:
                heading_cache[key] = heading_ids(normalized.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError):
                return "cannot read target for anchor validation"
        if anchor not in heading_cache[key]:
            return f"heading anchor #{anchor} not found"
    return None


def generated_manifest_paths(generated_root: Path, source_sha: str | None) -> tuple[set[str], list[str]]:
    if is_reparse(generated_root) or not generated_root.is_dir():
        return set(), ["generated API root is missing or unsafe"]
    errors = validate_api_manifest(generated_root, source_sha)
    try:
        manifest = json.loads((generated_root / ".manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return set(), errors
    paths = {
        "docs/api/" + row["path"]
        for row in manifest.get("files", [])
        if isinstance(row, dict) and isinstance(row.get("path"), str)
    }
    paths.add("docs/api/.manifest.json")
    return paths, errors


def validate_docs_links(
    catalog: dict[str, Any] | None = None,
    *,
    generated_root: Path | None = None,
    source_sha: str | None = None,
) -> list[dict[str, Any]]:
    catalog = catalog or load_json(SITE_CONTRACT_ROOT / "docs-catalog.json")
    generated_root = generated_root or REPO_ROOT / "docs" / "api"
    generated_paths, manifest_errors = generated_manifest_paths(generated_root, source_sha)
    errors = [
        {"source": "docs/api/.manifest.json", "line": 0, "target": "docs/api", "error": message}
        for message in manifest_errors
    ]
    try:
        documents = collect_document_records(catalog, REPO_ROOT)
    except LinkContractError as exc:
        return errors + [{"source": "docs-catalog.json", "line": 0, "target": "", "error": str(exc)}]
    heading_cache: dict[str, set[str]] = {}
    path_cache: dict[tuple[str, str], tuple[Path | None, str | None]] = {}
    filesystem_cache: dict[str, str | None] = {}
    line_count_cache: dict[str, int] = {}
    total_links = 0
    for document in documents:
        try:
            content = document.path.read_text(encoding="utf-8")
            links = extract_links(content, document.path)
        except (OSError, UnicodeDecodeError, LinkContractError) as exc:
            errors.append({"source": document.logical, "line": 0, "target": "", "error": str(exc)})
            continue
        total_links += len(links)
        if total_links > MAX_LINKS:
            errors.append({
                "source": document.logical,
                "line": 0,
                "target": "",
                "error": f"documentation link count exceeds {MAX_LINKS}",
            })
            break
        for link in links:
            if "error" in link:
                errors.append(link)
                continue
            error = resolve_link(
                document.path,
                link["target"],
                heading_cache,
                generated_paths,
                path_cache,
                filesystem_cache,
                line_count_cache,
                repo_root=REPO_ROOT,
            )
            if error:
                errors.append({**link, "error": error})
    return errors


def route_for(logical: str, overrides: dict[str, str]) -> str:
    if logical in overrides:
        return overrides[logical]
    path = PurePosixPath(logical)
    without_suffix = path.with_suffix("").as_posix()
    if without_suffix.endswith("/README"):
        without_suffix = without_suffix[:-7].rstrip("/")
    return without_suffix


def validate_docs_routes(catalog: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    catalog = catalog or load_json(SITE_CONTRACT_ROOT / "docs-catalog.json")
    errors: list[dict[str, Any]] = []
    overrides = catalog.get("routeOverrides")
    if not isinstance(overrides, dict):
        return [{"source": "docs-catalog.json", "line": 0, "target": "", "error": "routeOverrides must be an object"}]
    try:
        documents = collect_document_records(catalog, REPO_ROOT)
    except LinkContractError as exc:
        return [{"source": "docs-catalog.json", "line": 0, "target": "", "error": str(exc)}]
    logical_paths = {document.logical for document in documents}
    routes: dict[str, str] = {}
    for source, route in overrides.items():
        try:
            canonical = safe_relative(source).as_posix()
        except (LinkContractError, TypeError) as exc:
            errors.append({"source": "docs-catalog.json", "line": 0, "target": str(source), "error": str(exc)})
            continue
        if canonical not in logical_paths:
            errors.append({"source": "docs-catalog.json", "line": 0, "target": source, "error": "route source is not a discovered document"})
        if not isinstance(route, str) or not re.fullmatch(r"[a-z0-9][a-z0-9/_-]*", route):
            errors.append({"source": "docs-catalog.json", "line": 0, "target": str(route), "error": "route is not canonical lowercase"})
    for logical in sorted(logical_paths):
        route = route_for(logical, overrides)
        folded = route.casefold()
        prior = routes.setdefault(folded, logical)
        if prior != logical:
            errors.append({
                "source": "docs-catalog.json",
                "line": 0,
                "target": route,
                "error": f"case-insensitive route collision: {prior} vs {logical}",
            })
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--routes-only", action="store_true")
    parser.add_argument("--generated-root", type=Path)
    parser.add_argument("--source-sha")
    args = parser.parse_args(argv)
    catalog = load_json(SITE_CONTRACT_ROOT / "docs-catalog.json")
    errors = validate_docs_routes(catalog)
    if not args.routes_only:
        errors.extend(
            validate_docs_links(
                catalog,
                generated_root=args.generated_root,
                source_sha=args.source_sha,
            )
        )
    if args.json:
        print(json.dumps({"count": len(errors), "errors": errors}, indent=2))
    elif errors:
        print(f"Found {len(errors)} documentation contract error(s):")
        for error in errors[:100]:
            print(f"  {error['source']}:{error['line']}: {error['target']} - {error['error']}")
    else:
        print("All documentation links, anchors, manifests, and routes are valid.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
