#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the non-Windows texture file loader.

Texture::CreateFromFile (SparkEngine/Source/Graphics/TextureSystemLinuxTexture.cpp)
routes `.exr` files to the bounded EXRLoader and everything else to the
repo-authored stb_image stub (ThirdParty/Utils/stb/stb_image.h), which decodes
uncompressed 24/32-bit bottom-up BMP and uncompressed or RLE true-colour TGA up
to 16384 pixels per side. The fuzz adapter picks the `.exr` name when an input
starts with the OpenEXR magic.

BMP seeds are a 14-byte file header plus a 40-byte BITMAPINFOHEADER; TGA seeds
an 18-byte header. valid-rgb.exr is the EXR corpus's valid RGB half seed
(generate_exr_corpus.py).

The committed bytes are pinned by tools/fuzz-policy/corpus-manifest.json; this
script records how each seed was built so a reviewer can regenerate and diff.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import generate_exr_corpus


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "texture-loader-linux"
BMP_HEADER_BYTES = 54


def bmp(width: int, height: int, bpp: int, pixels: bytes, *, data_offset: int = BMP_HEADER_BYTES) -> bytes:
    file_size = BMP_HEADER_BYTES + len(pixels)
    file_header = b"BM" + struct.pack("<IHHI", file_size, 0, 0, data_offset)
    info_header = struct.pack("<IiiHHIIiiII", 40, width, height, 1, bpp, 0, len(pixels), 2835, 2835, 0, 0)
    return file_header + info_header + pixels


def bmp_rows(width: int, height: int, bpp: int) -> bytes:
    bytes_per_pixel = bpp // 8
    row = bytes((index % 255) + 1 for index in range(width * bytes_per_pixel))
    padding = (-len(row)) % 4
    return (row + b"\0" * padding) * height


def tga(width: int, height: int, bpp: int, image_type: int, payload: bytes) -> bytes:
    # ID length, colour-map type, image type, colour-map spec (5), origin x/y,
    # width, height, bits per pixel, descriptor (bit 5: top-left origin).
    return struct.pack("<BBB5xHHHHBB", 0, 0, image_type, 0, 0, width, height, bpp, 0x20) + payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    valid_24 = bmp(2, 2, 24, bmp_rows(2, 2, 24))
    valid_32 = bmp(2, 2, 32, bmp_rows(2, 2, 32))

    # RLE: one repeated run of 3 pixels (packet 0x82) and one raw pixel (packet 0x00).
    rle = tga(2, 2, 24, 10, bytes([0x82, 0x10, 0x20, 0x30, 0x00, 0x40, 0x50, 0x60]))

    # One pixel over STBI_MAX_DIMENSIONS; every pixel byte is present.
    wide = bmp(16385, 1, 24, bmp_rows(16385, 1, 24))

    # Top-down BMPs (negative height) are refused by the stub.
    negative_height = bmp(2, -2, 24, bmp_rows(2, 2, 24))

    # 4096x4096x32 declared by an 18-byte header and 4 pixel bytes.
    tga_overclaim = tga(4096, 4096, 32, 2, b"\x01\x02\x03\x04")

    # The pixel-data offset points past the end of the file.
    offset_past_eof = bmp(2, 2, 24, bmp_rows(2, 2, 24), data_offset=4096)

    # A PNG signature and a truncated IHDR: nothing here decodes it. The
    # pre-fix loader still returned S_OK and marked the texture loaded.
    undecodable_png = b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", 2, 2)

    valid_exr = generate_exr_corpus.scanline_file(
        [("B", generate_exr_corpus.HALF), ("G", generate_exr_corpus.HALF), ("R", generate_exr_corpus.HALF)], 2, 2
    )

    seeds = {
        "valid-2x2-24bit.bmp": valid_24,
        "valid-2x2-32bit.bmp": valid_32,
        "valid-2x2-rle.tga": rle,
        "bmp-width-16385.bmp": wide,
        "bmp-negative-height.bmp": negative_height,
        "tga-header-larger-than-file.tga": tga_overclaim,
        "bmp-pixel-offset-past-eof.bmp": offset_past_eof,
        "regression-undecodable-reports-loaded.png": undecodable_png,
        "valid-rgb.exr": valid_exr,
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} texture seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
