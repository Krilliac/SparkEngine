#!/usr/bin/env python3
"""Exercise the real engine entry point under Intel SDE CPU models (BLD-100)."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def run_model(sde: Path, model: str, engine: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(sde), model, "--", str(engine), "--version"],
        capture_output=True,
        text=True,
        timeout=90,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sde", required=True, type=Path)
    parser.add_argument("--engine", required=True, type=Path)
    args = parser.parse_args()

    for label, path in (("SDE", args.sde), ("engine", args.engine)):
        if not path.is_file():
            parser.error(f"{label} executable is missing: {path}")

    try:
        below = run_model(args.sde, "-pnr", args.engine)
        floor = run_model(args.sde, "-nhm", args.engine)
    except subprocess.TimeoutExpired as error:
        print(f"SDE process timed out: {error}", file=sys.stderr)
        return 1

    floor_output = floor.stdout + floor.stderr
    expected = (
        "SparkEngine: This processor is not supported.",
        "Missing: SSE4.2, POPCNT",
    )
    errors = []
    if below.returncode == 0:
        errors.append("-pnr accepted a CPU below the stable-v1 floor")
    if any(fragment not in below.stderr for fragment in expected):
        errors.append("-pnr did not report both missing features through the startup guard")
    if below.stdout.strip() or "SPARK_MODULE_READY" in below.stderr:
        errors.append("-pnr emitted version output or reached module initialization")
    if floor.returncode != 0:
        errors.append(f"-nhm rejected a CPU at the stable-v1 floor (exit {floor.returncode})")
    if not any(re.fullmatch(r"SparkEngine \d+\.\d+\.\d+", line) for line in floor.stdout.splitlines()):
        errors.append("-nhm did not reach the version path")
    if "This processor is not supported" in floor_output:
        errors.append("-nhm printed a below-floor rejection")

    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        print(f"-pnr exit={below.returncode}, stdout={below.stdout!r}, stderr={below.stderr!r}", file=sys.stderr)
        print(f"-nhm exit={floor.returncode}, stdout={floor.stdout!r}, stderr={floor.stderr!r}", file=sys.stderr)
        return 1
    print("Intel SDE -pnr refused before version/module initialization; -nhm reached --version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
