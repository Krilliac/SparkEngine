# SparkGame Module & Tools Status

> **Audience:** Programmers | Mixed
>
> **Thread Context:** Game modules run on the main thread via `OnLoad`/`OnUpdate`/`OnRender`. SparkConsole runs a background log-reader thread and communicates over named pipes (Windows) or stdin/stdout.
>
> **Platform/Backend Scope:** Source inventory only. Game-module code uses the RHI, but cross-platform and cross-backend behavior is not certified. SparkConsole IPC has Windows named-pipe code with a stdin/stdout fallback; SparkShaderCompiler source exposes D3D11, D3D12, Vulkan, and OpenGL selectors without proving artifacts on every backend.
>
> **`stable-v1` support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified, and its exact host is Windows 11 x64. The repository contains 11 in-tree module directories; only `SparkGameFPS` is the blocked in-profile first-party candidate. The other ten are prototype, template, or showcase source surfaces outside the profile, not completed or released games.

## Overview

This page inventories selected source surfaces in the FPS module, standalone debug console, and offline shader compiler. It does not assign completion percentages or certify that the products build, run, or pass tests at the current commit.

> **Naming note:** The original audit referred to the FPS game as `SparkGame`. The repository has since split the modules — `GameModules/SparkGame/` is a base/showcase module, while the FPS source lives in `GameModules/SparkGameFPS/`. The 11 directory inventory is SparkGame, SparkGameARPG, SparkGameFPS, SparkGameMMO, SparkGameMMOFPS, SparkGameOpenWorld, SparkGamePlatformer, SparkGameRacing, SparkGameRPG, SparkGameRTS, and SparkGameVisualScript.

## SparkGameFPS — Example FPS Game Module

### Module Loading Source

- Module entry point in `GameModules/SparkGameFPS/Source/Core/Main.cpp` (including the Windows `DllMain` path).
- Factory exports: `CreateModule()`, `DestroyModule()`, `CreateGameModule()`, `DestroyGameModule()`.
- ModuleManager and module source expose `OnLoad`, `OnUpdate`, and `OnRender` lifecycle paths.
- Source identifies the module as an engine-showcase arena candidate; loading and packaged execution remain uncertified.

### Game.cpp / Game.h Source Surface

- `Game.cpp` has 855 physical lines and `Game.h` has 668 in the current source inventory. The older audit recorded a 2,042-line `Game.cpp`; the current source is split into `GameSetup.cpp`, `GameConsoleOps.cpp`, `GameEngineSystems.cpp`, `GameMechanics.cpp`, and `GameMode.cpp`.
- Sets up camera, player, projectile pool, scene manager, vehicle system, gravity, interaction, game mode, HUD, inventory, quests.
- Console command source includes teleport, spawn, clear-scene, and time-scale operations.

### Player.cpp / Player.h Source Surface

- `Player.h` has 795 physical lines and `Player.cpp` has 1,084 in the current source inventory.
- Input and movement source covers WASD movement, jumping, crouching, and sprinting.
- Physics-related source includes gravity, friction, and stamina paths.
- Weapon source exposes `Fire()`, `Reload()`, and `ChangeWeapon()` with ammunition state.
- Health / armor / shield with damage reduction.
- Class system (Scout, Recon, Titan) with unique abilities (jetpack, cloak, energy shield) via `ClassSystem.cpp`.
- Console integration via `PlayerConsole.cpp` (includes memory-integrity speed/jump validation guards).

### HUD System Source Surface

- `HUDSystem.h` / `HUDSystem.cpp`.
- Source defines health/armor/shield bars, ammo counter, crosshair, damage indicators, kill feed, hit markers, vignette, weapon-switch notifications, minimap, interaction prompts, scoreboard, and class indicators. This inventory is not packaged visual-acceptance evidence.

### Weapons Source Surface

- Weapon definitions include Pistol, Rifle, Shotgun, Rocket Launcher, Sniper, SMG, and other types.
- Damage, fire rate, magazine, reload time, accuracy stats.
- Source connects player fire/reload paths to a projectile pool with Bullet, Rocket, and Grenade types.

### Vehicles Source Surface

- `VehicleSystem.h` / `VehicleSystem.cpp` (~1,009 lines).
- Ground (Buggy, Tank, APC, Motorcycle, Truck) + aerial (Helicopter, Jet, Dropship, Drone).
- Multi-seat system (Driver / Gunner / Passenger).
- Vehicle physics (acceleration, steering, aerial stabilization).
- Source routes vehicle weapons through the projectile pool, derives muzzle position from seat offset and vehicle rotation, and maps weapon type to projectile type.

### AI / Enemies Source Surface

- `Enemy.cpp` / `Enemy.h`: archetypes (Grunt, Guard, Scout, Heavy, Sniper, Medic).
- Each enemy owns a behavior tree with combat/patrol/idle branches.
- Distance-based perception, blackboard-driven targeting.
- `WaveSpawner.cpp` / `WaveSpawner.h`: escalating waves with difficulty scaling, boss rounds, rest periods.
- `LootSystem.cpp`: enemies drop loot on death.
- **Gap (unchanged):** no NavMesh pathfinding integration in the FPS module — a repository-wide search for `NavMesh`/`FindPath`/`Pathfind` under `GameModules/SparkGameFPS/Source/` returns no hits. Enemies move directly toward targets rather than navigating the mesh.

### Level Loading Source Surface

- `LoadScene()`, `SaveScene()`, `GetAvailableScenes()`, and scene-creation methods are defined in source.
- Source contains serialization/deserialization paths for game objects in `.scene` files.
- **Gap:** no dynamic level streaming inside the module.

## SparkConsole — Standalone Debug Console Source

- `SparkConsole/src/main.cpp` entry, `ConsoleApp.h/.cpp`.
- Source contains named-pipe transport on Windows and stdin/stdout fallback paths.
- Source implements a color-coded engine-log display path.
- Source implements command input with auto-completion and history handling.
- Built-in commands: help, clear, history, alias, exit. Game commands forwarded to the engine over the pipe.
- Source includes a background log-reader thread. SparkConsole is a required `stable-v1` product, but its build and packaged execution remain blocked and uncertified with the profile.

> **Wiring reminder (from CLAUDE.md):** `ConsoleProcessManager` launches the subprocess and owns the pipe; it must be initialized at engine startup with `ProcessCommands()` called each frame. `SimpleConsole` is the engine-side log sink only.

## SparkShaderCompiler — Offline Shader Tool Source

- `SparkShaderCompiler/src/main.cpp`.
- Single-file and batch modes (e.g. `SparkShaderCompiler BasicVS.hlsl -stage vertex -backend d3d11 -o BasicVS.cso`; `-batch Shaders/ -backend vulkan`).
- Stages: vertex, pixel, geometry, hull, domain, compute.
- Backend selectors in source: D3D11, D3D12, Vulkan, OpenGL; selector presence does not prove successful output on each backend.
- Preprocessor defines, include paths, optimization, debug info, reflection output.
- Delegates to `Spark::RHI::CompileShader()`. SparkShaderCompiler is a required `stable-v1` product, but its build and packaged execution remain blocked and uncertified with the profile.

## Anti-Bloat Status

The original audit flagged four oversized FPS files. The picture has improved after the `Game.cpp` split:

| File | Current Lines | `.cpp`/`.h` guideline | Status |
|------|--------------:|-----------------------|--------|
| `Game.cpp` | ~840 | ~500 (`.cpp`) | Reduced from 2,042; still over but split into helper TUs |
| `Player.cpp` | ~1,081 | ~500 | Still over guideline |
| `Player.h` | ~795 | ~300 (`.h`) | Still over guideline |
| `Game.h` | ~659 | ~300 | Still over guideline |
| `VehicleSystem.cpp` | ~1,009 | ~500 | Over guideline (one cohesive system) |

## Summary

| Component | Observed source surface | Boundary or known gap |
|-----------|-------------------------|-----------------------|
| Module loading (DLL) | Factory exports and lifecycle methods are present | Installed-public-SDK loading and packaged lifecycle evidence remain open |
| Game loop | Setup/update/render source paths are present | No certified author-cook-package-install-run loop |
| Player controller | Movement, health, class, and weapon source is present | No same-commit gameplay acceptance evidence |
| HUD system | HUD element source is present | No packaged visual-acceptance evidence |
| Weapons | Weapon and projectile source is present | Runtime balance and complete behavior are not certified |
| Vehicles | Ground/aerial vehicle and seat/weapon source is present | Runtime physics and gameplay behavior are not certified |
| AI / enemies | Behavior-tree, wave, and loot source is present | No NavMesh/pathfinding references in the FPS module source |
| Level loading | Scene load/save source is present | No dynamic level streaming in the module |
| SparkConsole.exe | Entry point, IPC, and console source is present | Required profile product; blocked and uncertified |
| SparkShaderCompiler.exe | Entry point and backend-selector source is present | Required profile product; blocked and uncertified |

**Boundary:** SparkGameFPS is the blocked `stable-v1` single-player candidate. Its source contains FPS gameplay surfaces, but it is not a complete, released, or certified game. SparkConsole and SparkShaderCompiler likewise have source surfaces and are required profile products, not certified tools.

## Source Snapshot and Limitations

- Original entry: `.claude/knowledge/sparkgame-module-status.md` (last updated 2026-03-16; vehicle weapons noted 2026-04-01).
- Older audit review date: 2026-06-08. This is not same-commit certification.

This correction enumerated the module directories, checked the named entry points and selected source paths, and measured physical file lines on 2026-08-28. It did not build or run the modules or tools and does not certify functionality.

Source observations retained or corrected:

- **Module renamed/split:** the FPS source is now `GameModules/SparkGameFPS/`; the repository contains 11 module directories, each with `CMakeLists.txt` and `Source/`.
- **`Game.cpp` is 855 physical lines** — split alongside `GameSetup.cpp`, `GameConsoleOps.cpp`, `GameEngineSystems.cpp`, `GameMechanics.cpp`, and `GameMode.cpp`.
- **`VehicleSystem.cpp` is 1,009 physical lines**, not the 10,000+ stated in the original audit.
- **NavMesh gap persists** for FPS enemies (verified: no NavMesh/pathfind references in the FPS module source).
- SparkConsole and SparkShaderCompiler entry points remain present; this review produced no build, test, or packaged-runtime evidence for either tool.

## Related Pages

- [Gameplay & Engine Systems Status](Gameplay-Systems-Status.md)
- [Game Modules (catalog)](../getting-started/Game-Modules.md)
- [Creating a Game Module](../getting-started/Creating-a-Game-Module.md)
- [Codebase Health](Codebase-Health.md)
