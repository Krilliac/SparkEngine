# Gameplay & Engine Systems Status

> **Audience:** Programmers | Mixed
>
> **Thread Context:** Most systems initialize and tick on the main thread via the shared gameplay lifecycle (`GameplayLifecycleShared.cpp`). PhysicsSystem dispatches Jolt jobs across threads; NetworkManager uses a queue mutex. ECS execution order is Physics → Animation → AI → Audio → Lifecycle → Render.
>
> **Platform/Backend Scope:** Engine-wide. Networking is opt-in (`ENABLE_NETWORKING`, ON by default). VR registers but ships as an OpenXR stub backend.

## Overview

This page is a live inventory of SparkEngine's gameplay and engine subsystems: which are fully wired (Initialize/Update/Shutdown called from the lifecycle path), which were deliberately deleted as orphaned stubs, and which previously-missing capabilities have since been added. It consolidates an audit originally run in March 2026 and re-verifies every claim against the current codebase.

The canonical wiring point for engine-lifetime gameplay systems is `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp` — its `Init*`, `Update*`, and `Shutdown*` functions are where each subsystem's lifecycle hooks are called.

## Working Systems

### Core Systems

| System | Status | Key Files |
|--------|--------|-----------|
| ECS (EnTT) | Fully wired | `Engine/ECS/` (CoreComponents.h + domain headers, Systems/ECSystems.h) |
| Physics (Jolt) | Fully wired | `PhysicsSystem.cpp` + `PhysicsSystemQueries.cpp` + `PhysicsShapeFactory.cpp` |
| AI | Fully wired | `AISystem`, `NavMesh`, `BehaviorTree` |
| Animation | Fully wired via ECS | `AnimationSystem.cpp` |
| Audio (XAudio2) | Fully wired | `AudioEngine.cpp` |
| Input | Fully wired | `InputManager.cpp` |
| Camera | Fully wired | `SparkEngineCamera.cpp` |
| Scripting (AngelScript) | Fully wired with hot-reload | `Engine/Scripting/` |
| Save system | Fully wired | `SaveSystem.cpp` |
| UI system | Registered in EngineContext, updated | `UISystem.cpp` |
| Modding | Registered in EngineContext | `ModSystem.cpp` |
| Events / EventBus | Active | `EventSystem.h` delegates to `EventBus.h` |
| Coroutines | Registered with EngineContext | `CoroutineScheduler.h` |
| 2D graphics / sprites | Phase-manager registration | `Systems2D.h`, `Physics2D.h` |
| Dialogue | Registered & updated via EngineContext | `DialogueSystem.cpp` |
| Weather | Registered & updated via EngineContext | `WeatherSystem.cpp` |
| World origin | Active | `WorldOriginSystem` |
| Localization | Wired & tested | `LocalizationSystem.cpp` |
| Destruction | Wired in the gameplay init path | `DestructionSystem.cpp` |

### TrinityCore-Inspired Systems

All wired into the gameplay lifecycle via the `Init*`/`Update*`/`Shutdown*` functions.

| System | Namespace | Init | Update | Shutdown | Tests |
|--------|-----------|------|--------|----------|-------|
| AbilitySystem | `Spark::Gameplay` | Yes | Yes (world, dt) | Yes | `TestAbilitySystem.cpp` |
| ConditionSystem | `Spark::Gameplay` | Yes | N/A (stateless) | Yes | `TestConditionSystem.cpp` |
| InstanceManager | `Spark::Gameplay` | Yes | Yes (dt) | Yes | `TestInstanceManager.cpp` |
| MovementSystem | `Spark::AI` | Yes | Yes (world, dt) | Yes | `TestMovementSystem.cpp` |
| SpatialGrid | `Spark::World` | Via ctor | SyncFromECS | N/A | `TestSpatialGrid.cpp` |
| AsyncDatabase | `Spark::Persistence` | `Open()` | `ProcessCallbacks()` | `Close()` | `TestAsyncDatabase.cpp` |
| ReplicationFields | `Spark::Net` | N/A (data) | N/A (data) | N/A | `TestReplicationFields.cpp` |
| ScriptHookManager | `Spark::Scripting` | Singleton | `DispatchHook()` | `Clear()` | `TestScriptHookManager.cpp` |
| ConsoleRBAC | `Spark::Console` | Singleton | N/A (query) | N/A | N/A (minimal) |
| ModuleHotReload | `Spark` | `Initialize()` | `PollChanges()` | `Stop()` | `TestModuleHotReload.cpp` |

### Previously Missing Systems (Added)

| System | Status | Notes |
|--------|--------|-------|
| Inventory (engine-level) | Added & wired | `Spark::Gameplay::InventorySystem` — item registry, per-entity slots, stacking, transfer. Init/Update/Shutdown called from the lifecycle path. |
| Quest (engine-level) | Added & wired | `Spark::Gameplay::QuestSystem` — objectives, progress tracking, rewards, prerequisites. Wired in the lifecycle path. |
| Terrain rendering | Added & wired | `Graphics::ClipmapTerrain` with heightmap sampling, LOD levels, GPU buffer creation. Init/Shutdown called from the lifecycle path. |

## Systems Deleted (orphaned / stub in prior sessions)

| System | Approx Size | Why Deleted |
|--------|-------------|-------------|
| Streaming (seamless areas) | ~1,000+ lines | Stub, never wired |
| Procedural generation | 52 KB | Never instantiated |
| Cinematic / Sequencer | 29 KB | Never initialized |
| Replay system | Large | Never initialized |
| Achievement system | Medium | Never initialized |
| Content Delivery | 337 lines | Stub-only CDN |
| Visual Scripting (2 copies) | 7,343 lines | Duplicate, neither wired |

> **Note:** Several of these names now appear again as active subsystem wiki pages (Replay System, Achievement System, Content Delivery, Cinematic, Streaming). The engine has since re-introduced or re-implemented some of these capabilities. Treat this table as the historical record of the *earlier* stub deletions, not the current presence of those features — verify against the corresponding subsystem wiki page.

## Notes

- Networking is fully implemented but opt-in. The default build (`ENABLE_NETWORKING=ON`) compiles it; transports such as `SteamTransport` remain stubs (see [Stub & Abandoned Features](Stub-and-Abandoned-Features.md)).
- VR is registered via core subsystems but ships an OpenXR stub backend — see the Stub page.
- ConsoleRBAC is minimal enough not to warrant dedicated tests.

## Source & Freshness

- Original entry: `.claude/knowledge/gameplay-systems-status.md` (last updated 2026-03-18; physics Jolt migration noted 2026-03-22; missing systems noted 2026-04-01).
- Verified against codebase 2026-06-08.

Status changes / verifications found during freshening:

- **Confirmed wired:** AbilitySystem, ConditionSystem, InstanceManager, InventorySystem, QuestSystem, and ClipmapTerrain all have `Initialize`/`Update`/`Shutdown` calls in `GameplayLifecycleShared.cpp` (lines ~414–418, 482, 871–877, 1176, 1209–1213).
- **Re-introduced features:** Replay, Achievement, Content Delivery, Cinematic, and Streaming — listed as deleted in the original — now exist again as documented subsystem wiki pages. The "Systems Deleted" table is retained as a historical record with a caveat.
- No regressions found: every "Working Systems" entry still has its named source file present.

## Related Pages

- [SparkGame Module Status](SparkGame-Module-Status.md)
- [Stub & Abandoned Features](Stub-and-Abandoned-Features.md)
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md)
- [Codebase Health](Codebase-Health.md)
