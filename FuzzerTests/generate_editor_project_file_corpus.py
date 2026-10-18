#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the editor project-file readers.

The adapter reads every seed both as a .sparkproject document (CheckProjectDocument,
ReadProjectDocumentFields) and as RecentProjects.json (ReadRecentProjectsDocument), so the
corpus holds documents of both kinds, damaged variants, and the three regression fixtures.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "editor-project-file"

# The document SaveProjectFile writes, with escapes in every string kind.
SAVED_PROJECT = b"""{
  "projectFileVersion": 1,
  "name": "Courtyard \\"Remake\\"",
  "version": "1.2.0",
  "description": "Line one\\nLine two\\twith a tab and a \\\\ backslash",
  "engineVersion": "0.9.4",
  "template": "first-person",
  "defaultScene": "Scenes/Arena.sparkscene",
  "lastOpenedScene": "Scenes/Caf\\u00e9.sparkscene",
  "createdTime": 1780000000,
  "lastModified": 1790000000,
  "modules": [
    "CourtyardGame",
    "Shared\\u0001Tools"
  ],
  "scenes": [
    "Scenes/Arena.sparkscene",
    "Scenes/Caf\\u00e9.sparkscene"
  ]
}
"""

# A hand-written legacy document: no projectFileVersion, compact, keys out of order.
LEGACY_PROJECT = (
    b'{"scenes":["Main.sparkscene"],"name":"Legacy","modules":[],"lastModified": 42,'
    b'"engineVersion":"0.1.0","defaultScene":"Main.sparkscene"}'
)

# What SaveRecentProjectsList writes.
RECENT_PROJECTS = b"""{
  "recentProjects": [
    {
      "name": "Courtyard",
      "path": "/home/dev/Projects/Courtyard/Courtyard.sparkproject",
      "engineVersion": "0.9.4",
      "lastOpened": 1790000000
    },
    {
      "name": "Caf\\u00e9 Prototype",
      "path": "C:\\\\Projects\\\\Cafe\\\\Cafe.sparkproject",
      "engineVersion": "0.9.3",
      "lastOpened": 1789000000
    },
    {
      "name": "",
      "path": "",
      "engineVersion": "0.1.0",
      "lastOpened": 1
    }
  ]
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-saved-project.sparkproject": SAVED_PROJECT,
        "valid-legacy-project.sparkproject": LEGACY_PROJECT,
        "valid-recent-projects.json": RECENT_PROJECTS,
        "newer-version.sparkproject": b'{"projectFileVersion": 2, "name": "Future"}',
        "version-zero.sparkproject": b'{"projectFileVersion": 0, "name": "Zero"}',
        "version-not-integer.sparkproject": b'{"projectFileVersion": "1", "name": "Text"}',
        "truncated-project.sparkproject": SAVED_PROJECT[: len(SAVED_PROJECT) // 2],
        "whitespace-only.sparkproject": b" \t\r\n",
        "bad-escapes.sparkproject": (
            b'{"name": "\\x41", "description": "\\ud800 lone", "version": "\\u12", '
            b'"scenes": ["ok", "\\q"], "modules": ["unterminated]}'
        ),
        "key-text-as-value.sparkproject": (
            b'{"modules": ["template", "name"], "scenes": ["rpg"], "name": "Real", "description": "\\"name\\": x"}'
        ),
        "recent-unreadable-entries.json": (
            b'{"recentProjects":[{"name":"NoPath"},{"path":"/p/a.sparkproject","lastOpened":-5},'
            b'{"path":"/p/b.sparkproject","lastOpened":12abc},{"name":"Nested {braces}","path":"/p/c"}]}'
        ),
        # std::stoull threw std::out_of_range for a timestamp past 2^64 - 1.
        "regression-timestamp-overflow.sparkproject": (
            b'{"projectFileVersion": 1, "name": "Overflow", "lastModified": 99999999999999999999999999}'
        ),
        # A scene path holding ']' (escaped here) was written back raw and the next read
        # ended the array inside it, dropping that scene and every one after it.
        "regression-bracket-in-scene-path.sparkproject": (
            b'{"projectFileVersion": 1, "name": "Brackets", '
            b'"scenes": ["Scenes/Level\\u005b2\\u005d.sparkscene", "Scenes/Boss.sparkscene"], "modules": ["Game"]}'
        ),
        # A project name holding '}' (escaped here) was written back raw and the next read
        # ended the entry inside it, dropping that recent project.
        "regression-brace-in-recent-name.json": (
            b'{"recentProjects":[{"name":"Game\\u007d","path":"/p/game.sparkproject",'
            b'"engineVersion":"1.0","lastOpened":7}]}'
        ),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
