#!/usr/bin/env python3
"""PERF-100: a golden baseline change must carry its own review record.

The SHA-256 pin in Tests/GoldenImages/manifest.json already forces a manifest
edit whenever a baseline PNG changes. This gate checks that edit between two
revisions. For every (scene, backendRow) entry that was added, or whose
baselineSha256, perPixelThreshold, tolerancePercent or software flag changed,
it fails when:

  (a) the reviewer record is identical to the base revision's (after whitespace
      normalization), so a changed baseline never inherits the old review;
  (b) a reviewed base record (no "owner review pending") is replaced by a
      pending one, so a reviewed baseline cannot be downgraded;
  (c) a pending record sits on a hardware row (rejected by the shared schema
      parser in Tests/Tools/test_golden_manifest.py, which both revisions go
      through, so an invalid manifest at either revision also fails).

It also fails when a PNG under Tests/GoldenImages changed while its manifest
entry did not. A base or head revision that cannot be resolved, or a git query
that fails, exits 2; the gate never passes vacuously on a missing base.

This does not prove that a human reviewed the change: it proves that every
baseline change was accompanied by a new, non-downgraded review record.
Requiring review on Tests/GoldenImages/** (CODEOWNERS or a ruleset) is an
owner action.

Exit status: 0 clean, 1 review violation or invalid manifest, 2 unresolvable
revision or git failure.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Tests" / "Tools"))

from test_golden_manifest import PENDING_REVIEW, parse_manifest_text  # noqa: E402

GOLDEN_DIR = "Tests/GoldenImages"
MANIFEST_PATH = f"{GOLDEN_DIR}/manifest.json"
MAX_MANIFEST_BYTES = 1 << 20
REVIEWED_FIELDS = ("baselineSha256", "perPixelThreshold", "tolerancePercent", "software")
ZERO_SHA = "0" * 40


class GitError(RuntimeError):
    pass


def _git(repo: Path, *args: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=False)


def resolve_commit(repo: Path, rev: str) -> str | None:
    if not rev or rev == ZERO_SHA:
        return None
    result = _git(repo, "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}")
    return result.stdout.decode().strip() if result.returncode == 0 else None


def resolve_base(repo: Path, base: str, fallback: str | None) -> str | None:
    """The base commit, or the merge-base of HEAD and the fallback ref."""
    commit = resolve_commit(repo, base)
    if commit is not None or fallback is None:
        return commit
    result = _git(repo, "merge-base", "HEAD", fallback)
    if result.returncode != 0:
        return None
    return resolve_commit(repo, result.stdout.decode().strip())


def manifest_at(repo: Path, commit: str) -> tuple[dict[tuple[str, str], dict[str, Any]], list[str]]:
    """Manifest entries keyed by (scene, backendRow); absent manifest = no entries."""
    listed = _git(repo, "ls-tree", "--name-only", commit, "--", MANIFEST_PATH)
    if listed.returncode != 0:
        raise GitError(f"git ls-tree {commit} failed: {listed.stderr.decode().strip()}")
    if not listed.stdout.strip():
        return {}, []
    size = _git(repo, "cat-file", "-s", f"{commit}:{MANIFEST_PATH}")
    if size.returncode != 0:
        raise GitError(f"git cat-file -s {commit}:{MANIFEST_PATH} failed")
    if int(size.stdout.decode().strip()) > MAX_MANIFEST_BYTES:
        return {}, [f"{commit[:12]}:{MANIFEST_PATH}: larger than {MAX_MANIFEST_BYTES} bytes"]
    shown = _git(repo, "show", f"{commit}:{MANIFEST_PATH}")
    if shown.returncode != 0:
        raise GitError(f"git show {commit}:{MANIFEST_PATH} failed")
    try:
        text = shown.stdout.decode("utf-8")
    except UnicodeDecodeError:
        return {}, [f"{commit[:12]}:{MANIFEST_PATH}: not UTF-8"]
    entries, errors = parse_manifest_text(text)
    if errors:
        return {}, [f"{commit[:12]}:{error}" for error in errors]
    return {(entry["scene"], entry["backendRow"]): entry for entry in entries}, []


def changed_pngs(repo: Path, base: str, head: str) -> list[tuple[str, str]]:
    """(scene, backendRow) of every PNG added, modified or deleted under GOLDEN_DIR."""
    result = _git(repo, "diff", "--name-only", "--no-renames", "-z", base, head, "--", GOLDEN_DIR)
    if result.returncode != 0:
        raise GitError(f"git diff {base}..{head} failed: {result.stderr.decode().strip()}")
    keys: list[tuple[str, str]] = []
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        path = PurePosixPath(raw.decode("utf-8", "replace"))
        if path.suffix.lower() == ".png":
            keys.append((path.stem, path.parent.name))
    return keys


def _normalized(reviewer: str) -> str:
    return " ".join(reviewer.split())


def review_errors(
    base: dict[tuple[str, str], dict[str, Any]],
    head: dict[tuple[str, str], dict[str, Any]],
    pngs: list[tuple[str, str]],
) -> tuple[list[str], int]:
    """Return (violations, number of changed entries checked)."""
    errors: list[str] = []
    changed: set[tuple[str, str]] = set()
    for key, entry in sorted(head.items()):
        previous = base.get(key)
        if previous is not None and all(previous[f] == entry[f] for f in REVIEWED_FIELDS):
            continue
        changed.add(key)
        label = f"{key[1]}/{key[0]}"
        if previous is not None:
            if _normalized(previous["reviewer"]) == _normalized(entry["reviewer"]):
                errors.append(f"{label}: baseline or threshold changed but the reviewer record is unchanged")
            if PENDING_REVIEW not in previous["reviewer"] and PENDING_REVIEW in entry["reviewer"]:
                errors.append(f"{label}: a reviewed baseline cannot be replaced by one with {PENDING_REVIEW!r}")
    for key in sorted(set(pngs)):
        removed = key in base and key not in head
        if key not in changed and not removed:
            errors.append(f"{key[1]}/{key[0]}.png: PNG changed but its manifest entry did not")
    return errors, len(changed)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", required=True, help="base revision (PR base or push 'before' SHA)")
    parser.add_argument("--head", default="HEAD", help="head revision (default HEAD)")
    parser.add_argument(
        "--fallback-base",
        help="ref whose merge-base with HEAD is used when --base is empty, all-zero or unknown",
    )
    parser.add_argument("--repo", type=Path, default=REPO_ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    base = resolve_base(args.repo, args.base, args.fallback_base)
    if base is None:
        print(f"check_golden_review: cannot resolve base revision {args.base!r}", file=sys.stderr)
        return 2
    head = resolve_commit(args.repo, args.head)
    if head is None:
        print(f"check_golden_review: cannot resolve head revision {args.head!r}", file=sys.stderr)
        return 2
    try:
        base_entries, base_errors = manifest_at(args.repo, base)
        head_entries, head_errors = manifest_at(args.repo, head)
        pngs = changed_pngs(args.repo, base, head)
    except GitError as exc:
        print(f"check_golden_review: {exc}", file=sys.stderr)
        return 2
    if base_errors or head_errors:
        for error in base_errors + head_errors:
            print(f"check_golden_review: invalid manifest: {error}", file=sys.stderr)
        return 1

    errors, checked = review_errors(base_entries, head_entries, pngs)
    print(f"check_golden_review: {base[:12]}..{head[:12]}: checked {checked} changed entries")
    for error in errors:
        print(f"check_golden_review: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
