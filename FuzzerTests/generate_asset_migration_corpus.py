#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the versioned asset migration reader.

Each seed is one byte naming the asset type the caller expects (Scene = 1, Material = 2,
Prefab = 3, ...) followed by a SPRK asset file: a 32-byte little-endian AssetFileHeader
[magic u32 "SPRK" = 0x5350524B][version major/minor/patch u16][asset type u8][pad u8]
[checksum u32 = CRC-32 of the payload][headerSize u32][pad u32][dataSize u64], then the
payload. The fuzz adapter (FuzzAssetMigrationProduction.cpp) registers a Scene 1.0.0 ->
1.1.0 step that reads a [u32 count][u32-length strings] payload, an any-type 1.1.0 -> 2.0.0
step and a Material 1.0.0 -> 2.0.0 step; Prefab stays current at 1.0.0.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "asset-migration"

MAGIC = 0x5350524B
HEADER_BYTES = 32
UNKNOWN, SCENE, MATERIAL, PREFAB, SAVEGAME = 0, 1, 2, 3, 4


def crc(payload: bytes) -> int:
    return zlib.crc32(payload) & 0xFFFFFFFF if payload else 0


def asset(
    payload: bytes,
    *,
    version: tuple[int, int, int] = (1, 0, 0),
    asset_type: int = SCENE,
    header_size: int = HEADER_BYTES,
    checksum: int | None = None,
    data_size: int | None = None,
    magic: int = MAGIC,
    extension: bytes = b"",
) -> bytes:
    header = struct.pack(
        "<IHHHBxIIxxxxQ",
        magic,
        *version,
        asset_type,
        crc(payload) if checksum is None else checksum,
        header_size,
        len(payload) if data_size is None else data_size,
    )
    return header + extension + payload


def names(*values: bytes) -> bytes:
    return struct.pack("<I", len(values)) + b"".join(struct.pack("<I", len(v)) + v for v in values)


def seed(expected_type: int, file: bytes) -> bytes:
    return bytes([expected_type]) + file


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    scene_names = names(b"Player", b"Camera", b"")
    seeds = {
        "valid-scene-v1.sprk": seed(SCENE, asset(scene_names)),
        "valid-scene-v1-1.sprk": seed(SCENE, asset(b"\x00\x01\x02 any payload", version=(1, 1, 0))),
        "valid-material-v1.sprk": seed(MATERIAL, asset(b"material payload", asset_type=MATERIAL)),
        "valid-current-prefab.sprk": seed(PREFAB, asset(b"current prefab", asset_type=PREFAB)),
        "valid-empty-payload.sprk": seed(SAVEGAME, asset(b"", version=(1, 1, 0), asset_type=SAVEGAME)),
        "scene-bad-names.sprk": seed(SCENE, asset(struct.pack("<I", 0xFFFF) + b"\x05\x00\x00\x00ab")),
        "scene-trailing-bytes.sprk": seed(SCENE, asset(scene_names + b"junk")),
        "type-mismatch.sprk": seed(MATERIAL, asset(scene_names)),
        "future-version.sprk": seed(SCENE, asset(scene_names, version=(3, 0, 0))),
        "no-path-version.sprk": seed(SAVEGAME, asset(b"old", version=(0, 9, 0), asset_type=SAVEGAME)),
        "bad-magic.sprk": seed(SCENE, asset(scene_names, magic=0x4B525053)),
        "unknown-asset-type.sprk": seed(9, asset(b"x", asset_type=9)),
        "header-size-past-end.sprk": seed(SCENE, asset(b"abc", version=(1, 1, 0), header_size=4096)),
        "truncated-header.sprk": seed(SCENE, asset(scene_names)[:20]),
        "malformed-unverified-checksum.sprk": seed(
            SCENE, asset(b"corrupt payload", version=(1, 1, 0), checksum=0x12345678)
        ),
        "malformed-wrong-data-size.sprk": seed(SCENE, asset(b"short payload", version=(1, 1, 0), data_size=99)),
        "malformed-longer-header.sprk": seed(
            SCENE, asset(b"payload after a 48-byte header", version=(1, 1, 0), header_size=48, extension=b"\xee" * 16)
        ),
        "malformed-short-header-size.sprk": seed(SCENE, asset(b"", version=(1, 1, 0), header_size=8, data_size=24)),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
