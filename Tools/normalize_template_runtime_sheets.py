#!/usr/bin/env python3
"""Normalize AI-authored 3x3 template sheets into runtime-safe PNGs.

The creative source is produced by the image model.  This utility performs only
deterministic shipping cleanup: removes a baked light checkerboard when present,
isolates each fixed cell, and recenters its non-transparent content with a safe
gutter.  Pillow is intentionally a maintainer-time dependency; runtime and CI
validation use the separate standard-library validator.
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path

from PIL import Image


SHEET_SIZE = 1254
GRID_SIZE = 3
CELL_SIZE = SHEET_SIZE // GRID_SIZE


def _checker_candidate(pixel: tuple[int, int, int, int]) -> bool:
    red, green, blue, alpha = pixel
    return (
        alpha >= 250
        and min(red, green, blue) >= 226
        and max(red, green, blue) - min(red, green, blue) <= 7
    )


def remove_baked_checkerboard(image: Image.Image) -> Image.Image:
    """Remove large connected light-neutral checker regions without erasing art."""
    rgba = image.convert("RGBA")
    width, height = rgba.size
    pixels = rgba.load()
    candidates = bytearray(width * height)
    for y in range(height):
        row = y * width
        for x in range(width):
            candidates[row + x] = _checker_candidate(pixels[x, y])

    seen = bytearray(width * height)
    minimum_enclosed_region = (CELL_SIZE * CELL_SIZE) // 80
    for start in range(width * height):
        if not candidates[start] or seen[start]:
            continue
        queue: deque[int] = deque([start])
        seen[start] = 1
        component: list[int] = []
        touches_border = False
        minimum_luma = 255
        maximum_luma = 0
        while queue:
            index = queue.popleft()
            component.append(index)
            x = index % width
            y = index // width
            if x == 0 or y == 0 or x + 1 == width or y + 1 == height:
                touches_border = True
            red, green, blue, _ = pixels[x, y]
            luma = (red + green + blue) // 3
            minimum_luma = min(minimum_luma, luma)
            maximum_luma = max(maximum_luma, luma)
            if x and candidates[index - 1] and not seen[index - 1]:
                seen[index - 1] = 1
                queue.append(index - 1)
            if x + 1 < width and candidates[index + 1] and not seen[index + 1]:
                seen[index + 1] = 1
                queue.append(index + 1)
            if y and candidates[index - width] and not seen[index - width]:
                seen[index - width] = 1
                queue.append(index - width)
            if y + 1 < height and candidates[index + width] and not seen[index + width]:
                seen[index + width] = 1
                queue.append(index + width)

        looks_like_enclosed_checker = (
            len(component) >= minimum_enclosed_region
            and maximum_luma - minimum_luma >= 8
        )
        if touches_border or looks_like_enclosed_checker:
            for index in component:
                x = index % width
                y = index // width
                red, green, blue, _ = pixels[x, y]
                pixels[x, y] = (red, green, blue, 0)
    return rgba


def isolate_cell_assets(image: Image.Image) -> list[Image.Image]:
    """Assign complete connected objects to their nearest cell before repacking.

    Image models occasionally let an object cross a nominal grid line.  Cropping
    first would amputate it.  Component assignment keeps the whole object while
    filtering sparse model-drawn dividers and microscopic checker remnants.
    """
    width, height = image.size
    pixels = image.load()
    visible = bytearray(width * height)
    for y in range(height):
        row = y * width
        for x in range(width):
            visible[row + x] = pixels[x, y][3] > 8

    seen = bytearray(width * height)
    groups: list[list[list[int]]] = [[] for _ in range(GRID_SIZE * GRID_SIZE)]
    for start in range(width * height):
        if not visible[start] or seen[start]:
            continue
        queue: deque[int] = deque([start])
        seen[start] = 1
        component: list[int] = []
        min_x = width
        min_y = height
        max_x = 0
        max_y = 0
        sum_x = 0
        sum_y = 0
        while queue:
            index = queue.popleft()
            component.append(index)
            x = index % width
            y = index // width
            min_x = min(min_x, x)
            min_y = min(min_y, y)
            max_x = max(max_x, x)
            max_y = max(max_y, y)
            sum_x += x
            sum_y += y
            if x and visible[index - 1] and not seen[index - 1]:
                seen[index - 1] = 1
                queue.append(index - 1)
            if x + 1 < width and visible[index + 1] and not seen[index + 1]:
                seen[index + 1] = 1
                queue.append(index + 1)
            if y and visible[index - width] and not seen[index - width]:
                seen[index - width] = 1
                queue.append(index - width)
            if y + 1 < height and visible[index + width] and not seen[index + width]:
                seen[index + width] = 1
                queue.append(index + width)

        component_width = max_x - min_x + 1
        component_height = max_y - min_y + 1
        fill_ratio = len(component) / (component_width * component_height)
        sparse_divider = (
            component_width > CELL_SIZE * 0.72
            and component_height > CELL_SIZE * 0.72
            and fill_ratio < 0.08
        )
        long_rule = (
            min(component_width, component_height) <= 4
            and max(component_width, component_height) > CELL_SIZE * 0.25
        )
        if len(component) < 28 or sparse_divider or long_rule:
            continue

        centroid_x = sum_x / len(component)
        centroid_y = sum_y / len(component)
        column = min(GRID_SIZE - 1, max(0, int(centroid_x // CELL_SIZE)))
        row = min(GRID_SIZE - 1, max(0, int(centroid_y // CELL_SIZE)))
        groups[row * GRID_SIZE + column].append(component)

    assets: list[Image.Image] = []
    for group_index, components in enumerate(groups):
        if not components:
            raise ValueError(f"cell {group_index} has no authored object")
        largest = max(len(component) for component in components)
        # Tiny detached flecks assigned to a neighboring cell are model noise.
        components = [
            component
            for component in components
            if len(component) >= max(28, largest // 180)
        ]
        layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
        layer_pixels = layer.load()
        for component in components:
            for index in component:
                x = index % width
                y = index // width
                layer_pixels[x, y] = pixels[x, y]
        bounds = layer.getchannel("A").getbbox()
        if bounds is None:
            raise ValueError(f"cell {group_index} became empty after noise filtering")
        assets.append(layer.crop(bounds))
    return assets


def normalize_sheet(source: Path, destination: Path, remove_checker: bool) -> None:
    image = Image.open(source).convert("RGBA")
    if image.size != (SHEET_SIZE, SHEET_SIZE):
        raise ValueError(f"{source}: expected {SHEET_SIZE}x{SHEET_SIZE}, got {image.size}")
    if remove_checker:
        image = remove_baked_checkerboard(image)

    assets = isolate_cell_assets(image)
    output = Image.new("RGBA", image.size, (0, 0, 0, 0))
    padding = 38
    maximum_extent = CELL_SIZE - 2 * padding
    resampling = getattr(Image, "Resampling", Image).LANCZOS
    for row in range(GRID_SIZE):
        for column in range(GRID_SIZE):
            left = column * CELL_SIZE
            top = row * CELL_SIZE
            asset = assets[row * GRID_SIZE + column]
            scale = min(maximum_extent / asset.width, maximum_extent / asset.height, 1.0)
            if scale < 1.0:
                asset = asset.resize(
                    (max(1, round(asset.width * scale)), max(1, round(asset.height * scale))),
                    resampling,
                )
            x = left + (CELL_SIZE - asset.width) // 2
            y = top + (CELL_SIZE - asset.height) // 2
            output.alpha_composite(asset, (x, y))

    destination.parent.mkdir(parents=True, exist_ok=True)
    output.save(destination, format="PNG", optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument(
        "--remove-checker",
        action="store_true",
        help="remove a baked light checkerboard before repacking cells",
    )
    args = parser.parse_args()
    normalize_sheet(args.source, args.destination, args.remove_checker)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
