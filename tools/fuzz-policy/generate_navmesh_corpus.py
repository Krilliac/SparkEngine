#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production .snav decoder (DecodeSnav)."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "navmesh-loader"

QUAD_VERTICES = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (1.0, 0.0, 1.0)]
QUAD_TRIANGLES = [((0, 1, 2), [1]), ((1, 3, 2), [0])]


def header(version: int = 1) -> bytearray:
    payload = bytearray(b"SNAV")
    payload += struct.pack("<I", version)
    payload += struct.pack("<3f", 0.3, 2.0, 0.6)  # cellSize, agentHeight, agentRadius
    payload += struct.pack("<3f", 0.0, 0.0, 0.0)  # boundsMin
    payload += struct.pack("<3f", 1.0, 0.0, 1.0)  # boundsMax
    return payload


def triangle(indices: tuple[int, int, int], adjacency: list[int]) -> bytes:
    record = struct.pack("<3I", *indices)
    record += struct.pack("<3f", 0.5, 0.0, 0.5)  # centroid
    record += struct.pack("<3f", 0.0, 1.0, 0.0)  # normal
    record += struct.pack("<f", 0.5)  # area
    record += struct.pack("<I", len(adjacency))
    record += struct.pack(f"<{len(adjacency)}I", *adjacency)
    return record


def mesh(vertices, triangles, version: int = 1) -> bytes:
    payload = header(version)
    payload += struct.pack("<I", len(vertices))
    for vertex in vertices:
        payload += struct.pack("<3f", *vertex)
    payload += struct.pack("<I", len(triangles))
    for indices, adjacency in triangles:
        payload += triangle(indices, adjacency)
    return bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    valid = mesh(QUAD_VERTICES, QUAD_TRIANGLES)
    adjacency_overclaim = header() + struct.pack("<I", 3)
    for vertex in QUAD_VERTICES[:3]:
        adjacency_overclaim += struct.pack("<3f", *vertex)
    adjacency_overclaim += struct.pack("<I", 1) + triangle((0, 1, 2), [])[:-4] + struct.pack("<I", 10_001)
    seeds = {
        "valid-2-tri.snav": valid,
        # A triangle naming vertex 4 in a 4-vertex mesh: accepted before DecodeSnav checked indices.
        "regression-tri-index-past-vertices.snav": mesh(QUAD_VERTICES, [((0, 1, 4), [])]),
        # A dynamic adjacency entry naming triangle 5 in a 2-triangle mesh: accepted before the check.
        "regression-adjacency-past-triangles.snav": mesh(QUAD_VERTICES, [((0, 1, 2), [5]), ((1, 3, 2), [0])]),
        "vertex-count-overclaim.snav": bytes(header() + struct.pack("<I", 10_000_000)),
        "adj-count-10001.snav": bytes(adjacency_overclaim),
        "bad-version.snav": mesh(QUAD_VERTICES, QUAD_TRIANGLES, version=2),
        "nan-vertex.snav": mesh([(float("nan"), 0.0, 0.0)] + QUAD_VERTICES[1:], QUAD_TRIANGLES),
        "truncated-triangle.snav": valid[:-10],
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} navmesh seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
