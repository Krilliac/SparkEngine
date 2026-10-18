#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production .skel/.sanim decoders (AnimationBinaryFormat)."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "animation-skel-sanim"

NAN = float("nan")
IDENTITY = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)


def name(value: bytes) -> bytes:
    return struct.pack("<I", len(value)) + value


def bone(bone_name: bytes, parent: int) -> bytes:
    return name(bone_name) + struct.pack("<i", parent) + struct.pack("<16f", *IDENTITY) * 2


def skeleton(bones: list[bytes], version: int = 1) -> bytes:
    return b"SKEL" + struct.pack("<II", version, len(bones)) + b"".join(bones)


def channel(bone_name: bytes, positions: list[float], rotations: int = 1, scales: int = 1) -> bytes:
    record = name(bone_name) + struct.pack("<i", 0)
    record += struct.pack("<I", len(positions))
    for time in positions:
        record += struct.pack("<4f", time, 0.0, 1.0, 0.0)
    record += struct.pack("<I", rotations)
    for index in range(rotations):
        record += struct.pack("<5f", float(index), 0.0, 0.0, 0.0, 1.0)
    record += struct.pack("<I", scales)
    for index in range(scales):
        record += struct.pack("<4f", float(index), 1.0, 1.0, 1.0)
    return record


def clip(clip_name: bytes, duration: float, rate: float, channels: list[bytes]) -> bytes:
    return name(clip_name) + struct.pack("<ffBI", duration, rate, 1, len(channels)) + b"".join(channels)


def clips(records: list[bytes], version: int = 1) -> bytes:
    return b"ANIM" + struct.pack("<II", version, len(records)) + b"".join(records)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    walk = clips([clip(b"walk", 1.0, 30.0, [channel(b"root", [0.0, 0.5, 1.0]), channel(b"spine", [0.0])])])
    truncated_keys = clips([clip(b"walk", 1.0, 30.0, [channel(b"root", [0.0, 0.5, 1.0])])])
    truncated_keys = truncated_keys[: len(truncated_keys) - 40]
    # 60 bytes declaring 1,000,000 rotation keys and holding none.
    amplification = clips([name(b"") + struct.pack("<ffBI", 1.0, 30.0, 0, 1)
                           + name(b"") + struct.pack("<iII", -1, 0, 1_000_000)])
    amplification += bytes(60 - len(amplification))
    seeds = {
        "valid-two-bones.skel": skeleton([bone(b"root", -1), bone(b"spine", 0)]),
        "valid-one-clip.sanim": walk,
        "truncated-keys.sanim": truncated_keys,
        "bad-parent.skel": skeleton([bone(b"root", -1), bone(b"spine", 1)]),
        # The streaming loader resized the rotation keys to the declared count (20 MB) before reading any.
        "regression-key-count-amplification.sanim": amplification,
        # Accepted before: playback computes fmod(time, duration) and time / duration.
        "regression-nonfinite-clip-timing.sanim": clips([clip(b"walk", NAN, 30.0, [channel(b"root", [0.0])])]),
        # Accepted before: key sampling assumes key times are sorted ascending.
        "regression-decreasing-key-times.sanim": clips([clip(b"walk", 1.0, 30.0, [channel(b"root", [1.0, 0.5])])]),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for seed_name, payload in sorted(seeds.items()):
        (args.output / seed_name).write_bytes(payload)
        print(f"{seed_name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
