# Game Modules

SparkEngine ships with **ten** fully playable game modules that double as genre showcases and integration tests. Each module is a shared library (DLL on Windows, `.so` on Linux) that implements the `Spark::IModule` interface and wires specific engine subsystems together to demonstrate one genre's conventions.

> **Building your own:** see [Creating a Game Module](Creating-a-Game-Module.md) for the step-by-step integration tutorial. This page catalogs what already exists.

---

## The catalog

| Module | LOC | Genre / focus | Key engine subsystems wired |
|--------|-----|---------------|------------------------------|
| [SparkGame](#sparkgame)                     | ~870   | Base / showcase — a bit of everything | All core subsystems |
| [SparkGameFPS](#sparkgamefps)               | ~22.7K | First-person shooter | Weapons, player controller, multiplayer, AI |
| [SparkGameMMO](#sparkgamemmo)               | ~10.0K | Massively multiplayer online | AreaServer, WorldServer, persistence, dialogue |
| [SparkGameRPG](#sparkgamerpg)               | ~5.1K  | RPG mechanics | Save, animation, AI, cinematic, quest, dialogue |
| [SparkGameARPG](#sparkgamearpg)             | ~3.3K  | Diablo-style action RPG | Combat, loot, abilities, dungeon generation |
| [SparkGameOpenWorld](#sparkgameopenworld)   | ~4.9K  | Large-world exploration | Seamless streaming, origin rebasing, weather |
| [SparkGamePlatformer](#sparkgameplatformer) | ~4.1K  | 3D platformer | Player controller, destruction, audio, save |
| [SparkGameRTS](#sparkgamerts)               | ~3.3K  | Real-time strategy | AI director, events, audio, destruction |
| [SparkGameRacing](#sparkgameracing)         | ~3.8K  | Vehicle racing | Vehicle physics, camera, audio, cinematic |
| [SparkGameVisualScript](#sparkgamevisualscript) | ~370 | Script-only module | Visual scripting graphs, zero C++ game logic |

All ten are auto-discovered at CMake configure time via `GameModules/*/CMakeLists.txt` and toggled by `BUILD_GAME_MODULES` (ON by default).

---

## SparkGame

**Purpose:** baseline module with a `GameplayShowcase` scene that walks new contributors through every subsystem at least once. Always built first; other modules inherit from its CMake targets.

- **Source:** `GameModules/SparkGame/Source/`
- **Primary headers:** `Core/SparkGame.h`, `Core/GameplayShowcase.h`

## SparkGameFPS

**Purpose:** production-scale first-person shooter showcase — the largest game module by LOC. Demonstrates the FPS-specific subset of the engine originally built for this project before SparkEngine generalized.

- **Source:** `GameModules/SparkGameFPS/Source/`
- **Notable:** `Game/Player.h` (~800 LOC), `Game/Game.h` (~660 LOC), weapon system, multiplayer integration.
- **Wires:** [Input](../subsystems/Input-System.md), [Camera](../subsystems/Camera-System.md), [Physics](../subsystems/Physics.md), [Networking](../subsystems/Networking.md), [Audio](../subsystems/Audio.md), [AI](../subsystems/AI-and-Navigation.md).

## SparkGameMMO

**Purpose:** MMO networking showcase built on the HeroEngine-inspired architecture.

- **Source:** `GameModules/SparkGameMMO/Source/`
- **Wires:** [Area Server Architecture](../subsystems/Area-Server-Architecture.md), [Dedicated Server](../subsystems/Dedicated-Server.md), [Persistence System](../gameplay-tools/Persistence-System.md), weather, abilities, dialogue.

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

## SparkGamePlatformer

**Purpose:** 3D platformer — movement feel, jump mechanics, collectibles.

- **Source:** `GameModules/SparkGamePlatformer/Source/`
- **Wires:** [Audio](../subsystems/Audio.md), [Event System](../subsystems/Event-System.md), [Save System](../gameplay-tools/Save-System.md), [Destruction System](../subsystems/Destruction-System.md).

## SparkGameRTS

**Purpose:** real-time strategy — unit selection, pathing, resource economy.

- **Source:** `GameModules/SparkGameRTS/Source/`
- **Wires:** [AI](../subsystems/AI-and-Navigation.md) (flocking + director), [Event System](../subsystems/Event-System.md), [Audio](../subsystems/Audio.md), weather, [Destruction System](../subsystems/Destruction-System.md).

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

1. `EngineContext` queries each built module's `IModule::GetModuleInfo()` for metadata (name, dependencies, load order).
2. Modules load in dependency order; each `OnLoad(context)` call gets a stable service-locator pointer.
3. `OnUpdate(dt)` is ticked every frame between physics and render.
4. `OnUnload()` runs on engine shutdown or when [Hot Reload](../advanced/Hot-Reload-Overview.md) rebuilds the DLL.

See [Creating a Game Module](Creating-a-Game-Module.md) for the full `IModule` contract.

## Related

- [Creating a Game Module](Creating-a-Game-Module.md) — build your own from the `EmptyProject` template.
- [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) — how game-module CMake targets are discovered.
- [Making Your First Game](Making-Your-First-Game.md) — step-by-step single-player tutorial.
- [Making Your First Multiplayer Game](Making-Your-First-Multiplayer-Game.md) — multiplayer walkthrough that uses SparkGameFPS.
