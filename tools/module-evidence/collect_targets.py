#!/usr/bin/env python3
"""Produce module target evidence from a real CMake configure.

This is the *producer* half of the target-evidence contract.  It writes a
CMake File API codemodel query, runs a configure, reads the reply CMake
generates, and serialises a target index that `validate_manifest.py` consumes.

It is deliberately a separate program from the validator: the validator must
never be able to manufacture the evidence it checks.

Usage:
    python tools/module-evidence/collect_targets.py \
        --build-dir build/module-evidence \
        --out build/module-evidence/module-targets.json \
        [--configure-arg -DBUILD_GAME_MODULES=ON] ...

    # Reuse an already-configured build tree that carries a codemodel reply:
    python tools/module-evidence/collect_targets.py \
        --reply-dir build/.cmake/api/v1/reply --out build/module-targets.json
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import targets as targets_mod  # noqa: E402
from provenance import resolve_head_sha  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]


def run_configure(build_dir: Path, extra_args: list[str], timeout: int) -> int:
    build_dir.mkdir(parents=True, exist_ok=True)
    targets_mod.write_query(build_dir)
    cmd = ["cmake", "-S", str(REPO_ROOT), "-B", str(build_dir), *extra_args]
    print(f"[collect_targets] {' '.join(cmd)}", flush=True)
    try:
        proc = subprocess.run(cmd, timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        print(f"FATAL: cmake configure exceeded {timeout}s", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"FATAL: cannot run cmake: {exc}", file=sys.stderr)
        return 1
    return proc.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="Build tree to configure (writes the File API query there)")
    parser.add_argument("--reply-dir", type=Path, default=None,
                        help="Existing .cmake/api/v1/reply directory to read instead "
                             "of configuring")
    parser.add_argument("--out", type=Path, required=True,
                        help="Path to write the target evidence document")
    parser.add_argument("--configure-arg", action="append", default=[],
                        dest="configure_args",
                        help="Extra argument passed through to cmake configure")
    parser.add_argument("--timeout", type=int, default=1800,
                        help="Configure timeout in seconds (default 1800)")
    parser.add_argument("--commit-sha", default=None,
                        help="Revision the evidence is produced for "
                             "(default: git HEAD of the checkout)")
    args = parser.parse_args()

    if not args.build_dir and not args.reply_dir:
        parser.error("one of --build-dir or --reply-dir is required")

    if args.reply_dir:
        reply_dir = args.reply_dir
    else:
        rc = run_configure(args.build_dir, args.configure_args, args.timeout)
        if rc != 0:
            print(
                "FATAL: cmake configure failed — no target evidence produced.\n"
                "       An unconfigured tree yields no evidence; it must not be\n"
                "       reported as an absence of problems.",
                file=sys.stderr,
            )
            return 1
        reply_dir = args.build_dir / ".cmake" / "api" / "v1" / "reply"

    try:
        index = targets_mod.extract_from_reply(reply_dir)
    except targets_mod.TargetEvidenceUnavailable as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 1

    sha = args.commit_sha
    if sha is None:
        sha, err = resolve_head_sha(REPO_ROOT)
        if sha is None:
            print(f"FATAL: {err}", file=sys.stderr)
            return 1

    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    targets_mod.write_index(
        index, args.out,
        commit_sha=sha,
        generated_at=generated_at,
        source=str(reply_dir),
    )
    game_modules = sorted(n for n in index if n.startswith("SparkGame"))
    print(f"OK: wrote {args.out} with {len(index)} target(s); "
          f"{len(game_modules)} game module target(s): {game_modules}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
