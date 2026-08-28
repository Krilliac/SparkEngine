#!/usr/bin/env python3
"""Shared, dependency-free helpers for SparkEngine's repository site data."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Iterable, Iterator


REPO_ROOT = Path(__file__).resolve().parents[2]
SITE_CONTRACT_ROOT = REPO_ROOT / "docs" / "site"
WORK_ITEM_ROOT = REPO_ROOT / "docs" / "readiness" / "work-items"
REPOSITORY = "Krilliac/SparkEngine"
REPOSITORY_URL = f"https://github.com/{REPOSITORY}"
SCHEMA_VERSION = 1
METRIC_IDS = {
    "code.files",
    "code.totalLines",
    "docs.authored",
    "editor.panels",
    "module.fps.files",
    "module.fps.lines",
    "module.mmofps.files",
    "module.mmofps.lines",
    "module.mmofps.replicationHz",
    "module.mmofps.serverHz",
    "modules.discovered",
    "networking.lines",
    "shaders.glsl",
    "shaders.hlsl",
    "tests.definitions",
    "tests.files",
    "visualScript.nodes",
}


class SiteDataError(RuntimeError):
    """A contract or generation error suitable for a concise CI message."""


def run_git(*arguments: str) -> str:
    """Run git at the repository root and return stripped stdout."""
    result = subprocess.run(
        ["git", *arguments],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise SiteDataError(f"git {' '.join(arguments)} failed: {detail}")
    return result.stdout.strip()


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """A repeated JSON key silently keeps the last value.

    A reviewer reading the diff sees the first one, so a duplicate is a way to
    make a contract say one thing and validate another. Refuse it outright.
    """
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys)
    except FileNotFoundError as error:
        raise SiteDataError(f"Required contract file does not exist: {relative_path(path)}") from error
    except json.JSONDecodeError as error:
        raise SiteDataError(
            f"Invalid JSON in {relative_path(path)}:{error.lineno}:{error.colno}: {error.msg}"
        ) from error
    except ValueError as error:
        raise SiteDataError(f"Invalid JSON in {relative_path(path)}: {error}") from error


def load_contract() -> dict[str, Any]:
    """Load the complete repository-authored site/readiness contract."""
    work_item_files = sorted(WORK_ITEM_ROOT.glob("*.json"))
    if not work_item_files:
        raise SiteDataError("No readiness work-item files were found")

    work_items: list[dict[str, Any]] = []
    parity_dimensions: dict[str, Any] | None = None
    for path in work_item_files:
        document = load_json(path)
        if document.get("schemaVersion") != SCHEMA_VERSION:
            raise SiteDataError(f"Unsupported schemaVersion in {relative_path(path)}")
        if not isinstance(document.get("workItems"), list):
            raise SiteDataError(f"{relative_path(path)} must contain a workItems array")
        work_items.extend(document["workItems"])
        if "parityDimensions" in document:
            if parity_dimensions is not None:
                raise SiteDataError("parityDimensions may be declared in only one work-item file")
            parity_dimensions = document["parityDimensions"]

    content = load_json(SITE_CONTRACT_ROOT / "content.json")
    readiness = load_json(SITE_CONTRACT_ROOT / "readiness.json")
    docs_catalog = load_json(SITE_CONTRACT_ROOT / "docs-catalog.json")
    return {
        "content": content,
        "readiness": readiness,
        "docsCatalog": docs_catalog,
        "workItems": work_items,
        "workItemFiles": work_item_files,
        "parityDimensions": parity_dimensions,
    }


def canonical_json_bytes(value: Any) -> bytes:
    """Stable JSON bytes used for published files and their digests."""
    return (
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")


def pretty_json_text(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def write_bytes_atomic(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        temporary_path.replace(path)
    finally:
        temporary_path.unlink(missing_ok=True)


def write_json(path: Path, value: Any, *, pretty: bool = False) -> dict[str, Any]:
    payload = pretty_json_text(value).encode("utf-8") if pretty else canonical_json_bytes(value)
    write_bytes_atomic(path, payload)
    return {"path": path.as_posix(), "sha256": sha256_bytes(payload), "bytes": len(payload)}


def write_text(path: Path, value: str) -> None:
    write_bytes_atomic(path, value.encode("utf-8"))


def relative_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def to_posix(path: str | Path) -> str:
    return str(path).replace("\\", "/").removeprefix("./")


def repository_source(
    *,
    branch_override: str | None = None,
    commit_override: str | None = None,
    committed_at_override: str | None = None,
) -> dict[str, str]:
    commit = run_git("rev-parse", f"{commit_override}^{{commit}}" if commit_override else "HEAD")
    branch = branch_override or run_git("branch", "--show-current") or "detached"
    git_committed_at = run_git("show", "-s", "--format=%cI", commit)
    if committed_at_override and committed_at_override != git_committed_at:
        raise SiteDataError(
            f"committed-at override {committed_at_override!r} does not match git metadata {git_committed_at!r}"
        )
    committed_at = committed_at_override or git_committed_at
    return {
        "repository": REPOSITORY,
        "repositoryUrl": REPOSITORY_URL,
        "branch": branch,
        "commit": commit,
        "shortCommit": commit[:7],
        "committedAt": committed_at,
    }


def git_dirty_paths() -> list[str]:
    output = run_git("status", "--porcelain=v1", "--untracked-files=all")
    paths: list[str] = []
    for line in output.splitlines():
        if not line:
            continue
        value = line[3:]
        if " -> " in value:
            value = value.split(" -> ", 1)[1]
        paths.append(value)
    return paths


def walk_files(root: Path, suffixes: Iterable[str] | None = None) -> Iterator[Path]:
    accepted = {suffix.lower() for suffix in suffixes} if suffixes else None
    if not root.exists():
        return
    for path in sorted(root.rglob("*")):
        if path.is_file() and (accepted is None or path.suffix.lower() in accepted):
            yield path


def slug_part(value: str) -> str:
    value = re.sub(r"\.md$", "", value, flags=re.IGNORECASE)
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1-\2", value)
    value = re.sub(r"[^a-zA-Z0-9]+", "-", value).strip("-")
    return value.lower()


def title_from_filename(value: str) -> str:
    value = re.sub(r"\.md$", "", value, flags=re.IGNORECASE)
    value = re.sub(r"[-_]+", " ", value)
    return " ".join(word[:1].upper() + word[1:] for word in value.split())


def plain_text(markdown: str) -> str:
    value = re.sub(r"```.*?```", " ", markdown, flags=re.DOTALL)
    value = re.sub(r"<!--.*?-->", " ", value, flags=re.DOTALL)
    value = re.sub(r"!\[([^]]*)\]\([^)]*\)", r"\1", value)
    value = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", value)
    value = re.sub(r"<[^>]+>", " ", value)
    value = re.sub(r"[`*_>#|~=-]", " ", value)
    return re.sub(r"\s+", " ", value).strip()


def extract_title(markdown: str, source_path: str) -> str:
    match = re.search(r"^#\s+(.+?)\s*#*\s*$", markdown, flags=re.MULTILINE)
    if match:
        return re.sub(r"[*_`]", "", match.group(1)).strip()
    return title_from_filename(Path(source_path).name)


def extract_excerpt(markdown: str) -> str:
    without_title = re.sub(r"^#\s+.*$", "", markdown, count=1, flags=re.MULTILINE)
    for paragraph in re.split(r"\n\s*\n", without_title):
        candidate = paragraph.strip()
        if not candidate or re.match(r"^(#|\||```|>|[-*+]\s|\d+\.\s|<)", candidate):
            continue
        text = plain_text(candidate)
        if len(text) > 35:
            return text[:240] + ("…" if len(text) > 240 else "")
    return "Repository-authored SparkEngine documentation."


def heading_slug(label: str) -> str:
    value = re.sub(r"<[^>]+>", "", label)
    value = re.sub(r"[`*_~]", "", value).lower()
    value = re.sub(r"[^a-z0-9\s-]", "", value).strip()
    return re.sub(r"-+", "-", re.sub(r"\s+", "-", value)) or "section"


def extract_headings(markdown: str, *, limit: int | None = 80) -> list[dict[str, Any]]:
    headings: list[dict[str, Any]] = []
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
        depth = len(match.group(1))
        if depth in (2, 3):
            headings.append({"depth": depth, "label": label, "id": identifier})
            if limit is not None and len(headings) >= limit:
                break
    return headings


def fail(message: str) -> None:
    raise SiteDataError(message)
