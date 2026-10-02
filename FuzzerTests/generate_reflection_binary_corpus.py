#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the reflection binary field decoder.

Each seed is a Spark::SerializeToBinary stream ([index:u16 LE][tag:u8] then [len:u32 LE][bytes]
for strings or [size:u16 LE][bytes] for plain fields) for the record the fuzz adapter
(FuzzReflectionBinaryProduction.cpp) reflects, or a damaged variant. Field indices follow
that record: 0 flag Bool, 1 count Int, 2 scale Float, 3 precise Double, 4 v2 Vector2,
5 v3 Vector3, 6 v4 Vector4, 7 mode Enum, 8 name String, 9 custom Custom, 10 unknown Unknown,
11 hidden Int (not serialized), 12 note String.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "reflection-binary-codec"

UNKNOWN, BOOL, INT, FLOAT, DOUBLE, STRING, VEC2, VEC3, VEC4, ENUM, CUSTOM = range(11)


def plain(index: int, tag: int, payload: bytes) -> bytes:
    return struct.pack("<HBH", index, tag, len(payload)) + payload


def text(index: int, value: bytes) -> bytes:
    return struct.pack("<HBI", index, STRING, len(value)) + value


def full_record() -> bytes:
    return b"".join(
        [
            plain(0, BOOL, b"\x01"),
            plain(1, INT, struct.pack("<i", -7)),
            plain(2, FLOAT, struct.pack("<f", 0.5)),
            plain(3, DOUBLE, struct.pack("<d", -1.25)),
            plain(4, VEC2, struct.pack("<2f", 1, 2)),
            plain(5, VEC3, struct.pack("<3f", 3, 4, 5)),
            plain(6, VEC4, struct.pack("<4f", 6, 7, 8, 9)),
            plain(7, ENUM, struct.pack("<I", 0)),
            text(8, b"decoded-name"),
            text(12, b"decoded-note"),
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    full = full_record()
    seeds = {
        "valid-full-record.bin": full,
        "valid-strings-only.bin": text(8, b"") + text(12, b"\x00\xff multi\nline"),
        "valid-nonfinite-floats.bin": plain(2, FLOAT, struct.pack("<I", 0x7FC00001)) + plain(3, DOUBLE, b"\xff" * 8),
        "bool-nonzero-byte.bin": plain(0, BOOL, b"\x7f"),
        "type-mismatch.bin": plain(1, FLOAT, struct.pack("<f", 3.0)) + plain(8, INT, b"\x01\x00\x00\x00"),
        "size-mismatch.bin": plain(1, INT, b"\x01\x00") + plain(6, VEC4, b"\x00" * 12),
        "custom-unknown-hidden.bin": plain(9, CUSTOM, b"\x41" * 24) + plain(10, UNKNOWN, b"\x00" * 4)
        + plain(11, INT, b"\xff\xff\xff\xff"),
        "unknown-index.bin": plain(999, INT, b"\x00" * 4) + text(65535, b"skipped"),
        "truncated-header.bin": full + b"\x08\x00",
        "truncated-string.bin": struct.pack("<HBI", 8, STRING, 1000) + b"short",
        "huge-string-length.bin": struct.pack("<HBI", 12, STRING, 0xFFFFFFFF) + b"x",
        "truncated-plain.bin": struct.pack("<HBH", 3, DOUBLE, 8) + b"\x00\x00",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
