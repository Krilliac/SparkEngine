#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor scene load entry.

The first byte of each seed picks the file name the adapter writes the rest under (see
kNames in FuzzEditorSceneLoadProduction.cpp): 0 .sparkscene, 1 .json, 2 .scenejson,
3 .SparkScene, 4 .JSON, 5 .SCENEJSON, 6 .scene, 7 .bin, 8 no extension, 9 .json.bak,
10 .sparkscene.tmp, 11 a trailing dot. The documents come from
generate_editor_scene_json_corpus.py.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_editor_scene_json_corpus import documents  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-scene-load-dispatch"

SCENE_FILE_MAGIC = 0x53504B53


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    docs = documents()
    binary_header = struct.pack("<II", SCENE_FILE_MAGIC, 2) + b"\x00" * 24
    seeds = {
        "sparkscene-full.bin": bytes([0]) + docs["valid-full.sparkscene"],
        "json-minimal.bin": bytes([1]) + docs["valid-minimal.sparkscene"],
        "scenejson-version-one.bin": bytes([2]) + docs["valid-version-one.sparkscene"],
        "uppercase-extension-escapes.bin": bytes([3]) + docs["valid-escapes.sparkscene"],
        "uppercase-json-count-mismatch.bin": bytes([4]) + docs["reject-count-mismatch.sparkscene"],
        "uppercase-scenejson-truncated.bin": bytes([5]) + docs["reject-truncated.sparkscene"],
        "binary-extension-valid-json.bin": bytes([6]) + docs["valid-minimal.sparkscene"],
        "bin-extension-magic-header.bin": bytes([7]) + binary_header,
        "no-extension-valid-json.bin": bytes([8]) + docs["valid-minimal.sparkscene"],
        "backup-extension-valid-json.bin": bytes([9]) + docs["valid-full.sparkscene"],
        "temporary-extension-valid-json.bin": bytes([10]) + docs["valid-minimal.sparkscene"],
        "trailing-dot-object.bin": bytes([11]) + b"{}",
        "json-name-magic-header.bin": bytes([1]) + binary_header,
        "sparkscene-short-brace.bin": bytes([0]) + b"{ }",
        "sparkscene-array-root.bin": bytes([0]) + b"[]",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
