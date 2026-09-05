# SparkGameFPS

SparkGameFPS is SparkEngine's playable first-person arena example and the blocked, uncertified `stable-v1`
single-player slice candidate (`MOD-310`). It connects the player controller, class loadouts, weapons, enemies,
wave spawning, progression, loot, vehicles, HUD, editor UI, and engine services in one module. The module owns no
console of its own: it registers commands into the engine `SimpleConsole`, and the unreachable in-game console
overlay was removed. The LAN/multiplayer code path is experimental and outside `stable-v1` (`MOD-315`); no
multiplayer result is required for the single-player slice.

## Play the arena

- `WASD` moves, mouse looks, left mouse fires, `R` reloads, and `Space` jumps.
- `1`-`4` select loadout slots; `F` and `G` activate class abilities.
- `F5`-`F10` select a class, while `[` and `]` cycle classes.
- `V` enters or exits the nearest vehicle.
- `F11` starts or restarts the survival match (this also revives the player).

The death -> respawn -> score loop is complete: `Player::TakeDamage` fires a death callback that `Game` routes to
`RespawnSystem::OnPlayerDeath` and `GameMode::RecordKill`; the respawn timer publishes `PlayerRespawnEvent`, and
`Game` restores health/armor, teleports, and reactivates the player. `RespawnSystem` (`Game/GameMechanicsRespawn.cpp`)
owns scoring, timing, and the event without a `Player*` dependency so the real class is testable without D3D11.

The module initializes without a D3D11 device: gameplay state is built in full and GPU resource creation and
`Render()` are skipped. Module assets (the scene, arena and weapon models, music tracks) resolve against a single
asset root discovered at runtime (`FPSAssets::Resolve`), not against paths relative to the working directory.

The editor's **Spark Arena** panel exposes the same survival and class actions and shows the live player, round, wave,
enemy, weapon, progression, time-scale, and engine-service state.

## Useful console commands

- `game_status` prints the same live state shown in the editor panel, including whether the save system is reachable.
- `wave_start` starts or restarts a complete survival match.
- `wave_status`, `wave_skip [number]`, and `wave_difficulty <scale>` inspect or tune the wave loop.
- `level`, `xp <amount>`, `loot_status`, and `powerup <type>` exercise progression and loot.
- `quicksave` / `quickload` write and read the slot `fps_quicksave`: the ECS world plus the local profile
  (progression XP/level, class, weapon, kills/deaths/score, playtime, health, armor), reached through the engine
  context's `SaveSystem` and `World`. They print the real result, or `Save system unavailable...` when the engine
  context exposes neither service. `save_list` reads the same `SaveSystem`.
- `net_host`, `net_connect`, `net_disconnect`, `net_status`, and `net_stats` exercise engine networking when enabled
  (experimental; outside `stable-v1`).
- The dev cheat commands (`god`, `noclip`) are excluded only when `SPARK_BUILD_SHIPPING` is defined, which today
  means the MinSizeRel configuration / the `windows-shipping` preset alone. A package built from the MSVC
  `Release` configuration still registers them — state which artifact a release actually ships before claiming
  the cheats are absent.

Build the `SparkGameFPS` target. CPU-only regression coverage is part of `SparkTests`; filter for `FPSInteg_`,
`FPSRespawn_`, `FPSLocalProfile_`, `FPSProgression_`, `FPSAssets_`, `FPSStateRules_`, `FPSComponentsReal_`, and
`WeaponMechanicsReal_` when running the test executable directly (`FPSMultiplayer_` covers the experimental LAN path).
