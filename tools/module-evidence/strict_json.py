#!/usr/bin/env python3
"""Strict, duplicate-aware, bounded JSON loading for module evidence documents.

`json.load` is unsuitable for validating a security-relevant manifest:

  - Duplicate object properties are silently accepted, last one winning.  A
    hostile document can therefore shadow a rejected value with an accepted
    one and the validator never sees the rejected value at all.
  - `NaN`, `Infinity` and `-Infinity` are accepted by default even though they
    are not valid JSON and cannot round-trip through other consumers.
  - There is no bound on document size, nesting depth or element count, so a
    small file can expand into an arbitrarily expensive parse.

This module provides a loader that rejects all of the above, at every object
and nested level, and reports the JSON pointer of the offending node.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

# Document limits.  These are deliberately far above any legitimate manifest
# (11 modules, 1 profile) and far below anything that could exhaust memory.
MAX_DOCUMENT_BYTES = 512 * 1024
MAX_DEPTH = 12
MAX_TOTAL_NODES = 20_000
MAX_STRING_LENGTH = 512
MAX_CONTAINER_ITEMS = 512


@dataclass(frozen=True)
class Limits:
    """Resource bounds applied to one document."""

    document_bytes: int = MAX_DOCUMENT_BYTES
    depth: int = MAX_DEPTH
    total_nodes: int = MAX_TOTAL_NODES
    string_length: int = MAX_STRING_LENGTH
    container_items: int = MAX_CONTAINER_ITEMS


DEFAULT_LIMITS = Limits()

# The readiness contract under docs/ is repository-controlled reference data
# rather than the artifact under adversarial validation, and it legitimately
# carries long prose fields.  Duplicate-key and non-finite rejection still
# apply — only the size bounds are relaxed.
CONTRACT_LIMITS = Limits(
    document_bytes=4 * 1024 * 1024,
    depth=24,
    total_nodes=200_000,
    string_length=8192,
    container_items=4096,
)


class StrictJSONError(ValueError):
    """A document that must be rejected before any semantic validation runs."""


def _reject_constant(name: str) -> Any:
    raise StrictJSONError(
        f"non-finite JSON constant {name!r} is not permitted anywhere in the document"
    )


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    seen: set[str] = set()
    for key, _ in pairs:
        if key in seen:
            raise StrictJSONError(
                f"duplicate JSON property {key!r} — duplicate properties are "
                f"rejected because they let a hostile value shadow a valid one"
            )
        seen.add(key)
    return dict(pairs)


def _walk(node: Any, pointer: str, depth: int, counter: list[int], limits: Limits) -> None:
    """Enforce depth, node-count, string and container limits recursively."""
    counter[0] += 1
    if counter[0] > limits.total_nodes:
        raise StrictJSONError(
            f"document exceeds the {limits.total_nodes} node limit at {pointer or '/'}"
        )
    if depth > limits.depth:
        raise StrictJSONError(
            f"document nesting exceeds the depth limit of {limits.depth} "
            f"at {pointer or '/'}"
        )

    if isinstance(node, dict):
        if len(node) > limits.container_items:
            raise StrictJSONError(
                f"{pointer or '/'}: object has {len(node)} properties, "
                f"limit is {limits.container_items}"
            )
        for key, value in node.items():
            if len(key) > limits.string_length:
                raise StrictJSONError(
                    f"{pointer or '/'}: property name exceeds "
                    f"{limits.string_length} characters"
                )
            _walk(value, f"{pointer}/{key}", depth + 1, counter, limits)
    elif isinstance(node, list):
        if len(node) > limits.container_items:
            raise StrictJSONError(
                f"{pointer or '/'}: array has {len(node)} items, "
                f"limit is {limits.container_items}"
            )
        for i, value in enumerate(node):
            _walk(value, f"{pointer}/{i}", depth + 1, counter, limits)
    elif isinstance(node, str):
        if len(node) > limits.string_length:
            raise StrictJSONError(
                f"{pointer or '/'}: string exceeds {limits.string_length} characters"
            )
    elif isinstance(node, float):
        # json's parse_constant only covers the bare literals; a value such as
        # 1e400 parses to inf through parse_float and must also be rejected.
        if node != node or node in (float("inf"), float("-inf")):
            raise StrictJSONError(
                f"{pointer or '/'}: non-finite number is not permitted"
            )


def loads(text: str, *, origin: str = "<string>", limits: Limits = DEFAULT_LIMITS) -> Any:
    """Parse `text` under the strict rules above, or raise StrictJSONError."""
    encoded = text.encode("utf-8")
    if len(encoded) > limits.document_bytes:
        raise StrictJSONError(
            f"{origin}: document is {len(encoded)} bytes, "
            f"limit is {limits.document_bytes}"
        )
    try:
        parsed = json.loads(
            text,
            object_pairs_hook=_no_duplicates,
            parse_constant=_reject_constant,
        )
    except StrictJSONError as exc:
        raise StrictJSONError(f"{origin}: {exc}") from None
    except json.JSONDecodeError as exc:
        raise StrictJSONError(f"{origin}: invalid JSON: {exc}") from exc

    _walk(parsed, "", 1, [0], limits)
    return parsed


def load_file(path: Path | str, *, limits: Limits = DEFAULT_LIMITS) -> Any:
    """Read and strictly parse a JSON document from disk."""
    path = Path(path)
    if not path.is_file():
        raise StrictJSONError(f"file not found: {path}")
    size = path.stat().st_size
    if size > limits.document_bytes:
        raise StrictJSONError(
            f"{path}: document is {size} bytes, limit is {limits.document_bytes}"
        )
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise StrictJSONError(f"{path}: file is not valid UTF-8: {exc}") from exc
    return loads(text, origin=str(path), limits=limits)
