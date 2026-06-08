# Stub & Abandoned Features

> **Audience:** Programmers
>
> **Thread Context:** Wiring decisions concern lifecycle hooks called from `GameplayLifecycleShared.cpp` (engine) and `EditorUI`/`EditorApplication` (editor), which run on the main thread.
>
> **Platform/Backend Scope:** Engine-wide, plus editor (`SparkEditor/`) and game modules. Some stubs are gated behind optional SDKs (OpenXR for VR, Steamworks/EOS for online transports, DirectStorage SDK for fast I/O).

## Overview

This is a catalog of features that were "started but abandoned," stubbed (class exists, body is a no-op), or implemented-but-never-wired. It exists so follow-up sessions can systematically resolve each item: **wire it in**, **delete it**, or **document it as an intentional utility**.

The catalog was first compiled on 2026-04-10 from three parallel code audits (Graphics, Engine, Editor/GameModules). Those audits produced several false positives — systems claimed "never referenced" that are actually wired — so every entry here was spot-verified with grep against `GameplayLifecycleShared.cpp` and the test suite. Many items have since been resolved.

> **Verification rule:** Before acting on any entry, re-run `grep` for the class name across the whole repo including `Tests/`, `GameModules/`, and `SparkEditor/`. Audit agents historically under-reported references.

## Tier 1 — Stub implementations (called, but a no-op or returns false)

These imply a capability the engine does not actually have.

| Feature | File | Current Status |
|---|---|---|
| **VRSystem** | `Engine/VR/VRSystem.cpp` | **Still a stub (verified 2026-06-08).** `Initialize()` sets `m_initialized = false` with the comment "Set to true when OpenXR is linked" and logs a warning. `m_runtimeStatus` reads "OpenXR runtime not linked in this build (ENABLE_VR currently provides stub backend)." Registered/instantiated, but tracking and rendering are inert until OpenXR is linked. |
| **SteamTransport** | `Engine/Networking/SteamTransport.h` | **Still a header-only stub (verified 2026-06-08).** A `@warning` block states "Framework stub — no Steamworks SDK linked." `Initialize()` always returns `false`; `Send()` returns `false`; `Receive()` returns `-1`. `NetworkStack::Initialize()` still succeeds without it. |
| **OnlineServices — Steam/Epic/Console** | `Engine/OnlineServices/OnlineServices.h` | `NullOnlinePlatform` works and is wired via `OnlineServiceManager`. `SteamPlatform`, `EpicPlatform`, `ConsolePlatform` remain declared stubs awaiting their SDKs (documented in header comments). |
| **FoliageSystem / FoliageManager** | `Graphics/FoliageSystem.h/.cpp` | **RESOLVED.** Full `.cpp` manager with deterministic scatter, plus a wired render-side consumer: `FoliageRenderer` (+ Windows variant), `FoliageImpostorBaker` (+ Windows variant), and `FoliageVS/PS.hlsl`. `FoliageRenderer::GetInstance().Initialize(...)` is called in `GameplayLifecycleShared.cpp` (~line 490), `CollectFromFoliageManager(dt)` per frame (~line 934), and `Shutdown()` (~line 1174). The previously-deferred AssetPipeline binding is now done via `InstallAssetPipelineLoader(pipeline)` (~line 493). |

**Recommended action:** For VR and Steam/Epic/Console — keep documented as stubs awaiting optional SDKs; do not claim them as shipping features. For `SteamTransport` — delete if no plan, or wire it properly behind the Steamworks SDK.

## Tier 2 — Header-only systems with zero engine references

Fully implemented-in-header classes that no `.cpp` calls. Candidates for deletion unless an owner claims them.

### Engine subsystems (resolved)

Verification found 3 of 6 were false positives (wrong class names in the audit). The 3 real orphans were wired in.

| File | Lines | Resolution |
|---|---|---|
| `Engine/DataTable/DataTableSystem.h` | 800 | **False positive.** Class is `DataTableRegistry` (not `DataTableManager`) and was already wired (Initialize + Shutdown). |
| `Engine/AI/NavMeshLink.h` | 134 | **Resolved.** `NavMeshLinkSystem::GetInstance().Initialize()/Shutdown()` now called from the lifecycle path. `Tests/TestNavMeshLink.cpp` added (5 tests). |
| `Engine/Gameplay/GameplayTags.h` | 386 | **False positive.** Class is `GameplayTagRegistry` (not `GameplayTagManager`) and was already wired + tested. |
| `Engine/Gameplay/GameplaySystemExtension.h` | 260 | **Resolved.** `GameplayExtensionRegistry::GetInstance()` touched at startup, `Clear()` at shutdown. Fixed an ODR hazard by renaming the extension-local `QuestDefinition`/`QuestContext` to `QuestExtensionInput`/`QuestExtensionState`. `Tests/TestGameplaySystemExtension.cpp` added (6 tests). |
| `Engine/ECS/RuntimePrefab.h` | 478 | **Resolved.** `PrefabRegistry::GetInstance()` touched at startup. Fixed a build hazard by switching forward declarations of `BinaryWriter`/`BinaryReader` to a real `#include "Utils/Serializer.h"`. Existing `Tests/TestRuntimePrefab.cpp` exercises the real class. |
| `Engine/SaveSystem/SaveSystemTypes.h` | 288 | **False positive.** Already `#include`d from `SaveSystem.h`. |

### Graphics subsystems

The Graphics audit listed ~30 header-only files with no engine call sites (~10,000 lines total). Per-file verification found 1 filename-collision false positive among the high-value set and 2 already-wired among the smaller set; the remainder are legitimate reusable utilities and were documented in-header (resolution path "(c) document as intentional utility"). None were deleted — each is substantial working code (200–750 lines) useful when the corresponding render feature is built.

High-value utilities, all **documented as intentional** (selected):

| File | Lines | Note |
|---|---|---|
| `Graphics/BVHAccelerator.h` | 441 | SAH-based hierarchical frustum/ray culling. |
| `Graphics/VolumeSystem.h` | 461 | Post-process volume blending. Tested via `TestGraphicsIntegration.cpp`. |
| `Graphics/VoxelConeTracing.h` | 463 | Voxel-cone-traced GI. |
| `Graphics/GTAOEffect.h` | 444 | Ground-truth AO post-process. |
| `Graphics/SSAOTemporal.h` | 231 | Temporal SSAO with history. |
| `Graphics/MeshOptimizer.h` | 471 | Vertex-cache / overdraw optimizer. Tested. |
| `Graphics/RenderTargetPool.h` | 398 | Pooled RT allocator. |
| `Graphics/PipelineStateCache.h` | 398 | **False positive (filename collision).** Orphan defines `D3D11PipelineStateCache`; `Graphics/RHI/PipelineStateCache.h` defines the wired `PipelineStateCache`. Disambiguated via header note. |
| `Graphics/ReflectionProbeCache.h` | 317 | Prefiltered env-map cache. |
| `Graphics/CachedShadowAtlas.h` | 328 | Per-light shadow atlas. Tested. |
| `Graphics/RTHandleSystem.h` | 265 | RT handle abstraction. |
| `Graphics/ShaderVariantSystem.h` | 391 | Keyword-based shader permutations. Tested. |
| `Graphics/ShaderCrossCompiler.h` | 376 | HLSL↔GLSL translation (may be superseded by Slang interface). |
| `Graphics/ConstantBufferRing.h` | 362 | Per-frame constant-buffer ring allocator. |
| `Graphics/GPUDebugMarkers.h` | 377 | Scoped PIX/RenderDoc/NSight markers. |
| `Graphics/GPUTimestampQuery.h` | 490 | GPU timestamp query pool. |
| `Graphics/PersistentMaterialCB.h` | 220 | Persistent constant buffer w/ dirty tracking. |
| `Graphics/UICompositor.h` | 231 | May be superseded by `Engine/UI`. |
| `Graphics/DenoiserInterface.h` | 251 | Abstract denoiser plugin interface. |
| `Graphics/FastNoise2SIMD.h` | 749 | SIMD procedural noise (FastNoise2 port). |

Smaller utilities — 2 of 8 were false positives (already wired); the rest documented as intentional:

| File | Lines | Note |
|---|---|---|
| `Graphics/LineTrailRenderer.h` | 127 | Data-carrier structs for line/trail components. |
| `Graphics/SpringArm.h` | 69 | Pure-CPU `SpringArmState`. `Tests/TestSpringArm.cpp` added (6 tests). |
| `Graphics/DirtyRectTracker.h` | 167 | CPU rectangle-merge for partial texture updates. Tested. |
| `Graphics/ClusteredLightGPU.h` | 163 | Bridge to structured-buffer arrays for forward+. Tested. |
| `Graphics/ConstantBufferDiff.h` | 138 | **False positive.** `ConstantBufferDiffManager` wired in the per-frame loop. |
| `Graphics/RHI/DeferredDeletionQueue.h` | 121 | **False positive.** Held as a member of `D3D11Device`. |
| `Graphics/RHI/RHIHandlePool.h` | 233 | Generic `HandlePool<T,N>` template. Tested. |
| `Graphics/RHI/TransientBufferAllocator.h` | 232 | Per-frame bump allocator. Tested. |

## Tier 3 — Systems with tests but no engine/editor integration

| Feature | File(s) | Current Status |
|---|---|---|
| `TutorialSystem` | `SparkEditor/Source/Core/TutorialSystem.h` | **Resolved.** `EditorUI::InitializeManagers` calls `Initialize()`, `EditorUI::Update` ticks it under a guarded update, `Shutdown()` on teardown. 7 new tests added. |
| `AssetAuditGraph` (was `AssetDependencyGraph`) | `SparkEditor/Source/Panels/AssetAuditGraph.h` | **Resolved.** Renamed from `AssetDependencyGraph` to break an ODR collision with the build-pipeline graph of the same name; wired into `EditorUI::InitializeManagers/Shutdown`; 11 new tests added. The build-system `AssetPipeline::AssetDependencyGraph` is untouched. |
| `ClusteredLightGPU` | `Graphics/ClusteredLightGPU.h` | Tested; documented as an intentional Tier-2 utility (no render-path call yet). |
| `ConstantBufferDiff` | `Graphics/ConstantBufferDiff.h` | **Now wired** (see Tier 2 false positive) — `ConstantBufferDiffManager` is in the per-frame loop. |
| `DirtyRectTracker` | `Graphics/DirtyRectTracker.h` | Tested; documented as intentional utility. |
| `AIIntegratedSystem` + `ParallelPerceptionSystem` | `Engine/AI/AIIntegration.h`, `Engine/AI/ParallelPerception.h/.cpp` | **Resolved (2026-04-14).** Added an `AIIntegrationConfig::runCoreAISystem` flag (default false) to avoid double-ticking behavior trees alongside the ECS `AIUpdateSystem`. Wired `Initialize`/`Update`/`Shutdown` into the AI lifecycle. `Tests/TestAIIntegratedSystem.cpp` added (11 tests). `@warning` headers rewritten as `@note WIRED`. |

## Tier 4 — Editor infrastructure with unimplemented bodies

| File | Current Status |
|---|---|
| `SparkEditor/Source/Core/EditorLayoutManager.h` | **Resolved.** Header de-coupled from ImGui (plain floats); real `EditorLayoutManager.cpp` with disk-backed JSON persistence; instantiated in `InitializeManagers()`, torn down in `Shutdown()`. 10 new tests. |
| `SparkEditor/Source/Core/EditorWindowManager.h` | **Resolved.** `SaveCurrentLayoutToFile()`/`LoadLayoutFromFile()` implemented with hand-rolled JSON helpers; `EditorApplication::Initialize/Shutdown` (Windows + SDL2 paths) call Initialize/Shutdown. 6 new tests. |
| `SparkEditor/Source/Panels/SelectionManager.h` | **Partially resolved.** Singleton now initialized/torn down from `EditorUI`. 13 new tests. **Still pending:** migrating `HierarchyPanel`/`InspectorPanel`/`SceneView` off their per-panel selection state — blocked on unifying `SelectionManager::EntityId` (`uint32_t`) with the editor's `ObjectID` (`uint64_t`). |
| `SparkEditor/Source/Panels/CSGEditorPanel.h` | **Resolved.** Testable public API wrapping `Spark::LevelDesign::CSGSystem`; real ImGui rendering behind `#if __has_include(<imgui.h>)`, no-op in headless/test builds. 10 new tests. |
| `SparkEditor/Source/Panels/NetworkDebugPanel.h` | **Resolved (data + UI).** Data model and ImGui rendering implemented. **Still pending:** wiring `NetworkManager` to feed `RecordBytesSent`/`RecordBytesRecv`/`LogPacket`/`SetCurrentLatency` — verified 2026-06-08 that no producer calls these in the networking `.cpp` files yet. |

## Confirmed false positives (do NOT add to any deletion list)

| System | Where verified |
|---|---|
| `OnlineServiceManager` | Wired (Initialize/Update/Shutdown) in `GameplayLifecycleShared.cpp`. |
| `LootTableManager`, `CraftingSystem` | Wired in `GameplayLifecycleShared.cpp`. |
| `AnimNotifyManager` | Wired in `GameplayLifecycleShared.cpp`. |
| `CollaborativeEditSession` | Real TCP + binary wire protocol in `SparkEditor/Source/Communication/`. |
| `VideoPlayer` | Wired (header-only by design). |
| `CoroutineScheduler` | Referenced from lifecycle + diagnostics (header-only by design). |
| `WeatherSystem`, `TemporalEffects` | Many `.cpp` call sites each (header-only by design). |
| `InventorySystem.h`, `QuestSystem.h` (FPS module) | Used in `GameModules/SparkGameFPS/Source/Game/`. |

## How to work through this in a follow-up session

1. One tier at a time — Tier 1 first (false promises), then Tier 4 (editor UX), then Tier 3 (tested-but-unwired), then Tier 2.
2. For each file, `grep -r "ClassName" --include="*.cpp" --include="*.h"` across the whole repo.
3. Choose exactly one per system: **wire it in** (Initialize/Update/Shutdown from `GameplayLifecycleShared.cpp`), **delete it** (file + tests + wiki), or **document it as an intentional utility**.
4. Avoid batch operations — each file needs its own decision.

### Files to read first

- `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp` — canonical wiring location.
- `SparkEditor/Source/Core/EditorPanelFactory.cpp` — where editor panels register.

## Source & Freshness

- Original entry: `.claude/knowledge/stub-and-abandoned-features-2026-04-10.md` (compiled 2026-04-10; AI integration resolution 2026-04-14).
- Verified against codebase 2026-06-08.

Status changes / verifications found during freshening:

- **VRSystem — still a stub:** confirmed `Initialize()` returns `false` with the OpenXR-not-linked warning.
- **SteamTransport — still a stub:** confirmed `@warning` block and false/-1 returns.
- **FoliageSystem — confirmed RESOLVED and further advanced:** `FoliageRenderer`, `FoliageImpostorBaker` (with Windows variants) exist and are wired in `GameplayLifecycleShared.cpp` (Initialize ~490, per-frame collect ~934, shutdown ~1174). The previously-deferred AssetPipeline binding is now done via `InstallAssetPipelineLoader` (~493).
- **NetworkDebugPanel producer wiring — still pending:** verified no `RecordBytesSent`/`RecordBytesRecv`/`SetCurrentLatency` calls exist in networking `.cpp` files.
- **SelectionManager panel migration — still pending** (EntityId/ObjectID width mismatch unresolved).
- All Tier-1 through Tier-4 "Resolved" items confirmed by file presence.

## Related Pages

- [Gameplay & Engine Systems Status](Gameplay-Systems-Status.md)
- [SparkGame Module Status](SparkGame-Module-Status.md)
- [Codebase Health](Codebase-Health.md)
- [Memory Integrity System](Memory-Integrity-System.md)
