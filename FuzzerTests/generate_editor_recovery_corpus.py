#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor recovery snapshot reader.

The adapter writes a seed's bytes up to the first NUL as the project's primary recovery
file (recovery-v1.json) and the bytes after it, when there is a NUL, as the backup. Each
document has the shape EditorRecoveryStore::Save writes (SnapshotToJson): a bounded JSON
envelope whose "serializedWorld" string is a reflected scene that must survive
Spark::DeserializeInto in StrictRecovery mode. The project identity matches the adapter's.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-recovery-snapshot"

IDENTITY = "/projects/fuzz/Fuzz.sparkproject"

TRANSFORM = {
    "type": "Transform",
    "fields": {
        "position": "1.000000,2.000000,3.000000",
        "rotation": "0.000000,90.000000,0.000000",
        "scale": "1.000000,1.000000,1.000000",
    },
}

WORLD = {
    "version": 1,
    "entities": [
        {"id": 0, "name": "Root", "parent": -1, "components": [TRANSFORM]},
        {
            "id": 1,
            "name": "Camera",
            "parent": 0,
            "components": [
                TRANSFORM,
                {
                    "type": "Camera",
                    "fields": {
                        "fov": "60.000000",
                        "nearPlane": "0.100000",
                        "farPlane": "1000.000000",
                        "isMainCamera": "true",
                    },
                },
            ],
        },
    ],
}


def snapshot(**overrides) -> dict:
    document = {
        "schemaVersion": 1,
        "projectIdentity": IDENTITY,
        "projectRelativeScene": "Scenes/Arena.sparkscene",
        "sceneDisplayName": "Arena",
        "serializedWorld": json.dumps(WORLD, indent=1),
        "layoutIniPath": "Config/layout.ini",
        "dirtySequence": 12,
        "capturedUnixMilliseconds": 1790000000123,
        "recentOperations": ["Move Root", "Rename Camera"],
    }
    document.update(overrides)
    return document


def dump(document: dict) -> bytes:
    return json.dumps(document, indent=2).encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    valid = dump(snapshot())
    broken_world = dict(WORLD)
    broken_world["entities"] = [{"id": 0, "name": "Root", "parent": 5, "components": [TRANSFORM]}]
    seeds = {
        "valid-primary.json": valid,
        "valid-minimal-paths.json": dump(
            snapshot(projectRelativeScene="", layoutIniPath="", recentOperations=[], dirtySequence=0)
        ),
        "damaged-primary-valid-backup.bin": valid[:40] + b"\x00" + valid,
        "valid-primary-damaged-backup.bin": valid + b"\x00" + b'{"schemaVersion": 1',
        "foreign-project.json": dump(snapshot(projectIdentity="/projects/other/Other.sparkproject")),
        "escaping-scene-path.json": dump(snapshot(projectRelativeScene="../outside/Arena.sparkscene")),
        "absolute-layout-path.json": dump(snapshot(layoutIniPath="/etc/layout.ini")),
        "schema-version-two.json": dump(snapshot(schemaVersion=2)),
        "world-not-json.json": dump(snapshot(serializedWorld="not a scene")),
        "world-unknown-parent.json": dump(snapshot(serializedWorld=json.dumps(broken_world))),
        "too-many-operations.json": dump(snapshot(recentOperations=["op"] * 51)),
        "inexact-sequence.json": dump(snapshot(dirtySequence=9007199254740993)),
        "fractional-timestamp.json": dump(snapshot(capturedUnixMilliseconds=1.5)),
        "deep-envelope.json": b'{"schemaVersion": 1, "extra": ' + b"[" * 80 + b"]" * 80 + b"}",
        "truncated.json": valid[: len(valid) // 2],
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
