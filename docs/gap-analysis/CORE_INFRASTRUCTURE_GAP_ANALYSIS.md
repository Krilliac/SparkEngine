# SparkEngine Core Infrastructure — Gap Analysis

> **Scope**: `SparkEngine/Source/Core/` (EngineContext, IEngineContext, Platform), `SparkEngine/Source/Engine/World/` (DayNightCycle), Engine-wide architectural patterns
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of Core/ headers, EngineContext, IEngineContext, and cross-cutting architectural analysis of all subsystem access patterns.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Core subsystem provides the foundational infrastructure: `EngineContext` (service locator), `IEngineContext` (module-facing interface), `Platform.h` (cross-platform type stubs), and the `SparkEngine` main class. The `EngineContext` currently exposes six subsystems: Graphics, Input, Timer, EventBus, Audio, and Physics. However, the engine has grown to include 14+ subsystems (AI, Animation, Networking, Scripting, SaveSystem, Coroutines, Sequencer, Procedural Generation, DayNightCycle, etc.), most of which bypass `EngineContext` entirely and use singletons for global access.

---

## Critical Gaps

### GAP-CI01 — EngineContext Only Exposes 6 of 14+ Subsystems

**Files**:
- `Core/EngineContext.h` (lines 33–44, getter methods)
- `Core/Spark/IEngineContext.h` (interface)

**Impact**: `EngineContext` and `IEngineContext` only provide access to: `GraphicsEngine`, `InputManager`, `Timer`, `EventBus`, `AudioEngine`, and `PhysicsSystem`. The following subsystems are **not** accessible through the service locator and rely on their own singleton patterns:

| Subsystem | Access Pattern | File |
|-----------|---------------|------|
| AnimationSystem | Unknown | `Engine/Animation/` |
| AISystem | Unknown | `Engine/AI/` |
| NetworkManager | Singleton likely | `Engine/Networking/` |
| ScriptEngine | Unknown | `Engine/Scripting/` |
| SaveSystem | `GetInstance()` | `Engine/SaveSystem/SaveSystem.h` |
| CoroutineScheduler | `GetInstance()` | `Engine/Coroutine/CoroutineScheduler.h` |
| SequencerManager | `GetInstance()` | `Engine/Cinematic/Sequencer.h` |
| DayNightCycle | Instance-based | `Engine/World/DayNightCycle.h` |
| PlatformInputManager | `GetInstance()` | `Input/PlatformInput.h` |

This defeats the purpose of the service locator pattern. Game modules (loaded as DLLs) receive an `IEngineContext*` but cannot access most engine systems through it. They must call singleton `GetInstance()` methods directly, creating tight coupling and making subsystems impossible to mock for testing.

**Evidence**: `EngineContext.h` has exactly 6 `Get*()` methods and 6 `Set*()` methods. `IEngineContext` declares the same 6 virtual getters. No other subsystem is registered.

**What is needed**: Expand `IEngineContext` and `EngineContext` to include all subsystems. Add `Get*()` and `Set*()` for each. Consider a generic `GetSystem<T>()` template method for extensibility. Gradually remove singleton patterns from subsystems.

---

## Major Gaps

### GAP-CI02 — No Scene or World Management System

**Files**:
- `Engine/World/` directory (contains only `DayNightCycle.h`)

**Impact**: Despite having a `World/` directory, the engine has no `SceneManager` or `WorldManager`. There is no system for: loading/unloading game levels, managing scene transitions (with loading screens), tracking which scene is active, or coordinating subsystem state between scenes. The ECS `World` (from the ECS subsystem) manages entities but not level/scene lifecycle.

**Evidence**: `Engine/World/` contains only `DayNightCycle.h`. No `SceneManager.h`, `LevelLoader.h`, or similar file exists. `SaveMetadata::sceneName` (in `SaveSystem.h`) stores a scene name but there is no `SceneManager` to load it during deserialization. The editor has `SparkEditor/Source/SceneSystem/` but this is editor-side, not engine-side.

**What is needed**: Create an engine-side `SceneManager` that handles: scene definition (list of assets, entities, settings per scene), loading/unloading scenes (with progress callbacks for loading screens), scene transitions (fade out, load, fade in), and integration with `SaveSystem` (load scene before restoring entity state).

---

### GAP-CI03 — No Engine-Level Asset Management Pipeline

**Files**:
- `Core/` directory
- `SparkEditor/Source/AssetPipeline/`, `SparkEditor/Source/AssetBrowser/`

**Impact**: The engine has no runtime asset management system. The editor has `AssetPipeline/` and `AssetBrowser/` directories, but these are editor-only tools. At runtime, there is no central asset registry, no reference counting, no async asset loading, and no asset dependency tracking. Each subsystem loads its own resources independently (e.g., `SoundEffect::Load()`, shader loading in `GraphicsEngine`).

**What is needed**: Create a runtime `AssetManager` in the engine core that provides: asset registry (path to loaded asset mapping), reference counting for shared assets, async loading with completion callbacks, and asset hot-reload support.

---

### GAP-CI04 — InputManager Exposed Instead of PlatformInputManager

**Files**:
- `Core/EngineContext.h` (lines 35–36, `GetInput()` returns `InputManager*`)
- `Input/PlatformInput.h` (`PlatformInputManager` — modern, cross-platform)

**Impact**: `EngineContext` exposes the legacy `InputManager` (Win32-only, no action mapping, no gamepad) rather than the modern `PlatformInputManager` (cross-platform, pluggable backends, action/axis mapping). Game modules using `context->GetInput()` get the inferior, platform-locked input system. See INPUT_GAP_ANALYSIS.md GAP-I01 for details.

**What is needed**: Replace `InputManager*` with `PlatformInputManager*` in `EngineContext` and `IEngineContext`.

---

## Moderate Gaps

### GAP-CI05 — DLL Module System Status Unclear

**Files**:
- `Core/` directory (likely contains `GameModuleLoader` or `ModuleManager`)
- `SparkGame/Source/` (example game module)

**Impact**: The engine architecture supports loading game modules as DLLs (evidenced by `SparkGame/` being a separate module and `IEngineContext` being a module-facing interface). However, the status of hot-reloading is unclear. If the module system supports hot-reload, it needs careful handling of: module unload (release all resources), state preservation across reloads, and pointer invalidation (raw pointers held by the engine become dangling when the DLL is unloaded).

**What is needed**: Document the module loading lifecycle. If hot-reload is supported, add a `OnModuleUnload()` callback that modules implement to release resources. If not supported, document that modules require engine restart.

---

### GAP-CI06 — No Engine Lifecycle Events

**Files**:
- `Core/EngineContext.h`, `Core/Spark/IEngineContext.h`

**Impact**: There are no engine lifecycle callbacks (OnStartup, OnShutdown, OnPause, OnResume, OnFocusLost, OnFocusGained). Subsystems that need to respond to engine state changes (e.g., pause audio when window loses focus, save state on shutdown) must be explicitly called by the main loop. There is no event-driven lifecycle notification.

**What is needed**: Add lifecycle events to `EventBus` (e.g., `EngineStartupEvent`, `EngineShutdownEvent`, `EnginePauseEvent`, `WindowFocusEvent`). Publish these from the main engine loop. Subsystems subscribe to respond to lifecycle changes.

---

### GAP-CI07 — Octree/Spatial Partitioning Not Integrated

**Files**:
- `Utils/` (likely contains `Octree.h` based on test evidence)
- Engine subsystems (Graphics, Physics, AI)

**Impact**: An Octree data structure exists in `Utils/` and is tested (`TestFrustumCulling.cpp` suggests spatial partitioning exists). However, it is unclear whether the Octree is integrated into: the graphics pipeline for frustum culling, the physics system for broad-phase collision, the AI system for spatial queries (nearest enemy, line-of-sight), or the audio system for spatial audio queries.

**What is needed**: Audit Octree usage across subsystems. Ensure it is used for frustum culling in the renderer and broad-phase in physics. Expose spatial query APIs (e.g., `QueryRadius(center, radius)`, `QueryFrustum(frustum)`) through a central `SpatialIndex` in the engine context.

---

### GAP-CI08 — Platform.h Cross-Platform Stubs Completeness Unknown

**Files**:
- `Core/Platform.h`

**Impact**: `Platform.h` defines `SPARK_PLATFORM_WINDOWS`, `SPARK_PLATFORM_LINUX`, `SPARK_PLATFORM_MACOS` and provides DirectXMath stubs for non-Windows. However, the completeness of these stubs is unclear. If `XMFLOAT3`, `XMFLOAT4`, `XMMATRIX`, etc. stubs are incomplete or non-functional, no subsystem that uses DirectXMath types will work on Linux/macOS.

**What is needed**: Audit `Platform.h` stubs against all DirectXMath usage across the codebase. Ensure all used types and functions have working cross-platform implementations. Consider using GLM or a purpose-built math library as the cross-platform backend.

---

## Minor Gaps

### GAP-CI09 — No Configuration/Settings Persistence System

**Files**: Core directory

**Impact**: There is no engine-wide configuration system for persisting settings (graphics quality, audio volume, key bindings, window resolution) between sessions. Each subsystem manages its own settings independently with no unified save/load mechanism for user preferences.

**What is needed**: Create an `EngineSettings` class that reads/writes a configuration file (JSON or INI). Subsystems register their configurable values. Settings are loaded at startup and saved on change or shutdown.

---

### GAP-CI10 — Version Constants Defined But Not Centralized

**Files**:
- `Core/EngineContext.h` (lines 54–55, `GetEngineVersion()`, `GetSDKVersion()`)

**Impact**: `EngineContext` has `GetEngineVersion()` and `GetSDKVersion()` methods, but the version values are defined in the `.cpp` file and not exposed as compile-time constants. Other subsystems that need version information (save system versioning, network protocol versioning) cannot access it without going through EngineContext.

**What is needed**: Define `SPARK_ENGINE_VERSION_MAJOR`, `SPARK_ENGINE_VERSION_MINOR`, `SPARK_ENGINE_VERSION_PATCH` as compile-time constants in `Platform.h` or a dedicated `Version.h`.

---
