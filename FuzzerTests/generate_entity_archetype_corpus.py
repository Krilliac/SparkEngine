#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the .archetype reader.

Each seed is a whole .archetype file as LoadArchetypeFromFile reads it through
Spark::ECS::ParseArchetypeDefinition, or a damaged variant.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "entity-archetype-loader"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    many = b"name = Crowd\n" + b"".join(b"component = Marker%d: %d / %d\n" % (i, i, i * 2) for i in range(64))
    seeds = {
        "valid-guard.archetype": b"// Guard NPC\nname = GuardNPC\ncategory = Characters\n"
        b"component = Transform: 0,0,0 / 0,0,0 / 1,1,1\n"
        b"component = LightComponent: Point / 1,1,1 / 1.0 / 10.0\ncomponent = NameComponent: Guard\n",
        "valid-minimal.archetype": b"name = Empty\n",
        "valid-no-params.archetype": b"name = Bare\ncomponent = RigidBody\ncomponent = Collider:\n",
        "valid-crlf-comments.archetype": b"// c\r\n\r\nname = Crlf\r\ncategory =  Props \r\n"
        b"component = Transform : 1,2,3 \r\nnot a pair\r\nunknown = x\r\n",
        "empty-params.archetype": b"name = Gaps\ncomponent = Light: / /  / Spot /\ncomponent = : orphan\n",
        "colon-and-equals.archetype": b"name = A=B\ncomponent = Script: path:to:file.as / k=v\n",
        "missing-name.archetype": b"category = Props\ncomponent = Transform: 0,0,0\n",
        "many-components.archetype": many,
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
