#!/usr/bin/env python3
"""Check that MOD-310 selector names resolve to registered CTest entries."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


SELECTORS = ("FPSSinglePlayerSlice", "FPSPackage", "FPSPublicSDK")
_TEST_LINE = re.compile(r"^\s*Test\s+#\d+:\s+(\S+)\s*$", re.MULTILINE)


def _selected_test_names(ctest: str, build_dir: Path, config: str, selector: str) -> list[str]:
    command = [
        ctest,
        "--test-dir",
        str(build_dir),
        "-C",
        config,
        "-N",
        "--no-tests=error",
        "-R",
        rf"^{re.escape(selector)}(_|$)",
    ]
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    output = f"{result.stdout}\n{result.stderr}"
    if result.returncode != 0:
        raise AssertionError(
            f"CTest selector {selector!r} failed with exit {result.returncode}:\n{output}"
        )
    names = _TEST_LINE.findall(result.stdout)
    if not names:
        raise AssertionError(f"CTest selector {selector!r} selected no registered tests:\n{output}")
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="MinSizeRel")
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args()

    failures: list[str] = []
    for selector in SELECTORS:
        try:
            names = _selected_test_names(args.ctest, args.build_dir, args.config, selector)
        except (AssertionError, OSError) as error:
            failures.append(str(error))
            continue
        print(f"{selector}: {len(names)} registered test(s) selected: {', '.join(names)}")

    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
