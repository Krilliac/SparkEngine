#!/usr/bin/env python3
"""Run one CMake configure and capture its owned File API transaction.

This command creates a unique stateful client query before invoking CMake.  It
will not sign or relabel a pre-existing reply directory.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import inventory


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, help="canonical stable-v1 build profile id")
    parser.add_argument("--build-dir", required=True, type=Path, help="canonical profile build directory")
    parser.add_argument("--cmake", default="cmake", help="exact CMake executable to invoke")
    parser.add_argument(
        "--build",
        action="store_true",
        help="build all configured targets and bind immutable post-build artifact identities",
    )
    args = parser.parse_args(argv)
    try:
        path = inventory.capture_codemodel_transaction(
            args.build_dir,
            args.profile,
            cmake_executable=args.cmake,
            build=args.build,
        )
        print(
            json.dumps(
                {"schemaVersion": 2, "profile": args.profile, "record": path.as_posix()},
                indent=2,
            )
        )
        return 0
    except (inventory.InventoryError, OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"schemaVersion": 2, "state": "internal-error", "internalError": str(error)}, indent=2))
        print(f"INTERNAL ERROR: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
