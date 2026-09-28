#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production .spark_save reader.

The layout mirrors SparkEngine/Source/Engine/SaveSystem/SaveFileCodec.cpp: the
magic "SPRK", a little-endian u32 version, a u32-length-prefixed metadata text
block (saveName, sceneName, playerClass and screenshotPath lines, then the
timestamp, playTime, health, armor, position, kills and deaths), a u32 entity
count, per entity a u16-prefixed name and a u16 component count, per component a
u16-prefixed type name and a u16 property count of u16-prefixed key/value pairs,
then a u32 custom-state count of u16-prefixed key/value pairs. Version 4 appends
a little-endian CRC-32 over every preceding byte; version 3 (N-1) has no trailer.

The committed bytes are pinned by tools/fuzz-policy/corpus-manifest.json; this
script records how each seed was built so a reviewer can regenerate and diff.
Seeds are binary (CRC-covered), so .gitattributes keeps them byte-exact. They use
the extension ".save" because regression fixture names (corpus_manifest.py) allow
only [a-z0-9] extensions.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "save-system"

CURRENT_VERSION = 4
OLDEST_VERSION = 3
MAX_METADATA_BYTES = 64 * 1024


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def text(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return u16(len(encoded)) + encoded


def metadata_block(save_name: str = "Seed", scene: str = "Level01") -> bytes:
    lines = [save_name, scene, "Soldier", "", "1700000000", "125.5", "80", "25", "1 2.5 -3", "7", "2"]
    return ("\n".join(lines) + "\n").encode("utf-8")


def entity(name: str, components: list[tuple[str, dict[str, str]]]) -> bytes:
    out = text(name) + u16(len(components))
    for type_name, properties in components:
        out += text(type_name) + u16(len(properties))
        for key, value in properties.items():
            out += text(key) + text(value)
    return out


def payload(
    version: int,
    entities: list[bytes],
    custom_state: dict[str, str] | None = None,
    metadata: bytes | None = None,
    entity_count: int | None = None,
) -> bytes:
    meta = metadata_block() if metadata is None else metadata
    out = b"SPRK" + u32(version) + u32(len(meta)) + meta
    out += u32(len(entities) if entity_count is None else entity_count)
    out += b"".join(entities)
    state = custom_state or {}
    out += u32(len(state))
    for key, value in state.items():
        out += text(key) + text(value)
    return out


def seal(body: bytes) -> bytes:
    """Append the v4 CRC-32 trailer (zlib.crc32 is the same reflected 0xEDB88320 CRC)."""
    return body + u32(zlib.crc32(body) & 0xFFFFFFFF)


def transform(parent: str, **extra: str) -> tuple[str, dict[str, str]]:
    properties = {"posX": "1", "posY": "0", "posZ": "-2", "parent": parent}
    properties.update(extra)
    return ("Transform", properties)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    player = entity("Player", [transform("-1"), ("Health", {"current": "80", "max": "100"})])
    minimal = seal(payload(CURRENT_VERSION, [player], {"quest.stage": "3"}))

    hierarchy = seal(
        payload(
            CURRENT_VERSION,
            [
                entity("Root", [transform("-1")]),
                entity("Arm", [transform("0")]),
                entity("Hand", [transform("1")]),
                entity("Named-only", []),
            ],
        )
    )

    # N-1 carries the same semantic payload without the CRC trailer and migrates in memory.
    v3 = payload(OLDEST_VERSION, [player], {"quest.stage": "3"})

    crc_mismatch = bytearray(minimal)
    crc_mismatch[-1] ^= 0xFF

    # A sealed v4 file whose version field alone was rewritten to 3: the trailer still
    # verifies under version 4, so it must not downgrade into the unchecked v3 path.
    version_damaged = bytearray(minimal)
    version_damaged[4:8] = u32(OLDEST_VERSION)

    # Four billion entities declared in a few dozen bytes; nothing may be sized from it.
    entity_overclaim = seal(payload(CURRENT_VERSION, [], metadata=b"a\nb\nc\n\n0\n0\n0\n0\n0 0 0\n0\n0\n",
                                    entity_count=0xFFFFFFFF))

    # A metadata length inside the 64 KiB cap that the file does not back.
    short_meta = b"SPRK" + u32(CURRENT_VERSION) + u32(MAX_METADATA_BYTES - 16) + b"x\n"
    metadata_overclaim = seal(short_meta)

    duplicate_type = seal(
        payload(CURRENT_VERSION, [entity("Twice", [("Health", {"current": "1"}), ("Health", {"current": "2"})])])
    )
    explicit_name = seal(payload(CURRENT_VERSION, [entity("Named", [("NameComponent", {"name": "Other"})])]))
    parent_self = seal(payload(CURRENT_VERSION, [entity("Loop", [transform("0")])]))
    trailing = seal(payload(CURRENT_VERSION, [player]) + b"\x00\x01")

    seeds = {
        "valid-v4-minimal.save": minimal,
        "valid-v4-hierarchy.save": hierarchy,
        "valid-v3-migrates.save": v3,
        "crc-mismatch.save": bytes(crc_mismatch),
        "v4-version-field-damaged.save": bytes(version_damaged),
        "entity-count-overclaim.save": entity_overclaim,
        "metadata-length-overclaim.save": metadata_overclaim,
        "duplicate-component-type.save": duplicate_type,
        "explicit-namecomponent.save": explicit_name,
        "parent-self-reference.save": parent_self,
        "trailing-bytes.save": trailing,
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in seeds.items():
        (args.output / name).write_bytes(data)
    total = sum(len(data) for data in seeds.values())
    print(f"generated {len(seeds)} .spark_save seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
