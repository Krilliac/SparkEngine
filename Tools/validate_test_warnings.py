#!/usr/bin/env python3
"""Validate ownership and expiry metadata for whole-test flaky waivers."""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import date
from pathlib import Path


MAX_WAIVERS = 256
OWNER_RE = re.compile(r"^[A-Za-z][A-Za-z0-9._/-]{1,63}$")
PLACEHOLDER_OWNERS = {"none", "n/a", "tbd", "todo", "unassigned", "unknown"}
ENTRY_RE = re.compile(
    r'\{\s*"((?:\\.|[^"\\])*)"\s*,\s*"((?:\\.|[^"\\])*)"\s*\}',
    re.DOTALL,
)


class PolicyError(ValueError):
    """A repository policy or schema violation."""


def parse_cpp_string(value: str) -> str:
    try:
        return json.loads(f'"{value}"')
    except json.JSONDecodeError as exc:
        raise PolicyError(f"invalid C++ string literal in TestWarnings.h: {exc}") from exc


def load_registry(path: Path) -> list[str]:
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise PolicyError(f"cannot read registry {path}: {exc}") from exc

    marker = "g_testWarningPatterns[]"
    marker_index = source.find(marker)
    if marker_index < 0:
        raise PolicyError("TestWarnings.h does not declare g_testWarningPatterns")
    opening = source.find("=", marker_index)
    closing = source.find("};", opening)
    if opening < 0 or closing < 0:
        raise PolicyError("TestWarnings.h warning registry has no complete initializer")

    patterns = [parse_cpp_string(match.group(1)) for match in ENTRY_RE.finditer(source[opening:closing])]
    if not patterns:
        raise PolicyError("TestWarnings.h warning registry is empty")
    if len(patterns) > MAX_WAIVERS:
        raise PolicyError(f"warning registry has {len(patterns)} entries; maximum is {MAX_WAIVERS}")

    duplicates = sorted({pattern for pattern in patterns if patterns.count(pattern) > 1})
    if duplicates:
        raise PolicyError(f"duplicate pattern in TestWarnings.h: {', '.join(duplicates)}")
    if any(not pattern.strip() for pattern in patterns):
        raise PolicyError("TestWarnings.h contains an empty warning pattern")
    return patterns


def load_metadata(path: Path, as_of: date) -> dict[str, dict[str, str]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PolicyError(f"cannot read warning metadata {path}: {exc}") from exc

    if not isinstance(payload, dict) or set(payload) != {"schemaVersion", "waivers"}:
        raise PolicyError("warning metadata must contain exactly schemaVersion and waivers")
    if payload["schemaVersion"] != 1:
        raise PolicyError("warning metadata schemaVersion must be 1")
    waivers = payload["waivers"]
    if not isinstance(waivers, list):
        raise PolicyError("warning metadata waivers must be a list")
    if len(waivers) > MAX_WAIVERS:
        raise PolicyError(f"warning metadata has {len(waivers)} entries; maximum is {MAX_WAIVERS}")

    result: dict[str, dict[str, str]] = {}
    for index, entry in enumerate(waivers):
        label = f"warning metadata waivers[{index}]"
        if not isinstance(entry, dict) or set(entry) != {"pattern", "owner", "expires"}:
            raise PolicyError(f"{label} must contain exactly pattern, owner, and expires")
        pattern, owner, expires = (entry[key] for key in ("pattern", "owner", "expires"))
        if not all(isinstance(value, str) for value in (pattern, owner, expires)):
            raise PolicyError(f"{label} pattern, owner, and expires must be strings")
        if not pattern.strip():
            raise PolicyError(f"{label} pattern must not be empty")
        if pattern in result:
            raise PolicyError(f"duplicate pattern in warning metadata: {pattern}")
        if not OWNER_RE.fullmatch(owner) or owner.lower() in PLACEHOLDER_OWNERS:
            raise PolicyError(f"{label} owner must be a named owner, got {owner!r}")
        try:
            expiry = date.fromisoformat(expires)
        except ValueError as exc:
            raise PolicyError(f"{label} expires must be an ISO date (YYYY-MM-DD)") from exc
        if expiry.isoformat() != expires:
            raise PolicyError(f"{label} expires must use YYYY-MM-DD")
        if expiry <= as_of:
            raise PolicyError(f"{label} is expired as of {as_of.isoformat()}")
        result[pattern] = {"owner": owner, "expires": expires}
    return result


def validate(registry_path: Path, metadata_path: Path, as_of: date) -> int:
    registry_patterns = load_registry(registry_path)
    metadata = load_metadata(metadata_path, as_of)
    registry_set = set(registry_patterns)
    metadata_set = set(metadata)
    missing = sorted(registry_set - metadata_set)
    extra = sorted(metadata_set - registry_set)
    if missing:
        raise PolicyError(f"missing metadata for registry pattern(s): {', '.join(missing)}")
    if extra:
        raise PolicyError(
            "metadata pattern(s) not present in TestWarnings.h: " + ", ".join(extra)
        )
    print(f"validated {len(registry_patterns)} flaky waiver(s) with owner and expiry metadata")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=Path("Tests/TestWarnings.h"))
    parser.add_argument("--metadata", type=Path, default=Path("Tests/test-warning-waivers.json"))
    parser.add_argument(
        "--as-of",
        type=date.fromisoformat,
        default=date.today(),
        help="policy date for deterministic tests (default: today)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return validate(args.registry, args.metadata, args.as_of)
    except PolicyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
