#!/usr/bin/env python3
"""Exercise SparkLauncher's shipped headless launch-request validation."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


def run(launcher: Path, arguments: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(launcher), *arguments],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: run_launcher_headless_smoke.py <SparkLauncher>", file=sys.stderr)
        return 2

    launcher = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="spark-launcher-smoke-") as temporary:
        root = Path(temporary)
        project_root = root / "project"
        binary_root = project_root / "bin"
        project_root.mkdir()
        binary_root.mkdir()
        (binary_root / ("SparkEditor.exe" if sys.platform == "win32" else "SparkEditor")).touch()

        valid_project = project_root / "Sample.sparkproject"
        valid_project.touch()
        valid = run(
            launcher,
            ["--validate-launch-request", str(valid_project), "--binary-directory", str(binary_root)],
            valid_project.parent,
        )
        if valid.returncode != 0:
            print(f"valid launch request failed ({valid.returncode}): {valid.stderr}", file=sys.stderr)
            return 1
        if "Launch request validated: target=Editor" not in valid.stdout:
            print(f"valid launch request lacked its confirmation: {valid.stdout!r}", file=sys.stderr)
            return 1

        invalid_project = project_root / "Sample.txt"
        invalid_project.touch()
        invalid = run(
            launcher,
            ["--validate-launch-request", str(invalid_project), "--binary-directory", str(binary_root)],
            invalid_project.parent,
        )
        if invalid.returncode != 2:
            print(f"invalid launch request returned {invalid.returncode}, expected 2", file=sys.stderr)
            return 1
        if "Expected a .sparkproject file" not in invalid.stderr:
            print(f"invalid launch request lacked its diagnostic: {invalid.stderr!r}", file=sys.stderr)
            return 1

        missing = run(launcher, ["--validate-launch-request"], project_root)
        if missing.returncode != 2 or "requires a project path" not in missing.stderr:
            print(f"missing project argument was not rejected: {missing.returncode}, {missing.stderr!r}", file=sys.stderr)
            return 1

        unknown = run(
            launcher,
            ["--validate-launch-request", str(valid_project), "--unexpected"],
            valid_project.parent,
        )
        if unknown.returncode != 2 or "Unexpected argument" not in unknown.stderr:
            print(f"unknown validation argument was not rejected: {unknown.returncode}, {unknown.stderr!r}", file=sys.stderr)
            return 1

        trailing = run(
            launcher,
            [
                "--validate-launch-request",
                str(valid_project),
                "--binary-directory",
                str(binary_root),
                "--unexpected",
            ],
            valid_project.parent,
        )
        if trailing.returncode != 2 or "Unexpected argument" not in trailing.stderr:
            print(f"trailing validation argument was not rejected: {trailing.returncode}, {trailing.stderr!r}", file=sys.stderr)
            return 1

    print("SparkLauncher headless smoke passed: valid request accepted; invalid request rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
