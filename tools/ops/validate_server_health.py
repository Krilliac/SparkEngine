#!/usr/bin/env python3
"""OPS-110: validate a SparkServer health snapshot against its external contract.

SparkServer publishes one compact JSON object per status interval, to stdout
and to the atomically replaced ``--health-file``. Its contract is versioned
by the leading ``schema`` field (``spark-server-health/1``) and documented
field by field in wiki/advanced/Server-Operations-Runbook.md. This module is
the single parser every outside consumer uses (tools/ops/server_soak.py,
tools/ops/server_recovery_drill.py, and this CLI):

* The document is bounded, duplicate-free, finite JSON (ops_strict_json).
* The key set is exact: an unknown key or a missing key is a violation, so a
  field rename can never be read as a quietly absent measurement.
* Every value has its documented type and range; booleans are never accepted
  as integers.
* ``commit`` is a full lowercase 40-hex SHA or ``unknown``; ``treeState`` is
  ``clean``, ``dirty`` or ``unknown``.
* Tick percentiles are ordered (p50 <= p95 <= p99 <= max) and each queue
  peak is at least its current depth.

``build_identity_problem`` applies the exact-SHA rule: with an expected SHA,
an ``unknown`` commit, a dirty tree, or any other commit is refused, so
telemetry can never be attributed to a build other than the one that ran.

CLI: ``validate_server_health.py <health.json> [--expected-sha <sha>]`` exits
0 when the snapshot satisfies the contract, 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ops_strict_json import StrictJsonError, loads_strict  # noqa: E402

SCHEMA = "spark-server-health/1"
MAX_HEALTH_BYTES = 64 * 1024
MAX_STRING_BYTES = 16 * 1024
# Parser bound only; the exact key set is enforced after parsing so an extra key is named in the error.
MAX_KEYS = 64
UINT64_MAX = 2**64 - 1
TREE_STATES = frozenset({"clean", "dirty", "unknown"})
_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
_EXPECTED_SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")

BOOL_FIELDS = ("live", "ready", "draining", "stopping")
STRING_FIELDS = ("gameModule", "map", "error", "version")
COUNT_FIELDS = ("players", "ticks", "loadedModules", "tickSamples", "tickP50Us", "tickP95Us", "tickP99Us",
                "tickMaxUs", "netQueueIn", "netQueueOut", "netQueueInPeak", "netQueueOutPeak")
FIELDS = frozenset({"schema", "port", "commit", "treeState", "rssBytes", *BOOL_FIELDS, *STRING_FIELDS,
                    *COUNT_FIELDS})


class HealthContractError(ValueError):
    """A health snapshot that does not satisfy spark-server-health/1."""


def _is_count(value: Any, maximum: int = UINT64_MAX) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= maximum


def validate_health(value: Any, *, source: str = "health") -> dict[str, Any]:
    """Return ``value`` when it satisfies the contract; raise HealthContractError otherwise."""
    if not isinstance(value, dict):
        raise HealthContractError(f"{source}: snapshot is not a JSON object")
    if value.get("schema") != SCHEMA:
        raise HealthContractError(f"{source}: schema is {value.get('schema')!r}, expected {SCHEMA!r}")
    missing = sorted(FIELDS - value.keys())
    unknown = sorted(value.keys() - FIELDS)
    if missing or unknown:
        raise HealthContractError(f"{source}: key set differs from {SCHEMA} (missing {missing}, unknown {unknown})")

    problems: list[str] = []
    for key in BOOL_FIELDS:
        if not isinstance(value[key], bool):
            problems.append(f"{key} must be a boolean")
    for key in STRING_FIELDS:
        if not isinstance(value[key], str):
            problems.append(f"{key} must be a string")
    for key in COUNT_FIELDS:
        if not _is_count(value[key]):
            problems.append(f"{key} must be a non-negative 64-bit integer")
    if not _is_count(value["port"], 65535):
        problems.append("port must be an integer in [0, 65535]")
    if value["rssBytes"] is not None and not _is_count(value["rssBytes"]):
        problems.append("rssBytes must be a non-negative 64-bit integer or null")
    commit = value["commit"]
    if not isinstance(commit, str) or not (commit == "unknown" or _COMMIT_RE.fullmatch(commit)):
        problems.append(f"commit must be a lowercase 40-hex SHA or 'unknown', got {commit!r}")
    if value["treeState"] not in TREE_STATES:
        problems.append(f"treeState must be one of {sorted(TREE_STATES)}, got {value['treeState']!r}")
    if problems:
        raise HealthContractError(f"{source}: " + "; ".join(problems))

    if not value["tickP50Us"] <= value["tickP95Us"] <= value["tickP99Us"] <= value["tickMaxUs"]:
        raise HealthContractError(f"{source}: tick percentiles are not ordered p50 <= p95 <= p99 <= max")
    for depth, peak in (("netQueueIn", "netQueueInPeak"), ("netQueueOut", "netQueueOutPeak")):
        if value[depth] > value[peak]:
            raise HealthContractError(f"{source}: {peak} {value[peak]} is below the current {depth} {value[depth]}")
    return value


def parse_health(data: bytes, *, source: str = "health") -> dict[str, Any]:
    """Parse one serialized snapshot under the bounded strict-JSON policy and validate it."""
    if len(data) > MAX_HEALTH_BYTES:
        raise HealthContractError(f"{source}: snapshot exceeds {MAX_HEALTH_BYTES} bytes")
    try:
        value = loads_strict(data.strip(), source=source, max_bytes=MAX_HEALTH_BYTES, max_depth=1,
                             max_collection_entries=MAX_KEYS, max_string_bytes=MAX_STRING_BYTES)
    except StrictJsonError as exc:
        raise HealthContractError(str(exc)) from exc
    return validate_health(value, source=source)


def build_identity_problem(health: dict[str, Any], expected_sha: str | None) -> str | None:
    """Why ``health`` cannot be attributed to ``expected_sha``, or None when it can (or none is expected)."""
    if expected_sha is None:
        return None
    if not _EXPECTED_SHA_RE.fullmatch(expected_sha):
        return f"expected SHA {expected_sha!r} is not a full 40-character hexadecimal commit SHA"
    commit = health.get("commit")
    if commit == "unknown":
        return "server reports commit 'unknown'; an exact-SHA run needs a stamped build"
    if commit != expected_sha.lower():
        return f"server reports commit {commit!r}, expected {expected_sha.lower()!r}"
    if health.get("treeState") != "clean":
        return f"server was built from a {health.get('treeState')!r} tree, not the clean commit {commit!r}"
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("health", type=Path, help="health snapshot file (SparkServer --health-file)")
    parser.add_argument("--expected-sha", help="require the snapshot to come from this clean, stamped commit")
    args = parser.parse_args(argv)
    try:
        with args.health.open("rb") as stream:
            data = stream.read(MAX_HEALTH_BYTES + 1)
        health = parse_health(data, source=str(args.health))
    except OSError as exc:
        print(f"validate_server_health: FAIL: cannot read {args.health}: {exc}", file=sys.stderr)
        return 1
    except HealthContractError as exc:
        print(f"validate_server_health: FAIL: {exc}", file=sys.stderr)
        return 1
    problem = build_identity_problem(health, args.expected_sha)
    if problem is not None:
        print(f"validate_server_health: FAIL: {args.health}: {problem}", file=sys.stderr)
        return 1
    print(f"validate_server_health: PASS: {args.health} satisfies {SCHEMA}"
          + (f" for {args.expected_sha.lower()}" if args.expected_sha else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
