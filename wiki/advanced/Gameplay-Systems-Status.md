# Gameplay & Engine Systems Status

> **Audience:** Programmers | Mixed
>
> **Thread Context:** Most systems initialize and tick on the main thread via the shared gameplay lifecycle (`GameplayLifecycleShared.cpp`). PhysicsSystem dispatches Jolt jobs across threads; NetworkManager uses a queue mutex. ECS execution order is Physics → Animation → AI → Audio → Lifecycle → Render.
>
> **Platform/Backend Scope:** Cross-platform source inventory. Networking is conditional (`ENABLE_NETWORKING`, ON by default); backend and platform support must be read from the release profile rather than inferred from source presence. VR registers but ships as an OpenXR stub backend.
>
> **`stable-v1` support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified. This page inventories source, lifecycle-call, and test-file surfaces, including experimental and out-of-profile capabilities; it is not a support matrix, passing-test report, or release claim.

## Overview

This page records a source-level inventory of SparkEngine gameplay and engine subsystems: named lifecycle calls, source surfaces, test-file references, and an older record of orphaned-stub deletions. Source or test-file presence does not establish runtime behavior, current test results, platform support, or release readiness.

The canonical wiring point for engine-lifetime gameplay systems is `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp` — its `Init*`, `Update*`, and `Shutdown*` functions are where each subsystem's lifecycle hooks are called.

## Source and Lifecycle Inventory

### Core Systems

| System | Status | Key Files |
|--------|--------|-----------|
| ECS (EnTT) | Source surface present | `Engine/ECS/` (CoreComponents.h + domain headers, Systems/ECSystems.h) |
| Physics (Jolt) | Lifecycle and query source present | `PhysicsSystem.cpp` + `PhysicsSystemQueries.cpp` + `PhysicsShapeFactory.cpp` |
| AI | Source surface present | `AISystem`, `NavMesh`, `BehaviorTree` |
| Animation | ECS system source present | `AnimationSystem.cpp` |
| Audio | Backend factory and engine source present | `Audio/AudioEngine.cpp`, with XAudio2 on Windows, OpenAL on non-Windows, and Null fallback paths |
| Input | Source surface present | `InputManager.cpp` |
| Camera | Source surface present | `SparkEngineCamera.cpp` |
| Scripting (AngelScript) | VM and file-watcher source present | `Engine/Scripting/` |
| Save system | Source surface present | `SaveSystem.cpp` |
| UI system | EngineContext registration/update source present | `UISystem.cpp` |
| Modding | EngineContext registration source present | `ModSystem.cpp` |
| Events / EventBus | Delegation source present | `EventSystem.h` delegates to `EventBus.h` |
| Coroutines | EngineContext registration source present | `CoroutineScheduler.h` |
| 2D graphics / sprites | Phase-manager registration source present | `Systems2D.h`, `Physics2D.h` |
| Dialogue | EngineContext registration/update source present | `DialogueSystem.cpp` |
| Weather | EngineContext registration/update source present | `WeatherSystem.cpp` |
| World origin | Source surface present | `WorldOriginSystem` |
| Localization | Source and test files present | `LocalizationSystem.cpp`; `Tests/TestLocalizationSystem.cpp` (test presence only) |
| Destruction | Lifecycle calls present | `DestructionSystem.cpp` |

### TrinityCore-Inspired Systems

The current lifecycle source contains the named hooks. Test filenames below are inventory references; they do not assert that a test was executed or passed for the current commit or profile.

| System | Namespace | Init | Update | Shutdown | Test source |
|--------|-----------|------|--------|----------|-------|
| AbilitySystem | `Spark::Gameplay` | Yes | Yes (world, dt) | Yes | `TestAbilitySystem.cpp` |
| ConditionSystem | `Spark::Gameplay` | Yes | N/A (stateless) | Yes | `TestConditionSystem.cpp` |
| InstanceManager | `Spark::Gameplay` | Yes | Yes (dt) | Yes | `TestInstanceManager.cpp` |
| MovementSystem | `Spark::AI` | Yes | Yes (world, dt) | Yes | `TestMovementSystem.cpp` |
| SpatialGrid | `Spark::World` | Via ctor | SyncFromECS | N/A | `TestSpatialGrid.cpp` |
| AsyncDatabase | `Spark::Persistence` | `Open()` | `ProcessCallbacks()` | `Close()` | `TestAsyncDatabase.cpp` |
| ReplicationFields | `Spark::Net` | N/A (data) | N/A (data) | N/A | `TestReplicationFields.cpp` |
| ScriptHookManager | `Spark::Scripting` | Singleton | `DispatchHook()` | `Clear()` | `TestScriptHookManager.cpp` |
| ConsoleRBAC | `Spark::Console` | Singleton | N/A (query) | N/A | `TestConsoleRBAC.cpp` |
| ModuleHotReload (`Spark::ModuleHotReloadManager`, `Core/ModuleHotReload.cpp`) | `Spark` | `Initialize()` | `PollChanges()` (polled by the platform frame loops) | `Stop()` | `TestModuleHotReload.cpp` -- the older `Engine/HotReload/ModuleHotReload` singleton is no longer in the lifecycle |

### Source Surfaces Added Since the Historical Audit

| System | Status | Notes |
|--------|--------|-------|
| Inventory (engine-level) | Lifecycle calls present | `Spark::Gameplay::InventorySystem` — item registry, per-entity slots, stacking, transfer. Init/Update/Shutdown calls appear in the lifecycle source. |
| Quest (engine-level) | Lifecycle calls present | `Spark::Gameplay::QuestSystem` — objectives, progress tracking, rewards, prerequisites. Lifecycle calls appear in the current source. |
| Terrain rendering | Lifecycle calls present | `Graphics::ClipmapTerrain` with heightmap sampling, LOD levels, GPU buffer creation. Init/Shutdown calls appear in the lifecycle source. |

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

> **Note:** Several of these names now have current source or documentation surfaces. Treat this table only as the historical record of the *earlier* stub deletions; verify current source and the readiness contract individually. A wiki page by itself is not implementation or support evidence.

## Notes

- Networking source and conditional test surfaces exist behind `ENABLE_NETWORKING` (ON by default in the root CMake configuration). The readiness contract classifies multiplayer as experimental and blocked; hostile multi-client, compatibility, and transport work remains open, and transports such as `SteamTransport` remain stubs (see [Stub & Abandoned Features](Stub-and-Abandoned-Features.md)).
- VR is registered via core subsystems but ships an OpenXR stub backend — see the Stub page.
- `Tests/TestConsoleRBAC.cpp` is a dedicated test source; its presence is not a claim that it passed for this commit.

## Source Snapshot and Limitations

- Original entry: `.claude/knowledge/gameplay-systems-status.md` (last updated 2026-03-18; physics Jolt migration noted 2026-03-22; missing systems noted 2026-04-01).
- Older audit review date: 2026-06-08. This is not same-commit verification.

This correction checked the named lifecycle call sites and readiness classifications on 2026-08-28 without executing builds or tests. It does not certify behavior or regression status.

Source observations retained or corrected:

- **Lifecycle call sites observed:** AbilitySystem, ConditionSystem, InstanceManager, InventorySystem, QuestSystem, MovementSystem, DestructionSystem, and ClipmapTerrain have named init/update/shutdown calls in `GameplayLifecycleShared.cpp`.
- **Re-introduced source surfaces:** Replay, Achievement, Cinematic, and Streaming have current lifecycle or source entries. The "Systems Deleted" table is retained only as a historical record with a caveat.
- Named paths must be checked at the commit under review; file presence alone cannot establish behavior, passing tests, or the absence of regressions.

## Related Pages

- [SparkGame Module Status](SparkGame-Module-Status.md)
- [Stub & Abandoned Features](Stub-and-Abandoned-Features.md)
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md)
- [Codebase Health](Codebase-Health.md)
