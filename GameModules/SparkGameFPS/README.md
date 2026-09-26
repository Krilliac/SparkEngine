# SparkGameFPS

SparkGameFPS is SparkEngine's playable first-person arena example and the blocked, uncertified `stable-v1`
single-player slice candidate (`MOD-310`). It connects the player controller, class loadouts, weapons, enemies,
wave spawning, progression, loot, vehicles, HUD, editor UI, and engine services in one module. The module owns no
console of its own: it registers commands into the engine `SimpleConsole`, and the unreachable in-game console
overlay was removed. The LAN/multiplayer code path is experimental and outside `stable-v1` (`MOD-315`); no
multiplayer result is required for the single-player slice.

## Play the arena

For an installed Windows package, use `bin/PlaytestSparkFPS.cmd` to launch the
real engine with the packaged FPS module. `check` verifies that both files are
present and prints the engine version; `smoke` runs eight headless NullRHI
frames; `report` opens the GitHub playtest issue form. The shipped
`bin/PLAYTESTING.md` explains the workflow and safe report contents. These
commands do not certify the still-blocked `stable-v1` release.

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

The arena also places a Blender-authored training kit from `Assets/Models/FPS/Kit/` (spawn pads, cover barriers,
weapon racks, ammo crates and target dummies; source and provenance in `Art/Blender/SparkGameFPS/`).
`asset-references.json` records every asset path the module source names.

Under the headless (NullRHI) host the engine context exposes no `GraphicsEngine` or `InputManager`, so the renderable
`Game` is not built. The module still simulates the authored arena (`Core/HeadlessArena.cpp`): it loads
`Scenes/level1.scene` through the data-only `SceneManager` path, binds `RespawnSystem` and a Deathmatch `GameMode` to
the scene's default spawns, ticks both on every `OnUpdate`, and at unload prints one
`SPARK_FPS_HEADLESS_ARENA objects=N spawns=S bound=B mode_spawns=M ticks=T match=1` record. `OnLoad` fails when the
scene or its default spawns cannot be loaded. The Linux CTest `FPSSinglePlayerSlice_HeadlessArenaLinux`
(`cmake/RunSparkFPSHeadlessArena.cmake`) runs `SparkEngine -headless -game libSparkGameFPS.so -test-frames 8` on
NullRHI and fails closed unless the record matches an independent parse of the staged scene, the host lifecycle
records pass, and the arena ticked once per reported update. It exercises no player, combat or HUD path and is
source-tree evidence, not package certification.

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
- `net_host`, `net_connect`, `net_disconnect`, `net_status`, and `net_stats` run the FPS multiplayer session when
  networking is enabled (experimental; outside `stable-v1`). `FPSMultiplayerSystem` is the module's only network path:
  the game ticks it each frame and sends the local player's WASD/look/fire input at a fixed 60 Hz.
- The dev cheat commands (`god`, `noclip`) are excluded only when `SPARK_BUILD_SHIPPING` is defined, which today
  means the MinSizeRel configuration / the `windows-shipping` preset alone. A package built from the MSVC
  `Release` configuration still registers them — state which artifact a release actually ships before claiming
  the cheats are absent.

Build the `SparkGameFPS` target. CPU-only regression coverage is part of `SparkTests`; filter for `FPSInteg_`,
`FPSRespawn_`, `FPSLocalProfile_`, `FPSProgression_`, `FPSAssets_`, `FPSStateRules_`, `FPSComponentsReal_`, and
`WeaponMechanicsReal_` when running the test executable directly. For the experimental LAN path, `FPSMultiplayer_`
covers the snapshot/input wire encoding and `FPSMultiplayerProduction_` drives the real `FPSMultiplayerSystem` in one
process: server input application, hit validation (a lag-compensated line-of-fire ray; damage reports also require a
living attacker aiming at the victim, and every hit deals at most the weapon's server-owned damage), death, the respawn
timer, scoreboard ordering, and client reconciliation after a real handshake. The system registers its handlers with
`NetworkManager` on every host/connect: admission and disconnect/timeout spawn and remove players, clients send
`FPSMessageType::PlayerInput` (the server drops malformed and over-rate inputs; `ApplyClientInput`, the one path from
any input, including the listen-server host's, to authoritative state, rejects non-finite fields and replayed or zero
sequences, clamps the axes and pitch, wraps yaw into [-pi, pi], and spawns at most one projectile per server-owned 0.1 s fire interval however often fire is held), and the server broadcasts one `FPSMessageType::StateSnapshot` batch (states plus scores) at 20 Hz that clients
accept only when well-formed and newer. `FPSMultiplayerProduction_NetworkPath*` exchange real datagrams with a raw
loopback peer on each side. `FPSLAN_ThreeProcessLoopbackConvergence` (CTest `FPSLANTwoClientConvergence`) runs one
server and two independent client processes (`SparkFPSLANLoopbackPeer`, each with its own `NetworkManager`) over
loopback UDP through a scripted spawn-move-kill-respawn-score round. It requires every client's spawn and respawn to
match the server's and all three views to agree on positions, life, health and scores, and it requires the server to
drop both players when their clients quit. This is unconstrained loopback on one machine: it does not yet cover the
production encrypted transport, packet loss or latency, or separate hosts (`MOD-315`).

## SDK module boundary

The module entrypoint implements only the installed `Spark::IModule` contract and receives services through its injected
`Spark::IEngineContext`. The retired private `IGameModule` inheritance/factories and concrete `EngineContext::Get()`
lookups are not part of SparkGameFPS. The installed-SDK consumer compiles `Core/SparkGameFPS.h` using only staged
`Spark/` headers and a source-boundary regression rejects either legacy dependency if it returns.

This proves the entrypoint boundary, not the complete DLL. Gameplay implementation files still include private engine
headers, the Windows module still links `SparkEngineLib`, and full public-SDK-only build/link qualification remains open.

## Installed persistence smoke

`FPSPackage_InstalledRuntime` stages the MinSizeRel stable-v1 runtime, validates the real installed NullRHI module
lifecycle, then launches two separate D3D11 WARP processes with isolated `LOCALAPPDATA`. The writer moves progression
from 0 to 37 XP and writes `fps_quicksave`; the fresh reader proves an initial 0 XP state, loads the same slot, and
restores 37 XP without changing the save bytes. The runner retains child output, semantic command audits, binary/save
hashes, build configuration, source identity, and host metadata under its per-attempt test root.

This is a bounded local progression-persistence slice, not stable-v1 certification. It does not yet prove every
`FPSLocalProfile` field, spawn/move/kill/respawn/score acceptance, a public-SDK-only module build, NullRHI save/reload,
clean-machine installation, recovery/soak, hardware rendering, or hosted exact-SHA qualification.
