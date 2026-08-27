#!/usr/bin/env python3
"""Black-box SparkCooker-to-SparkWorker scheduling and publication contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cooker", required=True, type=Path)
    parser.add_argument("--worker", required=True, type=Path)
    return parser.parse_args()


def invoke(cooker: Path, environment: dict[str, str], *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(cooker), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        timeout=15,
        env=environment,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    args = parse_args()
    cooker = args.cooker.resolve(strict=True)
    worker = args.worker.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="spark-cooker-worker-smoke-") as temporary:
        root = Path(temporary)
        source = root / "source"
        output = root / "output"
        scratch = root / "scratch"
        (source / "nested").mkdir(parents=True)
        scratch.mkdir()
        fixtures = {
            "alpha.txt": b"alpha\n",
            "nested/beta.bin": bytes(range(64)),
            "zeta.txt": b"zeta\n",
        }
        for relative, payload in fixtures.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)

        environment = os.environ.copy()
        environment.update({"TMP": str(scratch), "TEMP": str(scratch), "TMPDIR": str(scratch)})
        common = (
            "--source", str(source), "--output", str(output),
            "--worker", str(worker), "--jobs", "3",
        )

        first = invoke(cooker, environment, *common)
        require(first.returncode == 0, f"first worker cook failed:\n{first.stdout}")
        require("3 asset(s), 3 updated" in first.stdout, first.stdout)
        manifest_path = output / "spark-cook-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        require([entry["path"] for entry in manifest["assets"]] == sorted(fixtures), "manifest order is unstable")
        for entry in manifest["assets"]:
            relative = entry["path"]
            require((output / relative).read_bytes() == fixtures[relative], f"wrong output bytes for {relative}")
            require(entry["sha256"] == hashlib.sha256(fixtures[relative]).hexdigest(),
                    f"wrong digest for {relative}")

        second = invoke(cooker, environment, *common)
        require(second.returncode == 0, f"incremental worker cook failed:\n{second.stdout}")
        require("3 asset(s), 0 updated, 3 unchanged" in second.stdout, second.stdout)
        published_manifest = manifest_path.read_bytes()

        dry_output = root / "dry-output"
        dry_run = invoke(
            cooker, environment,
            "--source", str(source), "--output", str(dry_output),
            "--worker", str(worker), "--jobs", "2", "--dry-run",
        )
        require(dry_run.returncode == 0, f"worker dry-run failed:\n{dry_run.stdout}")
        require(not dry_output.exists(), "worker dry-run published output")

        invalid_jobs = invoke(cooker, environment, "--source", str(source), "--output", str(output),
                              "--worker", str(worker), "--jobs", "65")
        require(invalid_jobs.returncode == 2 and "--jobs" in invalid_jobs.stdout, invalid_jobs.stdout)
        missing_worker = invoke(cooker, environment, "--source", str(source), "--output", str(output), "--jobs", "2")
        require(missing_worker.returncode == 2, missing_worker.stdout)

        failed_worker = invoke(
            cooker, environment,
            "--source", str(source), "--output", str(output),
            "--worker", str(cooker), "--jobs", "2",
        )
        require(failed_worker.returncode == 1 and "worker job" in failed_worker.stdout, failed_worker.stdout)
        require(manifest_path.read_bytes() == published_manifest, "failed worker queue replaced the published manifest")
        for relative, payload in fixtures.items():
            require((output / relative).read_bytes() == payload, f"failed queue replaced {relative}")
        require(not list(scratch.glob("spark-cooker-workers-*")), "worker scratch directories leaked")

    print("Spark cooker/worker process smoke passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - black-box diagnostics are intentional.
        print(f"cooker/worker process smoke failed: {error}", file=sys.stderr)
        raise SystemExit(1)
