#!/usr/bin/env python3
"""Collect deterministic C++ LOC and test-definition metrics.

Counts use ``wc -l`` semantics (newline bytes), but do not depend on ``xargs``
batching. All source roots are explicit and bounded to the repository.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp"}
CATEGORIES = {
    "engine": ("SparkEngine/Source",),
    "editor": ("SparkEditor/Source",),
    "game": ("GameModules",),
    "tests": ("Tests",),
    "tools": ("SparkConsole/src", "SparkShaderCompiler/src"),
}
OUTPUT_CATEGORY_NAMES = {"tests": "test", "tools": "tool"}
TEST_RE = re.compile(rb"^[ \t]*TEST(?:_F)?[ \t]*\(", re.MULTILINE)


def source_files(roots: tuple[str, ...]) -> list[Path]:
    files: set[Path] = set()
    for relative in roots:
        root = REPO_ROOT / relative
        if not root.is_dir() or root.is_symlink():
            continue
        for path in root.rglob("*"):
            if (
                path.is_file()
                and not path.is_symlink()
                and path.suffix.lower() in SOURCE_SUFFIXES
            ):
                files.add(path)
    return sorted(files)


def newline_count(path: Path) -> int:
    total = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            total += chunk.count(b"\n")
    return total


def collect() -> dict[str, int]:
    files_by_category = {
        name: source_files(roots) for name, roots in CATEGORIES.items()
    }
    lines_by_category = {
        name: sum(newline_count(path) for path in files)
        for name, files in files_by_category.items()
    }
    all_files = sorted(
        {path for files in files_by_category.values() for path in files}
    )

    test_definitions = 0
    test_files = 0
    for path in files_by_category["tests"]:
        if path.suffix.lower() != ".cpp":
            continue
        matches = len(TEST_RE.findall(path.read_bytes()))
        test_definitions += matches
        test_files += bool(matches)

    return {
        "total_lines": sum(lines_by_category.values()),
        "file_count": len(all_files),
        **{
            f"{OUTPUT_CATEGORY_NAMES.get(name, name)}_lines": count
            for name, count in lines_by_category.items()
        },
        "test_definitions": test_definitions,
        "test_files": test_files,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--shell",
        action="store_true",
        help="emit uppercase numeric shell assignments",
    )
    args = parser.parse_args()
    metrics = collect()
    if args.shell:
        for key, value in metrics.items():
            print(f"{key.upper()}={value}")
    else:
        print(json.dumps(metrics, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
