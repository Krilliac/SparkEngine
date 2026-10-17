#!/usr/bin/env python3
"""Generate deterministic seeds for the asset cooker source-tree reader."""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "asset-cooker-input"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    seeds = {
        "valid-single.bin": bytes([0, 5]) + b"a.txt" + bytes([5]) + b"hello",
        "valid-nested.bin": bytes([0, 9]) + b"dir/file.txt" + bytes([3]) + b"abc",
        "malformed-truncated-entry.bin": bytes([0, 8]) + b"short",
        "malformed-directory.bin": bytes([1, 3]) + b"dir" + b"\x00",
        "malformed-link.bin": bytes([2, 7]) + b"escape" + b"\x00",
        "malformed-fifo.bin": bytes([4, 4]) + b"pipe" + b"\x00",
        "malformed-manifest-name.bin": bytes([0, 22]) + b"spark-cook-manifest.json" + bytes([1]) + b"x",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
