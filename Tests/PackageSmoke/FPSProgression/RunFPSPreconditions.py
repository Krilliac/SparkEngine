#!/usr/bin/env python3
"""Require real abort diagnostics from the SDK-only FPS precondition consumer."""

from __future__ import annotations

import os
import signal
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: RunFPSPreconditions.py <consumer>", file=sys.stderr)
        return 2
    executable = sys.argv[1]
    # _set_abort_behavior disables MSVC's report-fault path; abort then exits 3.
    abort_code = 3 if os.name == "nt" else -signal.SIGABRT
    expected = {
        "false": ("false", "intentional precondition failure"),
        "null": ("value", "must not be null"),
        "bound": ("bound precondition failure", "FPS_SDK_LOG_ATTEMPT"),
        "throwing": ("bound precondition failure", "FPS_SDK_LOG_ATTEMPT"),
        "single": (),
    }
    for mode, markers in expected.items():
        try:
            result = subprocess.run(
                [executable, mode], check=False, capture_output=True, text=True, timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"{mode} could not complete: {error}", file=sys.stderr)
            return 1
        required_code = 0 if mode == "single" else abort_code
        if result.returncode != required_code:
            print(f"{mode}: expected exit {required_code}, got {result.returncode}: {result.stderr}", file=sys.stderr)
            return 1
        if mode != "single":
            markers = ("FPS PRECONDITION FAILED:", *markers)
        if any(marker not in result.stderr for marker in markers):
            print(f"{mode}: missing precondition diagnostic: {result.stderr!r}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
