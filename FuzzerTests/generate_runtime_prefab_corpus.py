#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the binary runtime prefab reader.

Each seed is a Spark::ECS::RuntimePrefab::Serialize stream or a damaged variant:
[magic u32 "PRFB" = 0x50524642 LE][version u32 = 1][name: u32 length + bytes]
[component count u32] then per component [type name: u32 length + bytes]
[property count u32] and per property [key string][value string], all little-endian.
The fuzz adapter (FuzzRuntimePrefabProduction.cpp) decodes each one through
RuntimePrefab::Deserialize.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "runtime-prefab"

MAGIC = 0x50524642
VERSION = 1


def text(value: bytes) -> bytes:
    return struct.pack("<I", len(value)) + value


def component(type_name: bytes, properties: list[tuple[bytes, bytes]]) -> bytes:
    body = text(type_name) + struct.pack("<I", len(properties))
    for key, value in properties:
        body += text(key) + text(value)
    return body


def prefab(name: bytes, components: list[bytes], *, magic: int = MAGIC, version: int = VERSION) -> bytes:
    return struct.pack("<II", magic, version) + text(name) + struct.pack("<I", len(components)) + b"".join(components)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    guard = prefab(
        b"EnemyGuard",
        [
            component(b"Transform", [(b"posX", b"0"), (b"posY", b"1.5"), (b"posZ", b"-3")]),
            component(b"MeshRenderer", [(b"mesh", b"guard.fbx")]),
            component(b"Health", []),
        ],
    )
    seeds = {
        "valid-minimal.prefab": prefab(b"Empty", []),
        "valid-full.prefab": guard,
        "valid-duplicate-keys.prefab": prefab(
            b"Dupes", [component(b"Tag", [(b"k", b"first"), (b"k", b"second"), (b"other", b"x")])]
        ),
        "valid-trailing-bytes.prefab": guard + b"\xde\xad\xbe\xef trailing",
        "valid-empty-strings.prefab": prefab(b"", [component(b"", [(b"", b"")])]),
        "valid-binary-strings.prefab": prefab(
            b"bin\x00ary\xff", [component(b"\x01\x02\x03", [(b"\x00", b"\xff\xfe\n\r")])]
        ),
        "bad-magic.prefab": prefab(b"Wrong", [], magic=0x464F4F42),
        "future-version.prefab": prefab(b"Future", [], version=2),
        "truncated-header.prefab": struct.pack("<II", MAGIC, VERSION)[:6],
        "truncated-component.prefab": guard[: len(guard) - 5],
        "huge-component-count.prefab": struct.pack("<II", MAGIC, VERSION)
        + text(b"Huge")
        + struct.pack("<I", 0xFFFFFFFF)
        + component(b"Only", [(b"a", b"b")]),
        "huge-string-length.prefab": struct.pack("<II", MAGIC, VERSION) + struct.pack("<I", 0xFFFFFFF0) + b"short",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
