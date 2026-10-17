#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor preferences reader.

Each seed is a whole editor-preferences.ini as LoadEditorPreferences hands it to
SparkEditor::ParseEditorPreferences: what SaveEditorPreferences writes, hand-edited variants,
and damaged files. Seeds are LF-only so they stay byte-exact whatever the checkout's line-ending
settings are.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-preferences"


def lines(*entries: str) -> bytes:
    return ("\n".join(entries) + "\n").encode("utf-8")


SAVED = lines(
    "version=1",
    "palette=Ember Studio",
    "appearance=dark",
    "uiScale=1.250",
    "density=standard",
    "alwaysShowFocus=false",
    "reduceMotion=false",
    "showStartPage=true",
    "showStatusBar=true",
    "lastLayout=Layouts/Default.layout",
    "recentScene=Scenes/Arena.scene",
    "recentScene=Scenes/Menu.scene",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-saved.ini": SAVED,
        "valid-version-only.ini": b"version=1",
        "valid-hand-edited.ini": lines(
            "# hand edited",
            "; another comment",
            "  version = 1  ",
            "palette = Graphite Signal",
            "appearance=hc",
            "uiScale=0",
            "density=compact",
            "alwaysShowFocus=1",
            "reduceMotion=true",
            "recentScene=Scenes/Café.scene",
        ),
        "unknown-keys-and-malformed-lines.ini": lines(
            "version=1",
            "futureKey=whatever",
            "no equals sign here",
            "=empty key",
            "appearance=purple",
            "density=roomy",
            "showStatusBar=maybe",
        ),
        "bad-scales.ini": lines(
            "version=1",
            "uiScale=nan",
            "uiScale=inf",
            "uiScale=1e3",
            "uiScale=-0.5",
            "uiScale=99999999999999999",
            "uiScale=1.0625",
        ),
        "too-many-recent-scenes.ini": lines(
            "version=1",
            *(f"recentScene=Scenes/Level{index:02}.scene" for index in range(14)),
            "recentScene=Scenes/Level03.scene",
        ),
        "missing-version.ini": lines("palette=Ember Studio", "appearance=light"),
        "unsupported-version.ini": lines("version=2", "palette=Ember Studio"),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
