#!/usr/bin/env python3
"""Capture producer-owned provenance for configured CMake File API replies.

Run this only after CMake has completed the configure/generate step. The tool
derives the repository commit itself and binds it to digests of the exact index,
codemodel, cache, and target documents that the inventory will later consume.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import inventory


def parse_codemodels(values: list[str]) -> dict[str, Path]:
    return {
        profile: Path(directory)
        for profile, directory in inventory._parse_key_value_args(values, "--codemodel").items()
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--codemodel",
        action="append",
        required=True,
        metavar="PROFILE=BUILD_DIR",
        help="configured profile and build directory to bind (repeatable)",
    )
    args = parser.parse_args(argv)
    try:
        captured = []
        for profile, build_dir in sorted(parse_codemodels(args.codemodel).items()):
            path = inventory.capture_codemodel_provenance(build_dir, profile)
            captured.append({"profile": profile, "record": path.as_posix()})
        print(json.dumps({"schemaVersion": 1, "captured": captured}, indent=2))
        return 0
    except (inventory.InventoryError, OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"schemaVersion": 1, "state": "internal-error", "internalError": str(error)}, indent=2))
        print(f"INTERNAL ERROR: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
