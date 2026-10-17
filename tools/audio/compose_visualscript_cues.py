#!/usr/bin/env python3
"""
Compose the five sound cues the SparkGameVisualScript demo scripts request with playSound().

Each cue is a short non-looping Stinger rendered by the module-music composer
(compose_module_music.py): mono 16-bit 22050 Hz WAV, the one format the engine audio
backends decode, fading to silence. The cue names are the playSound() literals in the
module's generated scripts (Collectible, EnemyPatrol, GameManager, HealthPickup);
VisualScriptDemo_EverySoundCueShipsItsAudio fails when a script names a cue this
list does not ship.

All generated audio is free to use (public domain / CC0): it is procedurally generated
from this script and contains no third-party content. Output bytes are identical
across runs.

Usage:
    python3 tools/audio/compose_visualscript_cues.py [--repo .]

Cues land in ``GameModules/SparkGameVisualScript/Assets/Audio/VisualScript/<cue>.wav``;
record their SHA-256 in that module's ``Assets/manifest.json`` in the same change.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from compose_module_music import Stinger, compose_stinger, write_wav

MODULE = "SparkGameVisualScript"
OUTPUT_RELATIVE = Path("GameModules") / MODULE / "Assets" / "Audio" / "VisualScript"

# Events are (beat, length in beats, scale degree), as in compose_module_music.STINGERS.
CUES = (
    # Collectible: a bright two-note blip as a coin is picked up.
    Stinger(MODULE, "coin_pickup", 240.0, 76, "ionian",
            ((0, 0.5, 4), (0.5, 1.5, 9)),
            "square", (0.5, 1.5, ()), (), (), 0.15, 61),
    # EnemyPatrol: a falling saw stab over a snare as a strike lands.
    Stinger(MODULE, "enemy_attack", 240.0, 52, "aeolian",
            ((0, 0.5, 4), (0.5, 1.0, 0)),
            "saw", (0, 1.5, ()), ((0, 1.5, 0),), ((0, "snare"),), 0.1, 62),
    # GameManager: a brass fanfare when the fifth coin wins the game.
    Stinger(MODULE, "victory_fanfare", 140.0, 67, "ionian",
            ((0, 0.5, 0), (0.5, 0.5, 2), (1, 0.5, 4), (1.5, 2.5, 7)),
            "brass", (1.5, 2.5, (0, 2, 4)), ((0, 1.5, -7), (1.5, 2.5, -7)), ((0, "kick"), (1.5, "crash")),
            0.4, 63),
    # HealthPickup: a rising bell arpeggio as the player heals.
    Stinger(MODULE, "health_pickup", 200.0, 72, "ionian",
            ((0, 0.5, 0), (0.5, 0.5, 2), (1, 0.5, 4), (1.5, 1.5, 7)),
            "bell", (1.5, 1.5, (0, 2, 4)), (), (), 0.3, 64),
    # HealthPickup: a soft two-bell chime as the pack reappears.
    Stinger(MODULE, "pickup_respawn", 180.0, 79, "ionian",
            ((0, 0.5, 4), (0.5, 1.5, 7)),
            "bell", (0, 2, ()), (), (), 0.3, 65),
)


def compose_all(output_dir: Path) -> list[Path]:
    written = []
    for cue in CUES:
        path = output_dir / f"{cue.stem}.wav"
        write_wav(path, compose_stinger(cue))
        written.append(path)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    for path in compose_all(args.repo / OUTPUT_RELATIVE):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
