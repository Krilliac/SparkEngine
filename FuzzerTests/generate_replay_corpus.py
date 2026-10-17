#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production .replay loader (ReplaySystem::LoadFromFile)."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "replay-system"

MAGIC = 0x52504C59  # "RPLY"
NAN = float("nan")


def string(value: bytes) -> bytes:
    return struct.pack("<I", len(value)) + value


def entity(entity_id: int, x: float) -> bytes:
    record = struct.pack("<I", entity_id)
    record += struct.pack("<3f", x, 0.0, 1.0)  # position
    record += struct.pack("<4f", 0.0, 0.0, 0.0, 1.0)  # rotation
    record += struct.pack("<3f", 1.0, 0.0, 0.0)  # velocity
    record += struct.pack("<f", 100.0)  # health
    record += struct.pack("<i", 2)  # animationState
    record += struct.pack("<I", 0x03)  # flags: Alive | Visible
    return record


def event(timestamp: float, kind: bytes, data: bytes) -> bytes:
    record = struct.pack("<f", timestamp) + string(kind)
    record += struct.pack("<II", 1, 2)  # source, target
    record += struct.pack("<3f", 0.5, 0.0, 0.5)
    return record + string(data)


def replay(duration: float, frames: list[tuple[float, list[bytes]]], events: list[bytes], version: int = 1) -> bytes:
    """The layout ReplaySystem::SaveToFile writes."""
    payload = struct.pack("<II", MAGIC, version)
    payload += string(b"arena") + string(b"deathmatch")
    payload += struct.pack("<f", duration)
    payload += struct.pack("<I", len(frames))
    for number, (timestamp, entities) in enumerate(frames):
        payload += struct.pack("<fII", timestamp, number, len(entities))
        payload += b"".join(entities)
    payload += struct.pack("<I", len(events))
    return payload + b"".join(events)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    two_frames = replay(1.0, [(0.0, [entity(7, 0.0), entity(9, 3.0)]), (1.0, [entity(7, 0.5)])],
                        [event(0.5, b"kill", b"headshot")])
    truncated = replay(1.0, [(0.0, [entity(7, 0.0), entity(9, 3.0)])], [])
    # Cut the file inside the second entity record.
    truncated = truncated[: len(truncated) - 4 - 20]
    count_overclaim = struct.pack("<II", MAGIC, 1) + string(b"") + string(b"") + struct.pack("<fI", 1.0, 1_000_000)
    seeds = {
        "valid-two-frames.replay": two_frames,
        "valid-empty.replay": replay(0.0, [], []),
        "truncated-entity.replay": truncated,
        "count-overclaim.replay": count_overclaim,
        # Accepted before LoadFromFile validated the timeline: UpdatePlayback never stopped on a NaN duration.
        "regression-nonfinite-duration.replay": replay(NAN, [], []),
        # Accepted before: FindFrameIndex's std::lower_bound needs sorted timestamps.
        "regression-descending-timestamps.replay": replay(2.0, [(2.0, [entity(7, 0.0)]), (1.0, [entity(7, 1.0)])], []),
        # Accepted before: the version field was read and never checked.
        "regression-unknown-version.replay": replay(1.0, [(0.0, []), (1.0, [])], [], version=0xFFFFFFFF),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
