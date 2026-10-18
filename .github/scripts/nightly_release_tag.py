#!/usr/bin/env python3
"""Create the immutable, collision-resistant tag for one nightly run.

Nightly releases are intentionally additive.  The tag contains only GitHub's
numeric run identity and the source prefix, so a retry cannot reuse a tag from
another run and no existing tag ever needs to be moved.
"""

from __future__ import annotations

import re
import sys


TAG_RE = re.compile(r"nightly-[1-9][0-9]*-[1-9][0-9]*-[0-9a-f]{12}")


def make_tag(run_id: str, attempt: str, source_sha: str) -> str:
    if not run_id.isdigit() or int(run_id) < 1:
        raise ValueError("run id must be a positive integer")
    if not attempt.isdigit() or int(attempt) < 1:
        raise ValueError("run attempt must be a positive integer")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", source_sha):
        raise ValueError("source SHA must be a 40-hex commit")
    tag = f"nightly-{int(run_id)}-{int(attempt)}-{source_sha.lower()[:12]}"
    if TAG_RE.fullmatch(tag) is None:
        raise ValueError("generated nightly tag is malformed")
    return tag


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: nightly_release_tag.py RUN_ID ATTEMPT SOURCE_SHA", file=sys.stderr)
        return 2
    try:
        print(make_tag(*argv[1:]))
    except ValueError as error:
        print(f"nightly tag generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
