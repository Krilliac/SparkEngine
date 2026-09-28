#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the runtime .sparkterrain decoder (DecodeRuntime)."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "terrain-sparkterrain"
MAGIC = 0x53504B54  # 'SPKT'
VERSION = 2


def string(text: bytes) -> bytes:
    return struct.pack("<I", len(text)) + text


def terrain(
    width: int = 3,
    height: int = 3,
    *,
    name: bytes = b"Seed",
    size: float = 64.0,
    heights: int | None = None,
    layer_count: int | None = None,
    layers: int = 1,
    splat_resolution: int = 2,
    splat_bytes: int | None = None,
) -> bytes:
    """The layout TerrainEditor::SaveTerrain writes; the keyword overrides break one field at a time."""
    payload = bytearray(struct.pack("<II", MAGIC, VERSION))
    payload += string(name)
    payload += struct.pack("<f3fifB", size, 0.0, 0.0, 0.0, 3, 1.0, 1)
    payload += struct.pack("<ii3f", width, height, 1.0, 0.0, 10.0)
    sample_count = width * height if heights is None else heights
    payload += struct.pack(f"<{sample_count}f", *(index * 0.5 for index in range(sample_count)))
    payload += struct.pack("<I", layers if layer_count is None else layer_count)
    for _ in range(layers):
        payload += string(b"Grass") + string(b"Textures/Terrain/Grass_D.png") + string(b"") + string(b"")
        payload += struct.pack("<8f", 4.0, 4.0, 0.0, 0.0, 1.0, 0.0, 0.8, 1.0)
    payload += struct.pack("<i", splat_resolution)
    texels = splat_resolution * splat_resolution * 4 if splat_bytes is None else splat_bytes
    payload += bytes(index % 256 for index in range(texels))
    payload += struct.pack("<I", 0)  # detail meshes
    return bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    valid = terrain()
    seeds = {
        "valid-3x3.sparkterrain": valid,
        # 8x2 was accepted and published as resolution 8; the clipmap then indexed 64 of 16 samples.
        "regression-nonsquare-8x2.sparkterrain": terrain(8, 2),
        "heightmap-8194.sparkterrain": terrain(8194, 8194, heights=0),
        "layer-count-65.sparkterrain": terrain(layer_count=65, layers=0),
        "string-4097.sparkterrain": terrain(name=b"n" * 4097),
        "splat-overclaim.sparkterrain": terrain(splat_resolution=8192, splat_bytes=0),
        "nan-size.sparkterrain": terrain(size=float("nan")),
        "truncated-heights.sparkterrain": valid[:80],  # ends inside the 3x3 height samples
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} sparkterrain seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
