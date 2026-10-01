#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the SparkOrchestrator mutation identity state file.

Each seed is a whole state file as OrchestratorIdentityLease::Acquire writes it
("SPORCHCLI1", the client instance, the last used sequence) or a damaged variant.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "daemon-orchestrator-identity-state"

MAGIC = b"SPORCHCLI1\n"
UINT64_MAX = (1 << 64) - 1


def state(client: bytes, sequence: bytes) -> bytes:
    return MAGIC + client + b"\n" + sequence + b"\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    client = b"cli-0123456789abcdef0123456789abcdef"
    seeds = {
        "valid-sequence.state": state(client, b"41"),
        "valid-longest-client.state": state(b"c" * 64, b"7"),
        "valid-last-usable-sequence.state": state(client, str(UINT64_MAX - 1).encode()),
        "exhausted-sequence.state": state(client, str(UINT64_MAX).encode()),
        "client-too-long.state": state(b"c" * 65, b"7"),
        "signed-sequence.state": state(client, b"+7"),
        "missing-final-newline.state": MAGIC + client + b"\n7",
        "over-size.state": state(client, b"0" * (257 - len(MAGIC) - len(client) - 2)),
    }
    assert len(seeds["over-size.state"]) == 257
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
