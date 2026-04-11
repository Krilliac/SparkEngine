# Engine next-steps — Phase J (2026-04-11)

**Status:** Active. Phase J wires **four** more Tier 2 graphics orphans
from `stub-and-abandoned-features-2026-04-10.md` into the real
`PostProcessingPipeline`, extending the single-orphan activation done
in Phase I (`GTAOEffect`) to a full integration of the post-process
substrate.

## Context

Phase I activated `GTAOEffect.h` as the 15th post-process pass. The
remaining 24 Tier 2 graphics orphans were documented as intentional
reusable utilities but still had zero engine call sites. Phase J picks
four that share the same integration surface (`PostProcessingPipeline`)
so they can be activated coherently in one pass rather than scattered
across the render graph:

| Orphan | Role in Phase J |
|---|---|
| `SSAOTemporal.h` | 16th post-process pass — variance-clipped AO denoiser running immediately after GTAO. |
| `RenderTargetPool.h` | Owned per-pipeline, ticked every frame. Pool starts empty; Phase J establishes the lifecycle and public query API. |
| `GPUDebugMarkers.h` | Wraps PIX / RenderDoc annotations. Phase J brackets the whole post-process chain in a parent event region and each pass in an inner `ScopedGPUEvent`. |
| `GPUTimestampQuery.h` | Double-buffered per-pass D3D11 timestamp queries. Phase J calls `BeginFrame` / `EndFrame` around `Process()` and wraps each pass body in a `ScopedTimestamp`. |

Each of these files is ~200–500 lines of complete, working code. None
of them were called from anywhere. All four now have real engine-side
callers and portable integration tests against `PostProcessingPipeline`.

## Items closed

### J1 — SSAOTemporal activated as the 16th post-process pass

**Problem:** `SparkEngine/Source/Graphics/SSAOTemporal.h` (234 lines)
ships a CPU reference implementation of temporal SSAO denoising with
motion + depth rejection and variance clipping. It had no caller.

**Fix:**

- `SparkEngine/Source/Graphics/PostProcessingTypes.h`
  - `#include "SSAOTemporal.h"` — the settings struct now flows through
    the canonical post-process types header alongside `GTAOSettings`.
  - `PostProcessPass::SSAOTemporal` added as the second enum slot, right
    after `PostProcessPass::GTAO`. The pipeline iterates in declaration
    order so this is the correct execution slot: the denoise reads the
    AO-modulated scene before Bloom extracts highlights.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.h`
  - `#include "SSAOTemporal.h"` unconditionally (the filter is pure CPU
    code and works on Linux / headless builds).
  - New `SSAOTemporalFilter m_ssaoTemporalFilter` member.
  - New `GetSSAOTemporalSettings() / const` accessor pair forwarding to
    the filter's owned settings struct.
  - New `GetSSAOTemporalFilter() / const` accessors.
  - New `ComPtr<ID3D11PixelShader> m_ssaoTemporalPS` member.
  - `Resize()` now calls `m_ssaoTemporalFilter.Resize(w, h)` so the CPU
    history buffer always tracks the viewport size.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_ssaoTemporalFilter.Initialize(width, height)`.
  - `Shutdown()` calls `m_ssaoTemporalFilter.Shutdown()` and resets the
    new PS handle.
  - `CompileEffectShaders()` adds an inline HLSL `ssaoTemporalPS`
    literal implementing a variance-clipped bilateral denoiser: 3×3
    luminance statistics, depth-weighted bilateral blur, clamp to
    `[mean - γσ, mean + γσ]`, lerp(center, blurred, blendFactor). The
    shader is added as the second entry in the `ShaderDef shaders[]`
    compile table, keeping it adjacent to GTAO.
  - `ProcessPass(SSAOTemporal)` populates `PostProcessCB` from the
    filter's settings (blendFactor, rejection scales, varianceGamma,
    useVarianceClipping).
  - `GetPassMetrics()` adds `"SSAOTemporal"` as the second entry in the
    `passNames` array and the array is now guarded by a
    `static_assert` against `PostProcessPass::Count` so the
    enum/name pairs stay aligned forever.

- `SparkEngine/Source/Core/SubsystemConsoleCommands.cpp`
  - `pp_enable` and `pp_disable` gain `"gtao"` and `"ssaotemporal"` as
    toggle keywords — both maps were previously stale (missing the
    GTAO slot that Phase I added). Phase J brings them up to date in
    one place.

**Note on the spatial-only first cut.** `SSAOTemporalFilter::Apply`
performs history reprojection on the CPU using motion vectors. The GPU
side does **not** yet have a dedicated history target — the runtime
path uses the ping-pong buffers, which only give it the previous pass's
output, not last frame's AO. Phase J's shader is therefore a
"spatial fallback" that matches what the CPU filter does on
history-miss pixels (constant neighborhood). A real double-buffered AO
history target lands in a later phase; see *What's left* below.

### J2 — RenderTargetPool owned per-pipeline

**Problem:** `RenderTargetPool.h` (401 lines) is a per-device pooled
allocator for transient render targets. It had zero engine callers.

**Fix:**

- `PostProcessingPipeline.h`
  - `#include "RenderTargetPool.h"` under `SPARK_PLATFORM_WINDOWS`.
  - New `RenderTargetPool m_rtPool` member (Windows-only).
  - New public method `GetRenderTargetPoolSize()` returning
    `m_rtPool.GetMetrics().totalTargets` on Windows, 0 elsewhere.
  - New public method `Console_RenderTargetPoolStatus()` returning
    `m_rtPool.Console_GetStatus()` on Windows and a stable stub
    (`"Render Target Pool:\n  (not compiled on this platform)\n"`) on
    other platforms so UI panels never see an empty string.

- `PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_rtPool.Initialize(m_device, 60)` when a
    device is attached. The default reclaim-after-frames matches the
    header's documented value.
  - `Process()` calls `m_rtPool.Tick()` every frame so unused targets
    age out after 60 idle frames.
  - `Shutdown()` calls `m_rtPool.Shutdown()`.

**Note on migration surface.** Phase J establishes the pool's public
query API and per-frame tick but does **not** yet replace the
hard-coded ping-pong `m_pingPongTextures[2]` pair with pool
`Acquire/Release` calls. Doing that cleanly requires tracking two
handles per frame with the correct release order (the previous frame's
dst becomes next frame's src), which is a localised refactor worth its
own phase. For now the pool is live and queryable — any future pass
that wants a transient RT (e.g. Bloom's downsample chain, DOF's bokeh
buffer) can `Acquire` / `Release` against it without any further
plumbing.

### J3 — GPUDebugMarkers wrapping every post-process pass

**Problem:** `GPUDebugMarkers.h` (380 lines) wraps
`ID3DUserDefinedAnnotation` for PIX/RenderDoc/NSight captures and ships
a `ScopedGPUEvent` RAII helper. Zero engine callers.

**Fix:**

- `PostProcessingPipeline.h`
  - `#include "GPUDebugMarkers.h"` under Windows.
  - New `GPUDebugMarkers m_gpuMarkers` member (Windows-only).
  - New public method `GetGPUMarkerDepth()` returning the live
    `m_gpuMarkers.GetEventDepth()` on Windows, 0 elsewhere. Used by
    tests to assert that `Process()` never leaks an unbalanced event
    region.

- `PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_gpuMarkers.Initialize(m_context)` when a
    context is attached.
  - `Process()` brackets the entire post-process chain in
    `BeginEvent(L"PostProcessingPipeline")` / `EndEvent()` so PIX and
    RenderDoc captures get a single parent region covering all 16
    passes.
  - `ProcessPass()` wraps the `BeginPass` + `DrawFullscreen` call in a
    `ScopedGPUEvent` using a wide-string per-pass name table
    (`kPassNamesW[]`) static-asserted against `PostProcessPass::Count`.
  - `Shutdown()` calls `m_gpuMarkers.Shutdown()`.

### J4 — GPUTimestampQuery replacing CPU-side per-pass timing

**Problem:** `GPUTimestampQuery.h` (493 lines) implements a
double-buffered D3D11 timestamp query pool with a rolling 120-frame
history per pass. Zero engine callers. Meanwhile
`PostProcessingPipeline::m_passTimings[]` was populated with
`std::chrono::high_resolution_clock` measurements — CPU time around the
D3D11 submission, which is not the real GPU execution time.

**Fix:**

- `PostProcessingPipeline.h`
  - `#include "GPUTimestampQuery.h"` under Windows.
  - New `GPUTimestampQuery m_gpuTimer` member (Windows-only).
  - New public method `GetPassTimeMs(PostProcessPass pass)` — returns
    the GPU-side reading when one has been collected this frame, or
    falls back to the CPU-side `m_passTimings[]` value otherwise.
    Out-of-range passes return 0.0f.

- `PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_gpuTimer.Initialize(m_device,
    PostProcessPass::Count)` so the pool has one query pair per
    possible pass.
  - `Process()` calls `m_gpuTimer.BeginFrame(m_context)` before the
    loop and `m_gpuTimer.EndFrame(m_context)` after it.
  - `ProcessPass()` wraps each pass in a `ScopedTimestamp(m_gpuTimer,
    m_context, kPassNames[idx])` (narrow-string table also
    static-asserted against `PostProcessPass::Count`).
  - `GetPassMetrics()` prefers `m_gpuTimer.GetPassTimeMs(passName)`
    over the stored CPU timing whenever the GPU reading is non-zero.
    During the 2-frame warm-up latency the CPU value remains
    authoritative.
  - `Shutdown()` calls `m_gpuTimer.Shutdown()`.

## New tests

### `Tests/TestSSAOTemporalFilter.cpp` — 8 CPU reference tests

- `SSAOTemporal_DefaultSettings` — pins the 6 `SSAOTemporalSettings`
  defaults so future drift breaks this test before the pipeline CB.
- `SSAOTemporal_InitializeAndShutdown` — happy-path lifecycle.
- `SSAOTemporal_ResizeResetsHistory` — viewport resize keeps the
  filter initialised and rebuilds the history buffer.
- `SSAOTemporal_FirstFrameReturnsCurrent` — before any history exists
  the filter must return the raw current-frame values.
- `SSAOTemporal_ConstantFieldRemainsConstant` — 5 frames of a flat AO
  field must not drift; the variance-clip band collapses to the mean.
- `SSAOTemporal_NullInputIsSafe` — null current-buffer returns null.
- `SSAOTemporal_SettingsAreMutable` — accessor returns a reference.
- `SSAOTemporal_StepTowardHistoryOverFrames` — with all-history blend
  weight and no rejection, a sudden drop from 1.0 → 0.0 must remain
  above 0.5 at the center pixel (history is dominating).

### `Tests/TestPostProcessingPipelinePhaseJ.cpp` — 17 integration tests

Portable tests against the `PostProcessingPipeline` accessor surface
that Phase J added. Run on Windows, Linux, and macOS CI:

- Enum layout: `GTAO` at slot 0, `SSAOTemporal` at slot 1, `Bloom` at
  slot 2, `Sharpen` last, `Count == 16`.
- Lifecycle: headless `Initialize`/`Shutdown` round-trips set and clear
  `SSAOTemporalFilter::IsInitialized()`.
- Resize propagation: identical re-resize is a no-op, dimension change
  keeps the filter initialised.
- Safe `Process()` with no device attached (no active passes,
  no crash).
- `GetPassMetrics()` returns 16 entries with `"SSAOTemporal"` at slot 1.
- `Console_ListEffects()` contains both `"GTAO"` and `"SSAOTemporal"`.
- `GetRenderTargetPoolSize()` returns 0 when no device is attached.
- `Console_RenderTargetPoolStatus()` is always non-empty (the Linux
  stub keeps UI panels from showing a blank line).
- `GetGPUMarkerDepth()` is 0 at rest and returns to 0 after
  `Process()` completes — the `BeginEvent`/`EndEvent` pair is balanced.
- `GetPassTimeMs()` is non-negative for every valid enum index and
  returns 0 for out-of-range indices.
- `GetSSAOTemporalSettings()` is mutable through the accessor.
- `SetEffectEnabled(SSAOTemporal, …)` toggles correctly.

## Files touched

```
SparkEngine/Source/Graphics/PostProcessingTypes.h         (J1)
SparkEngine/Source/Graphics/PostProcessingPipeline.h      (J1–J4)
SparkEngine/Source/Graphics/PostProcessingPipeline.cpp    (J1–J4)
SparkEngine/Source/Core/SubsystemConsoleCommands.cpp      (J1)
Tests/TestSSAOTemporalFilter.cpp                          (new)
Tests/TestPostProcessingPipelinePhaseJ.cpp                (new)
Tests/CMakeLists.txt                                      (J1 registrations)
.claude/knowledge/engine-next-steps-phase-j-2026-04-11.md (new)
.claude/index.md                                          (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager / DebugHookManager / LoadingScreen ODR
  warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean.
- Full `SparkTests` run — **4395 passed, 0 failed, 1 warned**
  (pre-existing known-flaky `LoadTest_Severe_EntityFlood`), 119021
  assertions. +25 tests and +191 assertions over Phase I (4370 /
  118830).
- `clang-format --dry-run --Werror` on all touched files — clean after
  one auto-format pass.
- Direct-binary verification of the 25 new tests via
  `SPARK_TEST_NAME=PhaseJ` and `SPARK_TEST_NAME=SSAOTemporal`: all
  pass.

## What's left for Phase K

Phase J's pattern — co-locating four orphans that share an integration
surface — is reusable. Candidates still on the Tier 2 list from
`stub-and-abandoned-features-2026-04-10.md`:

- **`RenderTargetPool` migration** — replace the hard-coded
  `m_pingPongTextures[2]` in `PostProcessingPipeline` with
  `Acquire`/`Release` calls, then add a real double-buffered history
  target for `SSAOTemporal` so it can run its full temporal path. Same
  commit should add a `SSAOTemporal_HistoryTarget` test asserting that
  the filter's GPU-side blend equals the CPU reference on constant
  fields.
- **`BVHAccelerator.h`** — hierarchical frustum / ray culling. Feeds a
  scene renderer, not post-process. Needs a new integration surface
  (likely `GraphicsEngine::CollectVisible`).
- **`VoxelConeTracing.h`** — real-time GI. Large scope, parked until a
  GI work phase.
- **`MeshOptimizer.h`** — vertex-cache / overdraw optimiser. Called at
  mesh upload time (asset pipeline), not per-frame. Integration surface
  is `AssetPipeline::LoadMesh`.
- **`ShaderVariantSystem.h`**, **`ShaderCrossCompiler.h`** — shader
  permutation + HLSL↔GLSL translation. Integration surface is the
  shader manager.
- **`RTHandleSystem.h`**, **`ConstantBufferRing.h`**, **`PersistentMaterialCB.h`**,
  **`ReflectionProbeCache.h`**, **`CachedShadowAtlas.h`**,
  **`DenoiserInterface.h`**, **`FastNoise2SIMD.h`** — each has its own
  integration surface. Next phase should batch two or three that share
  a consumer (e.g. the shadow atlas + reflection probe cache both live
  in the lighting subsystem).
- **Smaller orphans** (`LineTrailRenderer`, `SpringArm`, `DirtyRectTracker`,
  `ClusteredLightGPU`, `RHIHandlePool`, `TransientBufferAllocator`) —
  already documented as intentional utilities with tests; most don't
  need further activation, they need consumers that'll arrive with
  future features.

None of these are blocked on Phase J. The I1 + J1–J4 activation pattern
(header include → member → lifecycle wire → per-frame tick → portable
accessor → integration test) works for any orphan that has a
CPU-runnable reference path.

## Design notes for follow-ups

- **Enum ordering is execution ordering — again.** Both Phase I and
  Phase J added new passes at the beginning of `PostProcessPass`. Any
  future activation should audit the enum position against the
  HDR → tonemap → LDR → present phase the pass belongs in.
- **Metrics / name tables are `static_assert`'d against
  `PostProcessPass::Count`.** The three tables (`passNames` in
  `GetPassMetrics`, `kPassNames`/`kPassNamesW` in `ProcessPass`) will
  now fail to compile rather than silently mismatch. Future enum
  additions must update all three; the static_assert will point at
  any forgotten one.
- **Headless / NullRHI still works.** Every Windows-only orphan has an
  `#ifdef` block in the pipeline's accessor methods so calling them
  without a device returns a stable fallback (0 / stub string / CPU
  timing). Tests run on Linux against that exact path, so the
  headless behaviour is contractual, not accidental.
- **`ScopedTimestamp` and `ScopedGPUEvent` are cheap.** Both RAII
  helpers no-op when the underlying subsystem was never initialised,
  so wrapping every `ProcessPass` call in them is free during tests
  and NullRHI rendering, and real on Windows with a device.
