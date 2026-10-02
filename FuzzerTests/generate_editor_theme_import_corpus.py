#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor theme import reader.

Each seed is a whole theme file as ThemeCustomizer::ImportTheme hands it to
SparkEditor::ParseThemeDocument: what ExportTheme writes, hand-edited variants, damaged
documents, and the non-finite colour regression fixture.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-theme-import"

COLOR_KEYS = (
    "background", "backgroundDark", "backgroundLight", "backgroundAccent", "backgroundHeader",
    "backgroundActive", "backgroundHover", "backgroundSelected", "text", "textDisabled",
    "textSecondary", "textAccent", "textWarning", "textError", "textSuccess", "button",
    "buttonHovered", "buttonActive", "frame", "frameHovered", "frameActive", "border",
    "borderLight", "borderAccent", "borderSeparator",
)


def exported(name: str, description: str, author: str, colors: dict[str, str] | None = None) -> bytes:
    """The document ExportTheme writes; colours default to a grey ramp."""
    lines = ["{", f'  "name": "{name}",', f'  "description": "{description}",', f'  "author": "{author}",',
             '  "colors": {']
    entries = []
    for index, key in enumerate(COLOR_KEYS):
        value = (colors or {}).get(key)
        if value is None:
            level = round(0.1 + index * 0.03, 2)
            value = f"{level}, {level}, {level}, 1"
        entries.append(f'    "{key}": [{value}]')
    lines.append(",\n".join(entries))
    lines.append("  }")
    lines.append("}")
    return ("\n".join(lines) + "\n").encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-exported-theme.json": exported("Spark Dusk", "Warm dark theme", "Spark Engine Team"),
        "valid-name-only.json": b'{"name": "Bare"}',
        "valid-unicode-and-backslashes.json": exported("Café \\\\ Night", "line one\\nline two", "神"),
        "three-component-colors.json": exported("Opaque", "alpha omitted",
                                                "x", {"background": "0.5, 0.25, 0.125", "text": "1,1,1"}),
        "missing-name.json": exported("", "no name", "nobody"),
        "malformed-colors.json": exported("Broken", "bad arrays", "x",
                                          {"background": "0.5, 0.25", "button": "red, green, blue",
                                           "frame": "", "border": "1, 2, 3, 4, 5"}),
        "unterminated.json": b'{\n  "name": "Cut',
        "out-of-range-colors.json": exported("Loud", "beyond 0..1", "x",
                                             {"background": "-4, 255, 1e30, -0", "text": "0x1p-1, 1e-40, 7, 2"}),
        # sscanf's %f accepts nan and inf and overflows to infinity; ImGui's colour packing is
        # undefined for NaN.
        "regression-nonfinite-color.json": exported("Poisoned", "nan colours", "x",
                                                    {"background": "nan, 0, 0, 1", "text": "inf, 1, 1, 1",
                                                     "button": "1, 1, 1e39, 1", "border": "0, 0, 0, -nan"}),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
