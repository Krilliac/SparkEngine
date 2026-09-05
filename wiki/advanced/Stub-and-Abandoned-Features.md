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

### Engine subsystems flagged by the 2026-04 audit

All six entries from the original audit are closed (three were wrong class names,
three were wired in with tests) and are no longer tracked here.

### Lifecycle registrations removed in the 2026-09 release-readiness sweep

These classes still exist but are **no longer initialized, ticked, or shut down by
the gameplay lifecycle**. Each had a lifecycle `Initialize()`/`Shutdown()` pair
and no producer or consumer anywhere in production, so the pretend-wiring was
removed rather than kept. Resolution path for each: wire a real consumer, or
delete. Any wiki page that describes these as active is stale.

| Class | Location | Note |
|---|---|---|
| `OcclusionCullingSystem` | `Graphics/` | No renderer consumes its results. |
| `LightProbeSystem` | `Graphics/` | No probe data reaches a shader. |
| `PipelineStateCache` (RHI) | `Graphics/RHI/PipelineStateCache.h` | `GetOrCreate`/`Invalidate` have no callers; D3D11 uses `D3D11PipelineStateCache`. |
| `TransientResourcePool` | `Graphics/` | No allocation call sites. |
| `VirtualTextureManager` | `Graphics/` | No feedback/page producer. |
| `DynamicQualityScaler` | `Graphics/` | `[DynamicQuality]` settings are read but nothing scales; see [Configuration Reference](Configuration-Reference.md). |
| `GPUStallProfiler` | `Graphics/` | No query submission. |
| `AsyncComputeScheduler` | `Graphics/` | No dispatch submission. |
| `AIDebugRenderer` | `Engine/AI/` | Tested (`Tests/TestAIDebugRendererReal.cpp`) but no draw consumer. |
| `HLODSystem` | `Engine/HLOD/` | No renderer consumer; see [HLOD and World Partition](../gameplay-tools/HLOD-And-World-Partition.md). |
| `Engine/HotReload/ModuleHotReload` singleton | `Engine/HotReload/` | Duplicate of the live `Spark::ModuleHotReloadManager` (`Core/ModuleHotReload.cpp`, polled by the platform frame loops). |
| `Spark::FileLogger` + `SPARK_FILE_LOG*` | `Utils/FileLogger.h` | Zero production writers; its two lifecycle calls were removed in the 2026-09 sweep. Engine log files come from the `Logger` `FileSink` installed by `InstallDefaultSinks`. Pending wire-it-in-or-delete. |
| `RenderingSettings::occlusionCulling` | `Core/EngineSettings.h` | Setting is still exposed although the implementation behind it is unreachable (see `OcclusionCullingSystem` above). |
| `ReplicatedFieldSet::WriteDirtyMask`/`ReadDirtyMask` | `Engine/Networking/` | Not wired into `EntityReplicator`'s serialize/deserialize path (zero callers), so `ReadDirtyMask`'s `[[nodiscard]]` "abandon the packet" contract is unenforced and untested. |
| `MobilePlatform`, `RagdollSystem` | `Engine/Mobile/`, `Engine/Physics/` | Lifecycle comment corrected; deletion routed to their owners. |
| `HybridRTManager` on Windows | `Graphics/` | Only constructed behind an RHI bridge; `GraphicsEngine::GetRHIBridge()`/`GetRHIDevice()` are `nullptr` on Windows (real bridge on Linux), so no hybrid RT runs on the D3D11 path. |

Also in this class: `AudioMixer::AddBusEffect` (per-bus DSP chains) and reverb
zones (`GetReverbAtPosition`) are **stored but not applied** to any voice. The
earlier claim that `AudioMixer::CalculateOcclusion` / `MusicManager::ComputeOcclusion`
could never report occlusion is retired: both trace `WorldStatic` hits once a
`PhysicsSystem` is attached (`AudioMixer::IsOcclusionAvailable()`).

### Deleted rather than completed (2026-09 sweep)

| Deleted | Why |
|---|---|
| `SparkEditor/Source/Gizmos/GizmoSystem.{h,cpp}` | Duplicate of the `SceneViewPanel` gizmo path, which now ships translate/rotate/scale with one `CommandHistory` entry per drag. |
| `SparkEditor/Source/Panels/PostProcessingPanel.{h,cpp}` | `SceneFile`-backed editor for a document model the editor no longer builds; use the Inspector's Post-Process Volume component. |
| `SparkEditor/Source/MaterialEditor/MaterialEditor.{h,cpp}` | Standalone class with no production caller; the shipped panel is `Panels/MaterialEditorPanel`. |
| `SparkEditor/Source/Lighting/LightingTools*.{h,cpp}` | No production caller. `BakeLightmaps` wrote a fixed radial ramp in texture space (nothing scene-derived) and `GenerateLightProbes` discarded its probes -- no lightmap baking or light probes are claimed anywhere now. |
| `SparkEditor/Source/Animation/*` (timeline, clip manager, curve, playback) | No production caller. |
| `SparkEditor/Source/AssetBrowser/AssetDatabase.{h,cpp}` | No production caller. |
| `Tests/TestAssetDatabase.cpp`, `Tests/TestPrefabManager.cpp` | Decoy tests that included no production header; `Tests/TestEditorSubsystemsReal.cpp` covers the real `PrefabManager`. |
| `GameModules/SparkGameFPS/Source/Game/{Console,Terrain,ArenaBuilder}.*` | Unreachable in-game console overlay, unreferenced terrain, and an `ArenaBuilder` whose body was entirely commented out. |
| `Shaders/Compiled/Basic{VS,PS}.cso`, `SparkEngine/Shaders/Compiled/Basic{VS,PS}.cso` | Prebuilt bytecode is no longer shipped; the basic shaders are compiled from source (`GraphicsDeviceResourcesWindowsShaders.cpp` embeds them; `Shaders/HLSL/BasicVS.hlsl` / `BasicPS.hlsl` are the on-disk copies for the Linux/RHI path). |

### Editor panels in an honest Preview state

| Panel | Status |
|---|---|
| `DebugVisualizerPanel` | Toggles are authored but no renderer consumes them and no debug-draw counts are reported; shows **Preview - not connected**. |
| `ObjectPlacementPanel` modes | Quick Place / Place Selected create real undoable entities; brush/line/grid/scatter, align, and snap settings are not read by the viewport. |
| `DedicatedServerPanel` discovery | Lists only the server launched from this editor; no discovery transport, no RCON, no kick/ban. |

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
| `Graphics/PipelineStateCache.h` | 398 | Filename collision: this header defines `D3D11PipelineStateCache`, which the D3D11 path uses. `Graphics/RHI/PipelineStateCache.h` defines a *different*, **not wired** `PipelineStateCache`: `GetOrCreate`/`Invalidate` have no callers, and its lifecycle Initialize/Shutdown lines were removed in the 2026-09 sweep (see below). |
| `Graphics/ReflectionProbeCache.h` | 317 | Prefiltered env-map cache. |
| `Graphics/CachedShadowAtlas.h` | 328 | Per-light shadow atlas. Tested. |
| `Graphics/RTHandleSystem.h` | 265 | RT handle abstraction. |
| `Graphics/ShaderVariantSystem.h` | 391 | Keyword-based shader permutations. Tested. |
| `Graphics/ShaderCrossCompiler.h` | 376 | The DXBC target now compiles for real via `d3dcompiler_47`; DXIL/SPIR-V/GLSL/MSL are explicit not-integrated failures (exit 1), no longer success-with-empty-bytecode. `CrossCompileHLSLtoGLSL` survives only because `Tests/TestGLSLPipelineIntegration.cpp` asserts on it; `CompileShader` no longer calls it. |
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
| `Graphics/RHI/DeferredDeletionQueue.h` | 121 | Header-only with real test coverage (`Tests/TestDeferredDeletionReal.cpp`) but **no production caller**: `D3D11Device` has no such member and no backend calls `Enqueue`/`ProcessQueue` (`D3D12Device` has its own fence-based queue). |
| `Graphics/RHI/RHIHandlePool.h` | 233 | Generic `HandlePool<T,N>` template. Tested. |
| `Graphics/RHI/TransientBufferAllocator.h` | 232 | Per-frame bump allocator. Tested. |

## Tier 3 — Systems with tests but no engine/editor integration

| Feature | File(s) | Current Status |
|---|---|---|
| `ClusteredLightGPU` | `Graphics/ClusteredLightGPU.h` | Tested; documented as an intentional Tier-2 utility (no render-path call yet). |
| `DirtyRectTracker` | `Graphics/DirtyRectTracker.h` | Tested; documented as intentional utility. |
| `DeferredDeletionQueue` | `Graphics/RHI/DeferredDeletionQueue.h` | Tested (`TestDeferredDeletionReal.cpp`); no production caller (see Tier 2). |
| `AdvancedAssetPipeline` | `SparkEditor/Source/AssetPipeline/AdvancedAssetPipeline.h` | 3,835 lines with a real test (`Tests/TestAdvancedAssetPipeline.cpp`) and no production caller; ship-or-delete needs a product decision. |

Resolved and no longer tracked: `TutorialSystem`, `AssetAuditGraph`, `ConstantBufferDiff` (wired), `AIIntegratedSystem` + `ParallelPerceptionSystem`.

## Tier 4 — Editor infrastructure with unimplemented bodies

| File | Current Status |
|---|---|
| `SparkEditor/Source/Core/EditorWindowManager.h` | **Resolved (2026-09).** `SaveCurrentLayoutToFile()`/`LoadLayoutFromFile()` now have real callers: `EditorApplication::Initialize` restores `<EditorData>/window_layout.json` and `Shutdown` writes it (`EditorApplication::WindowLayoutFilePath`). |
| `SparkEditor/Source/Panels/SelectionManager.h` | **Partially resolved.** Singleton now initialized/torn down from `EditorUI`. 13 new tests. **Still pending:** migrating `HierarchyPanel`/`InspectorPanel`/`SceneView` off their per-panel selection state — blocked on unifying `SelectionManager::EntityId` (`uint32_t`) with the editor's `ObjectID` (`uint64_t`). |
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
- Verified against codebase 2026-06-08; lifecycle-removal, deletion, and Preview-panel sections added from the 2026-09-05 release-readiness sweep (static verification against the working tree, no build/test evidence attached).

Status changes / verifications found during freshening:

- **VRSystem — still a stub:** confirmed `Initialize()` returns `false` with the OpenXR-not-linked warning.
- **SteamTransport — still a stub:** confirmed `@warning` block and false/-1 returns.
- **FoliageSystem — confirmed RESOLVED and further advanced:** `FoliageRenderer`, `FoliageImpostorBaker` (with Windows variants) exist and are wired in `GameplayLifecycleShared.cpp` (Initialize ~490, per-frame collect ~934, shutdown ~1174). The previously-deferred AssetPipeline binding is now done via `InstallAssetPipelineLoader` (~493).
- **NetworkDebugPanel producer wiring — still pending:** verified no `RecordBytesSent`/`RecordBytesRecv`/`SetCurrentLatency` calls exist in networking `.cpp` files.
- **SelectionManager panel migration — still pending** (EntityId/ObjectID width mismatch unresolved).
- Resolved items that no longer need tracking were removed from the tables in the 2026-09 sweep; the 2026-06 verification of the remaining rows stands.

## Related Pages

- [Gameplay & Engine Systems Status](Gameplay-Systems-Status.md)
- [SparkGame Module Status](SparkGame-Module-Status.md)
- [Codebase Health](Codebase-Health.md)
- [Memory Integrity System](Memory-Integrity-System.md)
