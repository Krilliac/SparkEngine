# SparkEngine Architecture Analysis & Improvement Recommendations

**Date**: 2026-03-11
**Scope**: Full codebase audit — 425 source files across engine, editor, game, tools, and tests

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Core Architecture](#1-core-architecture)
3. [ECS Layer](#2-ecs-layer)
4. [Graphics & Rendering](#3-graphics--rendering)
5. [AI Subsystem](#4-ai-subsystem)
6. [Animation](#5-animation)
7. [Networking](#6-networking)
8. [Editor](#7-editor)
9. [Utilities & Infrastructure](#8-utilities--infrastructure)
10. [Build System](#9-build-system)
11. [Testing](#10-testing)
12. [Prioritized Roadmap](#prioritized-roadmap)

---

## Executive Summary

SparkEngine is a well-structured C++20 game engine with solid foundations: clean ECS via EnTT, a proper service locator (`EngineContext`), an emerging RHI abstraction, and a comprehensive editor. However, the codebase has grown organically and exhibits several architectural tensions that, if addressed, would significantly improve maintainability, performance, and cross-platform viability.

**Top 5 Critical Findings:**

| # | Finding | Severity | Effort |
|---|---------|----------|--------|
| 1 | GraphicsEngine is a ~750-line god class directly coupling DX11 to the render pipeline | High | Large |
| 2 | RHI abstraction exists but is not integrated — GraphicsEngine bypasses it entirely | High | Large |
| 3 | ECS systems lack parallelism despite having a JobSystem available | Medium | Medium |
| 4 | EngineContext service locator mixes hardcoded getters with a generic registry | Medium | Small |
| 5 | No asset dependency graph or async loading pipeline | Medium | Large |

---

## 1. Core Architecture

### Current State

**EngineContext** (`Core/EngineContext.h`) is the service locator and central nervous system. It:
- Inherits from `Spark::IEngineContext` (interface in a `Spark/` subdirectory)
- Has hardcoded getters/setters for 13 named subsystems (Graphics, Input, Timer, EventBus, Audio, Physics, Animation, AI, Network, SceneManager, ScriptEngine, SaveSystem, CoroutineScheduler)
- Has a generic `RegisterSystem<T>()` / `GetSystem<T>()` backed by `std::unordered_map<std::type_index, std::any>`
- Is a singleton via `static EngineContext* Get()` and `static std::unique_ptr<EngineContext>& GetOwned()`
- Holds **non-owning** raw pointers — lifetime managed externally

### Issues

1. **Dual API surface**: Named getters (`GetGraphics()`, `GetAI()`) coexist with the generic `GetSystem<T>()`. Every new subsystem requires modifying the header with a new getter/setter pair, violating OCP (Open/Closed Principle). The generic API makes the named getters redundant.

2. **No initialization ordering or dependency declaration**: Subsystems are set via individual setters in whatever order the caller chooses. There is no mechanism to declare "AI depends on Physics and NavMesh" or to enforce correct initialization sequencing.

3. **Singleton lifetime ambiguity**: `GetOwned()` returns a `unique_ptr&` reference, meaning external code can `std::move` the singleton out or reset it at any time. This is a footgun.

4. **No shutdown protocol**: There is no `Shutdown()` or ordered teardown. Subsystems hold raw pointers to each other, so destruction order matters but isn't enforced.

### Recommendations

**R1.1 — Unify on the generic registry** (Small effort)
Remove the 13 named getter/setter pairs. Use `GetSystem<GraphicsEngine>()` everywhere. This eliminates the need to modify `EngineContext.h` when adding subsystems and removes the IEngineContext virtual interface (which must also be updated in lockstep).

**R1.2 — Add dependency-aware initialization** (Medium effort)
Introduce a subsystem registration mechanism with declared dependencies:
```cpp
context.Register<AISystem>(DependsOn<PhysicsSystem, NavMeshManager>{});
context.InitializeAll(); // topological sort, init in dependency order
context.ShutdownAll();   // reverse order
```

**R1.3 — Replace singleton with explicit passing** (Medium effort)
Pass `EngineContext&` through constructors rather than using `EngineContext::Get()`. This makes dependencies explicit, improves testability, and eliminates global state.

---

## 2. ECS Layer

### Current State

The ECS uses EnTT with a clean separation:
- **Components** are pure data structs organized in 7 category headers under `ECS/Components/`
- **Systems** implement `ISystem` with `Update(World&, float)` and are managed by `SystemManager`
- **World** wraps `entt::registry` with a thin API
- Execution order: Physics → Animation → AI → Audio → Lifecycle → Render

### Strengths

- Components are genuinely data-only — no virtual methods, no logic
- `SystemManager` owns systems via `unique_ptr` with clear insertion-order execution
- Systems communicate only through shared components (no direct system-to-system calls)
- Systems can be enabled/disabled at runtime via `SetEnabled()`
- `GetEntitiesWith<Components...>()` cleanly wraps EnTT views

### Issues

1. **Serial execution only**: `SystemManager::UpdateAll()` is a simple for-loop. Physics and AI could run in parallel with Audio since they operate on disjoint component sets. The `JobSystem` exists but is unused by the ECS.

2. **No fixed timestep for physics**: `PhysicsUpdateSystem` receives the variable frame `deltaTime`. Physics simulation stability requires a fixed timestep with accumulator pattern.

3. **No system groups or phases**: All systems are in a single flat list. There's no concept of "pre-physics", "post-physics", "pre-render" phases. This makes it harder to insert new systems at the right point.

4. **LifecycleSystem uses a single callback**: `SetDeathCallback()` overwrites any previous callback. This should be an event/signal that multiple listeners can subscribe to, or use the `EventBus`.

5. **No component change detection**: Systems iterate all matching entities every frame. EnTT supports `on_construct`, `on_update`, `on_destroy` signals and groups/observers for reactive iteration — none of which are used.

### Recommendations

**R2.1 — Parallel system execution** (Medium effort)
Analyze component read/write sets per system and run non-conflicting systems in parallel via the existing `JobSystem`:
```
Phase 1 (parallel): Physics, AI  (both write Transform, but different entities)
Phase 2 (parallel): Animation, Audio
Phase 3 (serial):   Lifecycle
Phase 4 (serial):   Render
```

**R2.2 — Fixed-timestep physics** (Small effort)
Add accumulator-based fixed timestep in `PhysicsUpdateSystem::Update()`. This is critical for deterministic physics:
```cpp
m_accumulator += deltaTime;
while (m_accumulator >= FIXED_DT) {
    StepPhysics(world, FIXED_DT);
    m_accumulator -= FIXED_DT;
}
InterpolateTransforms(world, m_accumulator / FIXED_DT);
```

**R2.3 — System phases** (Small effort)
Replace the flat `vector<ISystem>` with named phases:
```cpp
sysManager.AddSystem<PhysicsUpdateSystem>(Phase::PrePhysics, ...);
sysManager.AddSystem<RenderSystem>(Phase::Render, ...);
```

**R2.4 — Use EnTT reactive features** (Medium effort)
Use `entt::observer` for components that change rarely (e.g., `MeshRenderer` material changes, `LightComponent` configuration) to avoid full iteration every frame.

---

## 3. Graphics & Rendering

### Current State

This is the most complex subsystem with 60+ header files. The architecture has three layers:

1. **GraphicsEngine** (`Graphics/GraphicsEngine.h`): Monolithic DX11 engine class — owns device, swap chain, G-buffers, render targets, subsystems, and all rendering logic
2. **RHI** (`Graphics/RHI/`): Abstract rendering hardware interface with backends for D3D11, D3D12, Vulkan, OpenGL, Metal
3. **RenderGraph** (`Graphics/RenderGraph.h`): Declarative frame graph system

### The Central Problem

**GraphicsEngine and the RHI/RenderGraph are disconnected.** GraphicsEngine directly uses `ComPtr<ID3D11Device>`, `ComPtr<ID3D11DeviceContext>`, etc. It does not go through `IRHIDevice` or `RHIBridge`. The RHI exists as a parallel abstraction that nothing uses in production.

Similarly, `RenderGraph` provides a sophisticated DAG-based pass system with automatic resource lifetime management, but `GraphicsEngine` uses hardcoded `RenderForward()` / `RenderDeferred()` / `RenderForwardPlus()` methods instead.

### Issues

1. **God class**: `GraphicsEngine` is ~700+ lines of header with 40+ public methods, 50+ private members, and owns 7+ subsystems. It handles device creation, resource management, rendering, post-processing, statistics, console commands, and shader compilation.

2. **DX11 API leakage**: Public methods expose `ID3D11Device*`, `ID3D11DeviceContext*`, `IDXGISwapChain*`, `ID3D11RenderTargetView*` directly. Any code calling these is permanently bound to DX11.

3. **Dual render path**: Both `RenderScene(objects)` (legacy GameObject-based) and `SubmitMeshForRendering()`/`ProcessDrawList()` (ECS-based) coexist. This duplication should be consolidated.

4. **No RHI integration**: `RHIBridge` is designed to sit between `GraphicsEngine` and the GPU, but `GraphicsEngine` bypasses it. The 5 RHI backends (D3D11, D3D12, Vulkan, OpenGL, Metal) are dead code.

5. **`MeshDrawCommand` uses strings**: `meshPath` and `materialPath` are `std::string`, causing allocations every frame for every visible entity. These should be hashed asset handles.

6. **Thread safety concerns**: `m_frameInProgress` is atomic, `m_metricsMutex` protects statistics, but the draw list (`m_drawList`) is a plain `vector` with no synchronization. If `SubmitMeshForRendering()` is ever called from multiple threads, this is a data race.

### Recommendations

**R3.1 — Decompose GraphicsEngine** (Large effort, high impact)
Split into focused classes:
- `RenderDevice` — thin wrapper around RHI device + swap chain
- `RenderPipeline` — owns the frame graph, executes passes
- `SceneRenderer` — collects draw commands from ECS, feeds the pipeline
- `GraphicsSubsystems` — factory/registry for TextureSystem, MaterialSystem, LightingSystem, etc.
- `GraphicsConsole` — console command implementations (already partially separated in `GraphicsConsoleCommands.h`)

**R3.2 — Route all rendering through the RHI** (Large effort, critical for cross-platform)
Replace direct DX11 calls in `GraphicsEngine` with `IRHIDevice` / `IRHICommandList` calls via `RHIBridge`. This activates the existing D3D12/Vulkan/OpenGL/Metal backends and makes cross-platform rendering functional.

**R3.3 — Activate the RenderGraph** (Large effort, high payoff)
Rebuild the render pipeline using `RenderGraph`. Each pass (G-buffer, lighting, post-process, UI) becomes a graph node. Benefits:
- Automatic resource lifetime and aliasing
- Automatic barrier/transition insertion
- Easy to add/remove passes at runtime
- Self-documenting pipeline via graph visualization

**R3.4 — Replace string-based draw commands with handles** (Small effort)
```cpp
struct MeshDrawCommand {
    AssetHandle mesh;      // uint64_t hash
    AssetHandle material;  // uint64_t hash
    XMFLOAT4X4 worldMatrix;
    uint32_t sortKey;
};
```

**R3.5 — Consolidate render submission** (Medium effort)
Remove `RenderScene(vector<GameObject*>)`. All rendering should go through the ECS `SubmitMeshForRendering()` path. `GameObject` should be deprecated in favor of ECS entities.

---

## 4. AI Subsystem

### Current State

Well-architected with three clean layers:
- **BehaviorTree**: Template-based behavior trees with Blackboard data sharing, cloned per-agent
- **NavMesh**: Data layer (NavMeshData), query layer (NavMeshQuery), management layer (NavMeshBuilder/NavMeshManager)
- **AISystem**: Orchestrates perception → behavior → pathfinding → movement
- **PerceptionSystem**: Line-of-sight and hearing checks
- **SteeringBehaviors**: Seek, flee, wander, arrival, etc.

### Issues

1. **All AI runs on the main thread**: `AISystem::Update()` iterates all agents serially. Behavior tree ticks and NavMesh queries are independent per-agent and embarrassingly parallel.

2. **NavMesh is baked offline only**: `NavMeshBuilder::Build()` is an offline process. There's no runtime dynamic obstacle support (e.g., destructible walls, moving platforms).

3. **No spatial partitioning for perception**: `UpdatePerception()` likely does O(N^2) line-of-sight checks. Should use an octree or spatial hash (the engine has an `Octree` utility that appears unused by AI).

4. **Tight coupling to Transform component**: AI movement directly writes to `Transform`. If physics is also active on the same entity, this creates conflicting writes. Should write to velocity/force and let physics resolve position.

### Recommendations

**R4.1 — Parallelize AI updates** (Medium effort)
Use `JobSystem::ParallelFor()` over the agent list. Each agent's perception, behavior tree tick, and path query are independent.

**R4.2 — Integrate Octree for spatial queries** (Small effort)
Feed the existing `Octree` utility from `Utils/Octree.h` with entity positions and use it in `UpdatePerception()` for O(log N) neighbor queries.

**R4.3 — Use physics for AI movement** (Small effort)
Instead of writing directly to `Transform`, have AI set `RigidBodyComponent::linearVelocity` or apply forces. Let the physics system resolve the final position.

**R4.4 — Runtime NavMesh obstacle support** (Large effort)
Add `NavMeshObstacle` component that carves temporary holes in the NavMesh at runtime for dynamic obstacles.

---

## 5. Animation

### Current State

- Single header `AnimationSystem.h` covering skeletal animation, IK, blend layers, state machines
- `AnimationUpdateSystem` in ECS evaluates clips, blends, solves IK, uploads bone matrices to GPU
- State machine drives animation transitions

### Issues

1. **Monolithic file**: All animation functionality is in a single header. Skeleton, clip evaluation, blending, IK, and state machines should be separate files.

2. **GPU upload every frame**: Bone matrices are uploaded for all animated entities every frame, even if the animation hasn't changed (e.g., idle pose on a static entity).

3. **No animation compression**: No mention of curve compression, keyframe reduction, or quantization — important for memory with many animation clips.

### Recommendations

**R5.1 — Split AnimationSystem.h** (Small effort)
Separate into: `Skeleton.h`, `AnimationClip.h`, `AnimationEvaluator.h`, `AnimationBlender.h`, `IKSolver.h`, `AnimationStateMachine.h`.

**R5.2 — Dirty-flag GPU uploads** (Small effort)
Only upload bone matrices when the animation state actually changes. Use a dirty flag on `AnimationController`.

**R5.3 — Animation compression** (Medium effort)
Implement curve fitting and keyframe quantization for animation data to reduce memory footprint.

---

## 6. Networking

### Current State

- Client/server architecture over UDP
- Client-side prediction and server reconciliation
- Lag compensation (hitbox rewinding)
- Reliable and unreliable channels
- Guarded by `ENABLE_NETWORKING` (off by default)
- Cross-platform socket abstraction (WinSock2 / POSIX)

### Strengths

- Clean separation via `ENABLE_NETWORKING` guard — stub provided when disabled
- Proper FPS networking patterns (prediction, reconciliation, lag compensation)
- Thread safety via mutex-protected message queue

### Issues

1. **Raw UDP socket management**: No abstraction over transport. Should support pluggable transports (e.g., Steam Networking, EOS, WebRTC) without changing game code.

2. **No encryption or authentication**: Plain UDP with no DTLS, no token-based authentication. Required for any production multiplayer.

3. **No bandwidth throttling**: No congestion control, packet pacing, or priority system. Risk of flooding on low-bandwidth connections.

4. **`NetworkIdentity` is in `AIComponents.h`**: Networking component is misplaced in the AI components file. Should be in its own `NetworkComponents.h`.

### Recommendations

**R6.1 — Transport abstraction** (Medium effort)
Create `ITransport` interface with implementations for raw UDP, Steam, EOS. `NetworkManager` operates on `ITransport` rather than raw sockets.

**R6.2 — Move `NetworkIdentity`** (Trivial)
Move from `Components/AIComponents.h` to a new `Components/NetworkComponents.h`.

**R6.3 — Add security layer** (Large effort)
Implement DTLS or a custom encryption layer, plus connection token authentication.

---

## 7. Editor

### Current State

SparkEditor has ~22 subsystems organized into a well-structured directory hierarchy:

```
Core/           — EditorApplication, EditorUI, EditorPanel, EditorTheme, LayoutManager
Panels/         — 10+ panels (Inspector, Hierarchy, Console, AssetBrowser, GameView, etc.)
Animation/      — AnimationTimeline
AssetBrowser/   — AssetDatabase
AssetPipeline/  — AdvancedAssetPipeline
BuildSystem/    — (removed; build UI is now BuildCookPanel in Panels/)
Communication/  — EngineInterface
Gizmos/         — GizmoSystem
Integration/    — SparkEngineIntegration, SparkFutureIntegration, ExternalConsoleIntegration
LevelStreaming/ — LevelStreamingSystem
Lighting/       — LightingTools
MaterialEditor/ — MaterialEditor
Profiler/       — PerformanceProfiler
Reflection/     — ComponentReflection
SceneSystem/    — SceneManager, SceneSerializer, SceneFile
Terrain/        — TerrainEditor
Utils/          — SparkConsole
VersionControl/ — VersionControlSystem
VisualScripting/ — VisualScriptingSystem
```

### Strengths

- Well-organized directory structure with clear subsystem boundaries
- `EditorPanel` base class provides uniform panel interface
- Separate `Communication/EngineInterface` for editor↔engine communication
- `ComponentReflection` for runtime type introspection in the inspector

### Issues

1. **No plugin architecture**: All 22 subsystems are compiled into the editor statically. There's no way for third-party extensions to add panels, tools, or asset importers without modifying the editor source.

2. **Editor depends directly on engine internals**: `SparkEngineIntegration` includes engine headers directly rather than going through the `IEngineContext` interface. This tight coupling makes it impossible to run the editor against a remote engine instance.

3. **Duplicate `SparkConsole`**: Both `SparkEditor/Source/Utils/SparkConsole.h` and `SparkEngine/Source/Utils/SparkConsole.h` exist — potential for confusion and divergence.

### Recommendations

**R7.1 — Editor plugin system** (Large effort)
Define `IEditorPlugin` interface with lifecycle hooks. Plugins register panels, menu items, asset importers, and gizmos. Core editor functionality becomes built-in plugins.

**R7.2 — Communicate via IEngineContext only** (Medium effort)
`EngineInterface` should only hold an `IEngineContext*` and never include concrete engine headers. This enables remote debugging and headless testing.

**R7.3 — Consolidate SparkConsole** (Trivial)
Remove the editor duplicate and use the engine's `SparkConsole` via the integration layer.

---

## 8. Utilities & Infrastructure

### Current State

35 utility headers covering: logging, profiling, assertions, math, memory, timing, debugging, file I/O, string manipulation, and more.

Notable systems:
- **JobSystem**: Thread pool with `Submit()`, `ParallelFor()`, future-based results — *well-designed but underutilized*
- **FrameAllocator**: Bump allocator for per-frame temporary allocations
- **ObjectPool**: Pre-allocated object pool for avoiding dynamic allocation
- **RingBuffer**: Lock-free ring buffer
- **ChromeTracing**: Outputs `chrome://tracing` compatible profiling data
- **Profiler**: Scoped profiling macros
- **DebugDraw/DebugOverlay**: Runtime debug visualization
- **CrashHandler/StackTrace**: Crash reporting with stack traces
- **Octree**: Spatial partitioning (unused by AI/physics)
- **ConsoleVariable**: Runtime-tunable variables (cvar system)

### Issues

1. **JobSystem is underutilized**: Despite being a complete thread pool, it's not used by any core system (ECS, rendering, AI, physics). All game logic runs single-threaded.

2. **FrameAllocator exists but MeshDrawCommand allocates strings**: The `FrameAllocator` should be used for per-frame draw command data instead of heap-allocating strings.

3. **Octree is unused**: A spatial partitioning structure exists but isn't integrated with AI perception, physics broadphase, or frustum culling. The engine has a separate `BVHAccelerator` for culling.

4. **Logger and FileLogger coexist**: Two logging systems with unclear relationship. Should be unified.

### Recommendations

**R8.1 — Integrate JobSystem into core systems** (Medium effort, high impact)
This is the single highest-impact performance improvement. Target:
- AI agent updates (embarrassingly parallel)
- Frustum culling (BVH traversal per-object)
- Animation evaluation (independent per-entity)
- Component iteration (EnTT supports parallel `each()`)

**R8.2 — Use FrameAllocator for draw commands** (Small effort)
Allocate `MeshDrawCommand` data from the frame allocator rather than the heap.

**R8.3 — Unify logging** (Small effort)
Consolidate `Logger` and `FileLogger` into a single logging system with configurable sinks (console, file, network).

---

## 9. Build System

### Current State

- Root `CMakeLists.txt` with 7 sub-projects: SparkEngine (static lib), SparkEditor, SparkGame (shared lib/DLL), SparkConsole, SparkShaderCompiler, ThirdParty, Tests
- Feature toggles via `ENABLE_*` options
- Presets for Windows MSVC, Linux GCC
- MSVC toolset selection (v143/v144)

### Issues

1. **SparkEngine is a single static library**: All engine code compiles into one `SparkEngineLib` target. This means changing a utility header recompiles graphics, AI, animation, networking, etc.

2. **No module-level targets**: There's no `SparkEngine::Graphics`, `SparkEngine::AI`, etc. CMake target dependencies are coarse-grained.

3. **Tests link against include paths, not a library**: Tests include engine source directly and recompile everything rather than linking against `SparkEngineLib`.

4. **No precompiled header (PCH)**: With 425 source files, compile times would benefit significantly from PCH for commonly included headers (Platform.h, STL containers, EnTT).

### Recommendations

**R9.1 — Split into CMake component libraries** (Medium effort, high build-time impact)
```
SparkEngine::Core       — Platform.h, EngineContext, EventBus
SparkEngine::Utils      — Logger, Profiler, Math, JobSystem, etc.
SparkEngine::ECS        — Components, Systems, World
SparkEngine::Graphics   — GraphicsEngine, RHI, RenderGraph
SparkEngine::AI         — AISystem, BehaviorTree, NavMesh
SparkEngine::Animation  — AnimationSystem
SparkEngine::Audio      — AudioEngine
SparkEngine::Network    — NetworkManager
SparkEngine::Scripting  — AngelScriptEngine
```
Each target declares its own dependencies. Changing AI code only rebuilds AI + tests.

**R9.2 — Add precompiled headers** (Small effort)
Create a `SparkPCH.h` with Platform.h, STL headers, and EnTT. Apply via `target_precompile_headers()`.

**R9.3 — Link tests against library targets** (Small effort)
Tests should `target_link_libraries(SparkTests PRIVATE SparkEngine::ECS SparkEngine::Utils)` rather than re-including source.

---

## 10. Testing

### Current State

- 40 test files covering: math, ECS, AI, physics, animation, networking, graphics effects, utilities
- Custom lightweight test framework (no GTest/Catch2 dependency)
- CTest integration

### Strengths

- Good breadth: tests cover most subsystems
- Self-contained: no external test framework dependency
- Tests build and run on Linux (cross-platform)

### Issues

1. **No integration/system tests**: All 40 tests are unit tests. No tests verify system interaction (e.g., "PhysicsSystem updates transform, RenderSystem sees new position").

2. **No graphics tests**: Graphics subsystems are tested for data-layer logic (fog, culling, effects) but there's no GPU rendering test or screenshot comparison.

3. **No performance regression tests**: Despite having `Profiler` and `ChromeTracing`, there are no benchmark tests that fail if frame time exceeds a threshold.

4. **Custom framework limitations**: The lightweight test framework likely lacks fixtures, parameterized tests, and parallel execution that mature frameworks provide.

### Recommendations

**R10.1 — Add integration tests** (Medium effort)
Test end-to-end scenarios: entity creation → component setup → system update → verify component state changes across systems.

**R10.2 — Add performance benchmark tests** (Small effort)
Use the existing `Profiler` to measure key operations and assert they stay within budgets:
```cpp
TEST("BVH traversal under 1ms for 10K objects") { ... }
TEST("AI update under 2ms for 100 agents") { ... }
```

**R10.3 — Consider adopting Catch2** (Small effort)
Catch2 is header-only (like the current approach) but provides parameterized tests, BDD-style sections, benchmarking, and parallel execution.

---

## Prioritized Roadmap

### Phase 1: Quick Wins (1-2 weeks)

| # | Item | Impact | Effort | Ref |
|---|------|--------|--------|-----|
| 1 | Fixed-timestep physics | High (stability) | Small | R2.2 |
| 2 | Replace string-based MeshDrawCommand | Medium (perf) | Small | R3.4 |
| 3 | Move NetworkIdentity to own header | Low (hygiene) | Trivial | R6.2 |
| 4 | Consolidate SparkConsole duplicate | Low (hygiene) | Trivial | R7.3 |
| 5 | Use FrameAllocator for draw commands | Medium (perf) | Small | R8.2 |
| 6 | Add precompiled headers | Medium (build time) | Small | R9.2 |
| 7 | Dirty-flag animation GPU uploads | Medium (perf) | Small | R5.2 |

### Phase 2: Structural Improvements (1-2 months)

| # | Item | Impact | Effort | Ref |
|---|------|--------|--------|-----|
| 8 | Integrate JobSystem into AI + culling | High (perf) | Medium | R8.1, R4.1 |
| 9 | Add system phases to ECS | Medium (arch) | Small | R2.3 |
| 10 | Unify EngineContext on generic registry | Medium (arch) | Small | R1.1 |
| 11 | Integrate Octree for AI perception | Medium (perf) | Small | R4.2 |
| 12 | Split AnimationSystem.h | Low (maintainability) | Small | R5.1 |
| 13 | Split CMake into component libraries | High (build time) | Medium | R9.1 |
| 14 | Add integration tests | Medium (quality) | Medium | R10.1 |

### Phase 3: Major Architectural Shifts (3-6 months)

| # | Item | Impact | Effort | Ref |
|---|------|--------|--------|-----|
| 15 | Decompose GraphicsEngine | Critical (arch) | Large | R3.1 |
| 16 | Route rendering through RHI | Critical (cross-plat) | Large | R3.2 |
| 17 | Activate RenderGraph pipeline | High (arch + perf) | Large | R3.3 |
| 18 | Dependency-aware subsystem init | Medium (arch) | Medium | R1.2 |
| 19 | Network transport abstraction | Medium (extensibility) | Medium | R6.1 |
| 20 | Editor plugin system | Medium (extensibility) | Large | R7.1 |

### Phase 4: Polish & Production-Readiness (6+ months)

| # | Item | Impact | Effort | Ref |
|---|------|--------|--------|-----|
| 21 | Parallel ECS system execution | High (perf) | Medium | R2.1 |
| 22 | Runtime NavMesh obstacles | Medium (gameplay) | Large | R4.4 |
| 23 | Network security layer | High (production) | Large | R6.3 |
| 24 | Animation compression | Medium (memory) | Medium | R5.3 |
| 25 | EnTT reactive features | Medium (perf) | Medium | R2.4 |

---

## Architecture Dependency Map

```
                    ┌──────────────┐
                    │EngineContext │  (Service Locator)
                    └──────┬───────┘
           ┌───────┬───────┼───────┬───────┬────────┐
           │       │       │       │       │        │
        ┌──▼──┐ ┌──▼──┐ ┌─▼──┐ ┌─▼──┐ ┌──▼──┐ ┌──▼───┐
        │Graph│ │Phys │ │AI  │ │Anim│ │Audio│ │Script│
        │  ics│ │ics  │ │    │ │    │ │     │ │  ing │
        └──┬──┘ └──┬──┘ └─┬──┘ └─┬──┘ └──┬──┘ └──────┘
           │       │       │      │       │
     ┌─────┤   ┌───┘   ┌──┘      │       │
     │     │   │       │         │       │
  ┌──▼──┐ │ ┌─▼───┐ ┌─▼────┐   │       │
  │RHI  │ │ │Bullet│ │NavMsh│   │       │
  │(*)  │ │ └─────┘ └──────┘   │       │
  └─────┘ │                     │       │
     │   ┌▼──────────────────────▼───────▼──┐
     │   │         ECS (EnTT World)         │
     │   │  Components ←→ Systems           │
     │   └──────────────────────────────────┘
     │
     ▼
  ┌─────────────────────────────────┐
  │  RenderGraph (*)  → GPU        │
  └─────────────────────────────────┘

  (*) = exists but not integrated into main pipeline
```

---

*This analysis is based on static code review of all 425 source files. Performance claims should be validated with profiling data from representative scenes.*
