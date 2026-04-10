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

### Engine subsystems (verified orphaned — no refs outside their own headers)

| File | Lines | Purpose |
|---|---|---|
| `SparkEngine/Source/Engine/DataTable/DataTableSystem.h` | ~? | `DataTableManager` singleton, CSV/JSON row tables. Never instantiated. |
| `SparkEngine/Source/Engine/AI/NavMeshLink.h` | ~100+ | `NavMeshLinkSystem` singleton for off-mesh connections. Never called. |
| `SparkEngine/Source/Engine/Gameplay/GameplayTags.h` | ~? | `GameplayTagManager` singleton (Unreal-style tag system). Never referenced. |
| `SparkEngine/Source/Engine/Gameplay/GameplaySystemExtension.h` | ~? | Extension framework with `GetInstance()`. Never referenced. |
| `SparkEngine/Source/Engine/ECS/RuntimePrefab.h` | ~? | Runtime prefab instantiation with serialization. Only referenced in its own test file `Tests/TestRuntimePrefab.cpp`. |
| `SparkEngine/Source/Engine/SaveSystem/SaveSystemTypes.h` | ~? | Type file containing a `GetInstance()` declaration. Never used. |

### Graphics subsystems (reported by audit, require re-verification before action)

The Graphics audit listed ~30 header-only files with zero engine-code references.
Spot-checks against `GameplayLifecycleShared.cpp` confirmed none of the sampled
items are wired into the engine lifecycle. Several have test files but no
engine-side integration. **Before deleting, re-verify each file individually
with grep across all `.cpp` directories including `Tests/`, `SparkEditor/`, and
`GameModules/`.**

High-value orphans (substantial implementations, would be useful if wired):

| File | Lines | What it would provide |
|---|---|---|
| `SparkEngine/Source/Graphics/BVHAccelerator.h` | 441 | SAH-based hierarchical frustum culling |
| `SparkEngine/Source/Graphics/VolumeSystem.h` | 461 | Unity-style post-process volume blending |
| `SparkEngine/Source/Graphics/VoxelConeTracing.h` | 463 | Voxel cone traced global illumination |
| `SparkEngine/Source/Graphics/GTAOEffect.h` | 444 | Ground-truth AO implementation |
| `SparkEngine/Source/Graphics/SSAOTemporal.h` | 231 | Temporal SSAO variant |
| `SparkEngine/Source/Graphics/MeshOptimizer.h` | 471 | Mesh/index buffer optimization |
| `SparkEngine/Source/Graphics/RenderTargetPool.h` | 398 | RT pooling with acquire/release |
| `SparkEngine/Source/Graphics/PipelineStateCache.h` | 398 | PSO caching. **Note:** a file at `Graphics/RHI/PipelineStateCache.cpp` exists — verify whether it implements this header or a different class before action. |
| `SparkEngine/Source/Graphics/ReflectionProbeCache.h` | 317 | Reflection probe cache |
| `SparkEngine/Source/Graphics/CachedShadowAtlas.h` | 328 | Shadow atlas with caching |
| `SparkEngine/Source/Graphics/RTHandleSystem.h` | 265 | Render target handle abstraction |
| `SparkEngine/Source/Graphics/ShaderVariantSystem.h` | 391 | Shader variant permutation management |
| `SparkEngine/Source/Graphics/ShaderCrossCompiler.h` | 376 | HLSL↔GLSL translation (duplicates SlangShaderInterface?) |
| `SparkEngine/Source/Graphics/ConstantBufferRing.h` | 362 | Ring buffer allocator |
| `SparkEngine/Source/Graphics/GPUDebugMarkers.h` | 377 | GPU timing/debug markers |
| `SparkEngine/Source/Graphics/GPUTimestampQuery.h` | 490 | GPU timestamp queries |
| `SparkEngine/Source/Graphics/PersistentMaterialCB.h` | 220 | Persistent material constant buffer |
| `SparkEngine/Source/Graphics/UICompositor.h` | 231 | UI compositor (possibly superseded by Engine/UI) |
| `SparkEngine/Source/Graphics/DenoiserInterface.h` | 251 | Abstract denoiser plugin interface |
| `SparkEngine/Source/Graphics/FastNoise2SIMD.h` | 749 | SIMD procedural noise (appears to be a ported third-party header) |

Smaller orphans (consider deletion if no owner):

| File | Lines |
|---|---|
| `SparkEngine/Source/Graphics/LineTrailRenderer.h` | 127 |
| `SparkEngine/Source/Graphics/SpringArm.h` | 69 |
| `SparkEngine/Source/Graphics/DirtyRectTracker.h` | 167 |
| `SparkEngine/Source/Graphics/ClusteredLightGPU.h` | 163 |
| `SparkEngine/Source/Graphics/ConstantBufferDiff.h` | 138 |
| `SparkEngine/Source/Graphics/RHI/DeferredDeletionQueue.h` | 121 |
| `SparkEngine/Source/Graphics/RHI/RHIHandlePool.h` | 233 |
| `SparkEngine/Source/Graphics/RHI/TransientBufferAllocator.h` | 232 |

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
| `TutorialSystem` | `SparkEditor/Source/Core/TutorialSystem.h` (header-only, 240+ lines) | `Tests/TestTutorialSystem.cpp` | **None** — not referenced in any editor `.cpp` outside its test |
| `AssetDependencyGraph` | Header: `SparkEditor/Source/Panels/AssetDependencyGraph.h` (485 lines). Implementation: `SparkEditor/Source/AssetPipeline/AssetProcessors.cpp:739+` | `Tests/TestAssetDependencyGraph.cpp` | **Partial** — `.cpp` exists but split into an unrelated file; verify whether `AssetDatabase` actually calls it |
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
| `SparkEditor/Source/Panels/SelectionManager.h` | Centralized selection manager designed to deduplicate per-panel selection state. Never instantiated, never registered. Selection logic is currently duplicated across Hierarchy/Inspector/SceneView panels. |
| `SparkEditor/Source/Panels/CSGEditorPanel.h` | Header-only panel with rendering methods containing `// ImGui::Combo would go here in real editor` and `// ImGui::DragFloat3` placeholder comments. Registered in `EditorPanelFactory.cpp:151` but visually empty when opened. |
| `SparkEditor/Source/Panels/NetworkDebugPanel.h` | Header-only panel declaring `NetworkSnapshot`, `PacketLogEntry`, `ReplicationInfo` structs and a panel class with no visible implementation. Registered in `EditorPanelFactory.cpp:152`. |

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
