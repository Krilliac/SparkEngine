#!/usr/bin/env python3
"""Validate template runtime-sheet descriptors and PNG transparency using stdlib."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def read_rgba_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG file")
    offset = len(PNG_SIGNATURE)
    width = height = 0
    compressed = bytearray()
    while offset < len(data):
        if offset + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4 : offset + 8]
        payload_start = offset + 8
        payload_end = payload_start + length
        if payload_end + 4 > len(data):
            raise ValueError("truncated PNG payload")
        payload = data[payload_start:payload_end]
        expected_crc = struct.unpack_from(">I", data, payload_end)[0]
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(payload, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError(f"bad CRC in {chunk_type.decode('ascii', 'replace')} chunk")
        if chunk_type == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if (depth, color, compression, filtering, interlace) != (8, 6, 0, 0, 0):
                raise ValueError(
                    "runtime sheets must be non-interlaced 8-bit RGBA PNGs "
                    f"(got depth={depth}, color={color}, interlace={interlace})"
                )
        elif chunk_type == b"IDAT":
            compressed.extend(payload)
        elif chunk_type == b"IEND":
            break
        offset = payload_end + 4
    if not width or not height or not compressed:
        raise ValueError("PNG is missing IHDR or IDAT data")

    stride = width * 4
    expected_size = height * (stride + 1)
    decompressor = zlib.decompressobj()
    packed = decompressor.decompress(bytes(compressed), expected_size + 1)
    if not decompressor.eof or decompressor.unconsumed_tail or decompressor.unused_data:
        raise ValueError("PNG compressed stream is malformed or exceeds the declared dimensions")
    if len(packed) != expected_size:
        raise ValueError(f"unexpected decoded size {len(packed)} (expected {expected_size})")
    decoded = bytearray(height * stride)
    source_offset = 0
    for y in range(height):
        filter_type = packed[source_offset]
        source_offset += 1
        row = bytearray(packed[source_offset : source_offset + stride])
        source_offset += stride
        previous_offset = (y - 1) * stride
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            above = decoded[previous_offset + x] if y else 0
            upper_left = decoded[previous_offset + x - 4] if y and x >= 4 else 0
            if filter_type == 1:
                row[x] = (row[x] + left) & 0xFF
            elif filter_type == 2:
                row[x] = (row[x] + above) & 0xFF
            elif filter_type == 3:
                row[x] = (row[x] + ((left + above) // 2)) & 0xFF
            elif filter_type == 4:
                row[x] = (row[x] + _paeth(left, above, upper_left)) & 0xFF
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter {filter_type}")
        decoded[y * stride : (y + 1) * stride] = row
    return width, height, bytes(decoded)


def validate_descriptor(descriptor_path: Path) -> list[str]:
    errors: list[str] = []
    try:
        descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return [f"{descriptor_path}: cannot read descriptor: {error}"]

    image_name = descriptor.get("image")
    if not isinstance(image_name, str) or Path(image_name).name != image_name:
        return [f"{descriptor_path}: image must be a sibling filename"]
    image_path = descriptor_path.parent / image_name
    try:
        width, height, rgba = read_rgba_png(image_path)
    except (OSError, ValueError, zlib.error) as error:
        return [f"{image_path}: {error}"]

    if (width, height) != (descriptor.get("width"), descriptor.get("height")):
        errors.append(f"{descriptor_path}: descriptor and PNG dimensions differ")
    grid = descriptor.get("grid", {})
    columns = grid.get("columns")
    rows = grid.get("rows")
    cell_width = grid.get("cellWidth")
    cell_height = grid.get("cellHeight")
    safe_padding = grid.get("safePadding")
    if (columns, rows, cell_width, cell_height) != (3, 3, 418, 418):
        errors.append(f"{descriptor_path}: expected a 3x3 grid of 418x418 cells")
        return errors
    if not isinstance(safe_padding, int) or safe_padding < 18:
        errors.append(f"{descriptor_path}: safePadding must be at least 18 pixels")
        return errors
    if width != columns * cell_width or height != rows * cell_height:
        errors.append(f"{descriptor_path}: grid does not cover the PNG exactly")

    frames = descriptor.get("frames")
    if not isinstance(frames, dict) or len(frames) != rows * columns:
        errors.append(f"{descriptor_path}: expected exactly nine named frames")
        return errors
    expected_rectangles = {
        (column * cell_width, row * cell_height, cell_width, cell_height)
        for row in range(rows)
        for column in range(columns)
    }
    actual_rectangles: set[tuple[int, int, int, int]] = set()
    for name, frame in frames.items():
        if not isinstance(name, str) or not name or not isinstance(frame, dict):
            errors.append(f"{descriptor_path}: frame names and values must be objects")
            continue
        rectangle = tuple(frame.get(key) for key in ("x", "y", "width", "height"))
        if not all(isinstance(value, int) for value in rectangle):
            errors.append(f"{descriptor_path}: frame {name!r} has non-integer geometry")
            continue
        actual_rectangles.add(rectangle)  # type: ignore[arg-type]
    if actual_rectangles != expected_rectangles:
        errors.append(f"{descriptor_path}: frame rectangles must cover every grid cell once")

    alpha = rgba[3::4]
    if not any(value == 0 for value in alpha):
        errors.append(f"{image_path}: no transparent pixels (likely baked checkerboard)")
    gutter = safe_padding
    for row in range(rows):
        for column in range(columns):
            left = column * cell_width
            top = row * cell_height
            right = left + cell_width
            bottom = top + cell_height
            visible = 0
            gutter_violation: tuple[int, int] | None = None
            for y in range(top, bottom):
                alpha_row = y * width
                for x in range(left, right):
                    value = alpha[alpha_row + x]
                    if value:
                        visible += 1
                        if (
                            x < left + gutter
                            or x >= right - gutter
                            or y < top + gutter
                            or y >= bottom - gutter
                        ):
                            gutter_violation = (x, y)
                            break
                if gutter_violation:
                    break
            if visible < 100:
                errors.append(f"{image_path}: cell ({column}, {row}) is effectively empty")
            if gutter_violation:
                errors.append(
                    f"{image_path}: cell ({column}, {row}) violates its transparent gutter "
                    f"at {gutter_violation}"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("template_root", type=Path)
    args = parser.parse_args()
    descriptors = sorted(args.template_root.glob("*/Assets/runtime_sheet.json"))
    errors: list[str] = []
    if len(descriptors) != 9:
        errors.append(f"expected 9 runtime-sheet descriptors, found {len(descriptors)}")
    for descriptor in descriptors:
        errors.extend(validate_descriptor(descriptor))
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Validated {len(descriptors)} template runtime sheets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
