#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the bounded OpenEXR loader.

Spark::Graphics::EXRLoader::Load (SparkEngine/Source/Graphics/EXRLoader.h)
pre-bounds tinyexr: 16 MiB input, 16384 pixels per side, R/G/B[/A] half or
float channels, a 128 MiB working set, and single-part scanline files only.

Each seed is a hand-written OpenEXR scanline file: the magic 76 2f 31 01, a
version word (2 plus the tiled/multipart flag bits), the attribute list
(name NUL, type NUL, little-endian int32 size, value) closed by a NUL byte, the
chunk offset table (one little-endian uint64 per scanline block), then the
chunks (int32 first line, int32 byte count, data). NO_COMPRESSION blocks hold
one scanline; ZIP blocks hold 16 and are the zlib stream of OpenEXR's
byte-interleaved, delta-predicted scanline bytes.

The committed bytes are pinned by tools/fuzz-policy/corpus-manifest.json; this
script records how each seed was built so a reviewer can regenerate and diff.
regression-header-error-leak.exr is a mutation-run reproducer kept byte-exact;
it is not generated here.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "texture-exr-loader"

MAGIC = b"\x76\x2f\x31\x01"
TILED_FLAG = 0x200
MULTIPART_FLAG = 0x1000
HALF, FLOAT = 1, 2
NO_COMPRESSION, ZIP_COMPRESSION = 0, 3
HALF_ONE = 0x3C00
HALF_HALF = 0x3800


def attribute(name: str, type_name: str, value: bytes) -> bytes:
    return name.encode() + b"\0" + type_name.encode() + b"\0" + struct.pack("<i", len(value)) + value


def channel_list(channels: list[tuple[str, int]]) -> bytes:
    body = b""
    for name, pixel_type in channels:
        # name, pixel type, pLinear + 3 reserved bytes, x/y sampling
        body += name.encode() + b"\0" + struct.pack("<iB3xii", pixel_type, 0, 1, 1)
    return body + b"\0"


def header(
    channels: list[tuple[str, int]],
    width: int,
    height: int,
    compression: int,
    *,
    flags: int = 0,
    extra: bytes = b"",
) -> bytes:
    box = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    attributes = (
        attribute("channels", "chlist", channel_list(channels))
        + attribute("compression", "compression", bytes([compression]))
        + attribute("dataWindow", "box2i", box)
        + attribute("displayWindow", "box2i", box)
        + attribute("lineOrder", "lineOrder", b"\0")
        + attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
        + attribute("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
        + attribute("screenWindowWidth", "float", struct.pack("<f", 1.0))
        + extra
    )
    return MAGIC + struct.pack("<I", 2 | flags) + attributes + b"\0"


def scanline(channels: list[tuple[str, int]], width: int, value: float) -> bytes:
    """One scanline: every channel's run of `width` samples, in channel-list order."""
    line = b""
    for _, pixel_type in channels:
        if pixel_type == HALF:
            half = HALF_ONE if value == 1.0 else HALF_HALF
            line += struct.pack("<H", half) * width
        else:
            line += struct.pack("<f", value) * width
    return line


def zip_block(raw: bytes) -> bytes:
    """OpenEXR ZIP: interleave even/odd bytes, delta-predict, then zlib-compress."""
    reordered = bytearray(raw[0::2] + raw[1::2])
    for index in range(len(reordered) - 1, 0, -1):
        reordered[index] = (reordered[index] - reordered[index - 1] + 128) & 0xFF
    return zlib.compress(bytes(reordered), 9)


def scanline_file(
    channels: list[tuple[str, int]],
    width: int,
    height: int,
    *,
    compression: int = NO_COMPRESSION,
    flags: int = 0,
) -> bytes:
    lines_per_block = 16 if compression == ZIP_COMPRESSION else 1
    blocks = []
    for first in range(0, height, lines_per_block):
        count = min(lines_per_block, height - first)
        raw = b"".join(scanline(channels, width, 0.5 if (first + line) % 2 else 1.0) for line in range(count))
        data = zip_block(raw) if compression == ZIP_COMPRESSION else raw
        if compression == ZIP_COMPRESSION and len(data) >= len(raw):
            raise ValueError("ZIP seed must actually compress so the inflate path runs")
        blocks.append(struct.pack("<ii", first, len(data)) + data)

    head = header(channels, width, height, compression, flags=flags)
    offset = len(head) + 8 * len(blocks)
    table = b""
    for block in blocks:
        table += struct.pack("<Q", offset)
        offset += len(block)
    return head + table + b"".join(blocks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    rgb_half = [("B", HALF), ("G", HALF), ("R", HALF)]
    rgba_float = [("A", FLOAT), ("B", FLOAT), ("G", FLOAT), ("R", FLOAT)]

    valid_rgb = scanline_file(rgb_half, 2, 2)
    valid_rgba = scanline_file(rgba_float, 2, 2)
    # 16x16 constant rows compress well, so tinyexr really inflates the block.
    valid_zip = scanline_file(rgb_half, 16, 16, compression=ZIP_COMPRESSION)

    # EXRLoader rejects tiled and multipart files from the version flags alone.
    tiled = scanline_file(rgb_half, 2, 2, flags=TILED_FLAG)
    multipart = scanline_file(rgb_half, 2, 2, flags=MULTIPART_FLAG)

    # One pixel over the 16384 per-side cap. The header alone decides; the body
    # is a single scanline block that never gets read.
    wide = header(rgb_half, 16385, 1, NO_COMPRESSION) + struct.pack("<Q", 0)

    # A 16384x16384 float data window (inside the per-side cap) declared by a
    # few hundred bytes. chunkCount = 1 shrinks tinyexr's offset table to one
    # entry, so without EXRLoader's working-set bound tinyexr allocates each
    # 1 GiB channel plane before it discovers the body is missing.
    overclaim_channels = [("B", FLOAT), ("G", FLOAT), ("R", FLOAT)]
    overclaim_head = header(
        overclaim_channels,
        16384,
        16384,
        NO_COMPRESSION,
        extra=attribute("chunkCount", "int", struct.pack("<i", 1)),
    )
    overclaim_block = struct.pack("<ii", 0, 12) + struct.pack("<fff", 1.0, 1.0, 1.0)
    overclaim = overclaim_head + struct.pack("<Q", len(overclaim_head) + 8) + overclaim_block

    # A luminance-only channel list: tinyexr decodes it, EXRLoader refuses it.
    luminance = scanline_file([("Y", HALF)], 2, 2)

    # The offset table stops half way through its second entry.
    truncated = valid_rgb[: len(header(rgb_half, 2, 2, NO_COMPRESSION)) + 12]

    seeds = {
        "valid-2x2-rgb-half-none.exr": valid_rgb,
        "valid-2x2-rgba-float-none.exr": valid_rgba,
        "valid-zip-compressed.exr": valid_zip,
        "tiled-flag.exr": tiled,
        "multipart-flag.exr": multipart,
        "width-16385.exr": wide,
        "data-window-overclaim-small-body.exr": overclaim,
        "unsupported-channel-Y.exr": luminance,
        "truncated-offset-table.exr": truncated,
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} EXR seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
