#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production glTF/GLB mesh loaders.

The seeds drive FuzzerTests/FuzzGltfProduction.cpp, which writes each input to
<tmp>/root/model.gltf (or model.glb when it starts with the GLB magic) next to
two fixture buffers the adapter creates once per process:

* <tmp>/root/inside.bin: one triangle, three float VEC3 positions (36 bytes);
* <tmp>/outside.bin: the same layout outside the document root, every vertex
  holding the adapter's sentinel position, so a loader that follows a URI out
  of the root publishes a vertex the adapter recognises.

JSON seeds are compact UTF-8. GLB seeds follow the glTF 2.0 container layout
(12-byte header, 4-byte aligned JSON chunk padded with spaces, BIN chunk),
the same layout Tests/GLTFSkinningReference.h builds. The skinned rig matches
that header's fixture: Armature -> Root (+1 Y) -> Tip (+2 Y), a Body mesh node
bound to the two-joint skin.

The committed bytes are pinned by tools/fuzz-policy/corpus-manifest.json; this
script records how each seed was built so a reviewer can regenerate and diff.
"""

from __future__ import annotations

import argparse
import base64
import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "mesh-gltf-loader"

UNSIGNED_BYTE = 5121
UNSIGNED_SHORT = 5123
FLOAT = 5126
TRIANGLE = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]


def floats(values: list[float]) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


def compact(document: dict) -> bytes:
    return json.dumps(document, separators=(",", ":")).encode("utf-8")


class GLBBuilder:
    """Mirror of Tests/GLTFSkinningReference.h GLBBuilder: one view and accessor per blob."""

    def __init__(self) -> None:
        self.bin = bytearray()
        self.views: list[dict] = []
        self.accessors: list[dict] = []

    def add(self, blob: bytes, component_type: int, count: int, kind: str) -> int:
        start = len(self.bin)
        self.bin += blob
        self.views.append({"buffer": 0, "byteOffset": start, "byteLength": len(blob)})
        while len(self.bin) % 4:
            self.bin.append(0)
        self.accessors.append(
            {"bufferView": len(self.views) - 1, "componentType": component_type, "count": count, "type": kind}
        )
        return len(self.accessors) - 1

    def add_floats(self, values: list[float], count: int, kind: str) -> int:
        return self.add(floats(values), FLOAT, count, kind)

    def finish(self, tail: dict, *, bin_skew: int = 0) -> bytes:
        document = {
            "asset": {"version": "2.0"},
            "buffers": [{"byteLength": len(self.bin)}],
            "bufferViews": self.views,
            "accessors": self.accessors,
            **tail,
        }
        chunk = compact(document)
        chunk += b" " * (-len(chunk) % 4 + bin_skew)
        total = 12 + 8 + len(chunk) + 8 + len(self.bin)
        return (
            struct.pack("<III", 0x46546C67, 2, total)
            + struct.pack("<II", len(chunk), 0x4E4F534A)
            + chunk
            + struct.pack("<II", len(self.bin), 0x004E4942)
            + bytes(self.bin)
        )


def static_glb(indices: list[int], *, index_skew: int = 0, bin_skew: int = 0) -> bytes:
    """``index_skew`` shifts the index view off its 2-byte alignment; ``bin_skew`` pads the
    JSON chunk past its 4-byte boundary, which moves the BIN chunk with it."""
    builder = GLBBuilder()
    positions = builder.add_floats(TRIANGLE, 3, "VEC3")
    builder.bin += bytes(index_skew)
    index_bytes = struct.pack(f"<{len(indices)}H", *indices)
    index_accessor = builder.add(index_bytes, UNSIGNED_SHORT, len(indices), "SCALAR")
    return builder.finish(
        {
            "meshes": [{"primitives": [{"attributes": {"POSITION": positions}, "indices": index_accessor}]}],
            "nodes": [{"mesh": 0}],
        },
        bin_skew=bin_skew,
    )


def skinned_glb(*, animations: list[dict] | None = None, root_children: list[int] | None = None,
                tip_children: list[int] | None = None) -> bytes:
    builder = GLBBuilder()
    positions = builder.add_floats(TRIANGLE, 3, "VEC3")
    normals = builder.add_floats([0.0, 0.0, 1.0] * 3, 3, "VEC3")
    # Skin joint 0 is Root, joint 1 is Tip; every weight row sums to 1.
    joints = builder.add(bytes([0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0]), UNSIGNED_BYTE, 3, "VEC4")
    weights = builder.add_floats([0.25, 0.75, 0, 0, 1, 0, 0, 0, 0.5, 0.5, 0, 0], 3, "VEC4")
    indices = builder.add(struct.pack("<3H", 0, 1, 2), UNSIGNED_SHORT, 3, "SCALAR")

    encoded_animations = []
    for animation in animations or []:
        samplers = []
        for times, values, kind in animation["samplers"]:
            components = 4 if kind == "VEC4" else 3
            assert len(values) == len(times) * components
            time_accessor = builder.add_floats(times, len(times), "SCALAR")
            value_accessor = builder.add_floats(values, len(times), kind)
            samplers.append({"input": time_accessor, "output": value_accessor, "interpolation": "LINEAR"})
        channels = [
            {"sampler": sampler, "target": {"node": node, "path": path}}
            for sampler, node, path in animation["channels"]
        ]
        encoded_animations.append({"name": animation["name"], "samplers": samplers, "channels": channels})

    root = {"name": "Root", "translation": [0, 1, 0], "children": root_children if root_children is not None else [2]}
    tip = {"name": "Tip", "translation": [0, 2, 0]}
    if tip_children is not None:
        tip["children"] = tip_children
    tail = {
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {"POSITION": positions, "NORMAL": normals, "JOINTS_0": joints, "WEIGHTS_0": weights},
                        "indices": indices,
                    }
                ]
            }
        ],
        "nodes": [
            {"name": "Armature", "translation": [0, 0, 0], "children": [1, 3] if tip_children is None else [3]},
            root,
            tip,
            {"name": "Body", "mesh": 0, "skin": 0},
        ],
        "skins": [{"name": "Rig", "joints": [1, 2]}],
    }
    if encoded_animations:
        tail["animations"] = encoded_animations
    return builder.finish(tail)


def external_gltf(uri: str, *, byte_length: int = 36, byte_stride: int | None = None, count: int = 3) -> bytes:
    view = {"buffer": 0, "byteOffset": 0, "byteLength": 36}
    if byte_stride is not None:
        view["byteStride"] = byte_stride
    return compact(
        {
            "asset": {"version": "2.0"},
            "buffers": [{"uri": uri, "byteLength": byte_length}],
            "bufferViews": [view],
            "accessors": [{"bufferView": 0, "componentType": FLOAT, "count": count, "type": "VEC3"}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
            "nodes": [{"mesh": 0}],
        }
    )


def data_uri(payload: bytes) -> str:
    return "data:application/octet-stream;base64," + base64.b64encode(payload).decode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    one_channel = {
        "name": "Lift",
        "samplers": [([0.0, 2.0], [0, 1, 0, 0, 3, 0], "VEC3")],
        "channels": [(0, 1, "translation")],
    }

    seeds = {
        "valid-triangle.gltf": external_gltf("inside.bin"),
        "valid-embedded.glb": static_glb([0, 1, 2]),
        "valid-skinned-2-joint.glb": skinned_glb(),
        "valid-anim-1-channel.glb": skinned_glb(animations=[one_channel]),
        # Both URIs resolve to <tmp>/outside.bin; the loader must refuse them.
        "uri-parent-escape.gltf": external_gltf("../outside.bin"),
        "uri-absolute.gltf": external_gltf("/../outside.bin"),
        # stride * (count - 1) = 2^62 * 8 wraps to 0 in 64 bits, so an unchecked
        # product would accept an accessor whose second element lies 2^62 bytes
        # past its buffer view.
        "accessor-count-stride-overflow.gltf": external_gltf(
            data_uri(floats(TRIANGLE)), byte_stride=1 << 62, count=9
        ),
        "index-out-of-range.glb": static_glb([0, 1, 5]),
        # Root lists Tip as its child and Tip lists Root back; the Armature drops
        # Root so every node keeps a single parent and cgltf's own parse accepts it.
        "cyclic-joint-hierarchy.glb": skinned_glb(tip_children=[1]),
        # A 36-byte base64 payload declaring a 300 MB buffer. Before the fix
        # cgltf_load_buffers allocated the declared byteLength (up to 1 GiB)
        # before decoding, which the smoke's -rss_limit_mb malloc cap rejects.
        "regression-data-uri-length-overclaim.gltf": external_gltf(
            data_uri(floats(TRIANGLE)), byte_length=300_000_000
        ),
        # A uint16 index view at byte offset 37. cgltf_validate reads indices
        # through a typed pointer, so before the fix UBSan reported a misaligned
        # load (the local mutation campaign's finding, rebuilt from this corpus).
        "regression-misaligned-index-accessor.glb": static_glb([0, 1, 2], index_skew=1),
        # One byte of JSON padding past the 4-byte boundary places the BIN chunk,
        # which cgltf uses in place as buffer 0, at an odd file offset.
        "regression-glb-bin-unaligned.glb": static_glb([0, 1, 2], bin_skew=1),
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} glTF seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
