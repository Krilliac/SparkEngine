#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the dynamic-plugin metadata gate.

Each seed is a whole `<plugin>.sparkplugin.json` as DynamicPluginHost::Load reads it, or a
damaged variant. The fuzz adapter (FuzzPluginMetadataProduction.cpp) writes the seed beside
a fixed plugin image named `fuzz-plugin.so` whose SHA-256 is IMAGE_SHA256 below.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "plugin-metadata-json"

IMAGE = b"SparkFuzzPluginMetadata image: not a loadable plugin.\n"
IMAGE_SHA256 = hashlib.sha256(IMAGE).hexdigest()

# Spark/PluginABI.h: SPARK_PLUGIN_ABI_MAJOR 1, SPARK_PLUGIN_ABI_MINOR 1.
VALID = {
    "schema": 1,
    "id": "spark.fuzz.plugin",
    "version": "1.0.0",
    "type": "gameplay",
    "abi_major": 1,
    "abi_minor": 1,
    "entry_point": "SparkGetPluginDescriptor",
    "binary": "fuzz-plugin.so",
    "sha256": IMAGE_SHA256,
}


def document(**changes: object) -> bytes:
    value = dict(VALID)
    for key, replacement in changes.items():
        if replacement is None:
            value.pop(key)
        else:
            value[key] = replacement
    return json.dumps(value, indent=2).encode()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    assert IMAGE_SHA256 == "30733e1a836a3d81bf4b097e4568a78d9f25caea08854af251d20905ed41c68f"
    compact = json.dumps(VALID, separators=(",", ":")).encode()
    seeds = {
        "valid.json": document(),
        "valid-uppercase-hash-minor0.json": document(sha256=IMAGE_SHA256.upper(), abi_minor=0),
        "valid-escaped-identity.json": document(id="spark.été", version="1.0.0-\\u0041"),
        "wrong-binary.json": document(binary="other.so"),
        "hash-mismatch.json": document(sha256="0" * 64),
        "abi-too-new.json": document(abi_minor=2),
        "fractional-number.json": document(schema=1.5),
        "negative-number.json": document(abi_major=-1),
        "missing-field.json": document(type=None),
        "extra-field.json": document(extra=True),
        "nested-value.json": document(id={"nested": ["a", {"b": 1}]}),
        "empty-string.json": document(version=""),
        "duplicate-key.json": compact[:-1] + b',"id":"spark.fuzz.other"}',
        "truncated.json": compact[:40],
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
