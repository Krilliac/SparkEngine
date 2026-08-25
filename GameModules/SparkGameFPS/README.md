# SparkGameFPS

SparkGameFPS is SparkEngine's playable first-person arena example. It connects the player controller, class loadouts,
weapons, enemies, wave spawning, progression, loot, vehicles, HUD, editor UI, console, and engine services in one module.

## Play the arena

- `WASD` moves, mouse looks, left mouse fires, `R` reloads, and `Space` jumps.
- `1`-`4` select loadout slots; `F` and `G` activate class abilities.
- `F5`-`F10` select a class, while `[` and `]` cycle classes.
- `V` enters or exits the nearest vehicle.
- `F11` starts or restarts the survival match.

The editor's **Spark Arena** panel exposes the same survival and class actions and shows the live player, round, wave,
enemy, weapon, progression, time-scale, and engine-service state.

## Useful console commands

- `game_status` prints the same live state shown in the editor panel.
- `wave_start` starts or restarts a complete survival match.
- `wave_status`, `wave_skip [number]`, and `wave_difficulty <scale>` inspect or tune the wave loop.
- `level`, `xp <amount>`, `loot_status`, and `powerup <type>` exercise progression and loot.
- `net_host`, `net_connect`, `net_disconnect`, `net_status`, and `net_stats` exercise engine networking when enabled.

Build the `SparkGameFPS` target. The CPU-only regression coverage is part of `SparkTests`; filter for
`FPSInteg_` and `FPSMultiplayer_` when running the test executable directly.
