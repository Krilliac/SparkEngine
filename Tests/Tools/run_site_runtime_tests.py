#!/usr/bin/env python3
"""Run the site runtime's Node test files against one generated publication.

verifyBundle.test.mjs and siteDataRuntime.test.mjs each run generate.py when
SPARK_SITE_DATA_DIR is unset. generate.py republishes docs/api in the source
tree, so two files generating at once raced on that tree, and running the files
one after another instead doubled the generation cost. This runner generates the
publication once (unless SPARK_SITE_DATA_DIR already names one), exports it to
every file, and runs the files concurrently: with no generation left inside
them, their only shared input is a read-only bundle, and each file mutates
copies in its own scratch directory. Each file still runs in its own
`node --test` (`--test-concurrency` needs Node 20.10; the registration accepts
20.0+). Output is replayed per file in argument order, and the first failing
file's status (in argument order) is returned after every file has finished.

The generator runs under $PYTHON (the interpreter the Node files use), falling
back to this interpreter.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

REPO_ROOT = Path(__file__).resolve().parents[2]


def generate_publication(output: Path) -> int:
    python = os.environ.get("PYTHON") or sys.executable
    command = [python, "tools/site-data/generate.py", "--allow-dirty", "--skip-doc-health", "--output", str(output)]
    result = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(f"generate.py failed ({result.returncode}):\n{result.stdout}\n{result.stderr}", file=sys.stderr)
    return result.returncode


def run_files(node: str, test_files: list[str], environment: dict[str, str], scratch: Path) -> int:
    # Each child writes to its own log file: a pipe read in order would stall a
    # chatty later file on a full pipe buffer until the earlier ones finished.
    running = []
    for index, test_file in enumerate(test_files):
        log = (scratch / f"node-test-{index}.log").open("w+b")
        process = subprocess.Popen([node, "--test", test_file], stdout=log, stderr=subprocess.STDOUT,
                                   env=environment)
        running.append((test_file, process, log))
    failure = 0
    for test_file, process, log in running:
        returncode = process.wait()
        with log:
            log.seek(0)
            output = log.read().decode("utf-8", errors="replace")
        sys.stdout.write(f"---- node --test {test_file} (exit {returncode}) ----\n{output}")
        sys.stdout.flush()
        if returncode != 0 and failure == 0:
            failure = returncode
    return failure


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: run_site_runtime_tests.py NODE TEST_FILE...", file=sys.stderr)
        return 2
    node = sys.argv[1]
    test_files = sys.argv[2:]
    environment = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix="spark-site-runtime-") as directory:
        scratch = Path(directory)
        if not environment.get("SPARK_SITE_DATA_DIR"):
            publication = scratch / ".site-data"
            status = generate_publication(publication)
            if status != 0:
                return status
            environment["SPARK_SITE_DATA_DIR"] = str(publication)
        return run_files(node, test_files, environment, scratch)


if __name__ == "__main__":
    raise SystemExit(main())
