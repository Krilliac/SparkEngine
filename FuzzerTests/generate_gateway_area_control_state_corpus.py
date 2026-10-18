#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the gateway area-control epoch state file.

Each seed is a whole [GatewayControl] epoch_state_file as
LocalAreaControlService::SaveState writes it (SerializeAreaControlState) or a
damaged variant; the harness decodes it with ParseAreaControlState.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "gateway-area-control-state"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-two-sessions.state": b'v2\n"handoff-7" 3 4 1 2\n"with \\"quote\\" and space" 18446744073709551615 5 2 1\n',
        "valid-whitespace-only.state": b" \n\t\r\n",
        "valid-version-only.state": b"v2\n",
        "unknown-version.state": b'v1\n"handoff-7" 3 4 1 2\n',
        "repeated-session.state": b'v2\n"a" 1 1 1 2\n"a" 2 1 1 2\n',
        "unterminated-quote.state": b'v2\n"handoff-7 3 4 1 2\n',
        # Accepted before: std::istream negated "-1" into the unsigned epoch (UINT64_MAX), so
        # no later Prepare could ever advance the fence for this session.
        "regression-negative-epoch.state": b'v2\n"handoff-7" -1 1 1 2\n',
        # Accepted before: "-4294967295" wrapped to phase 1 (Prepare).
        "regression-wrapped-phase.state": b'v2\n"handoff-7" 3 -4294967295 1 2\n',
        # Accepted before: a leading '+' on an area id.
        "regression-signed-area.state": b'v2\n"handoff-7" 3 4 +1 2\n',
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
