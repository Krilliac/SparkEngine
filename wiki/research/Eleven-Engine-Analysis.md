# Eleven Engine + Three Rendering Framework Analysis

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** Cross-engine reference

## Overview

Cross-engine analysis of **11 engines and 3 rendering frameworks**, identifying the highest-value features and architectural patterns for SparkEngine: **Godot 4.x, O3DE, Wicked Engine, Flax Engine, Bevy, Stride (Xenko), Torque3D, Ogre-Next, The Forge, Google Filament, and bgfx**. Combined with the TrinityCore, CryEngine, Five-Engine, and PCSX2 analyses, this brought the total engines studied to ~20. All recommendations are architectural patterns only — no code is copied.

As of the 2026-06-08 codebase verification, **most Tier 1 recommendations and many Tier 2 items have been implemented.** Each item is annotated below.

> **Status legend:** **Implemented** — concrete subsystem exists. **Partial** — core exists, refinement incomplete. **Open** — not yet built.

## Engine-by-Engine Key Findings

### Godot 4.x
Collision layer/mask (32-bit layer + mask); WorkerThreadPool; render-thread command queue; Tween system; clustered lighting; unified `ResourceLoader`; `[Tool]` editor scripts; one-shot event subscriptions.

### O3DE
JSON prefabs with per-instance overrides (RFC 6902 patches); file-watching `AssetProcessor` + `IAssetBuilder`; Feature Processor (`Simulate()`/`Render()`); 3-phase reflection (`SPARK_REFLECT` → serialization → editor UI → scripting); per-entity EventBus addressing; module init ordering; XML network codegen; data-driven render passes. *Lesson: over-modularization hurt O3DE — keep SparkEngine's monolithic-with-toggles approach.*

### Wicked Engine
Bindless rendering (500K descriptors); GPU-driven occlusion culling (2-frame-delay readback, 64-frame bitmask history); standard C++ thread-pool job system; visibility buffer; bitmask component flags; FFT ocean + volumetric clouds; Bullet→Jolt migration. *Closest architectural match to SparkEngine.*

### Flax Engine
Software GI via Global SDF (DDGI without RTX); `API_FUNCTION`/`API_CLASS` scripting-binding macros; C++ DLL hot-reload; prefab system with diff tracking; auto-generated ImGui panels; attribute-based replication; Game Cooker (per-platform asset caching); distance-based replication hierarchy.

### Bevy Engine (Rust)
Archetypal ECS with change detection (`ComponentTicks`, `Changed<T>`/`Added<T>`); dual-world rendering (Extract→Prepare→Queue→Render); observers/lifecycle hooks; required components; plugin composition; system scheduling with run conditions + ambiguity detection; `RenderPhase<T>`.

### Stride (Xenko)
SDSL shader mixins (OO composition, cross-compiles to HLSL/GLSL/SPIR-V); clustered forward+ (VR/MSAA-friendly); EntityProcessor model; HRTF spatial audio; light probes + ray-marched light shafts; Graphics Compositor.

### Torque3D
Ghost system (per-connection scoping, 12-bit rotating ghost IDs); priority-based bandwidth allocation; datablock system (immutable shared data sent once); CVar system; zone/portal culling; virtual file system; client prediction.

### Ogre-Next (Ogre 3.0)
HLMS property-driven shader generation with deduplicated Macro/Blendblocks; forward clustered lighting (multi-threaded, SIMD); parallel frustum culling; AoSoA + SIMD transforms; background texture streaming with `StagingTextures` pool; render state dedup; workspace instancing for stereo VR.

### The Forge
Visibility Buffer (TVB 2.0, GPU-driven); software VRS (4xMS + stencil from gradients, works on D3D11); unified root signature; async resource loader; dropped Vulkan on Windows (D3D12 only).

### Google Filament
Declarative material format (compiled offline with `matc`); SH-based IBL (3rd-order, 9 coeffs); frame graph with dead-pass culling + resource aliasing; handle-based resources (two-phase create, generation counters); command-stream backend.

### bgfx
Transient vertex/index buffers (per-frame bump allocation); `uint64_t` state encoding; per-thread encoders; frame-delayed destruction; shaderc cross-compilation (SparkEngine should prefer DXC + SPIRV-Cross).

---

## Top 30 Recommendations (Prioritized + Freshened)

### Tier 1 — High Priority (Foundational)

| # | Recommendation | Sources | Status (2026-06-08) | Evidence |
|---|----------------|---------|---------------------|----------|
| 1 | **Job system / WorkerThreadPool** | Godot, Wicked | **Implemented** | `Utils/JobSystem.h` |
| 2 | **Frame-delayed resource destruction** | bgfx, Filament | **Partial / unverified** | RHI present; deferred-delete queue not separately confirmed |
| 3 | **Collision layer/mask system** | Godot, Torque3D | **Implemented** | Jolt layer/mask filtering in `Source/Physics/` + ECS physics components |
| 4 | **Per-connection object scoping** | Torque3D | **Implemented** | `ConnectionScope.h`, `ConnectionScopeFilter.h` |
| 5 | **Handle-based RHI resources** | Filament, bgfx | **Implemented** | `RHIHandlePool` (per Advanced Techniques RHI-parity phases) |
| 6 | **Background texture streaming** | Ogre-Next | **Partial** | Area streaming + DirectStorage loader present; dedicated staging-pool path partial |
| 7 | **Transient vertex/index allocator** | bgfx | **Implemented** | `TransientBufferAllocator` (RHI-parity phases) |
| 8 | **Prefab system (entity templates)** | O3DE, Flax | **Implemented** | `Engine/ECS/RuntimePrefab.h` |
| 9 | **SH-based image-based lighting** | Filament | **Implemented** | `LightProbeSystem` (L2 SH), `AdaptiveProbeVolumes` |
| 10 | **Parallel frustum culling** | Ogre-Next | **Partial** | Job system + GPU culling present; multi-threaded CPU frustum cull not separately confirmed |

### Tier 2 — Medium Priority

| # | Recommendation | Sources | Status (2026-06-08) | Evidence |
|---|----------------|---------|---------------------|----------|
| 11 | **Clustered forward/forward+ lighting** | Godot, Ogre-Next, Stride | **Implemented** | `ClusteredLightCulling`, `GPUClusterCulling` |
| 12 | **Tween system** | Godot | **Implemented** | `Engine/Tween/TweenSystem` |
| 13 | **Datablock/archetype registry** | Torque3D | **Implemented** | `Engine/Networking/DatablockRegistry.h` |
| 14 | **Per-entity EventBus addressing** | O3DE | **Open** | `EventSystem.h` present; entity-scoped `ById` dispatch not found |
| 15 | **Feature Processor pattern** | O3DE | **Open** | No `FeatureProcessor`-style modular render-feature base found |
| 16 | **Material definition format** | Filament, Ogre-Next | **Implemented** | `Graphics/MaterialDefinition.h` |
| 17 | **DrawIndirect/DispatchIndirect API** | The Forge, bgfx | **Partial** | GPU-driven culling implies indirect args; full public API path partial |
| 18 | **Unified ResourceManager** | Godot | **Partial** | VFS + asset loaders present; single unified manager not confirmed |
| 19 | **Virtual file system** | Torque3D | **Implemented** | `Engine/Modding/VirtualFileSystem` |
| 20 | **CVar system** | Torque3D | **Implemented** | `Utils/ConsoleVariable.h` |

### Tier 3 — Low Priority / Future

| # | Recommendation | Sources | Status (2026-06-08) |
|---|----------------|---------|---------------------|
| 21 | Render thread separation | Godot, PCSX2 | **Open** (large) |
| 22 | Macro-based API exposure for scripting | Flax | **Open** — AngelScript bindings still manual |
| 23 | Software VRS (stencil-based) | The Forge | **Open** — VRS capability flag exists, no impl |
| 24 | ECS change detection | Bevy | **Open / unverified** |
| 25 | Component dependency validation | Bevy, O3DE | **Open / unverified** |
| 26 | Auto-generated editor panels | Flax, O3DE | **Open / unverified** |
| 27 | Render state deduplication | Ogre-Next | **Implemented** — `PipelineStateCache` |
| 28 | Hybrid occlusion culling (GPU) | Wicked | **Implemented** — GPU-driven culling + HiZ (per Advanced Techniques baseline) |
| 29 | Shader mixin composition | Stride | **Partial** — `ShaderVariantSystem.h` exists; full mixin/inheritance not present |
| 30 | HRTF spatial audio | Stride | **Open / unverified** |

### Skip List (intentionally not adopted)

| Feature | Why Skip |
|---------|----------|
| Scene/node tree (Godot) | EnTT ECS is architecturally superior |
| EC pattern (O3DE) | ECS > EC for cache performance and parallelism |
| Custom ECS (Wicked) | EnTT already provides equivalent performance |
| Lua scripting (Wicked) | AngelScript is more type-safe and C++-like |
| Visual scripting (Flax, O3DE) | AngelScript hot-reload serves the need *(note: a VisualScript game module now exists — see GameModules/SparkGameVisualScript)* |
| Full Gem system (O3DE) | Over-engineered for SparkEngine's scale |
| bgfx view system | Render graph is superior |
| Filament command-stream | Adds latency/debugging complexity vs. immediate mode |
| Replace Jolt | Jolt is already the physics backend; no replacement needed *(original said "Replace Bullet with Jolt" — migration is done)* |

## Cross-Engine Pattern Analysis (strongest signals)

Patterns appearing in 3+ engines, with current status:

1. **Job system / thread pool** (Godot, Wicked, Ogre-Next, O3DE, Bevy) — *Implemented.*
2. **Clustered lighting** (Godot, Ogre-Next, Stride, Filament) — *Implemented.*
3. **Prefab system** (O3DE, Flax, Stride, Godot) — *Implemented.*
4. **Handle-based resources** (Filament, bgfx, The Forge, Bevy) — *Implemented.*
5. **Asset pipeline with hot-reload** (O3DE, Flax, Godot, Wicked) — *Partial.*
6. **Frame-delayed destruction** (bgfx, Filament, The Forge) — *Partial.*
7. **Per-connection scoping** (Torque3D, Flax, O3DE) — *Implemented.*
8. **Collision layers** (Godot, Torque3D, Stride) — *Implemented.*
9. **Data-driven materials** (Filament, Ogre-Next, Stride, Wicked) — *Implemented.*
10. **Reflection / auto-binding** (O3DE, Flax, Bevy, Stride) — *Partial* (`SPARK_REFLECT`/`ComponentReflection.cpp` exist; auto editor/script generation incomplete).

## Remaining Open Work (post-freshening)

The genuinely open items are: **#14 per-entity EventBus addressing**, **#15 Feature Processor pattern**, **#21 render-thread separation**, **#22 macro-based scripting API exposure**, **#23 software VRS**, **#24–26 Bevy-style ECS change detection / component validation / auto editor panels**, **#29 shader mixin composition** (partial), and **#30 HRTF audio**. The four strongest cross-engine signals from the original conclusion — job system, prefab system, handle-based resources, and clustered lighting — are all now done.

## Source & Freshness

- **Original analysis date:** 2026-03-20 (type: Decision).
- **Verified against codebase 2026-06-08.**
- **Annotations / updates made:**
  - Added per-recommendation status markers to all 30 items plus the cross-engine signals.
  - Confirmed implemented: job system, collision layer/mask, per-connection scoping, handle-based RHI resources, transient buffer allocator, prefab system, SH-IBL, clustered lighting, tween, datablock registry, material definition, VFS, CVar, PSO dedup, GPU occlusion culling.
  - Confirmed open: per-entity EventBus addressing, Feature Processor, render-thread separation, macro scripting exposure, software VRS, HRTF audio, shader mixins (partial).
  - Fixed stale claims: physics backend is **Jolt** (the "Replace Bullet with Jolt" skip-list item is obsolete — migration is done); a **VisualScript game module now exists**, softening the "skip visual scripting" note.
  - Stripped AI-session frontmatter and the recommended session-sequence ordering (superseded by status markers).

## Related Pages

- [Five Engine Analysis](Five-Engine-Analysis.md)
- [ThorVG + Unity Graphics Analysis](ThorVG-Unity-Graphics-Analysis.md)
- [Advanced Techniques Catalog](Advanced-Techniques-Catalog.md)
