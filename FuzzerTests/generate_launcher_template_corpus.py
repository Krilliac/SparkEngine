#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the launcher template.json reader.

Each seed is planted as "<tmp>/Templates/Fuzz Template/template.json" by the fuzz adapter
(FuzzLauncherTemplateProduction.cpp) and read with SparkLauncher::ReadTemplateEntry. A seed
that starts with three NUL bytes plants something else, chosen by its fourth byte: 1 a FIFO,
2 a directory, 3 nothing, 4 the rest of the seed padded past the 64 KiB cap.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "launcher-template-json"

TAG = b"\x00\x00\x00"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-shipped.json": b"""{
  "name": "Blank3D",
  "identity": "blank-3d",
  "description": "A ready-to-edit 3D composition study.",
  "genre": "General",
  "gameModule": "Blank3D",
  "defaultScene": "Scenes/Default.sparkscene",
  "features": ["primitive", "ground"]
}
""",
        "valid-minimal.json": b'{"name":"Mini"}',
        "missing-fields.json": b'{"identity":"no-name","features":[]}',
        "key-inside-string.json": b'{"description":"see \\"name\\" below","name":"Real"}',
        "escaped-quote.json": b'{"name":"Say \\"hi\\"","genre":"Quote"}',
        "no-colon.json": b'{"name" "Missing colon", "genre": "G"}',
        "unterminated.json": b'{"name": "open string',
        "nested-first.json": b'{"meta":{"name":"Inner"},"name":"Outer","gameModule":"\xc3\xa9\xff"}',
        "binary-noise.bin": bytes(range(256)),
        "unknown-plant-tag.bin": TAG + b'\x09{"name":"Tagged"}',
        "plant-directory.bin": TAG + b"\x02",
        "plant-missing.bin": TAG + b"\x03",
        "malformed-fifo-manifest.bin": TAG + b"\x01",
        "malformed-oversized-manifest.bin": TAG + b'\x04{"name":"Huge Kit","description":"padded"}',
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
