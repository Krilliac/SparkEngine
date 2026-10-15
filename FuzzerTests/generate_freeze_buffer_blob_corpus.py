#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the save-state freeze/thaw blob reader.

Each seed is a blob as FreezeSystem::FreezeAll lays it out for the two fixture subsystems in
FuzzFreezeBufferBlobProduction.cpp, or a damaged variant. The layout (little-endian, as on every
platform the engine ships for) is:

    u32 version (1) | u32 subsystem count (2)
    tag "Counter":  u32 length + bytes, then u32 count, string (u32 length + bytes), tag "counter-inner"
    tag "Legacy":   u32 length + bytes, then u32 saved size, then that many record bytes
    tag "END"

The fuzz adapter thaws each seed through FreezeSystem::ThawAll and also replays its bytes through a
FreezeBuffer read script, so the last seed is shaped for the raw primitive pass.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "freeze-buffer-blob"

RECORD_SIZE = 16  # sizeof(LegacyRecord): u32 id, float weight, u64 stamp


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def tag(text: str) -> bytes:
    raw = text.encode("ascii")
    return u32(len(raw)) + raw


def string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return u32(len(raw)) + raw


def record(ident: int, weight: float, stamp: int) -> bytes:
    return struct.pack("<IfQ", ident, weight, stamp)


def counter_body(count: int, label: str) -> bytes:
    return tag("Counter") + u32(count) + string(label) + tag("counter-inner")


def legacy_body(saved_size: int, payload: bytes) -> bytes:
    return tag("Legacy") + u32(saved_size) + payload


def blob(version: int = 1, count: int = 2, counter: bytes | None = None, legacy: bytes | None = None,
         end: bytes | None = None) -> bytes:
    return (
        u32(version)
        + u32(count)
        + (counter if counter is not None else counter_body(42, "hello"))
        + (legacy if legacy is not None else legacy_body(RECORD_SIZE, record(9, 1.5, 123456789)))
        + (end if end is not None else tag("END"))
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    valid = blob()
    seeds = {
        "valid-two-subsystems.bin": valid,
        "valid-empty-label.bin": blob(counter=counter_body(0, "")),
        "valid-legacy-short-record.bin": blob(legacy=legacy_body(8, record(5, 2.0, 0)[:8])),
        "valid-legacy-long-record.bin": blob(legacy=legacy_body(24, record(6, 3.5, 77) + bytes(range(8)))),
        "valid-trailing-bytes.bin": valid + bytes([0xDE, 0xAD, 0xBE, 0xEF]),
        "wrong-version.bin": blob(version=2),
        "wrong-subsystem-count.bin": blob(count=3),
        "missing-end-tag.bin": blob(end=b""),
        "wrong-tag-name.bin": blob(counter=tag("Kounter") + u32(1) + string("x") + tag("counter-inner")),
        "tag-length-overflow.bin": u32(1) + u32(2) + u32(0xFFFFFFFF) + b"Counter",
        "string-length-past-end.bin": u32(1) + u32(2) + tag("Counter") + u32(1) + u32(0xFFFFFFF0) + b"ab",
        "legacy-size-past-end.bin": blob(legacy=legacy_body(0xFFFFFFFF, record(1, 1.0, 1))),
        "truncated-mid-record.bin": valid[: len(valid) - len(tag("END")) - 6],
        # Read-script seed: selector bytes steer the raw FreezeBuffer pass through
        # u64, string, tag, legacy and u8 reads with small, exact and oversized sizes.
        "script-mixed-operations.bin": bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]) + u32(3) + b"abc"
        + tag("Counter") + bytes([0xFF] * 12),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
