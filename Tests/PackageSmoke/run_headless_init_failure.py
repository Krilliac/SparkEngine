#!/usr/bin/env python3
"""LIFE-200: fail inside real POSIX headless startup, then verify owned-resource cleanup.

The test-enabled engine injects exceptions after acquiring real RHI, physics,
core services, gameplay services and a module. Each process must fail normally,
report exactly one reached checkpoint and leave no runtime owner, context or
NullRHI resource. The adjacent ModuleReload_ test covers throwing module OnLoad.
This does not cover windowed graphics/audio startup or process-static allocations.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from run_headless_boot_loop import HarnessFailure, isolated_environment, require_no_sanitizer_reports

POINTS = ("headless-rhi-ready", "host-physics", "host-core", "gameplay-core",
          "gameplay-serializers", "gameplay-utilities", "gameplay-scripting", "gameplay-phases", "host-modules")
RECORD = re.compile(r"^SPARK_INIT_FAILURE point=([a-z-]+) hits=1 owners=0 context=0 world=0 live=0$", re.MULTILINE)


def validate(point: str, code: int, output: str) -> None:
    if code != 1:
        raise HarnessFailure(f"{point}: expected controlled failure exit 1, got {code}")
    require_no_sanitizer_reports(output, "")
    mentions = [line for line in output.splitlines() if "SPARK_INIT_FAILURE" in line]
    if len(mentions) != 1 or RECORD.fullmatch(mentions[0]) is None:
        raise HarnessFailure(f"{point}: missing, duplicate, malformed or dirty cleanup record: {mentions}")
    if RECORD.fullmatch(mentions[0]).group(1) != point:
        raise HarnessFailure(f"{point}: wrong checkpoint reached")


def self_test() -> None:
    good = "SPARK_INIT_FAILURE point=host-core hits=1 owners=0 context=0 world=0 live=0\n"
    validate("host-core", 1, good)
    mutations = [(0, good), (-11, good), (1, ""), (1, good * 2),
                 (1, good.replace("host-core", "host-modules")),
                 (1, good.replace("hits=1", "hits=0")),
                 (1, good.replace("owners=0", "owners=1")),
                 (1, good.replace("context=0", "context=1")),
                 (1, good.replace("world=0", "world=1")),
                 (1, good.replace("live=0", "live=-1")),
                 (1, good.replace("live=0", "live=1")),
                 (1, "[info] " + good),
                 (1, good + "ERROR: AddressSanitizer: leak\n")]
    for code, output in mutations:
        try:
            validate("host-core", code, output)
        except HarnessFailure:
            continue
        raise AssertionError(f"fault harness accepted a negative control: {code}, {output!r}")
    print(f"headless init failure self-test passed: 1 positive, {len(mutations)} negative controls")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--engine", type=Path)
    parser.add_argument("--module", type=Path)
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.engine or not args.module:
        parser.error("--engine and --module are required")
    engine = args.engine.resolve(strict=True)
    module = args.module.resolve(strict=True)
    command = [str(engine), "-headless", "-game", str(module), "-require-game",
               "-test-frames", "2", "-threads", "2", "-no-subprocess"]
    with tempfile.TemporaryDirectory(prefix="spark-init-failure-") as temporary:
        root = Path(temporary)
        for point in POINTS:
            cwd = root / point
            cwd.mkdir()
            env = isolated_environment(cwd / "user")
            env["SPARK_TEST_INIT_FAILURE"] = point
            result = subprocess.run(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8", errors="replace", timeout=60)
            try:
                validate(point, result.returncode, result.stdout)
            except HarnessFailure:
                print(result.stdout)
                raise
            print(f"{point}: exit=1 checkpoint reached, owners=0 context=0 world=0 live=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
