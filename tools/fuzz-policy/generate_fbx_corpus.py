#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production binary FBX importer.

The layout mirrors SparkEngine/Source/Graphics/FBXImporter.h: a 27-byte header
(FBXConstants::MAGIC, its trailing zero byte, then the u32 version), a list of
node records and a null record that ends the list. A node record is
endOffset, numProperties, propertyListLen (u32 each before v7500, u64 from
v7500 on), a u8 name length, the name, the properties and then the children,
which are always closed by a null record of the same width. Array properties
carry count, encoding (0 raw, 1 zlib) and stored length as u32 fields.

The committed bytes are pinned by tools/fuzz-policy/corpus-manifest.json; this
script records how each seed was built so a reviewer can regenerate and diff.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "mesh-fbx-importer"

MAGIC = b"Kaydara FBX Binary  \x00\x1a\x00"
HEADER_SIZE = 27
MAX_ARRAY_ELEMENTS = 1024 * 1024
MAX_NODE_DEPTH = 128


class Node:
    def __init__(self, name: str, properties: list[bytes] | None = None, children: list["Node"] | None = None):
        self.name = name
        self.properties = properties or []
        self.children = children or []


def prop_long(value: int) -> bytes:
    return b"L" + struct.pack("<q", value)


def prop_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return b"S" + struct.pack("<I", len(encoded)) + encoded


def prop_array(code: bytes, element: str, values: list, *, deflate: bool = False) -> bytes:
    raw = struct.pack(f"<{len(values)}{element}", *values)
    stored = zlib.compress(raw) if deflate else raw
    return code + struct.pack("<III", len(values), 1 if deflate else 0, len(stored)) + stored


def prop_doubles(values: list[float], *, deflate: bool = False) -> bytes:
    return prop_array(b"d", "d", values, deflate=deflate)


def prop_ints(values: list[int]) -> bytes:
    return prop_array(b"i", "i", values)


def record_width(version: int) -> int:
    return 25 if version >= 7500 else 13


def encode_node(node: Node, version: int, offset: int) -> bytes:
    """Encode @p node whose record starts at absolute file offset @p offset."""
    width = record_width(version)
    name = node.name.encode("ascii")
    properties = b"".join(node.properties)
    body_start = offset + width + len(name) + len(properties)
    children = bytearray()
    for child in node.children:
        children += encode_node(child, version, body_start + len(children))
    children += bytes(width)  # null record closing this node
    end = body_start + len(children)
    field = "<QQQB" if version >= 7500 else "<IIIB"
    head = struct.pack(field, end, len(node.properties), len(properties), len(name))
    return head + name + properties + bytes(children)


def document(version: int, nodes: list[Node]) -> bytes:
    payload = bytearray(MAGIC + struct.pack("<I", version))
    assert len(payload) == HEADER_SIZE
    for node in nodes:
        payload += encode_node(node, version, len(payload))
    payload += bytes(record_width(version))  # null record ending the top-level list
    return bytes(payload)


TRIANGLE = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
IDENTITY = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def scene(*, deflate: bool = False) -> list[Node]:
    geometry = Node(
        "Geometry",
        [prop_long(1), prop_string("Triangle\x00\x01Geometry"), prop_string("Mesh")],
        [
            Node("Vertices", [prop_doubles(TRIANGLE, deflate=deflate)]),
            Node("PolygonVertexIndex", [prop_ints([0, 1, ~2])]),
        ],
    )
    cluster = Node(
        "Deformer",
        [prop_long(2), prop_string("Root\x00\x01SubDeformer"), prop_string("Cluster")],
        [Node("Transform", [prop_doubles(IDENTITY)])],
    )
    stack = Node("AnimationStack", [prop_string("Take 001")])
    return [Node("Objects", [], [geometry, cluster, stack])]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    valid_v7400 = document(7400, scene())
    valid_v7500 = document(7500, scene())
    deflate_array = document(7400, scene(deflate=True))

    # A zlib-encoded double array that claims ten million elements (80 MB) in
    # a few stored bytes. The importer must reject the count before it sizes
    # the destination; without kMaxArrayElements the 80 MB resize trips the
    # smoke's -malloc_limit_mb.
    stored = zlib.compress(b"\x00" * 16)
    over_count = b"d" + struct.pack("<III", 10 * MAX_ARRAY_ELEMENTS, 1, len(stored)) + stored
    array_over_limit = document(7400, [Node("Vertices", [over_count])])

    # A string property whose length field claims 16 MB inside a tiny node.
    string_overclaim = bytearray(document(7400, [Node("Name", [prop_string("abc")])]))
    length_at = HEADER_SIZE + 13 + len("Name") + 1
    string_overclaim[length_at:length_at + 4] = struct.pack("<I", 16 * 1024 * 1024)

    # MAX_NODE_DEPTH + 1 nested nodes (depths 0-128); the null record closing
    # the deepest one sits at depth 129, past kMaxNodeDepth.
    deep = Node("N")
    for _ in range(MAX_NODE_DEPTH):
        deep = Node("N", [], [deep])
    node_depth_129 = document(7400, [deep])

    # A child whose endOffset points past its parent's end.
    escaping = bytearray(document(7400, [Node("Parent", [], [Node("Child")])]))
    child_at = HEADER_SIZE + 13 + len("Parent")
    child_end = struct.unpack_from("<I", escaping, child_at)[0]
    struct.pack_into("<I", escaping, child_at, child_end + 64)

    seeds = {
        "valid-v7400-triangle.fbx": valid_v7400,
        "valid-v7500-64bit-offsets.fbx": valid_v7500,
        "deflate-array.fbx": deflate_array,
        "array-count-over-1M.fbx": array_over_limit,
        "string-length-overclaim.fbx": bytes(string_overclaim),
        "node-depth-129.fbx": node_depth_129,
        "node-endoffset-past-parent.fbx": bytes(escaping),
        "truncated-header.fbx": valid_v7400[:HEADER_SIZE - 4],
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} FBX seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
