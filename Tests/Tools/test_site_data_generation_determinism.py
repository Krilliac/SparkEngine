#!/usr/bin/env python3
"""RDY-000: two clean site-data generations produce byte-identical trees.

The published bundle declares no wall-clock timestamp: ``generatedAt`` is the
source commit's ``committedAt``, so DECLARED_TIMESTAMP_KEYS exempts nothing. A
difference anywhere, including a file that exists in only one tree, fails.
"""

from __future__ import annotations

import filecmp
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))
import common  # noqa: E402
GENERATOR = REPO_ROOT / "tools" / "site-data" / "generate.py"
BUNDLE_INDEX = "latest.json"
MINIMUM_FILES = 100
# generatedAt is derived from the commit, not the clock; nothing is exempt today.
DECLARED_TIMESTAMP_KEYS: frozenset[str] = frozenset()


def tree_files(root: Path) -> set[str]:
    return {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}


def tree_differences(left: Path, right: Path, limit: int = 20) -> list[str]:
    """Paths present in only one tree, then paths whose bytes differ; at most ``limit`` entries."""
    left_files = tree_files(left)
    right_files = tree_files(right)
    differences = [f"only in first: {path}" for path in sorted(left_files - right_files)]
    differences += [f"only in second: {path}" for path in sorted(right_files - left_files)]
    for path in sorted(left_files & right_files):
        if len(differences) >= limit:
            break
        if not filecmp.cmp(left / path, right / path, shallow=False):
            differences.append(f"content differs: {path}")
    return differences[:limit]


def generate(output: Path) -> None:
    dirty = common.git_dirty_paths()
    if dirty:
        raise AssertionError(f"clean generation requires a clean checkout: {dirty}")
    # Each invocation independently regenerates documentation health. No shared
    # health file and no dirty-tree or health-skipping exemption can prove this.
    command = [sys.executable, "-B", str(GENERATOR), "--output", str(output)]
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env={**os.environ, "PYTHONHASHSEED": "random"},
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"generate.py failed ({result.returncode}):\n{result.stdout}\n{result.stderr}")


class TreeComparisonTests(unittest.TestCase):
    """The comparator itself cannot report two different trees as equal."""

    def write(self, root: Path, files: dict[str, bytes]) -> None:
        for relative, payload in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

    def test_one_byte_difference_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            self.write(Path(left), {"a/b.json": b'{"x": 1}'})
            self.write(Path(right), {"a/b.json": b'{"x": 2}'})
            self.assertEqual(["content differs: a/b.json"], tree_differences(Path(left), Path(right)))

    def test_differing_file_sets_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            self.write(Path(left), {"same.txt": b"x", "extra.txt": b"y"})
            self.write(Path(right), {"same.txt": b"x", "other.txt": b"y"})
            self.assertEqual(
                ["only in first: extra.txt", "only in second: other.txt"],
                tree_differences(Path(left), Path(right)),
            )

    def test_identical_trees_have_no_differences(self) -> None:
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            for root in (left, right):
                self.write(Path(root), {"x/y.txt": b"same"})
            self.assertEqual([], tree_differences(Path(left), Path(right)))


class GenerationPolicyTests(unittest.TestCase):
    def test_dirty_checkout_is_rejected_before_generation(self) -> None:
        with mock.patch.object(common, "git_dirty_paths", return_value=[" M README.md"]), \
             mock.patch.object(subprocess, "run") as run:
            with self.assertRaisesRegex(AssertionError, "clean checkout.*README.md"):
                generate(Path("unused-output"))
            run.assert_not_called()

    def test_each_generation_recomputes_health_without_waivers(self) -> None:
        with mock.patch.object(common, "git_dirty_paths", return_value=[]), \
             mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            generate(Path("first"))
            generate(Path("second"))
        self.assertEqual(2, run.call_count)
        for call in run.call_args_list:
            self.assertEqual([sys.executable, "-B", str(GENERATOR), "--output"], call.args[0][:-1])
            self.assertEqual("random", call.kwargs["env"]["PYTHONHASHSEED"])


class GenerationDeterminismTests(unittest.TestCase):
    """Two generate.py runs with identical flags produce the same bytes."""

    def test_two_generations_are_byte_identical(self) -> None:
        self.assertEqual(frozenset(), DECLARED_TIMESTAMP_KEYS)
        self.assertEqual([], common.git_dirty_paths(), "determinism requires a clean checkout")
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            first_root = Path(first) / "bundle"
            second_root = Path(second) / "bundle"
            generate(first_root)
            generate(second_root)
            files = tree_files(first_root)
            self.assertIn(BUNDLE_INDEX, files)
            self.assertGreaterEqual(len(files), MINIMUM_FILES, "an empty or truncated bundle proves nothing")
            differences = tree_differences(first_root, second_root)
            self.assertEqual([], differences, "\n".join(differences))


if __name__ == "__main__":
    unittest.main()
