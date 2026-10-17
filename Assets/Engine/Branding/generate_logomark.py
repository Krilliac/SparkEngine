#!/usr/bin/env python3
"""Generate the SparkEngine four-point "spark" logomark as an outlined SVG and a multi-size ICO.

The geometry constants below are shared verbatim with Spark::Ui::DrawLogomark (SparkUI Brand.h). Change them in both
places or not at all. The script is stdlib-only and deterministic: the same interpreter and zlib build always emit
byte-identical files. The committed bytes are hash-bound by tools/asset-integrity/provenance.json, so any change to
this script or its output must be followed by regenerating Assets/assets.integrity.json.

Usage: python generate_logomark.py [--out-dir DIR]   (default: the directory containing this script)
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path

# --- Geometry shared with Spark::Ui::DrawLogomark (unit square, y grows downwards) -----------------------------
CENTER = (0.5, 0.5)
OUTER_RADIUS = 0.46
INNER_RADIUS = 0.13
ROTATION_DEGREES = 0.0
STAR_POINTS = 4
DOT_CENTER = (0.78, 0.22)
DOT_RADIUS = 0.075
# Ember gradient, top-left -> bottom-right: (offset along the diagonal, sRGB hex).
GRADIENT_STOPS = ((0.0, "#e8611a"), (0.5, "#f1823a"), (1.0, "#ffc857"))
# ----------------------------------------------------------------------------------------------------------------

ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)
SVG_SIZE = 256
VERTICAL_SAMPLES = 16
SVG_NAME = "sparkengine_logomark.svg"
ICO_NAME = "sparkengine.ico"

Point = tuple[float, float]


def star_polygon() -> list[Point]:
    """Return the concave star outline: alternating outer and inner vertices, starting at the first outer tip."""
    vertices: list[Point] = []
    step = math.pi / STAR_POINTS
    start = math.radians(ROTATION_DEGREES)
    for index in range(STAR_POINTS * 2):
        radius = OUTER_RADIUS if index % 2 == 0 else INNER_RADIUS
        angle = start + index * step
        vertices.append((CENTER[0] + radius * math.cos(angle), CENTER[1] + radius * math.sin(angle)))
    return vertices


def hex_to_rgb(value: str) -> tuple[int, int, int]:
    return int(value[1:3], 16), int(value[3:5], 16), int(value[5:7], 16)


def gradient_color(t: float) -> tuple[int, int, int]:
    """Sample the ember gradient at diagonal position t in [0, 1]."""
    t = min(1.0, max(0.0, t))
    for (a_off, a_hex), (b_off, b_hex) in zip(GRADIENT_STOPS, GRADIENT_STOPS[1:]):
        if t <= b_off:
            f = (t - a_off) / (b_off - a_off)
            a, b = hex_to_rgb(a_hex), hex_to_rgb(b_hex)
            return tuple(round(a[i] + (b[i] - a[i]) * f) for i in range(3))  # type: ignore[return-value]
    return hex_to_rgb(GRADIENT_STOPS[-1][1])


def number(value: float) -> str:
    """Format a coordinate with fixed precision and no negative zero."""
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def build_svg() -> str:
    """Build the outlined-path SVG (no <text>, no external references)."""
    scale = float(SVG_SIZE)
    points = [(x * scale, y * scale) for x, y in star_polygon()]
    star = "M" + " L".join(f"{number(x)} {number(y)}" for x, y in points) + " Z"
    cx, cy, r = DOT_CENTER[0] * scale, DOT_CENTER[1] * scale, DOT_RADIUS * scale
    dot = (f"M{number(cx - r)} {number(cy)} A{number(r)} {number(r)} 0 1 0 {number(cx + r)} {number(cy)} "
           f"A{number(r)} {number(r)} 0 1 0 {number(cx - r)} {number(cy)} Z")
    stops = "\n".join(
        f'      <stop offset="{number(offset)}" stop-color="{color}"/>' for offset, color in GRADIENT_STOPS)
    size = number(scale)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 {size} {size}" '
        f'role="img" aria-label="Spark Engine logomark">\n'
        f"  <defs>\n"
        f'    <linearGradient id="ember" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{size}" y2="{size}">\n'
        f"{stops}\n"
        f"    </linearGradient>\n"
        f"  </defs>\n"
        f'  <path id="spark" fill="url(#ember)" d="{star}"/>\n'
        f'  <path id="ember-dot" fill="url(#ember)" d="{dot}"/>\n'
        f"</svg>\n")


def polygon_spans(polygon: list[Point], y: float) -> list[tuple[float, float]]:
    """Even-odd horizontal spans of a polygon at scanline y."""
    crossings: list[float] = []
    for (x0, y0), (x1, y1) in zip(polygon, polygon[1:] + polygon[:1]):
        if (y0 <= y < y1) or (y1 <= y < y0):
            crossings.append(x0 + (y - y0) / (y1 - y0) * (x1 - x0))
    crossings.sort()
    return list(zip(crossings[0::2], crossings[1::2]))


def circle_span(y: float) -> tuple[float, float] | None:
    dy = y - DOT_CENTER[1]
    if abs(dy) >= DOT_RADIUS:
        return None
    half = math.sqrt(DOT_RADIUS * DOT_RADIUS - dy * dy)
    return DOT_CENTER[0] - half, DOT_CENTER[0] + half


def merge_spans(spans: list[tuple[float, float]]) -> list[tuple[float, float]]:
    merged: list[list[float]] = []
    for lo, hi in sorted(spans):
        if merged and lo <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], hi)
        else:
            merged.append([lo, hi])
    return [(lo, hi) for lo, hi in merged]


def rasterize(size: int) -> bytes:
    """Rasterize the mark to straight-alpha RGBA rows (exact horizontal coverage, 16 vertical samples)."""
    polygon = star_polygon()
    rows = bytearray()
    for py in range(size):
        coverage = [0.0] * size
        for sample in range(VERTICAL_SAMPLES):
            y = (py + (sample + 0.5) / VERTICAL_SAMPLES) / size
            spans = polygon_spans(polygon, y)
            dot = circle_span(y)
            if dot is not None:
                spans.append(dot)
            for lo, hi in merge_spans(spans):
                first = max(0, int(math.floor(lo * size)))
                last = min(size - 1, int(math.ceil(hi * size)) - 1)
                for px in range(first, last + 1):
                    overlap = min(hi * size, px + 1.0) - max(lo * size, float(px))
                    if overlap > 0.0:
                        coverage[px] += overlap / VERTICAL_SAMPLES
        rows.append(0)  # PNG filter type: None
        for px in range(size):
            alpha = min(1.0, coverage[px])
            if alpha <= 0.0:
                rows += b"\x00\x00\x00\x00"
                continue
            t = ((px + 0.5) / size + (py + 0.5) / size) / 2.0
            r, g, b = gradient_color(t)
            rows += bytes((r, g, b, round(alpha * 255)))
    return bytes(rows)


def png_chunk(tag: bytes, payload: bytes) -> bytes:
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def build_png(size: int) -> bytes:
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA, no interlace
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header)
            + png_chunk(b"IDAT", zlib.compress(rasterize(size), 9)) + png_chunk(b"IEND", b""))


def build_ico() -> bytes:
    """Build a PNG-in-ICO container holding every size in ICO_SIZES."""
    images = [build_png(size) for size in ICO_SIZES]
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, image in zip(ICO_SIZES, images):
        dim = 0 if size >= 256 else size  # 0 encodes 256
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    for image in images:
        out += image
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / SVG_NAME).write_bytes(build_svg().encode("utf-8"))
    (args.out_dir / ICO_NAME).write_bytes(build_ico())
    print(f"wrote {SVG_NAME} and {ICO_NAME} to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
