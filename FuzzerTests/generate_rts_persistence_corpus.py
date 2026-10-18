#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the SparkGameRTS save snapshot (RTSPersistence::Deserialize).

Each seed is the "SparkGameRTS.match.v2" custom-state text as RTSPersistence::Serialize
writes it (SPARK_RTS_STATE_V2: one record per line, floats as IEEE-754 bit patterns)
or a damaged variant.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "rts-save-snapshot"

FACTIONS = 3  # RTSFaction::Count


def f(value: float) -> str:
    return str(struct.unpack("<I", struct.pack("<f", value))[0])


def line(*tokens: object) -> str:
    return " ".join(str(token) for token in tokens) + "\n"


def fog(width: int, height: int, runs: list[tuple[int, int]]) -> str:
    text = line("FOG", width, height)
    for _ in range(FACTIONS):
        text += line("F", len(runs), *[value for run in runs for value in run])
    return text


def snapshot(
    *,
    units: list[str] = (),
    buildings: list[str] = (),
    players: list[str] = (),
    nodes: list[str] = (),
    commands: list[str] = (),
    selection: list[int] = (),
    match_players: list[str] = (),
    fog_text: str | None = None,
    tick: str = "120",
) -> bytes:
    text = line("SPARK_RTS_STATE_V2") + line("TICK", tick) + line("IDS", 10, 5, 4) + line("HARVEST", f(0.25))
    text += line("UNITS", len(units)) + "".join(units)
    text += line("BUILDINGS", sum(1 for b in buildings if b.startswith("B "))) + "".join(buildings)
    text += line("PLAYERS", len(players)) + "".join(players)
    text += line("NODES", len(nodes)) + "".join(nodes)
    text += line("COMMANDS", len(commands)) + "".join(commands)
    text += line("SELECTION", len(selection), *selection)
    text += line("MATCH", 1, f(42.5), 0, 0, len(match_players)) + "".join(match_players)
    text += fog_text if fog_text is not None else fog(2, 2, [(0, 4)])
    return (text + line("END")).encode("ascii")


def unit(unit_id: int, unit_type: int = 1, health: float = 40.0) -> str:
    # id type faction state health maxHealth damage attackSpeed moveSpeed visionRange posX posY targetId
    return line("U", unit_id, unit_type, 0, 0, f(health), f(50.0), f(6.0), f(1.0), f(3.0), f(8.0), f(12.5), f(-4.0), 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    skirmish = snapshot(
        units=[unit(1, unit_type=0), unit(2)],
        buildings=[
            line("B", 1, 1, 0, f(900.0), f(1000.0), f(10.0), f(10.0), 1, f(1.0), f(30.0), 1),
            line("P", 1, f(4.0), f(12.0)),
        ],
        players=[line("R", 0, 400, 50, 3, 10), line("R", 1, 50, 0, 0, 10)],
        nodes=[line("N", 3, 0, f(20.0), f(20.0), 1500, 3, 1, 1)],
        commands=[line("Q", 2, 1, 0, f(5.0), f(6.0), 0, 2, f(3.0), f(3.0), f(5.0), f(6.0))],
        selection=[2],
        match_players=[line("M", 0, f(0.0), f(0.0), 0, 0, 0), line("M", 1, f(64.0), f(64.0), 1, 0, 0)],
        fog_text=fog(4, 2, [(2, 3), (1, 1), (0, 4)]),
    )
    seeds = {
        "valid-minimal.snap": snapshot(),
        "valid-skirmish.snap": skirmish,
        "legacy-v1.snap": b"SPARK_RTS_STATE_V1\nTICK 1\n",
        "trailing-data.snap": snapshot() + b"EXTRA\n",
        "count-overclaim.snap": line("SPARK_RTS_STATE_V2").encode() + b"TICK 1\nIDS 1 1 1\nHARVEST 0\nUNITS 10000\n",
        "fog-runs-overflow.snap": snapshot(fog_text=fog(2, 2, [(0, 3), (1, 3)])),
        "nonfinite-health.snap": snapshot(units=[unit(1, health=float("nan"))]),
        "signed-tick.snap": snapshot(tick="-1"),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
