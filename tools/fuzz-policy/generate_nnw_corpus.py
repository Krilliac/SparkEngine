#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production .nnw loader."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "neural-weights-nnw"
MAGIC = 0x574E4E53
OPTIMIZER_FLAG = 0x01


def network(version: int, layers: list[tuple[int, int, int]], optimizer: int = 0) -> bytes:
    total = sum(inputs * outputs + outputs for inputs, outputs, _ in layers)
    flags = OPTIMIZER_FLAG if optimizer else 0
    payload = bytearray(struct.pack("<IIII", MAGIC, version, len(layers), total))
    if version >= 2:
        payload.extend(struct.pack("<I", flags))
    for layer in layers:
        payload.extend(struct.pack("<III", *layer))
    payload.extend(struct.pack(f"<{total}f", *(index * 0.25 for index in range(total))))
    if optimizer:
        payload.extend(struct.pack("<II", optimizer, 17))
        payload.extend(struct.pack(f"<{total}f", *([0.1] * total)))
        if optimizer == 1:
            payload.extend(struct.pack(f"<{total}f", *([0.01] * total)))
    return bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    valid_v1 = network(1, [(0, 1, 4)])
    valid_v2 = network(2, [(2, 2, 0)])
    valid_adam = network(2, [(2, 1, 4)], optimizer=1)
    valid_sgd = network(2, [(2, 1, 4)], optimizer=2)
    seeds = {
        "valid-v1-constant.nnw": valid_v1,
        "valid-v2-plain.nnw": valid_v2,
        "valid-v2-adam.nnw": valid_adam,
        "valid-v2-sgd.nnw": valid_sgd,
        "truncated-header.nnw": valid_v2[:8],
        "truncated-layer.nnw": valid_v2[:24],
        "truncated-weights.nnw": valid_v2[:-1],
        "truncated-optimizer.nnw": valid_adam[:-1],
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} neural-weight seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
