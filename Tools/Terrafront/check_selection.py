#!/usr/bin/env python3
"""Fail closed unless CTest selected exactly the Terrafront process gate."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


SCENARIOS = (
    "OnboardSpawnMove", "CombatKillRespawn", "ForgedStateRejectedAndAudited",
    "ReconnectRestoresAllowedState", "TerritoryReplicates", "VehicleLifecycle",
)
EXPECTED = {
    "terrafront-multiclient": (
        *(f"TerrafrontMultiClient_{name}" for name in SCENARIOS),
        "TerrafrontMultiClient_ContinentMismatchRefused",
        *(f"TerrafrontMultiClient_ImpairedConvergence_{name}" for name in SCENARIOS),
        "TerrafrontRestart_ColdRestartRestoresAuthoritativeState",
        "TerrafrontRestart_UngracefulKillRestoresLastCommit",
    ),
    "terrafront-soak": ("TerrafrontSoak_Short",),
}


def check(document: dict, label: str) -> None:
    if document.get("kind") != "ctestInfo" or document.get("version", {}).get("major") != 1:
        raise ValueError("expected ctest --show-only=json-v1 output")
    tests = document["tests"]
    names = [test["name"] for test in tests]
    if sorted(names) != sorted(EXPECTED[label]):
        raise ValueError(f"{label}: expected {sorted(EXPECTED[label])}, got {sorted(names)}")
    for test in tests:
        properties = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
        if label not in properties.get("LABELS", []) or not test.get("command"):
            raise ValueError(f"{test['name']}: missing label or executable command")
    print(f"{label}: exactly {len(names)} expected tests selected")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("selection", type=Path)
    parser.add_argument("--label", choices=EXPECTED, required=True)
    args = parser.parse_args()
    try:
        check(json.loads(args.selection.read_text(encoding="utf-8-sig")), args.label)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Terrafront selection failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
