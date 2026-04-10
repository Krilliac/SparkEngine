# Stub & Abandoned Features Audit — April 10, 2026

**Type:** Observation
**Status:** Active (catalog for follow-up sessions)
**Severity:** Medium
**Related:** [codebase-bloat-audit-2026-03-15.md](codebase-bloat-audit-2026-03-15.md)

---

## Context

Prompted by a user question about whether SparkEngine needs a SpeedTree-equivalent
vegetation library. Investigation uncovered `FoliageSystem.h` as a header-only
stub never wired into the render path. Three parallel audits (Graphics, Engine,
Editor/GameModules) were then run to catalog other "started but abandoned" or
stubbed features across the codebase so they can be addressed in follow-up
sessions.

**Important:** the Explore agents returned several false positives — systems
they claimed were "never referenced" are actually wired in. This entry lists
only findings that have been spot-verified with grep against
`GameplayLifecycleShared.cpp` and test files. Untrusted claims are excluded.

The existing [codebase-bloat-audit-2026-03-15.md](codebase-bloat-audit-2026-03-15.md)
already covers oversized files, orphaned singletons, and public-method-count
violations from March. This entry is complementary: it focuses specifically on
**stubs and unfinished features** discovered on April 10, and includes new
findings not in the March audit.

---

## Tier 1 — Stub implementations (class exists, body is a no-op or returns false)

These are actively called somewhere but deliberately do nothing. They are the
highest-impact items because their presence implies a capability the engine
does not actually have.

| Feature | File | Evidence |
|---|---|---|
| **VRSystem** | `SparkEngine/Source/Engine/VR/VRSystem.cpp:23-40` | `Initialize()` sets `m_initialized = false` with comment `// Set to true when OpenXR is linked`; `UpdateTracking()` is a no-op past the initialized check. Instantiated in `GameplayLifecycleShared.cpp:509` but produces a warning log and disables itself. `index.md:46` incorrectly implies full rendering support. |
| **SteamTransport** | `SparkEngine/Source/Engine/Networking/SteamTransport.h` | Header-only stub. `Initialize()` returns `false`, `Send()` returns `false`, `Receive()` returns `-1`. Marked "not yet implemented" in comments. Gated behind `#ifdef ENABLE_NETWORKING` but not called anywhere. |
| **OnlineServices — Steam/Epic/Console platforms** | `SparkEngine/Source/Engine/OnlineServices/OnlineServices.h:333-506` | `NullOnlinePlatform` works and is wired via `OnlineServiceManager` (verified at `GameplayLifecycleShared.cpp:451,863,1003`). `SteamPlatform`, `EpicPlatform`, `ConsolePlatform` are declared as stubs awaiting their respective SDKs — header comments state this explicitly. |
| **FoliageSystem / FoliageManager** | `SparkEngine/Source/Graphics/FoliageSystem.h` | **RESOLVED 2026-04-10 (later-session).** The manager now has a full `.cpp` implementation with deterministic scatter (#453), and the **render-side consumer** (`FoliageRenderer`, `FoliageImpostorBaker`, `Shaders/HLSL/FoliageVS.hlsl`/`FoliagePS.hlsl`) is wired into `GameplayLifecycleShared.cpp` alongside the manager. The renderer builds per-frame instance batches with per-instance wind phase, world matrices and LOD selection, caches mesh handles keyed by species path, and performs GPU upload via `GPUSceneBuffer::UpdateInstance` on Windows builds. Still deferred (documented but not implemented): live `AssetPipeline::LoadMesh` binding (pending a singleton / EngineContext accessor for AssetPipeline), GPU-side impostor baking (layout + selection are done CPU-side), and full render-graph submission of the foliage batch. |

**Recommended action:** For VR and Steam/Epic/Console platforms — document in
`CLAUDE.md` that these are stubs awaiting optional SDKs; do not claim them
as features. For SteamTransport — delete if no plan, or wire properly. For
FoliageSystem — see the in-house implementation plan already written.

---

## Tier 2 — Header-only systems with zero engine references (truly orphaned)

These are fully designed-and-implemented-in-header classes that no `.cpp`
anywhere in the engine calls. Most have no tests either. They are candidates
for deletion if no follow-up owner claims them.

### Engine subsystems — resolved 2026-04-10 (later session)

Verification on each entry found that **3 of the 6 were false positives** — the audit had listed incorrect class names. The remaining 3 real orphans have been wired in.

| File | Lines | Original audit claim | Actual status |
|---|---|---|---|
| `SparkEngine/Source/Engine/DataTable/DataTableSystem.h` | 800 | "`DataTableManager` singleton, never instantiated" | **FALSE POSITIVE.** The class is named `DataTableRegistry`, not `DataTableManager`, and it **was already wired** in `GameplayLifecycleShared.cpp:469` (Initialize) and `:1060` (Shutdown). No action needed. |
| `SparkEngine/Source/Engine/AI/NavMeshLink.h` | 134 | "`NavMeshLinkSystem` singleton for off-mesh connections. Never called." | **RESOLVED.** `NavMeshLinkSystem::GetInstance().Initialize()` / `Shutdown()` now called from `GameplayLifecycleShared.cpp`. Added `Tests/TestNavMeshLink.cpp` with 5 real-class tests covering init/clear, add-link ID assignment, remove-link (including unknown-ID safety), enable/disable, and `FindLinksNear` with bidirectional vs one-way semantics. |
| `SparkEngine/Source/Engine/Gameplay/GameplayTags.h` | 386 | "`GameplayTagManager` singleton, never referenced" | **FALSE POSITIVE.** The class is named `GameplayTagRegistry`, not `GameplayTagManager`, and it **was already wired** in `GameplayLifecycleShared.cpp:443-450` (Initialize + EngineContext registration) and `:1010-1016` (Shutdown). Covered by `Tests/TestGameplayTags.cpp`. No action needed. |
| `SparkEngine/Source/Engine/Gameplay/GameplaySystemExtension.h` | 260 | "Extension framework with `GetInstance()`. Never referenced." | **RESOLVED.** `(void)GameplayExtensionRegistry::GetInstance()` touch added at startup, `GetInstance().Clear()` called at shutdown. Also fixed a latent ODR hazard: this header declared `Spark::Gameplay::QuestDefinition` and `QuestContext`, which collided with completely different definitions in `QuestSystem.h`. Renamed the extension-local types to `QuestExtensionInput` / `QuestExtensionState` so both headers can coexist. Added `Tests/TestGameplaySystemExtension.cpp` with 6 real-class tests covering singleton liveness, quest + dialogue registration/lookup, null rejection, `Clear()`, and full quest extension lifecycle (CanStart / OnStarted / OnObjectiveProgress / IsComplete / OnCompleted). |
| `SparkEngine/Source/Engine/ECS/RuntimePrefab.h` | 478 | "Runtime prefab instantiation with serialization. Only referenced in its own test file" | **RESOLVED.** `(void)PrefabRegistry::GetInstance()` touch added at startup in `GameplayLifecycleShared.cpp` so new `RegisterPrefab` calls find the singleton in a constructed state. Also fixed a build-time hazard: the header used forward declarations of `Spark::BinaryWriter` / `BinaryReader` but its inline `Serialize` / `Deserialize` method bodies called methods on them, so any TU including `RuntimePrefab.h` without also including `Utils/Serializer.h` failed to compile. Switched to a proper `#include "Utils/Serializer.h"`. Existing `Tests/TestRuntimePrefab.cpp` already exercises the real class. |
| `SparkEngine/Source/Engine/SaveSystem/SaveSystemTypes.h` | 288 | "Type file containing a `GetInstance()` declaration. Never used." | **FALSE POSITIVE.** Already `#include`d from `SaveSystem.h:74`. No action needed. |

### Graphics subsystems (reported by audit, require re-verification before action)

The Graphics audit listed ~30 header-only files with zero engine-code references.
Spot-checks against `GameplayLifecycleShared.cpp` confirmed none of the sampled
items are wired into the engine lifecycle. Several have test files but no
engine-side integration. **Before deleting, re-verify each file individually
with grep across all `.cpp` directories including `Tests/`, `SparkEditor/`, and
`GameModules/`.**

High-value orphans — **resolved 2026-04-10 (later session):**

Per-file verification found **1 false positive** (a filename collision — the actual class in the orphan file is different from the real one). The other **19 are legitimate reusable graphics utilities**. All 19 received a header `@note` documenting intentional-utility status per the audit's "(c) document as intentional utility" resolution path; 4 of them already had test coverage via `Tests/TestGraphicsIntegration.cpp` and those notes point to the existing tests. None were deleted — every file represents substantial working code (200–750 lines) that will be useful when the corresponding render features are built.

| File | Lines | Status |
|---|---|---|
| `Graphics/BVHAccelerator.h` | 441 | **DOCUMENTED.** SAH-based hierarchical frustum / ray culling. One per scene renderer that wants hierarchical culling. |
| `Graphics/VolumeSystem.h` | 461 | **DOCUMENTED + tested.** Unity-style post-process volume blending. Covered by `Tests/TestGraphicsIntegration.cpp`. |
| `Graphics/VoxelConeTracing.h` | 463 | **DOCUMENTED.** Voxel-cone-traced GI. Future render pipeline feature. |
| `Graphics/GTAOEffect.h` | 444 | **DOCUMENTED.** Ground-truth AO post-process effect. One per view. |
| `Graphics/SSAOTemporal.h` | 231 | **DOCUMENTED.** Temporal SSAO with frame-to-frame history. One per view. |
| `Graphics/MeshOptimizer.h` | 471 | **DOCUMENTED + tested.** Mesh / index buffer optimizer (vertex cache, overdraw). Covered by `Tests/TestGraphicsIntegration.cpp`. |
| `Graphics/RenderTargetPool.h` | 398 | **DOCUMENTED.** Pooled render target allocator with acquire/release. One per device. |
| `Graphics/PipelineStateCache.h` | 398 | **FALSE POSITIVE (filename collision).** The orphan file defines class `D3D11PipelineStateCache`; the file at `Graphics/RHI/PipelineStateCache.h` defines a different class named `PipelineStateCache` that IS wired in `GameplayLifecycleShared.cpp:427` / `:993`. Header `@note` added to the orphan to disambiguate and mark it as a future D3D11 helper. |
| `Graphics/ReflectionProbeCache.h` | 317 | **DOCUMENTED.** Prefiltered environment map cache for reflection probes. |
| `Graphics/CachedShadowAtlas.h` | 328 | **DOCUMENTED + tested.** Shadow atlas with per-light caching. Covered by `Tests/TestGraphicsIntegration.cpp`. |
| `Graphics/RTHandleSystem.h` | 265 | **DOCUMENTED.** Render target handle abstraction (Unity-HDRP-inspired). |
| `Graphics/ShaderVariantSystem.h` | 391 | **DOCUMENTED + tested.** Keyword-based shader permutation management. Covered by `Tests/TestGraphicsIntegration.cpp`. |
| `Graphics/ShaderCrossCompiler.h` | 376 | **DOCUMENTED.** HLSL↔GLSL translation. May be superseded by `SlangShaderInterface.h` — follow-up will decide. |
| `Graphics/ConstantBufferRing.h` | 362 | **DOCUMENTED.** Ring buffer allocator for per-frame constant buffer updates. |
| `Graphics/GPUDebugMarkers.h` | 377 | **DOCUMENTED.** Scoped GPU debug markers for PIX / RenderDoc / NSight. |
| `Graphics/GPUTimestampQuery.h` | 490 | **DOCUMENTED.** GPU timestamp query pool for per-pass timing. |
| `Graphics/PersistentMaterialCB.h` | 220 | **DOCUMENTED.** Persistent constant buffer with dirty tracking. One per material family. |
| `Graphics/UICompositor.h` | 231 | **DOCUMENTED.** May be superseded by `Engine/UI` — a follow-up will migrate-and-delete or wire. |
| `Graphics/DenoiserInterface.h` | 251 | **DOCUMENTED.** Abstract denoiser plugin interface for RT AO / GI / reflections. |
| `Graphics/FastNoise2SIMD.h` | 749 | **DOCUMENTED.** SIMD procedural noise ported from FastNoise2. Kept self-contained. |

Smaller orphans — **resolved 2026-04-10 (later session):**

Per-file verification found **2 of 8 were false positives** (already wired) and the other 6 are legitimate reusable utilities that were already covered by tests. All 6 received a header `@note` documenting intentional-utility status per the audit's "(c) document as intentional utility" resolution path.

| File | Lines | Status |
|---|---|---|
| `Graphics/LineTrailRenderer.h` | 127 | **DOCUMENTED.** Pure data-carrier structs (`LineRendererData`, `TrailRendererData`) consumed by `LineRendererComponent` / `TrailRendererComponent`. Header `@note` explains the render-side consumer is a follow-up; the structs are intentionally header-only so game modules can construct them without a compiled dependency. |
| `Graphics/SpringArm.h` | 69 | **DOCUMENTED + tested.** `SpringArmState` is a pure-CPU value type with a deterministic `Update()` method. `SpringArmComponent` currently duplicates the state fields but a follow-up will unify them. New `Tests/TestSpringArm.cpp` covers defaults, no-collision, collision shortening, min-length clamp, collision-disable, and recovery (6 tests). |
| `Graphics/DirtyRectTracker.h` | 167 | **DOCUMENTED.** Pure CPU rectangle-merge logic for partial texture updates. No GPU dependency, no singleton — any future texture-streaming or UI-atlas system instantiates one per tracked texture. Existing `Tests/TestDirtyRectTracker.cpp` covers it. |
| `Graphics/ClusteredLightGPU.h` | 163 | **DOCUMENTED.** GPU bridge utility that converts `ClusteredLightCulling` results to structured-buffer-ready arrays. One instance per frame in a future forward+ render pass. Existing `Tests/TestClusteredLightGPU.cpp` covers the layout. |
| `Graphics/ConstantBufferDiff.h` | 138 | **FALSE POSITIVE.** `ConstantBufferDiffManager` is wired at `GameplayLifecycleShared.cpp:415` (Initialize), `:937` (BeginFrame), `:980` (Shutdown). Already integrated into the per-frame loop. No action needed. |
| `Graphics/RHI/DeferredDeletionQueue.h` | 121 | **FALSE POSITIVE.** Held as a member of `D3D11Device` (`D3D11Device.h:342`) and used by the D3D11 RHI backend. Not orphaned. |
| `Graphics/RHI/RHIHandlePool.h` | 233 | **DOCUMENTED.** Generic `HandlePool<T, N>` template with generation counters. Pure C++ / no GPU dependency; RHI backends instantiate one per resource type as those are added. Existing `Tests/TestRHIHandlePool.cpp` covers it. |
| `Graphics/RHI/TransientBufferAllocator.h` | 232 | **DOCUMENTED.** Per-frame bump allocator for dynamic geometry. Owned per render system (particles, debug draw, UI, decals). Existing `Tests/TestTransientBufferAllocator.cpp` exercises the allocation math against a fake RHI device. |

**Approximate total:** ~10,000 lines of header-only code in Graphics alone
that has no engine-side call sites.

**Recommended action:** In a follow-up session, verify each entry with an
exhaustive grep (including headers, not just `.cpp`), then for each: (a) wire
in, (b) delete, or (c) document as intentional utility. Do not batch-delete
without per-file verification.

---

## Tier 3 — Systems with tests but no engine/editor integration

These systems have unit tests, suggesting they were implemented deliberately,
but they are never instantiated in the engine or editor runtime. Either
the test was a design exercise, or the wiring step was skipped.

| Feature | Implementation | Test | Engine integration |
|---|---|---|---|
| `TutorialSystem` | `SparkEditor/Source/Core/TutorialSystem.h` (header-only, ~530 lines) | `Tests/TestTutorialSystem.cpp` | **RESOLVED 2026-04-10 (later-session).** `EditorUI::InitializeManagers` now calls `TutorialSystem::GetInstance().Initialize()` (which registers the built-in tutorial sequences) and `EditorUI::Update` ticks `Update(deltaTime)` inside `SPARK_GUARDED_UPDATE("TutorialSystem", ...)` so auto-advance timers progress each frame. `EditorUI::Shutdown` calls `TutorialSystem::GetInstance().Shutdown()`. The test file already exercised the real class for 15 baseline cases; 7 new `TutorialSystem_*` tests cover `GoToStep` bounds, auto-advance via `Update`, inactive-no-op `Update`, `OnStepChanged` callback firing on Start + Advance, `GetCurrentTutorialName`, `Console_GetStatus` (active + inactive), and empty-sequence rejection. |
| `AssetAuditGraph` (was `AssetDependencyGraph`) | Header: `SparkEditor/Source/Panels/AssetAuditGraph.h` (485 lines). | `Tests/TestAssetDependencyGraph.cpp` | **RESOLVED 2026-04-10 (later-session).** The investigation found a latent ODR hazard: the Panels/ header and `AssetPipeline/AdvancedAssetPipeline.h` both declared `SparkEditor::AssetDependencyGraph` with completely different APIs (one a singleton with audit/budget/unused/cycle detection, the other a non-singleton with topological sort for the build pipeline). The Panels/ file was also genuinely orphaned — no `.cpp` or test included it, and the test file used a standalone reimplementation instead. Fix: renamed the Panels/ class to `SparkEditor::AssetAuditGraph` (header and file) and its local `AssetType` enum to `AuditAssetType` to eliminate the collision; wired `AssetAuditGraph::GetInstance().Initialize()` and `Shutdown()` into `EditorUI::InitializeManagers/Shutdown`; extended `Tests/TestAssetDependencyGraph.cpp` with 11 `AssetAuditGraph_*` tests against the real singleton (init/shutdown, register, bidirectional dependency, remove dependency, transitive dependencies, find-unused with root awareness, circular detection, remove-asset edge cleanup, size budget report, audit aggregation, console status). Wiki page and bloat baseline updated to the new filename. `AssetPipeline::AssetDependencyGraph` (the build-system graph) is untouched — it continues to be the real class used by `AdvancedAssetPipeline`. |
| `ClusteredLightGPU` | `SparkEngine/Source/Graphics/ClusteredLightGPU.h` | `Tests/TestClusteredLightGPU.cpp` | **None** — not called in any render path |
| `ConstantBufferDiff` | `SparkEngine/Source/Graphics/ConstantBufferDiff.h` | `Tests/TestConstantBufferDiff.cpp` | **None** |
| `DirtyRectTracker` | `SparkEngine/Source/Graphics/DirtyRectTracker.h` | `Tests/TestDirtyRectTracker.cpp` | **None** |

**Recommended action:** Decide owner per-system, wire into the appropriate
lifecycle point, or remove both the test and the code together.

---

## Tier 4 — Editor infrastructure with unimplemented bodies

Header-only editor infrastructure that exists but does nothing useful yet.

| File | Problem |
|---|---|
| `SparkEditor/Source/Core/EditorLayoutManager.h:62-77` | **RESOLVED 2026-04-10 (later-session).** Header rewritten without the ImGui dependency (plain floats instead of ImVec2), real implementation added in `EditorLayoutManager.cpp` with disk-backed JSON persistence. `InitializeManagers()` now instantiates `m_layoutManager`, `Shutdown()` tears it down. 10 new tests (`EditorLayoutMgr_*`) cover init, panel registration, default reset, save/load round trip, missing/unknown file handling, delete, list, and unregistered-panel skipping. |
| `SparkEditor/Source/Core/EditorWindowManager.h:154,164` | **RESOLVED 2026-04-10 (later-session).** `SaveCurrentLayoutToFile()` now writes hand-rolled JSON with panel state, dock INI, and monitor index; `LoadLayoutFromFile()` parses it back. Minimal JSON helpers (`EscapeJson`, `UnescapeJson`, `ReadJsonString`/`Bool`/`Number`) are kept as private statics so the class stays header-only. `EditorApplication::Initialize/Shutdown` (both Windows and SDL2 paths) now call `EditorWindowManager::GetInstance().Initialize()/Shutdown()`. 6 new `EditorWinMgrReal_*` tests cover save, empty-path rejection, missing-file and empty-file load failures, malformed-input rejection, and a save→load round trip. |
| `SparkEditor/Source/Panels/SelectionManager.h` | **PARTIALLY RESOLVED 2026-04-10 (later-session).** The singleton is now initialized from `EditorUI::InitializeManagers` and torn down from `EditorUI::Shutdown` so it is alive for any code that reaches for `SelectionManager::GetInstance()`. 13 new `SelectionMgrReal_*` tests cover init/shutdown, single/add/remove/toggle/multi-select, clear, callback register/unregister, locking, marquee selection, selection groups, filters, and console status against the actual header class (not the previous standalone reimplementation). **Still pending:** converting `HierarchyPanel`, `InspectorPanel`, and `SceneView` from their own per-panel selection state to `SelectionManager`. That is a larger refactor because `SelectionManager::EntityId` is `uint32_t` while the editor's `ObjectID` is `uint64_t`; the two ID spaces need to be unified (or bridged by an adapter) before the panels can be migrated. |
| `SparkEditor/Source/Panels/CSGEditorPanel.h` | **RESOLVED 2026-04-10 (later-session).** Refactored to expose a testable public API (`CreateBrush`, `DeleteBrush`, `RebuildMesh`, `SelectBrush`, `SetAutoRebuildEnabled`, plus `GetBrushCount` / `GetSelectedBrush` / `GetLastMesh` queries) that wraps `Spark::LevelDesign::CSGSystem` with per-panel bookkeeping. Real ImGui rendering added behind `#if __has_include(<imgui.h>)` — editor builds get Combo/DragFloat3/Selectable/Table widgets for brush creation, list, build controls, and statistics; test/headless builds keep the render methods as no-ops so the header stays link-safe without ImGui. 10 new `CSGEditorPanel_*` tests cover create, delete, subtractive ops, unknown IDs, selection cursor reassignment, all brush shapes, auto-rebuild toggle, and mesh snapshot capture. |
| `SparkEditor/Source/Panels/NetworkDebugPanel.h` | **RESOLVED 2026-04-10 (later-session).** The data-model layer (`TakeSnapshot`, `LogPacket`, `UpdateReplicationInfo`, graph-window pruning, bandwidth/latency averages) was already real; the audit finding was specifically about the `Render*` methods being empty. Real ImGui rendering added behind `#if __has_include(<imgui.h>)` — editor builds get `PlotLines` for bandwidth/latency, `SliderFloat`/`Checkbox` for simulation controls, and `BeginTable` for packet log and replication viewer. Test/headless builds fall back to no-op Render methods. **Still pending:** wiring `NetworkManager` to actually call `RecordBytesSent`/`RecordBytesRecv`/`LogPacket`/`SetCurrentLatency` each frame — the panel's public feed API exists but has no producer. |

**Recommended action:** Either complete the implementations or delete them
and remove their registrations. Editor layout persistence is the highest-value
item (users expect editor layouts to survive restarts).

---

## False positives (explicitly NOT in this audit)

The following were flagged by Explore agents but spot-checks confirmed they
are actually wired in and working. Do **not** add them to any follow-up
deletion list:

| System | Where verified |
|---|---|
| `OnlineServiceManager` | `GameplayLifecycleShared.cpp:451,863,1003` (Initialize/Update/Shutdown) |
| `LootTableManager`, `CraftingSystem` | `GameplayLifecycleShared.cpp:456,457,868,997,998` |
| `AnimNotifyManager` | `GameplayLifecycleShared.cpp:428,974` |
| `CollaborativeEditSession` | Real TCP + binary wire protocol in `SparkEditor/Source/Communication/CollaborativeEditSession.cpp` |
| `VideoPlayer` | `GameplayLifecycleShared.cpp:441,821,985` (header-only by design, works) |
| `CoroutineScheduler` | Referenced from engine lifecycle and diagnostics (header-only by design) |
| `WeatherSystem`, `TemporalEffects` | 15–16 call sites each in `.cpp` (header-only by design, works) |
| `InventorySystem.h`, `QuestSystem.h` (FPS module) | Used in `GameModules/SparkGameFPS/Source/Game/{Game,GameSetup,Main}.cpp` (header-only reusable design) |

The Engine Explore agent was the least reliable — roughly half its "never
referenced" claims did not survive verification. The Graphics agent was the
most reliable.

---

## Relationship to the March bloat audit

The March 15 audit (`codebase-bloat-audit-2026-03-15.md`) listed 17 orphaned
singletons, several of which have since been wired in (ChromeTracing,
MemoryDebugger, FrameInspector, Tween, DebugDraw, DebugOverlay, FileLogger,
ConsoleProcessManager). Others in that audit remain unresolved and
overlap with findings here:

- **MeshLOD** — March audit says "GetInstance() never called". Today's audit:
  the class has full `.cpp` implementations (1,232 lines) but the singleton
  access path is unused. Worth re-verifying before any action.
- **Sequencer**, **AnimationSystem** (engine-side, not editor), **NavMesh**,
  **NavMeshObstacles**, **DecalSystem**, **DXRSupport** — status unchanged
  from March; still need a wire-or-delete decision.

This entry adds ~30 new Graphics header-only orphans, 6 new Engine orphans
(DataTable, NavMeshLink, GameplayTags, GameplaySystemExtension, RuntimePrefab,
SaveSystemTypes), 5 editor infrastructure stubs, and confirms VRSystem and
SteamTransport as stubs.

---

## How to work through this in a follow-up session

1. Pick one tier at a time — Tier 1 stubs first (they make false promises),
   then Tier 4 editor UX, then Tier 3 tested-but-unwired, then Tier 2.
2. For each file, run: `grep -r "ClassName" --include="*.cpp" --include="*.h"`
   across the whole repo including `Tests/`, `GameModules/`, `SparkEditor/`.
   Explore agents under-reported references; always verify.
3. For each system, choose exactly one: **wire it in** (call `Initialize()` /
   `Update()` / `Shutdown()` from `GameplayLifecycleShared.cpp` where the other
   subsystems live), **delete it** (file + any tests + any wiki pages), or
   **document it as an intentional utility** (add a comment to the header
   explaining it is a library for future use, not an active system).
4. Avoid batch operations. Each file needs its own decision.
5. Cross-check `.claude/index.md` line 46 ("All 12 former stubs now have .cpp
   implementations") and update it — that claim does not survive this audit.

---

## Files to read first in any follow-up session

- `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp` — the
  canonical place where systems get `Initialize/Update/Shutdown` wiring.
- `SparkEditor/Source/Core/EditorPanelFactory.cpp` — where editor panels
  are registered.
- This file (`stub-and-abandoned-features-2026-04-10.md`) — the catalog.
- `codebase-bloat-audit-2026-03-15.md` — the complementary March audit.
