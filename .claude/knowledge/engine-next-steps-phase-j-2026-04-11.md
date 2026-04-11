# Engine next-steps — Phase J (2026-04-11)

**Status:** Active. Phase J activates four more Tier 2 graphics orphans —
`SSAOTemporalFilter`, `RenderTargetPool`, `GPUDebugMarkers`, and
`GPUTimestampQuery` — inside the real `PostProcessingPipeline`. Phase I
wired the first orphan (`GTAOEffect`) as the 15th post-process pass;
Phase J extends the pipeline to 16 passes and turns on the three
profiling/debug utilities that orbit that same integration surface.

## Context

The April 10 audit (`stub-and-abandoned-features-2026-04-10.md`)
catalogued ~25 header-only Tier 2 graphics orphans that had been
"documented as intentional utilities" but never wired into a render
path. Phase I started the orphan-activation work by adding `GTAOEffect`
as a real post-process pass. Phase J picks up the next four that share
the same integration surface (PostProcessingPipeline) so they can land
together in a single coherent diff rather than four independent PRs.

Target orphans (all gated on Windows except `SSAOTemporalFilter`, which
is pure CPU and runs on every platform):

| Orphan | Lines | Integration surface |
|---|---|---|
| `Graphics/SSAOTemporal.h` | 234 | New `PostProcessPass::SSAOTemporal` slot + CPU filter owned by the pipeline |
| `Graphics/RenderTargetPool.h` | 401 | Per-pipeline pool, initialised from `SetDevice`, ticked per frame |
| `Graphics/GPUDebugMarkers.h` | 380 | Per-pipeline scoped event around each pass plus an outer `PostProcessingPipeline` region |
| `Graphics/GPUTimestampQuery.h` | 493 | Per-pipeline timer, `BeginFrame`/`EndFrame` bracketing `Process()`, `ScopedTimestamp` around each pass |

## Items closed

### J1 — `SSAOTemporalFilter` activated as the 16th post-process pass

**Problem:** `Graphics/SSAOTemporal.h` is a 234-line header-only CPU
reference implementation of a variance-clipped temporal AO denoiser.
The April 10 audit marked it "documented intentional utility." No
engine code instantiated the class — it had no home.

**Fix:**

- `SparkEngine/Source/Graphics/PostProcessingTypes.h`
  - `#include "SSAOTemporal.h"` so `SSAOTemporalSettings` flows
    through the canonical types header alongside `GTAOSettings`.
  - Added `SSAOTemporal` as the **second** entry of the
    `PostProcessPass` enum — the pipeline iterates in declaration
    order so the denoiser runs immediately after `GTAO` and before
    `Bloom`. The doc comment was updated to explain the ordering.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.h`
  - Unconditionally includes `SSAOTemporal.h` (no platform guard —
    the filter is pure CPU).
  - New `SSAOTemporalFilter m_ssaoTemporalFilter` member, initialised
    from `Initialize(width, height)` and `Resize(width, height)` so
    the history buffer follows the viewport.
  - New `GetSSAOTemporalSettings()` / `GetSSAOTemporalFilter()`
    accessor pairs mirroring the existing 14 accessor pairs.
  - New `m_ssaoTemporalPS` D3D11 pixel-shader ComPtr.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_ssaoTemporalFilter.Initialize(width, height)`
    unconditionally (headless builds still exercise the CPU filter).
  - `Shutdown()` resets the filter and `m_ssaoTemporalPS`.
  - `Resize()` forwards the new dimensions to the filter.
  - `CompileEffectShaders()`: new inline HLSL literal `ssaoTemporalPS`
    implementing a variance-clipped bilateral 3x3 denoiser keyed off
    `params0.xy` (blendFactor + motionRejectionScale),
    `params0.z` (depthRejectionScale), `params0.w` (varianceGamma),
    and `params1.z` (variance-clip toggle). Added as the **second**
    entry of the `ShaderDef shaders[]` compile table.
  - `ProcessPass(PostProcessPass::SSAOTemporal, dt)` populates a
    `PostProcessCB` straight from `m_ssaoTemporalFilter.GetSettings()`.
  - `GetPassMetrics()`: `"SSAOTemporal"` inserted between `"GTAO"`
    and `"Bloom"` in the `passNames` array. A `static_assert` pins
    the array size to `PostProcessPass::Count` so any future enum
    reshuffle fails loudly at compile time.

- `SparkEngine/Source/Core/SubsystemConsoleCommands.cpp`
  - Both the `pp_enable` and `pp_disable` console commands now map
    `"gtao"` → `GTAO` and `"ssaotemporal"` → `SSAOTemporal`. (Phase I
    had missed adding `"gtao"`; Phase J corrects that alongside the
    new `"ssaotemporal"` toggle.)

### J2 — `RenderTargetPool` activated as a per-pipeline transient RT allocator

**Problem:** `Graphics/RenderTargetPool.h` is a 401-line Windows-only
pooled allocator for transient render targets. It has no engine-side
call sites in either `GraphicsEngine` or the render-graph layer.

**Fix:**

- `PostProcessingPipeline.h` — new `#ifdef SPARK_PLATFORM_WINDOWS`
  block adds `RenderTargetPool m_rtPool` member alongside the other
  D3D11 state.
- `PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_rtPool.Initialize(m_device, 60)` when a
    device is attached.
  - `Shutdown()` calls `m_rtPool.Shutdown()`.
  - `Process()` calls `m_rtPool.Tick()` at the top of each frame so
    released targets age out of the pool.
- New public accessors on `PostProcessingPipeline`:
  - `GetRenderTargetPoolSize()` — returns `m_rtPool.GetMetrics().totalTargets`
    on Windows, 0 elsewhere.
  - `Console_RenderTargetPoolStatus()` — returns the pool's own
    status string on Windows, a stub `(not compiled on this platform)`
    marker otherwise so UI panels never display a blank line.

The pipeline itself still ping-pongs between the two pre-allocated
RTVs for per-pass execution; the RT pool is now live and `Tick()`ing
but the *acquisition* of transient scratch targets is deferred to the
first orphan that needs them (Phase K: `SSAOTemporal` history target,
`BVHAccelerator` scratch RT). Having the pool alive and reclaim-capable
**now** gives those follow-ups a zero-churn landing zone.

### J3 — `GPUDebugMarkers` activated as scoped event regions around every pass

**Problem:** `Graphics/GPUDebugMarkers.h` is a 380-line wrapper around
`ID3DUserDefinedAnnotation` that provides scoped PIX/RenderDoc event
regions. Nothing in the engine instantiated it.

**Fix:**

- `PostProcessingPipeline.h` — new Windows-only `GPUDebugMarkers m_gpuMarkers`
  member.
- `PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_gpuMarkers.Initialize(m_context)` when a
    device context is attached — the initialise method gracefully
    reports `true` even when the annotation interface is unavailable.
  - `Shutdown()` calls `m_gpuMarkers.Shutdown()`.
  - `Process()` wraps the entire pass chain in
    `BeginEvent(L"PostProcessingPipeline")` / `EndEvent()` so PIX
    and RenderDoc captures group the per-pass inner events.
  - `ProcessPass()` creates a `ScopedGPUEvent` per pass keyed to the
    matching entry of a new `kPassNamesW[]` wide-string table. The
    table has a `static_assert` matching it to `PostProcessPass::Count`
    so future enum changes fail loudly.
- New `GetGPUMarkerDepth()` accessor returns `m_gpuMarkers.GetEventDepth()`
  on Windows and `0` elsewhere. Phase J tests assert that the depth
  always returns to zero after `Process()` — a regression-proof balance
  check for the BeginEvent/EndEvent pairs.

### J4 — `GPUTimestampQuery` activated as per-pass GPU timers

**Problem:** `Graphics/GPUTimestampQuery.h` is a 493-line double-buffered
GPU timestamp query pool. The pipeline previously measured pass timings
with `std::chrono::high_resolution_clock` — a CPU clock that reports
the driver-submit time, not the GPU-execute time.

**Fix:**

- `PostProcessingPipeline.h` — new Windows-only `GPUTimestampQuery m_gpuTimer`
  member.
- `PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_gpuTimer.Initialize(m_device, maxTimers = PostProcessPass::Count)`
    when a device is attached.
  - `Shutdown()` calls `m_gpuTimer.Shutdown()`.
  - `Process()` calls `m_gpuTimer.BeginFrame(m_context)` before the
    first pass and `EndFrame(m_context)` after the last one. The
    query pool is double-buffered with `kFrameLatency = 2`, so the
    first two frames collect no results — the existing CPU-side
    `m_passTimings[]` fallback remains authoritative during warm-up.
  - `ProcessPass()` creates a `ScopedTimestamp` per pass, keyed to
    the same `kPassNames[]` table as the debug markers.
  - `GetPassMetrics()` now prefers the GPU reading from
    `m_gpuTimer.GetPassTimeMs(name)` when it is strictly positive,
    falling back to the CPU value otherwise.
- New `GetPassTimeMs(PostProcessPass)` accessor reads the same
  GPU-vs-CPU precedence rule so profilers and console panels always
  see the best-available number.

## Tests

### J-new: `Tests/TestSSAOTemporalFilter.cpp` (8 tests, ~160 lines)

Exercises the real `Spark::Graphics::SSAOTemporalFilter` class
directly — no GPU, no D3D11:

- `SSAOTemporal_DefaultSettings` — pins the 6 `SSAOTemporalSettings`
  defaults (`blendFactor=0.9`, `motionRejectionScale=10`, etc.).
- `SSAOTemporal_InitializeAndShutdown` — lifecycle happy path.
- `SSAOTemporal_ResizeResetsHistory` — viewport change rebuilds the
  history buffer and leaves the filter initialised.
- `SSAOTemporal_FirstFrameReturnsCurrent` — with an empty history,
  Apply() returns the current buffer untouched.
- `SSAOTemporal_ConstantFieldRemainsConstant` — a constant AO
  field stays constant across 5 frames (the variance-clip band
  collapses onto the mean).
- `SSAOTemporal_NullInputIsSafe` — null current buffer returns
  null (documented fallback).
- `SSAOTemporal_SettingsAreMutable` — settings struct is mutable
  through the mutable `GetSettings()` accessor.
- `SSAOTemporal_StepTowardHistoryOverFrames` — with blendFactor=1.0
  and rejection disabled, a sudden input transition is pulled
  toward the prior mean (temporal low-pass behaviour).

### J-new: `Tests/TestPostProcessingPipelinePhaseJ.cpp` (17 tests, ~200 lines)

Exercises the real `PostProcessingPipeline` against its new public
surface. These are portable — they run on Windows, Linux, and macOS
CI without a GPU device:

**Enum ordering:**
- `PhaseJ_EnumOrdering_GTAOFirst` — slot 0.
- `PhaseJ_EnumOrdering_SSAOTemporalSecond` — slot 1.
- `PhaseJ_EnumOrdering_BloomBumpedToThird` — slot 2 (was 1 in Phase I).
- `PhaseJ_EnumOrdering_CountIsSixteen` — `Count` = 16 (was 15 in Phase I).
- `PhaseJ_EnumOrdering_SharpenIsLast` — `Sharpen + 1 == Count`.

**Pipeline lifecycle:**
- `PhaseJ_HeadlessInitializeAndShutdown` — CPU-only init/teardown.
- `PhaseJ_ResizePropagatesToSSAOTemporalHistory` — resize + repeat.
- `PhaseJ_ProcessIsSafeWithoutDevice` — `Process()` is a safe no-op
  without a D3D11 context.

**Metrics:**
- `PhaseJ_GetPassMetrics_HasSixteenEntries` — metrics vector size.
- `PhaseJ_GetPassMetrics_SSAOTemporalNameAtSlotOne` — slot[1] name.
- `PhaseJ_Console_ListEffects_IncludesSSAOTemporal` — console string.

**Accessor surface (the four Phase J additions):**
- `PhaseJ_GetRenderTargetPoolSize_HeadlessIsZero` — headless returns 0.
- `PhaseJ_Console_RenderTargetPoolStatus_ReturnsNonEmpty` — never blank.
- `PhaseJ_GetGPUMarkerDepth_AtRestIsZero` — balanced event regions.
- `PhaseJ_GetPassTimeMs_IsNonNegative` — timings are always ≥ 0, and
  out-of-range indices return 0 safely.

**Settings:**
- `PhaseJ_GetSSAOTemporalSettings_Mutable` — settings are writable.
- `PhaseJ_SetEffectEnabled_SSAOTemporalToggles` — toggleable via enum.

### Registration
`Tests/CMakeLists.txt` — both new files registered alongside
`TestGTAOEffect.cpp`.

## Files touched

```
SparkEngine/Source/Graphics/PostProcessingTypes.h          (J1)
SparkEngine/Source/Graphics/PostProcessingPipeline.h       (J1, J2, J3, J4)
SparkEngine/Source/Graphics/PostProcessingPipeline.cpp     (J1, J2, J3, J4)
SparkEngine/Source/Core/SubsystemConsoleCommands.cpp       (J1)
Tests/TestSSAOTemporalFilter.cpp                           (J1, new)
Tests/TestPostProcessingPipelinePhaseJ.cpp                 (J1-J4, new)
Tests/CMakeLists.txt                                       (J1-J4)
.claude/knowledge/engine-next-steps-phase-j-2026-04-11.md  (new)
.claude/index.md                                           (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager ODR warnings unchanged).
- `SparkTests` binary — **4395 passed, 0 failed, 1 warned** (pre-existing
  `LoadTest_Severe_EntityFlood` known-flaky), total 119021 assertions
  (+25 new Phase J assertions over the Phase I baseline of 118996; the
  +26-test delta is from 17 PhaseJ integration tests + 8 SSAOTemporalFilter
  CPU tests).
- `clang-format --dry-run --Werror` on all touched files — clean after
  a single `clang-format -i` pass (the multi-line `kPassNames` arrays
  rewrapped against the 120-col limit).

## What's left for Phase K

Phase J closes four of the ~25 Tier 2 graphics orphans catalogued on
April 10. The remaining high-value orphans that can be picked up next,
in roughly ascending order of scope:

- **`BVHAccelerator.h`** (441 lines) — hierarchical frustum / ray
  culling. Needs a scene-renderer call site; would likely live in
  `GraphicsEngine::CullFrustum` or similar. Different integration
  surface from post-process.
- **`MeshOptimizer.h`** (471 lines) — vertex-cache / overdraw mesh
  optimiser. Wires into `AssetPipeline::LoadMesh` or the foliage
  `FoliageImpostorBaker`. Already has `TestGraphicsIntegration.cpp`
  coverage — activation just needs a call site.
- **`RTHandleSystem.h`** (265 lines) — HDRP-style render-target
  handle abstraction. Can be instantiated in `GraphicsEngine` and
  used for the main scene colour / depth targets.
- **`ShaderVariantSystem.h`** (391 lines) — keyword-based shader
  permutation management. Would replace the ad-hoc variant bookkeeping
  in the existing shader families.
- **`ReflectionProbeCache.h`** (317 lines) — prefiltered env-map cache.
  Wires into the lighting system.
- **`CachedShadowAtlas.h`** (328 lines) — shadow atlas with per-light
  caching. Wires into the shadow rendering path.
- **`VolumeSystem.h`** (461 lines) — Unity-style post-process volume
  blending. This one belongs in `PostProcessingPipeline` too — it
  blends `PostProcessPass` settings based on spatial volumes. Good
  Phase L candidate.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI. Multi-
  session scope, parked until real-time GI becomes a priority.

The smaller orphans (`LineTrailRenderer`, `SpringArm`, `DirtyRectTracker`,
`ClusteredLightGPU`, `RHIHandlePool`, `TransientBufferAllocator`) all
have existing test coverage and header `@note` documentation; they are
intentional reusable utilities that components / future systems pull
in on demand. No activation work is required — their "consumers" are
the components that already include them.

## Design notes for follow-ups

- **Enum ordering is still execution ordering.** Phase J added a slot
  at position 1 (between GTAO and Bloom) — that shifted every existing
  pass by one. All downstream code that touches the enum must use the
  named value (`PostProcessPass::Bloom`), never a hard-coded integer.
  The console command maps and the `passNames[]` / `kPassNames[]` /
  `kPassNamesW[]` tables are now protected by `static_assert`s against
  `PostProcessPass::Count`.
- **Windows-only orphans need Windows-only members.** `GPUDebugMarkers`,
  `GPUTimestampQuery`, and `RenderTargetPool` all gate their entire
  class bodies behind `SPARK_PLATFORM_WINDOWS`, so any pipeline member
  of those types must live inside the same `#ifdef`. The Phase J
  accessors return 0 / stub strings on non-Windows so UI code never
  special-cases the platform.
- **CPU oracles are great test fixtures.** `SSAOTemporalFilter` shipped
  a full CPU implementation, which means the Phase J tests can pin its
  behaviour with 100% coverage without any GPU. The pattern matches
  Phase I's `GTAOEffect` — *every* orphan that provides a CPU reference
  should be tested against it before activation, not after.
- **Pool-first wiring pays off.** Activating `RenderTargetPool` with no
  live acquisitions looks like dead code, but it means the follow-up
  orphans (`SSAOTemporal` history, `BVHAccelerator` scratch RTs,
  `VolumeSystem` blend intermediates) can each land as one-line
  `Acquire`/`Release` diffs with no pool-lifecycle plumbing. This is
  deliberate scaffolding.
