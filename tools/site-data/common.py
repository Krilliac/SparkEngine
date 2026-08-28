#!/usr/bin/env python3
"""Shared, dependency-free helpers for SparkEngine's repository site data."""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, Iterator


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_ROOT = REPO_ROOT / "tools"
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))
import docs_contract  # noqa: E402

SITE_CONTRACT_ROOT = REPO_ROOT / "docs" / "site"
WORK_ITEM_ROOT = REPO_ROOT / "docs" / "readiness" / "work-items"
REPOSITORY = "Krilliac/SparkEngine"
REPOSITORY_URL = f"https://github.com/{REPOSITORY}"
SCHEMA_VERSION = 1
MAX_AUTHORITATIVE_JSON_BYTES = 8 * 1024 * 1024
MAX_JSON_NODES = 250_000
MAX_JSON_DEPTH = 128
MAX_JSON_STRING_BYTES = 2 * 1024 * 1024
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


MAX_CONTRACT_JSON_BYTES = 8 * 1024 * 1024


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


def _reject_json_constant(value: str) -> Any:
    raise ValueError(f"non-finite JSON number {value!r}")


def _is_reparse(metadata: os.stat_result) -> bool:
    return bool(int(getattr(metadata, "st_file_attributes", 0)) & 0x400)


def _identity(metadata: os.stat_result) -> tuple[int, int, int]:
    # mode/attributes are synthesized differently by Windows lstat/fstat.
    return (int(metadata.st_dev), int(metadata.st_ino), int(metadata.st_size))


def _mutation_token(metadata: os.stat_result) -> tuple[int, ...]:
    return (
        int(metadata.st_dev),
        int(metadata.st_ino),
        int(metadata.st_size),
        int(getattr(metadata, "st_mtime_ns", int(metadata.st_mtime * 1_000_000_000))),
        int(getattr(metadata, "st_ctime_ns", int(metadata.st_ctime * 1_000_000_000))),
        int(getattr(metadata, "st_file_attributes", 0)),
    )


def _directory_token(metadata: os.stat_result) -> tuple[int, ...]:
    return (
        int(metadata.st_dev),
        int(metadata.st_ino),
        int(metadata.st_mode),
        int(getattr(metadata, "st_mtime_ns", int(metadata.st_mtime * 1_000_000_000))),
        int(getattr(metadata, "st_ctime_ns", int(metadata.st_ctime * 1_000_000_000))),
        int(getattr(metadata, "st_file_attributes", 0)),
    )


def _validate_directory_chain(path: Path, label: str) -> list[tuple[Path, tuple[int, ...]]]:
    absolute = Path(os.path.abspath(os.fspath(path)))
    parts = absolute.parts
    if not parts:
        raise SiteDataError(f"{label} has no absolute directory")
    current = Path(parts[0])
    result: list[tuple[Path, tuple[int, ...]]] = []
    for component in parts[1:]:
        current /= component
        try:
            metadata = os.lstat(current)
        except OSError as error:
            raise SiteDataError(f"cannot inspect {label} component {current}: {error}") from error
        if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata):
            raise SiteDataError(f"{label} component {current} is a symlink or reparse point")
        if not stat.S_ISDIR(metadata.st_mode):
            raise SiteDataError(f"{label} component {current} is not a directory")
        result.append((current, _directory_token(metadata)))
    return result


def _ensure_directory(path: Path, label: str) -> Path:
    absolute = Path(os.path.abspath(os.fspath(path)))
    parts = absolute.parts
    if not parts:
        raise SiteDataError(f"{label} has no absolute directory")
    current = Path(parts[0])
    for component in parts[1:]:
        current /= component
        try:
            metadata = os.lstat(current)
        except FileNotFoundError:
            try:
                os.mkdir(current)
            except FileExistsError:
                pass
            metadata = os.lstat(current)
        if stat.S_ISLNK(metadata.st_mode) or _is_reparse(metadata):
            raise SiteDataError(f"{label} component {current} is a symlink or reparse point")
        if not stat.S_ISDIR(metadata.st_mode):
            raise SiteDataError(f"{label} component {current} is not a directory")
    _validate_directory_chain(absolute, label)
    return absolute


def read_bytes_stable(path: Path, maximum: int, label: str | None = None) -> bytes:
    """Read a bounded, regular, one-link file through a stable open identity."""
    absolute = Path(os.path.abspath(os.fspath(path)))
    display = label or relative_path(absolute)
    ancestors = _validate_directory_chain(absolute.parent, f"{display} parent")
    try:
        before = os.lstat(absolute)
    except FileNotFoundError as error:
        raise SiteDataError(f"Required file does not exist: {display}") from error
    except OSError as error:
        raise SiteDataError(f"cannot inspect {display}: {error}") from error
    if stat.S_ISLNK(before.st_mode) or _is_reparse(before):
        raise SiteDataError(f"{display} must not be a symlink or reparse point")
    if not stat.S_ISREG(before.st_mode):
        raise SiteDataError(f"{display} is not a regular file")
    if before.st_size > maximum:
        raise SiteDataError(f"{display} exceeds the {maximum}-byte bound")

    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(absolute, flags)
    except OSError as error:
        raise SiteDataError(f"cannot open {display} without following links: {error}") from error
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode) or _is_reparse(opened):
            raise SiteDataError(f"opened {display} is not a regular non-reparse file")
        if int(opened.st_nlink) != 1:
            raise SiteDataError(f"opened {display} must have exactly one hard link")
        if _identity(opened) != _identity(before):
            raise SiteDataError(f"{display} was replaced while opening")
        opened_token = _mutation_token(opened)

        def consume() -> bytes:
            remaining = int(opened.st_size)
            chunks: list[bytes] = []
            while remaining:
                chunk = os.read(descriptor, min(remaining, 1024 * 1024))
                if not chunk:
                    raise SiteDataError(f"{display} was truncated while reading")
                chunks.append(chunk)
                remaining -= len(chunk)
            if os.read(descriptor, 1):
                raise SiteDataError(f"{display} grew while reading")
            return b"".join(chunks)

        payload = consume()
        if _mutation_token(os.fstat(descriptor)) != opened_token:
            raise SiteDataError(f"{display} changed while reading")
        os.lseek(descriptor, 0, os.SEEK_SET)
        if consume() != payload or _mutation_token(os.fstat(descriptor)) != opened_token:
            raise SiteDataError(f"{display} content was unstable while reading")
    finally:
        os.close(descriptor)

    try:
        after = os.lstat(absolute)
    except OSError as error:
        raise SiteDataError(f"cannot revalidate {display}: {error}") from error
    if _mutation_token(after) != _mutation_token(before):
        raise SiteDataError(f"{display} changed after reading")
    for ancestor, token in ancestors:
        try:
            current = os.lstat(ancestor)
        except OSError as error:
            raise SiteDataError(f"cannot revalidate {display} parent {ancestor}: {error}") from error
        if _directory_token(current) != token:
            raise SiteDataError(f"{display} parent {ancestor} changed while reading")
    return payload


def decode_json_bytes(value: bytes, label: str, maximum: int = MAX_AUTHORITATIVE_JSON_BYTES) -> Any:
    if len(value) > maximum:
        raise SiteDataError(f"{label} exceeds the {maximum}-byte JSON bound")
    try:
        decoded = json.loads(
            value.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_json_constant,
        )
    except UnicodeDecodeError as error:
        raise SiteDataError(f"Invalid UTF-8 in {label}: {error}") from error
    except json.JSONDecodeError as error:
        raise SiteDataError(
            f"Invalid JSON in {label}:{error.lineno}:{error.colno}: {error.msg}"
        ) from error
    except ValueError as error:
        raise SiteDataError(f"Invalid JSON in {label}: {error}") from error

    def utf8_size(text: str, kind: str) -> int:
        try:
            return len(text.encode("utf-8"))
        except UnicodeEncodeError as error:
            raise SiteDataError(f"{label} contains invalid Unicode in a JSON {kind}") from error

    nodes = 0
    stack: list[tuple[Any, int]] = [(decoded, 1)]
    while stack:
        item, depth = stack.pop()
        nodes += 1
        if nodes > MAX_JSON_NODES:
            raise SiteDataError(f"{label} exceeds the {MAX_JSON_NODES}-node JSON bound")
        if depth > MAX_JSON_DEPTH:
            raise SiteDataError(f"{label} exceeds the {MAX_JSON_DEPTH}-level JSON depth bound")
        if isinstance(item, dict):
            for key, child in item.items():
                if utf8_size(key, "object key") > MAX_JSON_STRING_BYTES:
                    raise SiteDataError(f"{label} contains an oversized JSON object key")
                stack.append((child, depth + 1))
        elif isinstance(item, list):
            stack.extend((child, depth + 1) for child in item)
        elif isinstance(item, str):
            if utf8_size(item, "string") > MAX_JSON_STRING_BYTES:
                raise SiteDataError(f"{label} contains an oversized JSON string")
        elif isinstance(item, float) and not math.isfinite(item):
            raise SiteDataError(f"{label} contains a non-finite JSON number")
    return decoded


def load_json(path: Path, *, maximum: int = MAX_AUTHORITATIVE_JSON_BYTES) -> Any:
    label = relative_path(path)
    return decode_json_bytes(read_bytes_stable(path, maximum, label), label, maximum)


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
    absolute = Path(os.path.abspath(os.fspath(path)))
    parent = _ensure_directory(absolute.parent, f"output parent for {absolute.name}")
    parent_identity = _identity(os.lstat(parent))
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{absolute.name}.", suffix=".tmp", dir=parent
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        temporary_identity = _identity(os.lstat(temporary_path))
        if read_bytes_stable(
            temporary_path, len(value), f"temporary output for {absolute.name}"
        ) != value:
            raise SiteDataError(f"temporary output for {absolute.name} differs before publication")
        if _identity(os.lstat(parent)) != parent_identity:
            raise SiteDataError(f"output parent for {absolute.name} was replaced before publication")
        os.replace(temporary_path, absolute)
        if _identity(os.lstat(absolute)) != temporary_identity:
            raise SiteDataError(
                f"published output {absolute.name} is not the verified temporary file"
            )
        if read_bytes_stable(
            absolute, len(value), f"published output {absolute.name}"
        ) != value:
            raise SiteDataError(f"published output {absolute.name} differs after publication")
        if _identity(os.lstat(parent)) != parent_identity:
            raise SiteDataError(f"output parent for {absolute.name} was replaced during publication")
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


def github_heading_slug(label: str) -> str:
    """Return the case-folded fragment generated by GitHub Markdown headings."""
    value = re.sub(r"<[^>]+>", "", label)
    value = re.sub(r"[`*~]", "", value).strip().casefold()
    value = "".join(
        character
        for character in value
        if character.isalnum() or character in {"-", "_"} or character.isspace()
    )
    return "".join("-" if character.isspace() else character for character in value) or "section"


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
