#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the UI layout readers.

The fuzz adapter (FuzzUILayoutProduction.cpp) hands every seed to both readers:
UILayoutLoader::LoadFromJSON, which scans a "children" array of {"type", "name", "text",
"src", "x", "y", "w", "h"} objects (type label, button, progressbar, image or panel; panels
nest through their own "children"), and UIFactory::ParseConfig, which reads
`type key="value" ...` lines. So the corpus holds layouts in both shapes plus damaged ones.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "ui-layout"
MAX_NESTING_DEPTH = 64  # UILayoutLoader::kMaxNestingDepth


def nested_panels(depth: int) -> bytes:
    opening = "".join(f'{{"type":"panel","name":"p{i}","children":[' for i in range(depth))
    return ('{"children":[' + opening + "]}" * depth + "]}").encode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-layout.json": b"""{
  "name": "hud",
  "children": [
    {"type": "label", "name": "title", "text": "Spark", "x": 10, "y": 12.5, "w": 200, "h": 24},
    {"type": "button", "name": "play", "text": "Play", "x": -4, "y": 40},
    {"type": "progressbar", "name": "health", "w": 180.75},
    {"type": "image", "name": "logo", "src": "Textures/logo.png", "x": 0.001}
  ]
}
""",
        "valid-nested-panels.json": b"""{"children":[
  {"type":"panel","name":"outer","x":5,"children":[
    {"type":"label","name":"inner-label","text":"nested"},
    {"type":"panel","name":"inner","children":[{"type":"button","name":"deep","text":"OK"}]}
  ],"y":7}
]}""",
        "valid-config.uicfg": b"""# HUD widgets
panel id="hud" bind="hud.visible" layout="vertical"
label id="title" text="Spark" size="24"
button{ id="ok" label="OK" onClick="close"
\tprogressBar id="health" bind="player.health"
image id="logo" src="Textures/logo.png"
""",
        "layout-at-depth-bound.json": nested_panels(MAX_NESTING_DEPTH),
        "malformed-unclosed.json": b'{"children":[{"type":"label","name":"cut","text":"no end"',
        "no-children.json": b'{"type":"panel","name":"lonely","items":[{"type":"label","name":"x"}]}',
        "children-not-array.json": b'{"children":{"type":"label","name":"x"},"tail":[{"type":"label","name":"y"}]}',
        "quote-tricks.json": b'{"children":[{"type":"label","name":"a\\"b","text":"{[}]\\\\"},'
        b'{"type":"button","name":"x\\\\","text":"\\"}"}]}',
        "numbers-edge.json": b'{"children":[{"type":"label","name":"n","x":-,"y":1e999,'
        b'"w":99999999999999999999999999999999999999999,"h":-.5}]}',
        "nameless-and-unknown.json": b'{"children":[{"type":"label"},{"type":"slider","name":"s"},'
        b'{"name":"untyped"},{},{"type":"panel","name":"","children":[{"type":"label","name":"lost"}]}]}',
        "sibling-after-children.json": b'{"children":[{"type":"label","name":"in"}],"extra":{"type":"label","name":"out"}}',
        "config-edge.uicfg": b'{ a="1" id="x"\r\n\tlabel \t="tab key" ="empty key"\n{\nlabel\n'
        b'panel{x="1"y="2" id="a" id="b"\n   # indented comment\nbutton x="unterminated\n',
        "binary-noise.bin": bytes(range(256)),
        "malformed-nesting-beyond-bound.json": nested_panels(1000),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
