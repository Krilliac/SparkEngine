#!/usr/bin/env python3
"""Validate internal markdown links, heading anchors, and image references.

Walks every document in the docs catalog and checks that each relative link
target resolves to a real file and, when an anchor is present, that the target
file contains a matching heading.  External URLs (http/https) are skipped.

Exit codes:
  0 — all links resolve
  1 — broken links found (details on stdout as JSON)
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import unquote

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

from common import (
    REPO_ROOT as _REPO_ROOT,
    extract_headings,
    heading_slug,
    load_json,
    SITE_CONTRACT_ROOT,
)

IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".bmp", ".ico"}

GENERATED_ROOTS = {"docs/api"}


def _is_generated_target(source_path: Path, file_part: str) -> bool:
    """True when the link target falls inside a known generated-but-gitignored tree."""
    resolved = (source_path.parent / PurePosixPath(file_part)).resolve()
    try:
        rel = resolved.relative_to(REPO_ROOT.resolve()).as_posix()
    except ValueError:
        return False
    return any(rel == root or rel.startswith(root + "/") for root in GENERATED_ROOTS)
LINK_RE = re.compile(
    r"""
    (?:!?\[(?:[^\[\]]|\[(?:[^\[\]]|\[[^\]]*\])*\])*\])   # [text] or ![alt]
    \(                                                     # opening paren
      \s*([^)\s]+?)                                        # target (group 1)
      (?:\s+"[^"]*")?                                      # optional title
    \s*\)                                                  # closing paren
    """,
    re.VERBOSE,
)
HTML_IMG_RE = re.compile(r'<img\b[^>]*\bsrc=["\']([^"\']+)["\']', re.IGNORECASE)


def _heading_ids(markdown: str) -> set[str]:
    """Extract all heading anchor IDs from markdown content."""
    ids: set[str] = set()
    counts: dict[str, int] = {}
    in_fence = False
    for line in markdown.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = re.match(r"^(#{1,6})\s+(.+?)\s*#*\s*$", line)
        if not match:
            continue
        label = re.sub(r"<[^>]+>|[`*_~]", "", match.group(2)).strip()
        base = heading_slug(label)
        occurrence = counts.get(base, 0)
        counts[base] = occurrence + 1
        identifier = base if occurrence == 0 else f"{base}-{occurrence}"
        ids.add(identifier)
    return ids


def _is_external(target: str) -> bool:
    return target.startswith(("http://", "https://", "mailto:", "#"))


def _is_anchor_only(target: str) -> bool:
    return target.startswith("#")


def collect_documents(catalog: dict[str, Any]) -> list[Path]:
    """Collect all markdown documents described by the docs catalog."""
    include = catalog.get("include", {})
    exclude_prefixes = tuple(catalog.get("excludePrefixes", []))
    exclude_paths = set(catalog.get("excludePaths", []))
    documents: list[Path] = []

    for root_doc in include.get("rootDocuments", []):
        path = REPO_ROOT / root_doc
        if path.is_file():
            documents.append(path)

    for root_dir in include.get("recursiveMarkdownRoots", []):
        root = REPO_ROOT / root_dir
        if not root.is_dir():
            continue
        for md_path in sorted(root.rglob("*.md")):
            rel = md_path.relative_to(REPO_ROOT).as_posix()
            if rel in exclude_paths:
                continue
            if any(rel.startswith(prefix) for prefix in exclude_prefixes):
                continue
            documents.append(md_path)

    return documents


def extract_links(
    content: str, source_path: Path
) -> list[dict[str, Any]]:
    """Extract all markdown links and image references from content."""
    links: list[dict[str, Any]] = []
    for line_no, line in enumerate(content.splitlines(), start=1):
        for match in LINK_RE.finditer(line):
            target = unquote(match.group(1).strip())
            if not _is_external(target):
                links.append({
                    "target": target,
                    "line": line_no,
                    "source": source_path.relative_to(REPO_ROOT).as_posix(),
                })
        for match in HTML_IMG_RE.finditer(line):
            target = unquote(match.group(1).strip())
            if not _is_external(target):
                links.append({
                    "target": target,
                    "line": line_no,
                    "source": source_path.relative_to(REPO_ROOT).as_posix(),
                    "kind": "image",
                })
    return links


def resolve_link(
    source_path: Path,
    target: str,
    heading_cache: dict[str, set[str]],
) -> str | None:
    """Resolve a relative link target. Returns an error string or None if valid."""
    if _is_anchor_only(target):
        anchor = target[1:]
        source_rel = source_path.relative_to(REPO_ROOT).as_posix()
        if source_rel not in heading_cache:
            try:
                heading_cache[source_rel] = _heading_ids(
                    source_path.read_text(encoding="utf-8", errors="replace")
                )
            except OSError:
                return f"cannot read source file"
        if anchor not in heading_cache[source_rel]:
            return f"heading anchor #{anchor} not found"
        return None

    file_part = target.split("#")[0]
    anchor = target.split("#")[1] if "#" in target else None

    if not file_part:
        if anchor:
            source_rel = source_path.relative_to(REPO_ROOT).as_posix()
            if source_rel not in heading_cache:
                try:
                    heading_cache[source_rel] = _heading_ids(
                        source_path.read_text(encoding="utf-8", errors="replace")
                    )
                except OSError:
                    return f"cannot read source file"
            if anchor not in heading_cache[source_rel]:
                return f"heading anchor #{anchor} not found"
        return None

    resolved = (source_path.parent / PurePosixPath(file_part)).resolve()

    try:
        resolved.relative_to(REPO_ROOT.resolve())
    except ValueError:
        return f"link escapes repository root"

    if not resolved.exists():
        if _is_generated_target(source_path, file_part):
            return None
        return f"target does not exist: {file_part}"

    if anchor and resolved.suffix.lower() == ".md":
        resolved_rel = resolved.relative_to(REPO_ROOT).as_posix()
        if resolved_rel not in heading_cache:
            try:
                heading_cache[resolved_rel] = _heading_ids(
                    resolved.read_text(encoding="utf-8", errors="replace")
                )
            except OSError:
                return f"cannot read target for anchor check"
        if anchor not in heading_cache[resolved_rel]:
            return f"heading anchor #{anchor} not found in {file_part}"

    return None


def validate_docs_links(
    catalog: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    """Validate all internal links in catalog documents. Returns list of errors."""
    if catalog is None:
        catalog = load_json(SITE_CONTRACT_ROOT / "docs-catalog.json")

    documents = collect_documents(catalog)
    heading_cache: dict[str, set[str]] = {}
    errors: list[dict[str, Any]] = []

    for doc_path in documents:
        try:
            content = doc_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            errors.append({
                "source": doc_path.relative_to(REPO_ROOT).as_posix(),
                "line": 0,
                "target": str(doc_path),
                "error": "cannot read document",
            })
            continue

        links = extract_links(content, doc_path)
        for link in links:
            error = resolve_link(doc_path, link["target"], heading_cache)
            if error is not None:
                errors.append({
                    "source": link["source"],
                    "line": link["line"],
                    "target": link["target"],
                    "error": error,
                })

    return errors


def validate_docs_routes(
    catalog: dict[str, Any] | None = None,
) -> list[dict[str, Any]]:
    """Validate that route overrides point to existing files."""
    if catalog is None:
        catalog = load_json(SITE_CONTRACT_ROOT / "docs-catalog.json")

    errors: list[dict[str, Any]] = []
    for source_path, route in catalog.get("routeOverrides", {}).items():
        full = REPO_ROOT / source_path
        if not full.exists():
            errors.append({
                "source": "docs-catalog.json",
                "line": 0,
                "target": source_path,
                "error": f"route override source does not exist: {source_path}",
            })
    return errors


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--json", action="store_true", help="emit results as JSON"
    )
    parser.add_argument(
        "--routes-only",
        action="store_true",
        help="only validate route overrides, skip full link scan",
    )
    args = parser.parse_args()

    catalog = load_json(SITE_CONTRACT_ROOT / "docs-catalog.json")

    all_errors: list[dict[str, Any]] = []
    all_errors.extend(validate_docs_routes(catalog))
    if not args.routes_only:
        all_errors.extend(validate_docs_links(catalog))

    if args.json:
        print(json.dumps({"errors": all_errors, "count": len(all_errors)}, indent=2))
    else:
        if all_errors:
            print(f"Found {len(all_errors)} broken link(s):")
            for error in all_errors:
                print(f"  {error['source']}:{error['line']}: {error['target']} — {error['error']}")
        else:
            print("All document links resolve.")

    return 1 if all_errors else 0


if __name__ == "__main__":
    sys.exit(main())
