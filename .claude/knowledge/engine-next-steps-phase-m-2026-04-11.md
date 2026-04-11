# Engine next-steps — Phase M (2026-04-11)

**Status:** Active. Phase M activates two more Tier 2 graphics orphans
as a cohesive lighting-adjacent pair — `ReflectionProbeCache` and
`CachedShadowAtlas`. Both are now per-instance members of
`LightingSystem`, lifecycle-wired through `Initialize` / `Shutdown`
and ticked every frame from `Update()` on both the Windows and Linux
implementation paths.

## Context

Phases I → L wired eight Tier 2 graphics orphans (`GTAOEffect`,
`SSAOTemporal`, `RenderTargetPool`, `GPUDebugMarkers`,
`GPUTimestampQuery`, `VolumeManager`, `MeshOptimizer`,
`BVHAccelerator`). All of them lived on `PostProcessingPipeline`,
`LODManager`, or `SceneRenderer`. Phase M moves to the lighting
surface, where two orphans from the audit
(`stub-and-abandoned-features-2026-04-10.md`) had been documented
as "intentional reusable utility" but never instantiated by any
lighting subsystem:

- `Graphics/ReflectionProbeCache.h` (320 lines) — prefiltered
  environment-map cache with LRU eviction, static vs. dynamic probe
  scheduling, parallax-correction support, and a render budget.
- `Graphics/CachedShadowAtlas.h` (331 lines) — shadow atlas with
  per-light caching. Static lights cache their shadow page; dynamic
  or invalidated lights render every frame. Two sub-atlases
  (dynamic + cached), FNV-1a state hash for change detection,
  per-frame render list.

Both headers are **pure CPU** — no `SPARK_PLATFORM_WINDOWS` guard.
`CachedShadowAtlas` depends on `ShadowAtlas.h`, which is also
portable. This lets the lifecycle wiring land on both the Windows
and Linux `LightingSystem` implementation paths and lets tests run
on every platform's CI.

## Items closed

### M1 — `ReflectionProbeCache` activated in `LightingSystem`

**Problem:** `ReflectionProbeCache` had no engine caller. The class
shipped a complete scheduling API (`RegisterProbe`, `Update`,
`InvalidateProbe`, `GetCacheSlot`, LRU eviction) but no render
system instantiated one.

**Fix:**

- `SparkEngine/Source/Graphics/LightingSystem.h`
  - `#include "ReflectionProbeCache.h"` outside the Windows guard.
  - New private member `Spark::Graphics::ReflectionProbeCache m_probeCache;`
    alongside the existing lighting state.
  - New public accessor pair
    `GetReflectionProbeCache()` const/non-const. Tests and future
    render code register probes, pull the per-frame render list, and
    query cache slots through this accessor — no globals.

- `SparkEngine/Source/Graphics/LightingSystem.cpp`
  - **Windows Initialize:** calls
    `m_probeCache.Initialize(/*maxCachedProbes*/ 64, /*renderBudget*/ 4)`
    after the existing `CreateDefaultEnvironment()` call.
  - **Windows Shutdown:** calls `m_probeCache.Shutdown()` alongside
    the other teardown steps.
  - **Windows Update:** extracts the camera world position from
    `XMMatrixInverse(viewMatrix).r[3]` and calls
    `m_probeCache.Update(camX, camY, camZ)` before the existing
    light-culling loop. The call produces the per-frame list of
    probes to render; downstream code can query it via
    `GetReflectionProbeCache().Update(...)` for its own camera feed.
  - **Linux Initialize / Shutdown / Update:** mirror the Windows
    hooks so the Linux stub path ticks the cache identically.
    Tests exercising the real `LightingSystem` on Linux see the
    same state the Windows path produces.

### M2 — `CachedShadowAtlas` activated in `LightingSystem`

**Problem:** `CachedShadowAtlas` had no engine caller either. The
class owned two sub-atlases (dynamic + cached), a FNV-1a state hash
for change detection, and a per-frame render list — but nothing
called `BeginFrame` / `RequestShadow` / `EndFrame` on it.

**Fix:**

- `SparkEngine/Source/Graphics/LightingSystem.h`
  - `#include "CachedShadowAtlas.h"` outside the Windows guard.
  - New private member `Spark::Graphics::CachedShadowAtlas m_shadowCache;`
    alongside the existing `m_shadowMaps` per-light cache (the two
    do not overlap; the existing cache is a per-light `ShadowMap`
    resource pool, while `m_shadowCache` is the atlas-based
    cache-or-render decision layer).
  - New public accessor pair `GetCachedShadowAtlas()` const/non-const.

- `SparkEngine/Source/Graphics/LightingSystem.cpp`
  - **Windows Initialize:** calls
    `m_shadowCache.Initialize(/*dynamic*/ 2048, /*cached*/ 4096, /*minTile*/ 256)`.
    On failure, logs a warning but continues — the existing
    `ShadowMap` path still works.
  - **Windows Shutdown:** calls `m_shadowCache.Shutdown()`.
  - **Windows Update:** calls `m_shadowCache.BeginFrame()` at the
    top of the method (before any metrics or light-culling work)
    and `m_shadowCache.EndFrame()` at the bottom (after the culling
    metrics are written). Downstream callers can issue
    `RequestShadow()` during the frame and iterate
    `GetShadowsToRender()` to drive their own shadow render pass.
  - **Linux Initialize / Shutdown / Update:** same hooks on the
    stub path.

### Test coverage

**M1-new: `Tests/TestReflectionProbeCache.cpp`** (~260 lines, 16 tests)

Exercises the real `Spark::Graphics::ReflectionProbeCache` directly.
Every test runs on every platform's CI:

- **Lifecycle:** `InitializeAndShutdown`, `DefaultInitializeValues`.
- **Register/Unregister:** `RegisterProbeAppearsInCount`,
  `UnregisterProbeRemovesIt`, `UnregisterUnknownIsSafe`.
- **Update budget + scheduling:**
  `UpdateRendersAllWhenBudgetAllows` (big budget → everything renders),
  `UpdateRespectsBudget` (budget=2 → only 2 render),
  `UpdateStaticProbeRendersOnceThenCached` (static = render-once),
  `DynamicProbeReRendersEveryFrame` (dynamic invalidates each frame).
- **Cache slot queries:** `GetCacheSlotAfterRender`,
  `GetCacheSlotUnregisteredIsInvalid`, `GetCachedCountTracksRenders`.
- **Invalidation:** `InvalidateProbeMarksNeedsRender`,
  `InvalidateAllForcesReRender`.
- **Eviction:** `LRUEvictionWhenSlotsExhausted` (cache with only
  2 slots + 3 static probes → at most 2 cached).
- **Metrics:** `Console_GetStatusIncludesProbeCount`.

**M2-new: `Tests/TestCachedShadowAtlas.cpp`** (~220 lines, 13 tests)

Exercises the real `Spark::Graphics::CachedShadowAtlas` directly:

- **Lifecycle:** `InitializeAndShutdown`, `DefaultInitializeValues`,
  `RequestShadowBeforeInitializeIsRejected`.
- **Request routing:** `FirstRequestSchedulesRender`,
  `MultipleLightsAllRenderOnFirstFrame`.
- **Cache hit path:** `StaticLightCachesAfterFirstRender` —
  `MarkRendered` on frame 1, identical request on frame 2 produces
  zero renders and increments `GetCachedRendersAvoided()`.
- **Cache miss path:** `CacheMissWhenLightMoves` (state hash
  changes → re-render), `ForceUpdateBypassesCache` (`forceUpdate`
  flag short-circuits the cache), `DynamicLightAlwaysRendersOnStateChange`.
- **Invalidation:** `InvalidateShadowForcesReRender`,
  `InvalidateAllMarksEveryLight`.
- **Metrics / routing:** `Console_GetStatusFormatting`,
  `GetTileRoutingByStaticFlag` (static → cached atlas, dynamic →
  dynamic atlas, both reachable via `GetTile()`).

No separate LightingSystem integration test was added — the direct
orphan tests cover the full public API, and the `LightingSystem`
wiring itself is the production caller that CI exercises at runtime
when the lighting system ticks each frame.

### Registration

`Tests/CMakeLists.txt` — `TestReflectionProbeCache.cpp` and
`TestCachedShadowAtlas.cpp` registered alongside the Phase L test
files.

## Files touched

```
SparkEngine/Source/Graphics/LightingSystem.h                (M1, M2 — includes + members + accessors)
SparkEngine/Source/Graphics/LightingSystem.cpp              (M1, M2 — Windows + Linux lifecycle hooks)
Tests/TestReflectionProbeCache.cpp                          (M1, new — 16 portable tests)
Tests/TestCachedShadowAtlas.cpp                             (M2, new — 13 portable tests)
Tests/CMakeLists.txt                                        (M1, M2)
.claude/knowledge/engine-next-steps-phase-m-2026-04-11.md   (new)
.claude/index.md                                            (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings unchanged).
- `SparkTests` binary — **4461 passed, 0 failed, 1 warned**
  (pre-existing `LoadTest_Severe_EntityFlood` known-flaky), total
  119289 assertions (+61 from Phase L's baseline of 119228; the
  +29-test delta is 16 `ReflectionProbeCache_*` + 13
  `CachedShadowAtlas_*`).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass (the new `LightingSystem.h`
  accessor block needed one reflow against the 120-col limit).

## What's left for Phase N

Phase M closes two more Tier 2 graphics orphans, bringing the
running total across I→M to **ten orphans activated**. Remaining
candidates, in roughly ascending order of scope:

- **`ConstantBufferRing.h`** (365 lines, Windows-only) — add as
  per-pipeline member with `BeginFrame` / `EndFrame` lifecycle.
  Zero behavior change, live surface for future sub-allocation
  adopters.
- **`RTHandleSystem.h`** (265 lines, Windows-only) — HDRP-style
  render-target handle abstraction. Instantiate in `GraphicsEngine`
  and use for the main scene colour / depth targets as an
  alternative to `RenderTargetPool`.
- **`ShaderVariantSystem.h`** (391 lines) — keyword-based shader
  permutation management. Would replace ad-hoc variant bookkeeping
  in existing shader families.
- **`PersistentMaterialCB.h`** (220 lines) — persistent constant
  buffer with dirty tracking. One per material family.
- **`DenoiserInterface.h`** (251 lines) — abstract denoiser plugin
  interface. Needs a concrete denoiser implementation to plug in.
- **`UICompositor.h`** (231 lines) — may be superseded by
  `Engine/UI`. Needs a wire-or-delete decision.
- **`FastNoise2SIMD.h`** (749 lines) — SIMD procedural noise.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope.

**Tightest fit for Phase N:** `ConstantBufferRing` + `RTHandleSystem`
as a "render-target + constant-buffer plumbing" pair. Both are
Windows-only, both fit cleanly alongside the existing
`RenderTargetPool` that Phase J already wired into
`PostProcessingPipeline`. They share the same integration surface
and can land in one commit.

## Design notes for follow-ups

- **Portable orphan classes travel across platform-gated consumers.**
  Both `ReflectionProbeCache` and `CachedShadowAtlas` are pure CPU,
  so their members live outside the `SPARK_PLATFORM_WINDOWS` guard
  on `LightingSystem`. This means the stub Linux implementation of
  `LightingSystem` can still exercise them identically, and portable
  tests see consistent state on every platform. When a future
  orphan is Windows-only (uses D3D11 types directly), its member
  declaration must be inside the guard — the same pattern Phase J
  used for `RenderTargetPool` / `GPUTimestampQuery` / `GPUDebugMarkers`.
- **"Caller is the production code" is a valid activation for
  subsystem-owned caches.** Phase M's `CachedShadowAtlas` has no
  direct test against `LightingSystem::GetCachedShadowAtlas()`.
  Instead, the direct orphan tests cover the full public API, and
  the `LightingSystem::Update()` tick at runtime is the production
  caller — CI exercises it every time any test or game tick causes
  lighting to run. This avoids duplicating coverage between orphan
  tests and integration tests; new orphans on similar subsystem
  surfaces should follow the same pattern.
- **Mirror Windows and non-Windows implementation paths for
  portable orphans.** Phase M's `LightingSystem.cpp` has two
  implementations for `Initialize` / `Shutdown` / `Update`: the
  full Windows path (lines 166–289) and the Linux stub (lines
  970–1050). Both branches now call the exact same orphan
  lifecycle hooks. Any future orphan that lands on a platform-
  gated subsystem should wire both branches identically so
  headless and GPU builds produce consistent state.
- **`LightingSystem` lives in the global namespace.** Unlike every
  other class in the Graphics folder, `LightingSystem` is not in
  `Spark::Graphics`. Any new members of types from that namespace
  must be qualified `Spark::Graphics::TypeName` in both the header
  declaration and the accessor signatures. This caught a build
  error during Phase M — fixed by adding the namespace prefix
  explicitly on the four affected lines.
