#!/usr/bin/env python3
"""Bounded, duplicate-free JSON parsing shared by OPS-100 validators."""

from __future__ import annotations

import json
import math
from typing import Any


class StrictJsonError(ValueError):
    """Input is not valid under the bounded OPS JSON policy."""


def _reject_constant(value: str) -> None:
    raise StrictJsonError(f"non-finite JSON number '{value}' is not allowed")


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise StrictJsonError(f"duplicate JSON object key '{key}'")
        result[key] = value
    return result


def _utf8_size(value: str, location: str) -> int:
    try:
        return len(value.encode("utf-8", errors="strict"))
    except UnicodeEncodeError as exc:
        raise StrictJsonError(f"{location}: invalid Unicode scalar value") from exc


def validate_json_shape(
    value: Any,
    *,
    max_depth: int,
    max_collection_entries: int,
    max_string_bytes: int,
) -> None:
    """Validate every nested collection and string without recursive Python calls."""

    pending: list[tuple[Any, int, str]] = [(value, 0, "$")]
    while pending:
        current, depth, location = pending.pop()
        if depth > max_depth:
            raise StrictJsonError(f"{location}: JSON nesting exceeds depth {max_depth}")

        if isinstance(current, dict):
            if len(current) > max_collection_entries:
                raise StrictJsonError(
                    f"{location}: object has {len(current)} entries; limit is {max_collection_entries}"
                )
            for key, child in current.items():
                if _utf8_size(key, f"{location} key") > max_string_bytes:
                    raise StrictJsonError(f"{location}: object key exceeds {max_string_bytes} UTF-8 bytes")
                pending.append((child, depth + 1, f"{location}.{key}"))
        elif isinstance(current, list):
            if len(current) > max_collection_entries:
                raise StrictJsonError(
                    f"{location}: array has {len(current)} entries; limit is {max_collection_entries}"
                )
            for index, child in enumerate(current):
                pending.append((child, depth + 1, f"{location}[{index}]"))
        elif isinstance(current, str):
            if _utf8_size(current, location) > max_string_bytes:
                raise StrictJsonError(f"{location}: string exceeds {max_string_bytes} UTF-8 bytes")
        elif type(current) is float and not math.isfinite(current):
            raise StrictJsonError(f"{location}: non-finite JSON number is not allowed")


def loads_strict(
    data: bytes,
    *,
    source: str,
    max_bytes: int,
    max_depth: int,
    max_collection_entries: int,
    max_string_bytes: int,
) -> Any:
    """Decode bounded UTF-8 JSON and reject duplicates, non-finite values, and deep shapes."""

    if not data:
        raise StrictJsonError(f"{source}: JSON input is empty")
    if len(data) > max_bytes:
        raise StrictJsonError(f"{source}: input is {len(data)} bytes; limit is {max_bytes}")

    try:
        text = data.decode("utf-8", errors="strict")
        value = json.loads(text, object_pairs_hook=_unique_object, parse_constant=_reject_constant)
    except UnicodeDecodeError as exc:
        raise StrictJsonError(f"{source}: input is not valid UTF-8") from exc
    except json.JSONDecodeError as exc:
        raise StrictJsonError(f"{source}: invalid JSON: {exc}") from exc
    except RecursionError as exc:
        raise StrictJsonError(f"{source}: JSON nesting exceeds parser limits") from exc

    validate_json_shape(
        value,
        max_depth=max_depth,
        max_collection_entries=max_collection_entries,
        max_string_bytes=max_string_bytes,
    )
    return value
