#!/usr/bin/env python3
"""Collect deterministic first-party native-source and test metrics.

The inventory is the tracked Git tree, not an open-ended filesystem walk. This
keeps generated/build trees out of repository statistics and makes every
supported first-party native source belong to exactly one reporting category.
Counts use ``wc -l`` semantics (newline bytes).
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path, PurePosixPath


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".mm", ".inl"}

# Keep the established output schema used by badges and CI. Newly covered
# first-party roots are assigned to the closest existing reporting category so
# category line totals still add up exactly to total_lines.
CATEGORIES = {
    "engine": ("SparkEngine/Source", "SparkSDK"),
    "editor": ("SparkEditor",),
    "game": ("GameModules", "Templates"),
    "services": ("SparkServer", "SparkGateway", "SparkDaemon"),
    "pipeline": (
        "SparkAssetPipelineCore",
        "SparkCooker",
        "SparkWorker",
        "SparkAutomation",
    ),
    "tests": ("Tests",),
    "tools": (
        "SparkConsole",
        "SparkShaderCompiler",
        "SparkBuild",
        "SparkInstaller",
        "SparkLauncher",
        "SparkCrashReporter",
        "cmake/mingw-shims",
    ),
}
OUTPUT_CATEGORY_NAMES = {"tests": "test", "tools": "tool"}
TEST_RE = re.compile(rb"^[ \t]*TEST(?:_F)?[ \t]*\(", re.MULTILINE)
EXCLUDED_TOP_LEVEL = {"ThirdParty", "build", "out", "dist"}


def _is_under(path: PurePosixPath, root: str) -> bool:
    root_path = PurePosixPath(root)
    return path == root_path or root_path in path.parents


def is_excluded(relative: PurePosixPath) -> bool:
    if not relative.parts:
        return True
    top = relative.parts[0]
    return top in EXCLUDED_TOP_LEVEL or top.startswith("cmake-build-")


def classify(relative: PurePosixPath) -> str | None:
    matches = [
        name
        for name, roots in CATEGORIES.items()
        if any(_is_under(relative, root) for root in roots)
    ]
    if len(matches) > 1:
        raise ValueError(
            f"native source {relative.as_posix()} matches multiple categories: "
            f"{', '.join(matches)}"
        )
    return matches[0] if matches else None


def native_source_paths(paths: list[PurePosixPath]) -> list[PurePosixPath]:
    return sorted(
        {
            path
            for path in paths
            if path.suffix.lower() in SOURCE_SUFFIXES and not is_excluded(path)
        },
        key=lambda path: path.as_posix(),
    )


def tracked_native_sources(repo_root: Path = REPO_ROOT) -> list[PurePosixPath]:
    try:
        result = subprocess.run(
            ["git", "-C", str(repo_root), "ls-files", "-z", "--cached"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValueError(f"cannot enumerate tracked repository files: {exc}") from exc

    tracked: list[PurePosixPath] = []
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        try:
            relative = PurePosixPath(raw.decode("utf-8"))
        except UnicodeDecodeError as exc:
            raise ValueError("tracked path is not valid UTF-8") from exc
        tracked.append(relative)
    return native_source_paths(tracked)


def newline_count(path: Path) -> int:
    total = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            total += chunk.count(b"\n")
    return total


def collect(
    repo_root: Path = REPO_ROOT,
    *,
    tracked_paths: list[PurePosixPath] | None = None,
) -> dict[str, int]:
    files_by_category: dict[str, list[Path]] = {name: [] for name in CATEGORIES}
    unclassified: list[str] = []

    inventory = (
        tracked_native_sources(repo_root)
        if tracked_paths is None
        else native_source_paths(tracked_paths)
    )
    for relative in inventory:
        category = classify(relative)
        if category is None:
            unclassified.append(relative.as_posix())
            continue

        path = repo_root / Path(*relative.parts)
        # A tracked-but-locally-deleted source is absent from the working tree
        # being measured. Symlinks remain excluded from the native corpus.
        if path.is_file() and not path.is_symlink():
            files_by_category[category].append(path)

    if unclassified:
        rendered = "\n  - ".join(unclassified)
        raise ValueError(
            "tracked first-party native sources are not classified:\n  - " + rendered
        )

    lines_by_category = {
        name: sum(newline_count(path) for path in files)
        for name, files in files_by_category.items()
    }
    all_files = [path for files in files_by_category.values() for path in files]

    test_definitions = 0
    test_files = 0
    for path in all_files:
        if path.suffix.lower() not in {".cpp", ".mm"}:
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
    try:
        metrics = collect()
    except ValueError as exc:
        parser.error(str(exc))
    if args.shell:
        for key, value in metrics.items():
            print(f"{key.upper()}={value}")
    else:
        print(json.dumps(metrics, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
