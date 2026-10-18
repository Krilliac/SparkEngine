#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the virtual-filesystem mount resolver.

Each seed is one virtual path as a mod, scene manifest or asset reference names it. The
fuzz adapter (FuzzVirtualFileSystemProduction.cpp) resolves it against a fixture mount
holding a.txt, sub/b.bin, empty.txt, an `escape` directory link and a `secret-link` file
link that both point outside the mount, and a lower-priority mount holding its own a.txt.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "virtual-filesystem-mounts"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-file.path": b"a.txt",
        "valid-nested.path": b"sub/b.bin",
        "valid-backslash.path": b"sub\\b.bin",
        "valid-dot-segments.path": b"sub/../a.txt",
        "valid-empty-file.path": b"empty.txt",
        "missing-file.path": b"sub/missing.dat",
        "parent-escape.path": b"../outside/secret.txt",
        "backslash-escape.path": b"sub\\..\\..\\outside\\secret.txt",
        "link-escape.path": b"escape/secret.txt",
        "file-link-escape.path": b"secret-link",
        "absolute.path": b"/etc/passwd",
        "drive-relative.path": b"C:evil.txt",
        "alternate-stream.path": b"a.txt:secret",
        "device-name.path": b"sub/CON.txt",
        "control-byte.path": b"a.txt\x00.png",
        "regression-mount-root-directory.path": b".",
        "regression-subdirectory.path": b"sub",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
