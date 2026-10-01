#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the achievement progress reader.

Each seed is an AchievementSystem::SaveToWriter payload or a damaged variant: a u32 record
count, then records of [id u32][progress f32][unlocked u8][unlock timestamp u64], all
little-endian. The fuzz adapter (FuzzAchievementProgressProduction.cpp) registers ids 1, 2,
3, 5, 8, 13 and 0xFFFFFFFF (targets 1, 10, 100.5, 0, 3, 2.5 and 1) before loading a seed.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "achievement-definitions"

NAN_BITS = 0x7FC00000
INF_BITS = 0x7F800000


def record(achievement_id: int, value: float, unlocked: int = 0, timestamp: int = 0) -> bytes:
    return struct.pack("<IfBQ", achievement_id, value, unlocked, timestamp)


def raw_record(achievement_id: int, value_bits: int, unlocked: int = 0, timestamp: int = 0) -> bytes:
    return struct.pack("<IIBQ", achievement_id, value_bits, unlocked, timestamp)


def payload(records: list[bytes], count: int | None = None) -> bytes:
    return struct.pack("<I", len(records) if count is None else count) + b"".join(records)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    typical = [
        record(1, 1.0, 1, 1_790_000_000),
        record(2, 7.0),
        record(3, 100.5, 1, 1_790_000_123),
        record(8, 2.0),
        record(13, 0.5),
    ]
    seeds = {
        "valid-typical.bin": payload(typical),
        "valid-single.bin": payload([record(2, 9.5)]),
        "valid-zero-target.bin": payload([record(5, 0.0, 1, 1)]),
        "valid-max-id.bin": payload([record(0xFFFFFFFF, 1.0, 1, 0xFFFFFFFFFFFFFFFF)]),
        "unregistered-ids.bin": payload([record(4, 1.0, 1), record(1_000_000, 3.0), record(0, 0.0)]),
        "out-of-range-progress.bin": payload([record(2, 1.0e9), record(8, -4.0), record(13, 3.0e38)]),
        "infinite-progress.bin": payload([raw_record(2, INF_BITS), raw_record(8, INF_BITS | 0x80000000)]),
        "duplicate-records.bin": payload([record(2, 1.0), record(2, 6.0, 1), record(2, 3.0)]),
        "truncated-record.bin": payload(typical)[:-7],
        "count-overstated.bin": payload([record(1, 0.25)], count=0xFFFFFFFF),
        "count-only.bin": struct.pack("<I", 3),
        "short-count.bin": b"\x02\x00",
        "malformed-nonfinite-progress.bin": payload([raw_record(8, NAN_BITS), record(2, 2.0)]),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
