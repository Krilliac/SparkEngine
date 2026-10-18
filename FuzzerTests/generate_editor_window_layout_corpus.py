#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor window-layout reader.

Each seed is a whole window-layout file as EditorWindowManager::LoadLayoutFromFile reads
it: what SaveCurrentLayoutToFile writes, hand-edited and damaged variants, and the
monitor-index regression fixture.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-window-layout"


def panel(name: str, *, open_: bool = True, floating: bool = False, x: str = "0", y: str = "0",
          width: str = "400", height: str = "300", monitor: str = "-1") -> str:
    return (
        "      {\n"
        f'        "panelName": "{name}",\n'
        f'        "isOpen": {"true" if open_ else "false"},\n'
        f'        "isFloating": {"true" if floating else "false"},\n'
        f'        "posX": {x},\n'
        f'        "posY": {y},\n'
        f'        "width": {width},\n'
        f'        "height": {height},\n'
        f'        "monitorIndex": {monitor}\n'
        "      }"
    )


def layout(name: str, dock_ini: str, panels: list[str]) -> bytes:
    """The document SaveCurrentLayoutToFile writes."""
    text = (
        "{\n"
        '  "windowLayout": {\n'
        f'    "name": "{name}",\n'
        '    "version": 1,\n'
        f'    "dockLayoutINI": "{dock_ini}",\n'
        '    "panels": [\n' + ",\n".join(panels) + "\n    ]\n"
        "  }\n}\n"
    )
    return text.encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    dock_ini = "[Window][SceneView]\\nPos=0,19\\nSize=1280,700\\nDockId=0x00000001\\n\\n[Docking][Data]\\n"
    seeds = {
        "valid-two-panels.json": layout("Default", "", [panel("SceneView"), panel("Inspector", x="1280", width="320")]),
        "valid-floating-second-monitor.json": layout(
            'Dual \\"Screen\\"', dock_ini,
            [panel("Console", floating=True, x="-1920.5", y="100", monitor="1"), panel("Profiler", open_=False)],
        ),
        "valid-no-panels.json": layout("Empty", "", []),
        "missing-window-layout-key.json": b'{"layout": {"name": "Wrong root"}}',
        "unnamed-and-unterminated-panels.json": b'{"windowLayout": {"panels": [{"isOpen": false}, {"panelName": "A"',
        "string-and-garbage-numbers.json": layout(
            "Garbage", "", [panel("A", x='"12"', y="1e39", width="--5", height="1.5e", monitor="2.9")]
        ),
        "escapes-in-names.json": layout("Tab\\there", "a\\\\b\\\\nc", [panel("Back\\\\slash"), panel('Quote\\"d')]),
        # A monitor index outside int32 was narrowed with static_cast<int32_t>, which is undefined.
        "regression-monitor-index-overflow.json": layout("Monitors", "", [panel("Far", monitor="3e9"),
                                                                           panel("Below", monitor="-1e10")]),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
