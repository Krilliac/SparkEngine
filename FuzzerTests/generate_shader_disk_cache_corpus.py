#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the shader disk-cache blob reader.

Each seed is the content of a cached `<hash>_<target>_<stage>.blob` file, which
ShaderDiskCache::Lookup hands to the driver as bytecode: a well-formed SPIR-V or DXBC
module, a torn (truncated) write, or planted junk. libFuzzer always runs the empty input
too, which is the zero-length entry a crash between Store's open and write leaves behind.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "shader-disk-cache-blob"


def spirv_module() -> bytes:
    # Header: magic, version 1.0, generator, bound, schema; then OpCapability Shader and
    # OpMemoryModel Logical GLSL450.
    words = [0x07230203, 0x00010000, 0, 8, 0, (2 << 16) | 17, 1, (3 << 16) | 14, 0, 1]
    return struct.pack(f"<{len(words)}I", *words)


def dxbc_container() -> bytes:
    body = b"SHEX" + struct.pack("<I", 8) + b"\x50\x00\x01\x00\x02\x00\x00\x00"
    header = b"DXBC" + b"\x00" * 16 + struct.pack("<IIII", 1, 32 + 4 + len(body), 1, 36)
    return header + body


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    spirv = spirv_module()
    seeds = {
        "spirv-module.blob": spirv,
        "spirv-torn-write.blob": spirv[:7],
        "dxbc-container.blob": dxbc_container(),
        "single-byte.blob": b"\x00",
        "planted-text.blob": b"#!/bin/sh\necho not bytecode\n",
        "zero-filled-4k.blob": b"\x00" * 4096,
        "large-64k.blob": bytes((i * 131 + 7) & 0xFF for i in range(65536)),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
