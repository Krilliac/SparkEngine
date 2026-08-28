# Game Modules

SparkEngine contains **11 in-tree game-module directories** with differing prototype maturity. They are source showcases and test fixtures, not released games. Only the blocked and uncertified `SparkGameFPS` single-player slice is inside `stable-v1`; the other modules and multiplayer breadth are outside it. Dynamic modules implement `Spark::IModule` and build as shared libraries where their targets are enabled.

> **Building your own:** see [Creating a Game Module](Creating-a-Game-Module.md) for the step-by-step integration tutorial. This page catalogs what already exists.

---

## The catalog

| Module | LOC | Genre / focus | Key engine subsystems wired |
|--------|-----|---------------|------------------------------|
| [SparkGame](#sparkgame)                     | ~870   | Base / showcase — a bit of everything | All core subsystems |
| [SparkGameFPS](#sparkgamefps)               | ~22.7K | First-person shooter | Weapons, player controller, multiplayer, AI |
| [SparkGameMMO](#sparkgamemmo)               | ~10.0K | Massively multiplayer online | AreaServer, WorldServer, persistence, dialogue |
| [SparkGameMMOFPS](#sparkgamemmofps)         | —      | MMOFPS / Terrafront | Combined-arms MMOFPS systems |
| [SparkGameRPG](#sparkgamerpg)               | ~5.1K  | RPG mechanics | Save, animation, AI, cinematic, quest, dialogue |
| [SparkGameARPG](#sparkgamearpg)             | ~3.3K  | Diablo-style action RPG | Combat, loot, abilities, dungeon generation |
| [SparkGameOpenWorld](#sparkgameopenworld)   | ~4.9K  | Large-world exploration | Seamless streaming, origin rebasing, weather |
| [SparkGamePlatformer](#sparkgameplatformer) | ~4.1K  | 3D platformer | Player controller, destruction, audio, save |
| [SparkGameRTS](#sparkgamerts)               | ~3.3K  | Real-time strategy | AI director, events, audio, destruction |
| [SparkGameRacing](#sparkgameracing)         | ~3.8K  | Vehicle racing | Vehicle physics, camera, audio, cinematic |
| [SparkGameVisualScript](#sparkgamevisualscript) | ~370 | Script-only module | Visual scripting graphs, zero C++ game logic |

The root build enumerates 11 module targets when `BUILD_GAME_MODULES` is enabled (ON by default); this target inventory is not a claim that every module is playable or release-ready.

---

## SparkGame

**Purpose:** baseline module with a `GameplayShowcase` scene that walks new contributors through every subsystem at least once. It follows the same CMake module pattern as the other in-tree targets; it is not a prerequisite target for them.

- **Source:** `GameModules/SparkGame/Source/`
- **Primary headers:** `Core/SparkGame.h`, `Core/GameplayShowcase.h`

## SparkGameFPS

**Purpose:** first-person shooter implementation showcase and the blocked stable-v1 vertical-slice candidate. It is not yet a certified or released product.

- **Source:** `GameModules/SparkGameFPS/Source/`
- **Notable:** `Game/Player.h` (~800 LOC), `Game/Game.h` (~660 LOC), weapon system, multiplayer integration.
- **Wires:** [Input](../subsystems/Input-System.md), [Camera](../subsystems/Camera-System.md), [Physics](../subsystems/Physics.md), [Networking](../subsystems/Networking.md), [Audio](../subsystems/Audio.md), [AI](../subsystems/AI-and-Navigation.md).

## SparkGameMMO

**Purpose:** MMO networking showcase built on the HeroEngine-inspired architecture.

- **Source:** `GameModules/SparkGameMMO/Source/`
- **Wires:** [Area Server Architecture](../subsystems/Area-Server-Architecture.md), [Dedicated Server](../subsystems/Dedicated-Server.md), [Persistence System](../gameplay-tools/Persistence-System.md), weather, abilities, dialogue.

## SparkGameMMOFPS

**Purpose:** Terrafront MMOFPS source module. It is an in-tree development artifact, not a `stable-v1` certified or released game.

- **Source:** `GameModules/SparkGameMMOFPS/`
- **Reference:** `GameModules/SparkGameMMOFPS/README.md`

## SparkGameRPG

**Purpose:** classic RPG mechanics — quests, dialogue, party members, save/load.

- **Source:** `GameModules/SparkGameRPG/Source/`
- **Wires:** [Save System](../gameplay-tools/Save-System.md), [Animation](../subsystems/Animation.md), [AI](../subsystems/AI-and-Navigation.md), [Cinematic Sequencer](../gameplay-tools/Cinematic-Sequencer.md), [Dialogue System](../subsystems/Dialogue-System.md), [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) (quests, abilities, conditions).

## SparkGameARPG

**Purpose:** Diablo-style click-to-move action RPG.

- **Source:** `GameModules/SparkGameARPG/Source/`
- **Notable:** `Combat/ARPGCombatSystem.h`, `Dungeon/ARPGDungeonSystem.h`, `Hero/ARPGHeroSystem.h`, `Loot/ARPGLootSystem.h`.
- **Wires:** [Loot and Crafting](../gameplay-tools/Loot-And-Crafting-System.md), ability system, procedural dungeon generation.

## SparkGameOpenWorld

**Purpose:** large-world exploration — kilometer-scale terrain with streaming.

- **Source:** `GameModules/SparkGameOpenWorld/Source/`
- **Wires:** [Large World Support](../subsystems/Large-World-Support.md) (origin rebasing + seamless streaming), [Day/Night Cycle and Weather](../gameplay-tools/Day-Night-Cycle-and-Weather.md), [Terrain and Procedural Generation](../gameplay-tools/Terrain-and-Procedural-Generation.md), [HLOD and World Partition](../gameplay-tools/HLOD-And-World-Partition.md).
- **Playable loop:** entering a point of interest records discovery; fast-travel POIs unlock immediately, and teleports/travel synchronize the active biome used by wildlife and dynamic events.

## SparkGamePlatformer

**Purpose:** 3D platformer — movement feel, jump mechanics, collectibles.

- **Source:** `GameModules/SparkGamePlatformer/Source/`
- **Wires:** [Audio](../subsystems/Audio.md), [Event System](../subsystems/Event-System.md), [Save System](../gameplay-tools/Save-System.md), [Destruction System](../subsystems/Destruction-System.md).

## SparkGameRTS

**Purpose:** real-time strategy — unit selection, pathing, resource economy.

- **Source:** `GameModules/SparkGameRTS/Source/`
- **Wires:** [AI](../subsystems/AI-and-Navigation.md) (flocking + director), [Event System](../subsystems/Event-System.md), [Audio](../subsystems/Audio.md), weather, [Destruction System](../subsystems/Destruction-System.md).
- **Playable loop:** selection is backed by the live unit roster, while move/stop/hold commands and queued waypoints drive unit state and deterministic map-space movement.

## SparkGameRacing

**Purpose:** vehicle racing — wheeled/tracked/motorcycle vehicles from Jolt.

- **Source:** `GameModules/SparkGameRacing/Source/`
- **Wires:** [Physics](../subsystems/Physics.md) (vehicle subsystem), [Camera System](../subsystems/Camera-System.md) (chase/cockpit), [Audio](../subsystems/Audio.md), [Cinematic Sequencer](../gameplay-tools/Cinematic-Sequencer.md).

## SparkGameVisualScript

**Purpose:** demonstrates the "zero C++" game module — all gameplay authored through [Visual Scripting](../subsystems/Visual-Scripting.md) graphs that compile to AngelScript at runtime.

- **Source:** `GameModules/SparkGameVisualScript/Source/`
- **Smallest module** at ~370 LOC because the C++ side only loads graph assets; logic lives in `.sgraph` files.

---

## Lifecycle — how the engine loads them

1. At runtime, the host resolves a game-module candidate through `-game`, a manifest, or discovery; CMake-built targets are candidates, not a bulk-load list.
2. `ModuleManager` reads a candidate's `IModule::GetModuleInfo()` while loading it and permits only one `Game`-kind primary module.
3. The selected primary game module receives `OnLoad(context)` and `OnUpdate(dt)` in the engine lifecycle.
4. Its `OnUnload()` runs on engine shutdown or when [Hot Reload](../advanced/Hot-Reload-Overview.md) rebuilds the DLL.

See [Creating a Game Module](Creating-a-Game-Module.md) for the full `IModule` contract.

## Related

- [Creating a Game Module](Creating-a-Game-Module.md) — build your own from the `EmptyProject` template.
- [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) — how game-module CMake targets are discovered.
- [Making Your First Game](Making-Your-First-Game.md) — step-by-step single-player tutorial.
- [Making Your First Multiplayer Game](Making-Your-First-Multiplayer-Game.md) — multiplayer walkthrough that uses SparkGameFPS.
