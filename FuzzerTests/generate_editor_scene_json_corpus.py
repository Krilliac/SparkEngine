#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor JSON scene decoder.

Each seed is a whole .sparkscene document as SceneSerializer::LoadJSON hands it to
SparkEditor::DecodeSceneJSONDocument: valid version-2 and version-1 scenes and damaged
variants of them. generate_editor_scene_load_corpus.py reuses these documents.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-scene-json"


def _mesh_renderer(object_id: int) -> dict:
    return {
        "type": "MeshRenderer",
        "objectID": object_id,
        "enabled": True,
        "data": {
            "schema": 1,
            "fields": {
                "meshAssetPath": "Meshes/crate.obj",
                "materialAssetPath": "Materials/wood.sparkmat",
                "castShadows": True,
                "receiveShadows": False,
                "renderLayer": 2,
                "tintColor": [1.0, 0.5, 0.25, 1.0],
            },
        },
    }


def _light(object_id: int) -> dict:
    return {
        "type": "Light",
        "objectID": object_id,
        "enabled": False,
        "data": {
            "schema": 1,
            "fields": {
                "type": 2,
                "color": [1.0, 0.9, 0.8],
                "intensity": 3.5,
                "range": 25.0,
                "spotAngle": 60.0,
                "spotInnerAngle": 45.0,
                "castShadows": True,
                "shadowMapSize": 2048,
            },
        },
    }


def _script(object_id: int) -> dict:
    return {
        "type": "Script",
        "objectID": object_id,
        "enabled": True,
        "data": {"schema": 1, "fields": {"scriptPath": "Scripts/door.as", "className": "Door", "autoStart": True}},
    }


def full_scene() -> dict:
    return {
        "sceneName": "Courtyard",
        "version": 2,
        "description": "Three objects, a hierarchy, payload components and asset references",
        "objectCount": 3,
        "componentCount": 5,
        "assetReferenceCount": 2,
        "timestamp": 1790000000,
        "gravity": [0.0, -9.81, 0.0],
        "ambientColor": [0.2, 0.25, 0.3, 1.0],
        "ambientIntensity": 0.75,
        "objects": [
            {
                "id": 1,
                "name": "Gate",
                "tag": "Structure",
                "layer": 0,
                "active": True,
                "staticObject": True,
                "transform": {
                    "position": [0.0, 0.0, 5.0],
                    "rotation": [0.0, 0.7071068, 0.0, 0.7071068],
                    "scale": [2.0, 3.0, 0.5],
                    "parentID": 0,
                    "childIDs": [2, 3],
                },
                "componentTypes": ["Transform", "MeshRenderer"],
            },
            {
                "id": 2,
                "name": "Lantern",
                "tag": "Light",
                "layer": 1,
                "active": True,
                "staticObject": False,
                "transform": {
                    "position": [0.5, 2.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                    "scale": [1.0, 1.0, 1.0],
                    "parentID": 1,
                },
                "componentTypes": ["Transform", "Light"],
            },
            {
                "id": 3,
                "name": "Latch",
                "tag": "Default",
                "layer": -4,
                "active": False,
                "staticObject": False,
                "transform": {"position": [-0.5, 1.0, 0.0], "parentID": 1},
                "componentTypes": ["Script"],
            },
        ],
        "components": [
            {"type": "Transform", "objectID": 1, "enabled": True},
            _mesh_renderer(1),
            {"type": "Transform", "objectID": 2, "enabled": True},
            _light(2),
            _script(3),
        ],
        "environment": {
            "skyType": 2,
            "skyColor": [0.4, 0.6, 0.9, 1.0],
            "horizonColor": [0.8, 0.8, 0.9, 1.0],
            "skyboxAssetPath": "Textures/sky.dds",
            "fogEnabled": True,
            "fogColor": [0.6, 0.6, 0.6, 1.0],
            "fogDensity": 0.02,
            "fogStart": 5.0,
            "fogEnd": 80.0,
            "windDirection": [1.0, 0.0, 0.5],
            "windStrength": 2.0,
            "windTurbulence": 0.3,
            "bloomEnabled": True,
            "bloomIntensity": 1.5,
            "bloomThreshold": 0.9,
            "tonemappingEnabled": True,
            "exposure": 1.2,
            "gamma": 2.2,
        },
        "defaultCamera": {
            "projectionType": 1,
            "fieldOfView": 60.0,
            "orthographicSize": 8.0,
            "nearPlane": 0.05,
            "farPlane": 500.0,
            "clearColor": [0.1, 0.1, 0.12, 1.0],
            "isMainCamera": True,
            "renderTargetWidth": 1280,
            "renderTargetHeight": 720,
        },
        "assetReferences": [
            {
                "assetPath": "Meshes/crate.obj",
                "assetType": "mesh",
                "lastModified": 1789999999,
                "fileSize": 40960,
                "checksum": "9f86d081884c7d65",
                "dependencies": ["Materials/wood.sparkmat", "Textures/wood_albedo.dds"],
            },
            {"assetPath": "Scripts/door.as", "assetType": "script"},
        ],
    }


def version_one_scene() -> dict:
    return {
        "sceneName": "Legacy",
        "version": 1,
        "objects": [
            {"id": 7, "name": "Root", "position": [1.0, 2.0, 3.0], "componentTypes": ["Transform"]},
            {
                "id": 8,
                "name": "Sprite",
                "transform": {"parentID": 0, "childIDs": []},
                "componentTypes": ["SpriteAnimator"],
            },
        ],
        "components": [
            {"type": "Transform", "objectID": 7},
            {"type": "SpriteAnimator", "objectID": 8, "enabled": False},
        ],
    }


def _dump(document: dict) -> bytes:
    return (json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def documents() -> dict[str, bytes]:
    """Every seed document, by file name."""
    full = full_scene()

    count_mismatch = full_scene()
    count_mismatch["objectCount"] = 5

    cycle = full_scene()
    cycle["objects"][0]["transform"]["parentID"] = 2
    cycle["objects"][1]["transform"]["childIDs"] = [1]

    unknown_component = full_scene()
    unknown_component["components"][4]["type"] = "Teleporter"

    bad_schema = full_scene()
    bad_schema["components"][1]["data"]["schema"] = 2

    missing_field = full_scene()
    del missing_field["components"][3]["data"]["fields"]["shadowMapSize"]

    raw_v1_payload = version_one_scene()
    raw_v1_payload["objects"][0]["componentTypes"].append("MeshRenderer")
    raw_v1_payload["components"].append({"type": "MeshRenderer", "objectID": 7, "data": "0a0b0c0d0e0f"})

    full_bytes = _dump(full)
    return {
        "valid-full.sparkscene": full_bytes,
        "valid-minimal.sparkscene": b'{"version": 2}',
        "valid-version-one.sparkscene": _dump(version_one_scene()),
        "valid-escapes.sparkscene": (
            b'{"version":2,"sceneName":"Caf\\u00e9 \\ud83c\\udf05","description":"tab\\there\\/slash",'
            b'"objects":[{"id":18446744073709551614,"name":"\\u0000nul","tag":"\\"q\\"","layer":-2147483648}]}'
        ),
        "reject-count-mismatch.sparkscene": _dump(count_mismatch),
        "reject-duplicate-version.sparkscene": b'{"version": 2, "objects": [], "version": 2}',
        "reject-parent-cycle.sparkscene": _dump(cycle),
        "reject-unknown-component.sparkscene": _dump(unknown_component),
        "reject-schema-version.sparkscene": _dump(bad_schema),
        "reject-missing-payload-field.sparkscene": _dump(missing_field),
        "reject-version-one-raw-payload.sparkscene": _dump(raw_v1_payload),
        "reject-nonfinite-number.sparkscene": b'{"version": 2, "gravity": [0, 1e400, 0]}',
        "reject-deep-nesting.sparkscene": b'{"version": 2, "extra": ' + b"[" * 200 + b"]" * 200 + b"}",
        "reject-invalid-utf8.sparkscene": b'{"version": 2, "objects": [{"id": 1, "name": "\xc3\x28"}]}',
        "reject-lone-surrogate.sparkscene": b'{"version": 2, "sceneName": "\\udc00"}',
        "reject-truncated.sparkscene": full_bytes[: len(full_bytes) // 2],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(documents().items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
