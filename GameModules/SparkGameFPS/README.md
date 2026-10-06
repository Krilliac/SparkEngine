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

- `WASD` moves, left mouse fires, `R` reloads, and `Space` jumps.
- On Windows, click in the window to capture the mouse for mouse-look; `Esc` releases it.
- `1`-`4` select loadout slots; `F` and `G` activate class abilities.
- `F2` saves the current world and local profile; `F3` restores the saved slot. Release before pressing again.
  Use one key at a time. These controls work without the developer console.
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

The headless module also registers `level`, `xp <amount>`, `quicksave` and `quickload` (`Core/HeadlessPersistence.cpp`)
with the same output text as the windowed commands. They are backed by a CPU-only `ProgressionSystem`, the arena's
Deathmatch scoreboard and the accumulated `OnUpdate` play time, and write and read the same `fps_quicksave` slot and
`FPSLocalProfile` block. The headless host has no `Player`, so class, weapon, health and armor keep their profile
defaults on save and are not applied on load.

The editor's **Spark Arena** panel exposes the same survival and class actions and shows the live player, round, wave,
enemy, weapon, progression, time-scale, and engine-service state.

## Editor-authored startup content

An editor-built package places its selected default scene in `Startup.sparkscene`
beside the game executable. The windowed FPS module loads that file through
SceneManager, uses its authored camera for the player start, and renders its meshes
instead of creating the procedural arena. Selection is executable-relative; changing
the working directory does not select a different startup scene. The host's `-scene`
preview mode is separate and does not run the FPS module.

The current adapter supports root entities with unique nonempty names, each containing
a Transform and exactly one OBJ MeshRenderer, the single main perspective Camera,
or a supported SpawnPointComponent. At least one mesh and one main camera remain required.
Meshes must use the default material and default rendering flags, with OBJ references
inside the packaged project. Transforms, camera FOV and supported clipping values are
preserved. Hierarchy, other components, explicit materials, unsupported settings and
missing or invalid meshes are rejected rather than silently replaced. See
[the reflected startup contract](../../wiki/subsystems/Scene-Management.md#reflected-startup-scenes-in-fps)
for the exact camera limits and loading behavior.

Player spawn points support `spawnTag="default"`, `enabled=true`, `teamID=0`,
`spawnRadius=0`, `respawnDelay=0`, and `maxConcurrent=-1`. Their priority, position
and facing are preserved; they require root transforms, unit scale, supported pitch
[-89,89], and at most 32 points. F11 match start and player respawn use the existing
highest-priority selector, keeping its first-stored tie behavior. The component
delay is a point-reuse cooldown; it does not replace the player's death-to-respawn
timer. Other spawn settings, including `wave_spawn`, reject the scene. In particular,
the component's default radius 1 and cooldown 5 need explicit authoring changes.
This support does not establish a playable arena, collision fidelity or a completed match.

Only an absent `Startup.sparkscene` permits the legacy `Assets/Scenes/level1.scene`
startup. A present unreadable, malformed or unsupported scene fails module initialization;
neither a `.bak` scene nor a placeholder mesh substitutes for it. The separate headless
arena described above still uses the legacy scene and cannot verify this adapter.

This is bounded source support, not native-qualified editor-to-game or overall release
acceptance. The installed windowed lineage tests must run against the new source and
real FPS module; normal-input playthrough and final Shipping qualification remain open.
F2/F3 save and restore the host ECS world and declared local profile. They do not save
the authored scene back to disk or reconstruct the module's complete live scene, and
the current profile restoration does not restore the player's position.

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
  the game ticks it each frame and sends the local player's WASD/look/fire input at a fixed 60 Hz once the session is
  connected. `net_status` reports a client as `connecting` until the server admits it and assigns an id; a rejected,
  timed-out or lost connection ends the session and logs `[FPSMultiplayer] Connection failed: <reason>`. The game does not
  yet read the authoritative state back (open under `MOD-315`): the rendered local `Player` keeps its own movement,
  collision and health, remote players are not rendered, and server-side hits do not change the local `Player`. The
  convergence evidence below is of `FPSMultiplayerSystem` state, not of the rendered game.
- The developer commands listed in `Source/Console/FPSConsolePolicy.h` (`god`, `noclip`, `player_tp`, `spawn`,
  `game_timescale`, `scene_load`, `scene_save`, `gamemode`, `give`, `quest_start`, `quest_all`, `destroy`,
  `weather`, `dialogue_start`, `seq_play`, `seq_stop`, `seq_time`, `wave_skip`, `wave_difficulty`, `xp`,
  `powerup`, `fps_autoplay`) are not registered when `SPARK_BUILD_SHIPPING` is defined and `ENABLE_DEVCOMMANDS_IN_SHIPPING` is
  OFF, which today means the MinSizeRel configuration / the `windows-shipping` preset alone. A package built from
  the MSVC `Release` configuration still registers them — state which artifact a release actually ships before
  claiming the cheats are absent. The headless host's own `xp` command (`HeadlessPersistence.cpp`) is a
  server-operator command and is not covered by this list.

Build the `SparkGameFPS` target. CPU-only regression coverage is part of `SparkTests`; filter for `FPSInteg_`,
`FPSRespawn_`, `FPSArenaAutopilot_`, `FPSLocalProfile_`, `FPSProgression_`, `FPSAssets_`, `FPSStateRules_`, `FPSComponentsReal_`, and
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
`FPSLocalProfile` field, spawn/move/kill/respawn/score acceptance, a public-SDK-only module build, clean-machine
installation, recovery/soak, hardware rendering, or hosted exact-SHA qualification. The headless host registers the
same quicksave/quickload commands (see above). `FPSHeadlessPackage_NullRHISaveReload` drives them through the same
writer/reader test on NullRHI, run against the staged package (`SPARK_FPS_PACKAGE_MODE=headless-save-reload`).

## Installed arena loop

`fps_autoplay on|off` (a developer command) hands the player to `FPSArenaAutopilot`: it turns the camera with
`SparkEngineCamera::Yaw`, holds W through `InputManager::HandleMessage` so `Player::UpdateMovement` runs unmodified,
fires with `Player::Fire`, stands still after the first kill until the arena's enemies land a death, and hunts again
after the respawn. `game_status` then prints `Loop: kills=K deaths=D respawns=R score=S moved=M autopilot=<phase>`;
kills, deaths and score come from `GameMode`'s `Player1` row and respawns from `RespawnSystem` (a death whose
`PlayerRespawnEvent` was published), never from the autopilot. `FPSArenaAutopilot_` in `SparkTests` pins those rules.

`FPSSinglePlayerSlice_InstalledArenaLoop` (`SPARK_FPS_PACKAGE_MODE=arena-loop`, not offered for MinSizeRel, which
drops developer commands) installs a fresh prefix, runs the staged `SparkEngine.exe` on D3D11/WARP with
`fps_autoplay on`, and requires the final loop record to show kills >= 2, deaths >= 1, respawns >= 1, score > 0,
at least 2 m moved and `autopilot=complete`. `FPSArenaLoopParserContract` proves the verdict rejects each missing
piece. A fresh install prefix on the build host is not a clean machine.
