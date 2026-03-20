# Eleven Engine + Three Rendering Framework Analysis

**Last updated:** 2026-03-20
**Type:** Decision
**Status:** Active
**Engines analyzed:** Godot 4.x, O3DE, Wicked Engine, Flax Engine, Bevy, Stride (Xenko), Torque3D, Ogre-Next, The Forge, Google Filament, bgfx

## Description

Comprehensive cross-engine analysis of 11 engines and 3 rendering frameworks, identifying the highest-value features and architectural patterns for SparkEngine. Combined with previous analyses (TrinityCore, CryEngine, Cocos/Defold/Panda3D/S&box/Halley, PCSX2), this brings total engines studied to ~20.

## Context

SparkEngine already has: EnTT ECS, D3D11 primary RHI with D3D12/Vulkan/Metal/OpenGL experimental, Bullet Physics, AngelScript scripting, ImGui editor (32 panels), UDP networking with AreaServer/WorldServer, EntityReplicator with dirty tracking, RenderGraph, PipelineStateCache, TransientResourcePool, JobSystem, SaveSystem, and 45+ working systems from prior analyses.

## Engine-by-Engine Key Findings

### Godot 4.x
- **Collision layer/mask system**: 32-bit layer + 32-bit mask per body, named layers in editor. Bullet supports `btOverlapFilterCallback`.
- **WorkerThreadPool**: Standard thread pool with task submission. SparkEngine lacks a general-purpose thread pool.
- **Render thread separation**: RenderingServer uses command queue for thread-safe submission. SparkEngine is main-thread only.
- **Tween system**: Lightweight procedural animation (`create_tween().tween_property(...)`) for UI, camera, gameplay juice.
- **Clustered lighting**: Compute shader 3D frustum grid for multi-light performance.
- **Resource system**: Unified `ResourceLoader` with pluggable format loaders, path-based caching, async loading.
- **`[Tool]` scripts**: Scripts that run in editor for custom tools/gizmos.
- **One-shot event subscriptions**: `CONNECT_ONE_SHOT` auto-disconnects after first emission.

### O3DE (Open 3D Engine)
- **Prefab system**: JSON prefabs with per-instance overrides (JSON patches RFC 6902). Nested prefabs with propagation.
- **Asset pipeline**: File-watching `AssetProcessor` with pluggable `IAssetBuilder` interface, hot-reload notifications, critical asset escalation.
- **Feature Processor pattern**: `Simulate()`/`Render()` base class for modular rendering features (mesh, shadows, post-FX).
- **Reflection system**: `SPARK_REFLECT` macro → serialization → editor UI → scripting bindings (3-phase).
- **Per-entity EventBus addressing**: `ById` dispatch for entity-scoped events (major perf win).
- **Module initialization ordering**: `IModule` interface with dependency graph for init order.
- **Network codegen**: XML-based auto-generation of replication/RPC boilerplate.
- **Data-driven render passes**: JSON pass definitions for customizable render pipeline.
- **Lessons**: Over-modularization hurt O3DE. Keep SparkEngine's monolithic-with-toggles approach.

### Wicked Engine
- **Bindless rendering**: 500K descriptors, GPU-driven occlusion culling (2-frame-delay query readback with 64-frame bitmask history).
- **Job system (`wi::jobsystem`)**: Standard C++ thread pool, `Dispatch(count, batchSize, lambda)`, 128-byte job storage (cache-line pair). No fibers, no platform dependencies.
- **Visibility buffer**: UINT texture with primitive IDs from depth prepass. GPU-driven rendering path.
- **Bitmask flags in components**: Saves memory vs. individual bools.
- **FFT ocean + volumetric clouds**: Production-quality weather/atmosphere.
- **Jolt migration**: Wicked moved from Bullet to Jolt for better performance.

### Flax Engine
- **Software GI via Global SDF**: Real-time DDGI without RTX hardware. Signed distance fields + Global Surface Atlas.
- **`API_FUNCTION`/`API_CLASS` macros**: Auto-generate scripting bindings from C++ declarations.
- **C++ DLL hot-reload**: Unload/reload SparkGame.dll in editor without restart.
- **Prefab system**: Per-instance property overrides, nested prefabs with diff tracking.
- **Auto-generated ImGui panels**: Reflect component properties → auto-generate editor UI.
- **Attribute-based replication**: `[NetworkReplicated]` auto-syncs properties, `DirtyObject()` for immediate replication.
- **Game Cooker**: Per-platform asset caching, non-blocking background cooking, CI/CD support.
- **Replication hierarchy**: Distance-based culling + per-object replication tick rate.

### Bevy Engine (Rust)
- **Archetypal ECS with change detection**: `ComponentTicks` tracks added/changed ticks per component. Queries filter by `Changed<T>` or `Added<T>`.
- **Dual-world rendering**: Main World + Render World. Extract → Prepare → Queue → Render phases. CPU prepares frame N+1 while GPU renders frame N.
- **Observers/hooks**: `on_add`, `on_remove`, `on_insert`, `on_replace`, `on_despawn` lifecycle hooks. Observers are event-driven systems that only run when triggered.
- **Required components**: `Component` trait declares required companion components (auto-inserted).
- **Plugin composition**: App = collection of plugins. Each plugin adds systems, resources, and sub-plugins.
- **System scheduling**: System sets, run conditions (`.run_if(...)`), ordering constraints, ambiguity detection for parallel safety.
- **Render phases**: `RenderPhase<T>` with sorted draw lists and composable `DrawFunctions`.

### Stride (Xenko)
- **SDSL shader mixins**: Object-oriented shader composition with inheritance, mixins, `stage`/`stream`/`clone` keywords. Cross-compiles to HLSL/GLSL/SPIR-V.
- **Clustered forward+**: Default renderer, good for VR (MSAA-compatible).
- **EntityProcessor model**: Components + Processors (similar to systems but tightly coupled).
- **HRTF spatial audio**: Built-in HRTF support via Windows Spatial Audio framework.
- **Light probes + light shafts**: Ray-marched shadow-based light shafts.
- **Graphics Compositor**: Separate asset for render pipeline customization.

### Torque3D
- **Ghost system (per-connection scoping)**: Server determines which objects each client can see. Only in-scope objects consume bandwidth. 12-bit rotating ghost IDs for anti-cheat.
- **Priority-based bandwidth allocation**: Objects closer to camera or with larger state deltas get bandwidth first. Per-frame byte budget.
- **Datablock system**: Immutable shared-data objects sent once at mission start. Entities reference by ID, zero ongoing bandwidth.
- **CVar system**: Typed console variables auto-exposed to script/editor/config.
- **Zone/portal culling**: Box-shaped zones connected by portals for coarse occlusion.
- **Virtual file system**: Mount directories and ZIP archives to virtual paths. Enables modding.
- **Client prediction**: Already implemented in SparkEngine.

### Ogre-Next (Ogre 3.0)
- **HLMS (High Level Material System)**: Property-driven shader generation with deduplicated Macro/Blendblocks.
- **Forward Clustered lighting**: 3D frustum grid, multi-threaded construction (slices per thread), SIMD-optimized intersection.
- **Parallel frustum culling**: Split entity list across worker threads, thread-local result lists, merge.
- **AoSoA + SIMD transforms**: 4 nodes packed for SSE2, dummy pointers instead of null checks.
- **Background texture streaming**: Dedicated worker thread + pre-allocated `StagingTextures` pool with memory budget.
- **Render state deduplication**: Shared Macro/Blendblocks via pooling.
- **Workspace instancing**: Parameterize render graph for stereo VR rendering.

### The Forge
- **Visibility Buffer (TVB 2.0)**: Fully GPU-driven, compute dispatches replace draw calls. Requires bindless textures.
- **Software VRS**: 4xMS + stencil-based VRS map from image gradients. Works on D3D11 without hardware VRS.
- **Unified root signature**: One graphics + one compute root signature shared across all shaders.
- **Async resource loader**: Multi-threaded loading across renderers.
- **Dropped Vulkan on Windows**: D3D12 only on Windows, Vulkan for Android/Switch/Steam Deck.

### Google Filament
- **Material definition format**: Declarative materials compiled offline with `matc` tool. PBR with Lambertian + Cook-Torrance.
- **SH-based IBL**: 3rd-order spherical harmonics (9 coefficients) for diffuse, prefiltered cubemaps for specular. <1% error at near-zero runtime cost.
- **Frame graph**: Dead-pass culling, resource aliasing for non-overlapping lifetimes.
- **Handle-based resources**: Two-phase create (handle allocated sync, GPU resource created async). Generation counters for validation.
- **Command-stream backend**: Frontend records driver calls, backend thread decodes and executes. Automatic threading.

### bgfx
- **Transient vertex/index buffers**: Per-frame bump allocation from ring buffer, reset each frame. Eliminates per-draw map overhead.
- **State encoding**: Full pipeline state packed into `uint64_t` for fast hashing/sorting.
- **Per-thread encoders**: One command buffer per thread, double-buffered frame pipeline.
- **Frame-delayed destruction**: Destroy calls deferred 1+ frames to prevent use-after-free on in-flight resources.
- **Cross-compilation (shaderc)**: GLSL-like source → HLSL/GLSL/SPIR-V/Metal. SparkEngine should use DXC + SPIRV-Cross instead.

---

## TOP 30 RECOMMENDATIONS (Deduplicated, Prioritized)

### TIER 1 — HIGH PRIORITY (Foundational improvements)

| # | Recommendation | Sources | SparkEngine Gap | Effort |
|---|----------------|---------|-----------------|--------|
| 1 | **Job system / WorkerThreadPool** | Godot, Wicked | No general-purpose thread pool. Physics/AI/particles run serially. | Medium |
| 2 | **Frame-delayed resource destruction** | bgfx, Filament | Immediate destruction risks use-after-free with in-flight resources. | Small |
| 3 | **Collision layer/mask system** | Godot, Torque3D | No collision filtering beyond Bullet defaults. | Small |
| 4 | **Per-connection object scoping** | Torque3D | EntityReplicator sends to all connections, no visibility filtering. | Medium |
| 5 | **Handle-based RHI resources** | Filament, bgfx | Handle typedefs exist but unused; raw pointers returned. | Medium |
| 6 | **Background texture streaming** | Ogre-Next | Area streaming exists but texture loads hitch the main thread. | Medium |
| 7 | **Transient vertex/index allocator** | bgfx | Dynamic buffers use per-draw Map/Unmap. | Small |
| 8 | **Prefab system (entity templates)** | O3DE, Flax | No prefab/template system for editor or runtime spawning. | Medium |
| 9 | **SH-based image-based lighting** | Filament | IBL fields exist but no SH coefficients or prefiltered cubemaps. | Medium |
| 10 | **Parallel frustum culling** | Ogre-Next | No multi-threaded culling. | Small |

### TIER 2 — MEDIUM PRIORITY (Significant improvements)

| # | Recommendation | Sources | SparkEngine Gap | Effort |
|---|----------------|---------|-----------------|--------|
| 11 | **Clustered forward/forward+ lighting** | Godot, Ogre-Next, Stride | Only deferred path exists. Needed for transparency + VR. | Medium |
| 12 | **Tween system** | Godot | No procedural animation for UI/camera/gameplay juice. | Small |
| 13 | **Datablock/archetype registry** | Torque3D | No immutable shared-data objects. Entities duplicate stats. | Small |
| 14 | **Per-entity EventBus addressing** | O3DE | EventBus broadcasts globally, no entity-scoped dispatch. | Small |
| 15 | **Feature Processor pattern** | O3DE | Rendering features hard-coded in pipeline, not modular. | Medium |
| 16 | **Material definition format** | Filament, Ogre-Next | Hard-coded cbuffer structs. No declarative material system. | Medium |
| 17 | **DrawIndirect/DispatchIndirect API** | The Forge, bgfx | `IndirectArgs` buffer usage exists but no draw calls. | Small |
| 18 | **Unified ResourceManager** | Godot | Asset loading scattered across subsystems. | Medium |
| 19 | **Virtual file system** | Torque3D | No VFS for mod/DLC mounting. | Medium |
| 20 | **CVar system** | Torque3D | No typed console variable registry exposed to script/editor. | Small |

### TIER 3 — LOW PRIORITY (Polish and future)

| # | Recommendation | Sources | SparkEngine Gap | Effort |
|---|----------------|---------|-----------------|--------|
| 21 | **Render thread separation** | Godot, PCSX2 | Main-thread rendering only. | Large |
| 22 | **Macro-based API exposure for scripting** | Flax | AngelScript bindings are manual registration. | Medium |
| 23 | **Software VRS (stencil-based)** | The Forge | VRS capability flag exists, no implementation. | Medium |
| 24 | **ECS change detection** | Bevy | No per-component change tracking for reactive systems. | Medium |
| 25 | **Component dependency validation** | Bevy, O3DE | Editor doesn't validate required companion components. | Small |
| 26 | **Auto-generated editor panels** | Flax, O3DE | ImGui component panels are hand-coded. | Medium |
| 27 | **Render state deduplication** | Ogre-Next | PipelineStateCache exists but verify state object dedup. | Small |
| 28 | **Hybrid occlusion culling** | Wicked | No GPU-driven occlusion culling. | Medium |
| 29 | **Shader mixin composition** | Stride | Shaders are monolithic HLSL files, no modular composition. | Large |
| 30 | **HRTF spatial audio** | Stride | XAudio2 exists but no HRTF/spatial audio processing. | Small |

### SKIP LIST (Already covered or wrong direction)

| Feature | Why Skip |
|---------|----------|
| Scene/node tree (Godot) | EnTT ECS is architecturally superior |
| EC pattern (O3DE) | ECS > EC for cache performance and parallelism |
| Custom ECS (Wicked) | EnTT already provides equivalent performance |
| Lua scripting (Wicked) | AngelScript is more type-safe and C++-like |
| Visual scripting (Flax, O3DE) | Massive effort, AngelScript hot-reload serves the need |
| Full Gem system (O3DE) | Over-engineered for SparkEngine's scale |
| SDFGI/VoxelGI (Godot) | Backends not mature enough yet |
| bgfx view system | Render graph is superior |
| Filament command-stream | Adds latency/debugging complexity vs. immediate mode |
| Replace Bullet with Jolt | Adequate for now; evaluate when physics becomes bottleneck |

## Cross-Engine Pattern Analysis

### Patterns appearing in 3+ engines (strongest signals):

1. **Job system / thread pool** — Godot, Wicked, Ogre-Next, O3DE, Bevy all use some form
2. **Clustered lighting** — Godot, Ogre-Next, Stride, Filament all implement
3. **Prefab system** — O3DE, Flax, Stride, Godot all have; SparkEngine does not
4. **Handle-based resources** — Filament, bgfx, The Forge, Bevy all use typed handles
5. **Asset pipeline with hot-reload** — O3DE, Flax, Godot, Wicked all have file-watching
6. **Frame-delayed destruction** — bgfx, Filament, The Forge all defer GPU resource deletion
7. **Per-connection scoping** — Torque3D, Flax, O3DE all filter replication by relevance
8. **Collision layers** — Godot, Torque3D, Stride, Bullet (native support) all use
9. **Data-driven materials** — Filament, Ogre-Next, Stride, Wicked all have declarative systems
10. **Reflection / auto-binding** — O3DE, Flax, Bevy, Stride all reflect types to editor/script

## Implementation Order (recommended session sequence)

**Session 1 (Quick wins, <200 lines each):**
- Frame-delayed destruction (#2)
- Collision layer/mask system (#3)
- Transient buffer allocator (#7)
- Tween system (#12)

**Session 2 (Job system + parallelism):**
- WorkerThreadPool (#1)
- Parallel frustum culling (#10)
- DrawIndirect API (#17)

**Session 3 (Networking):**
- Per-connection object scoping (#4)
- Datablock registry (#13)
- Per-entity EventBus addressing (#14)
- CVar system (#20)

**Session 4 (Rendering quality):**
- SH-based IBL (#9)
- Clustered forward lighting (#11)
- Handle-based RHI resources (#5)
- Material definition format (#16)

**Session 5 (Asset & editor):**
- Prefab system (#8)
- Background texture streaming (#6)
- Unified ResourceManager (#18)
- Virtual file system (#19)

## Notes

- All recommendations are architectural patterns only — no code is copied from any engine
- Prior analyses (TrinityCore, CryEngine, 5-engine, PCSX2) contributed ~58 recommendations; this adds 30 more for ~88 total
- The strongest cross-engine signal is that SparkEngine needs: (1) a job system, (2) a prefab system, (3) handle-based resources, and (4) an asset pipeline — these appear in nearly every engine studied
- Wicked Engine is the closest architectural match to SparkEngine (C++, similar scale, similar renderer trajectory)
- Bevy's ECS innovations (change detection, observers, required components) can be adapted to EnTT without replacing it
