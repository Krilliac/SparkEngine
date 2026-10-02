#!/usr/bin/env python3
"""Generate deterministic seeds for spark.modules.json parsing."""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "launcher-module-manifest"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    seeds = {
        "valid-game.json": b'{"modules":[{"path":"build/Release/libGame.so"}]}',
        "valid-multiple.json": b'{"modules":[{"path":"build/Release/libGame.so"},{"path":"mods/Local.so"}]}',
        "malformed-whitespace.json": b" ",
        "malformed-no-modules.json": b'{}',
        "malformed-module-not-array.json": b'{"modules":{}}',
        "malformed-empty-array.json": b'{"modules":[]}',
        "malformed-path-not-string.json": b'{"modules":[{"path":7}]}',
        "malformed-traversal.json": b'{"modules":[{"path":"../outside/Evil.so"}]}',
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
