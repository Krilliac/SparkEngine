#!/usr/bin/env python3
"""Run Node's test files one at a time for portable Node 20 compatibility."""

from __future__ import annotations

import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: run_site_runtime_tests.py NODE TEST_FILE...", file=sys.stderr)
        return 2
    node = sys.argv[1]
    failure = 0
    for test_file in sys.argv[2:]:
        result = subprocess.run([node, "--test", test_file], check=False)
        if result.returncode != 0 and failure == 0:
            failure = result.returncode
    return failure


if __name__ == "__main__":
    raise SystemExit(main())
