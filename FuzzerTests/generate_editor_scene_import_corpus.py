#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the Scene Import panel's game INI reader.

Each seed is a whole game INI .scene file as SceneImportPanel::ParseSceneFile hands it to
SparkEditor::ParseGameSceneIni: a shipped MMOFPS zone, synthetic files covering every key
and skip path, damaged float triples, and the non-finite regression fixture.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-scene-ini-import"
SHIPPED_ZONE = ROOT / "Assets" / "Scenes" / "MMOFPS" / "sanctuary_haven.scene"

EVERY_KEY = b"""# every key the reader knows, plus ones it ignores
; semicolon comment
[Scene]
name=Courtyard
author=ignored

[Object]
type=cube
name=wall_north
position=0,1.5,-10
rotation=0,90,0
scale=20,3,0.5
material=Assets/Materials/Concrete.json
tag=ignored

[Object]
type=Model
model=Assets/Models/crate.obj
position=1.25, 0, 2.5
scale=1,1,1

[SpawnPoint]
type=spawnpoint
position=0,0,0

[Terrain]
heightmap=terrain.raw

[Object]
type=terrain
tfSize=4096
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "shipped-sanctuary-haven.scene": SHIPPED_ZONE.read_bytes(),
        "every-key.scene": EVERY_KEY,
        "crlf-trailing-spaces.scene": b"[Scene]\r\nname=Windows File   \r\n[Object]  \r\ntype=cube \r\n"
        b"position=1,2,3\r\n\r\n",
        "no-final-newline.scene": b"[Object]\ntype=model\nmodel=a.obj\nposition=4,5,6",
        "short-and-malformed-triples.scene": b"[Object]\ntype=cube\nposition=1,2\nrotation=abc\n"
        b"scale=2,x,4\n[Object]\ntype=cube\nposition=  7 ,8,9\nscale=0x1p3,1e-3,-0\n",
        "sections-without-type.scene": b"[Object]\nname=untyped\n[]\nposition=1,1,1\n[Scene]\nname=Late Name\n",
        "keys-outside-sections.scene": b"type=cube\nname=orphan\nposition=1,2,3\n=\n[Object\nno equals sign\n",
        "embedded-nul.scene": b"[Object]\ntype=cube\nname=nul\x00name\nposition=1\x00,2,3\n",
        # strtof accepts nan and inf and overflows to infinity; each reached the imported Transform.
        "regression-nonfinite-transform.scene": b"[Object]\ntype=cube\nname=poisoned\nposition=nan,0,0\n"
        b"rotation=0,inf,0\nscale=1,1,1e39\n",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
