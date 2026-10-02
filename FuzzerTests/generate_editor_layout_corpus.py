#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor panel-layout reader.

Each seed is a whole layout file as EditorLayoutManager::LoadLayout reads it: what
WriteLayoutFile writes for the adapter's registered panels (Hierarchy, Inspector,
SceneView, Console, Asset Browser), the legacy dialect, damaged variants, and the three
numeric-narrowing regression fixtures.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-layout"


def panel(name: str, **fields: str) -> str:
    values = {
        "displayName": f'"{name}"',
        "dock": "0",
        "sizeX": "300",
        "sizeY": "200",
        "posX": "0",
        "posY": "0",
        "visible": "true",
        "floating": "false",
        "canClose": "true",
        "canDock": "true",
        "dockRatio": "0.25",
        "tabOrder": "0",
        "parentDock": '""',
    }
    values.update(fields)
    body = [f'        "name": "{name}"'] + [f'        "{key}": {value}' for key, value in values.items()]
    return "      {\n" + ",\n".join(body) + "\n      }"


def layout(name: str, panels: list[str], *, version: str | None = "1", description: str = "") -> bytes:
    """The document WriteLayoutFile writes (version None: the legacy dialect)."""
    head = ["{", '  "layout": {', f'    "name": "{name}",', f'    "description": "{description}",']
    if version is not None:
        head.append(f'    "version": {version},')
    head.append('    "panels": [')
    text = "\n".join(head) + "\n" + ",\n".join(panels) + "\n    ]\n  }\n}\n"
    return text.encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    registered = [
        panel("Hierarchy", dock="0", sizeX="280.5", sizeY="900"),
        panel("Inspector", dock="1", sizeX="350", floating="true", posX="1600", posY="120"),
        panel("SceneView", dock="4", canClose="false", dockRatio="0.6000000238"),
        panel("Console", dock="3", tabOrder="2", visible="false"),
        panel("Asset Browser", displayName='"Assets {All}"', dock="3", tabOrder="1", parentDock='"Console"'),
    ]
    seeds = {
        "valid-registered-panels.json": layout("Workbench", registered, description="Five registered panels"),
        "valid-legacy-no-version.json": layout("Legacy", registered[:2], version=None),
        "valid-unregistered-panel.json": layout("Extras", [panel("Profiler", dock="5"), registered[0]]),
        "valid-escaped-strings.json": layout("Esc\\\"aped", [panel("Console", displayName='"Tab\\there \\\\ end"')]),
        "newer-version.json": layout("Future", registered, version="2"),
        "fractional-version.json": layout("Half", registered, version="1.5"),
        "truncated.json": layout("Cut", registered)[:400],
        "content-after-panels.json": layout("Tail", registered[:1]) + b"{}\n",
        "panel-without-name.json": layout("Nameless", [registered[0], '      {\n        "dock": 2\n      }']),
        "non-numeric-fields.json": layout("Words", [panel("Hierarchy", sizeX='"wide"', tabOrder="x", dock="")]),
        # static_cast<int> of a double outside int range is undefined behaviour.
        "regression-dock-overflow.json": layout("Docks", [panel("Hierarchy", dock="1e300")]),
        "regression-tab-order-overflow.json": layout("Tabs", [panel("Console", tabOrder="-1e20")]),
        # 1e300 narrowed to float became +inf, which the writer saved as "inf" and the reader loaded as 0.
        "regression-size-overflow.json": layout("Sizes", [panel("SceneView", sizeX="1e300", posY="-1e39")]),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
