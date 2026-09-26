# SparkGamePlatformer

SparkGamePlatformer is a prototype of platformer systems: levels, a player controller with unlockable
abilities, checkpoints, collectibles, hazards, and a camera system.

**Release classification:** experimental prototype, outside the stable-v1 release profile
(`tools/module-evidence/manifest.json`). Finishing it as a complete level slice is tracked under MOD-340.

## What runs

`Source/Core/Main.cpp` creates the level, checkpoint, player-controller, collectible, hazard, camera, and
engine-bridge systems on load. Its `OnUpdate`/`OnFixedUpdate` forward to `Source/Core/PlatformerLevelFlow`, which
owns the per-frame level orchestration: platform simulation, player physics, and wind in `StepFixed`; collection,
ability unlocks, checkpoint activation, hazard damage and knockback, and the goal test in `StepFrame`. The
`platformer_*` console commands inspect and drive the level and player
(`platformer_status`, `platformer_level`, `platformer_next`, `platformer_respawn`, `platformer_restart`, and
others). `plat_save` and `plat_load` save and restore progress through the engine bridge, and
`plat_replay_start`, `plat_replay_stop`, and `plat_ghost` record a run and toggle ghost playback.

## Level 0 toybox kit and music

When the engine exposes a world, the module dresses each level with the Blender-authored kit in
`Assets/Models/Platformer/Kit`. `PlatformerLevelFlow` places a `floating_platform` stretched to every platform
collider (a scaled `spring_pad` for bouncy ones) and a `goal_flag` on the goal platform, moves the meshes with their
colliders each fixed step, and hides disappearing platforms while they are not solid. `PlatformerHazardSystem`
tiles every spike pit with `spike_hazard`, and `PlatformerCollectibleSystem` places a spinning, bobbing `coin` on
each coin and hides it once collected. The meshes are set dressing only: collision stays with the module's own
colliders, and rotating platforms keep an unrotated mesh. `PlatformerEngineSystems` registers the five generated
WAV music cues in `Assets/Audio/Platformer/Music`. Source, provenance, and preview are in
`Art/Blender/SparkGamePlatformer/`, and `asset-references.json` records every asset path the module source names.
Placement has not yet been observed in a running engine; the tests run without a world and skip it.

## Progress persistence

`PlatformerEngineSystems::SaveProgress` writes the ECS world through `SaveSystem` together with one custom-state
entry, `SparkGamePlatformer.progress.v1`, encoded by `Source/Core/PlatformerProgress`. The entry holds every
level's progress (completed, unlocked, stars, best time as exact float bits, best deaths, secret found), the ids of
collected items plus the coin/gem/star/key counters, the activated checkpoints and the respawn checkpoint, and the
player's lives and unlocked abilities. The autosave (every 60 s) uses the same path.

`LoadProgress` decodes the entry and validates it against the live systems inside the `SaveSystem` validator
callback, before the world restore commits. The decoder rejects another format version, input over 32 KiB,
truncation, trailing data, and out-of-range values; validation rejects a different level count, unknown
collectible or checkpoint ids, and unlocks or ratings the saved stars cannot explain. A rejected slot leaves the
world and every gameplay system unchanged. Collectible and checkpoint ids come from a fixed build order, so a save
maps to the same items only within builds that place the same items; changing placement requires a new key.

## Known limitations

- Localization resources are missing. `PlatformerEngineSystems::SetupLocalization()` loads
  `Data/Localization/platformer_en.json`, `_fr`, `_de`, and `_ja`, and then logs that four languages loaded, but
  none of those files exists anywhere in the repository.
- The module has no networking.

## Tests

`Tests/TestGameModulePlatformerARPG.cpp` (`Platformer_*`) compiles the module's real player-controller,
checkpoint, and engine-bridge sources into SparkTests and covers checkpoints, player damage and respawn,
deterministic movement, jump buffering, and dash.

`Tests/TestMOD340PlatformerCompletionReal.cpp` (`PlatformerCompletion_*`) additionally compiles the level,
collectible, hazard, and `PlatformerLevelFlow` sources. It covers platform collision, the kill plane, automatic
checkpoint restart, and bouncy-pad launches, and it drives `PlatformerLevelFlow` with a route-following input
script that completes level 0 inside a 180-second simulated budget, including one run that dies to hazards at
the second checkpoint and restarts there. This is in-process evidence at 60 Hz, not a packaged run.

`Tests/TestMOD340PlatformerProgressReal.cpp` (also `PlatformerCompletion_*`) saves earned progress through the
real `SaveSystem` into a temporary directory, loads it into freshly built systems, and compares every field; it
also shows malformed, incompatible, and inconsistent entries are rejected without changing anything.

Both families are registered from `module.json` as exact-count CTests
(`ModuleManifest_SparkGamePlatformer_Platformer` and `ModuleManifest_SparkGamePlatformer_PlatformerCompletion`).
