# Engine next-steps — Phase N (2026-04-11)

**Status:** Active. Phase N activates two more Tier 2 graphics
orphans — `RTHandleSystem` (portable) and `ConstantBufferRing`
(Windows-only) — as per-instance members of `PostProcessingPipeline`
with full lifecycle wiring. Phase N continues the "PostProcessingPipeline
as integration surface" thread that Phases I, J, and K established,
bringing the pipeline to a total of six orphan activations on that one
class alone.

## Context

Phase M moved off `PostProcessingPipeline` to the lighting surface
(`LightingSystem`) and wired two lighting-adjacent orphans. Phase N
returns to the post-process surface for the two remaining orphans
that belong there:

- `Graphics/RTHandleSystem.h` (265 lines) — Unity-HDRP-inspired
  scale-based render-target handle abstraction. Each handle is
  declared as a fraction of a reference viewport
  (`scaleX = scaleY = 0.5f` for a half-resolution target), and
  allocated sizes are grow-only so window shrinking does not
  reallocate. Supports dynamic resolution scaling.
- `Graphics/ConstantBufferRing.h` (365 lines, Windows-only) — ring
  buffer allocator for dynamic D3D11 constant buffers. Creates one
  large `ID3D11Buffer`, maps it once per frame with
  `D3D11_MAP_WRITE_DISCARD`, then bump-allocates sub-ranges and
  binds them via `VSSetConstantBuffers1` / `PSSetConstantBuffers1`.
  Eliminates per-draw `Map`/`Unmap` traffic.

Both had been catalogued in
`stub-and-abandoned-features-2026-04-10.md` as "intentional
reusable utility" with no engine caller. Phase N gives each a
real home.

Key discovery while scoping: **`RTHandleSystem.h` is fully portable**
despite living in the Graphics folder. It uses `uint32_t` for its
format field (DXGI_FORMAT as a raw integer), so the header compiles
on Linux and macOS. This lets the pipeline member live outside the
`SPARK_PLATFORM_WINDOWS` guard and enables full CPU-side test
coverage on every platform's CI.

## Items closed

### N1 — `RTHandleSystem` activated as a per-pipeline portable member

**Problem:** `RTHandleSystem.h` is a 265-line pure-CPU scale-based
RT handle manager. Zero engine callers.

**Fix:**

- `SparkEngine/Source/Graphics/PostProcessingPipeline.h`
  - Unconditionally `#include "RTHandleSystem.h"` alongside
    `SSAOTemporal.h` and `VolumeSystem.h`.
  - New member `RTHandleSystem m_rtHandleSystem` outside the
    Windows guard (pure CPU — runs on every platform).
  - New public accessor pair `GetRTHandleSystem()` const/non-const.
  - `Resize()` now forwards the new viewport size via
    `m_rtHandleSystem.SetReferenceSize(width, height)` when the
    system is initialised. Allocated handles are grow-only so
    shrinking the window does not reallocate.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp`
  - `Initialize()` calls
    `m_rtHandleSystem.Initialize(width, height)` unconditionally —
    headless pipelines still get a live handle system.
  - `Shutdown()` calls `m_rtHandleSystem.Shutdown()`.

Callers can now allocate scale-based RTHandles via
`pipeline->GetRTHandleSystem().Allocate(0.5f, 0.5f)` and rely on
the pipeline to track reference-size changes automatically. A
future refactor that actually binds GPU textures to the handles
only needs to extend the system's texture-allocation side — the
metadata plumbing is already live.

### N2 — `ConstantBufferRing` activated as a per-pipeline Windows-only member

**Problem:** `ConstantBufferRing.h` is a 365-line D3D11 ring-buffer
allocator. No engine caller.

**Fix:**

- `SparkEngine/Source/Graphics/PostProcessingPipeline.h`
  - Added `#include "ConstantBufferRing.h"` inside the existing
    `#ifdef SPARK_PLATFORM_WINDOWS` block alongside
    `GPUDebugMarkers`, `GPUTimestampQuery`, `RenderTargetPool`.
  - New member `ConstantBufferRing m_cbRing` under the same guard.
  - New public accessors `GetConstantBufferRingCapacity()` and
    `GetConstantBufferRingPeakUsage()`. Both return 0 on
    non-Windows; on Windows they read back `m_cbRing.GetMetrics()`.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_cbRing.Initialize(m_device, 2 * 1024 * 1024)`
    when a D3D11 device is attached. 2 MB is plenty for the
    `PostProcessCB` traffic the pipeline generates today.
  - `Shutdown()` calls `m_cbRing.Shutdown()`.
  - `Process()` calls `m_cbRing.BeginFrame(m_context)` at the top
    of the frame (alongside `m_gpuTimer.BeginFrame`) and
    `m_cbRing.EndFrame()` at the bottom. The ring is mapped with
    `WRITE_DISCARD` so the driver renames the buffer under the
    hood — no GPU stall on the per-frame open.

The ring is now live every frame on Windows. Per-pass sub-
allocation is deferred: the existing `BeginPass` path still uses
the dedicated `m_constantBuffer` with per-pass `Map`/`Unmap`.
Converting each pass's constant buffer update to an
`m_cbRing.Allocate` call is a one-touch follow-up once the
DX11.1 `PSSetConstantBuffers1` binding path is wired through the
shader binding layer.

### Test coverage

**N1-new: `Tests/TestRTHandleSystem.cpp`** (~250 lines, 14 tests)

Exercises the real `Spark::Graphics::RTHandleSystem` directly.
Every test runs on every platform's CI:

- **Lifecycle:** `InitializeAndShutdown`,
  `AllocateBeforeInitializeReturnsZero`.
- **Allocation:** `AllocateFullResolutionHandle`,
  `AllocateHalfResolutionHandle`,
  `MultipleHandlesGetDistinctIds`, `ReleaseRemovesHandle`.
- **Reference size changes:** `SetReferenceSize_GrowsHandles` (grow
  triggers allocation bump), `SetReferenceSize_ShrinkDoesNotReallocate`
  (allocated sizes stay at max-history), `SetReferenceSize_HalfScaleHandleTracksReference`.
- **Dynamic resolution:** `SetDynamicResolutionScale_ShrinksCurrent`,
  `SetDynamicResolutionScale_ClampsToRange` (clamps to [0.25, 1.0]).
- **UV scale:** `GetUVScaleTracksAllocatedVsCurrent` — UV scale
  reflects the current/allocated ratio for sub-region sampling.
- **Console status:** `Console_GetStatusFormatting`.
- **BufferedRTHandle helper:** `BufferedRTHandle_InitializeAllocatesTwoHandles`,
  `BufferedRTHandle_SwapAlternatesCurrentAndPrevious` — double-
  buffered wrapper used for TAA / motion blur history.

**N2-new: `Tests/TestConstantBufferRing.cpp`** (~100 lines, 9 tests)

Windows-gated. Covers the uninitialised-state contract and input
rejection paths — a full D3D11 device is not available in the test
framework, so the tests focus on what can be validated without one:

- `DefaultIsUninitialised` — zeroed metrics, null buffer,
  IsInitialized/IsFrameActive both false.
- `InitializeNullDeviceFails` — null device returns false.
- `ShutdownBeforeInitializeIsSafe` — idempotent teardown.
- `BeginFrameBeforeInitializeFails` — returns false.
- `EndFrameBeforeBeginIsSafe` — idempotent no-op.
- `AllocateBeforeBeginFrameReturnsInvalid` — default invalid
  allocation.
- `AllocateZeroBytesReturnsInvalid`.
- `DefaultCBAllocationIsInvalid` — POD contract.
- `Console_GetStatusUninitialised` — status reflects the state.

**N1+N2: `Tests/TestPostProcessingPipelinePhaseN.cpp`** (~150 lines, 6 tests)

Portable integration tests against the real `PostProcessingPipeline`:

- `RTHandleSystem_InitializedWithPipeline` — follows Init/Shutdown.
- `RTHandleSystem_AllocateViaPipeline` — half-res handle allocation.
- `RTHandleSystem_ResizeForwardsReferenceSize` — pipeline
  `Resize(1280, 720)` updates every live handle.
- `RTHandleSystem_SurvivesHeadlessProcess` — Process() with no
  device is still safe.
- `ConstantBufferRing_HeadlessCapacityIsZero` — portable accessor
  surface returns 0 on Linux / headless Windows.
- `ConstantBufferRing_AccessorsSurviveShutdown` — post-shutdown
  reads don't crash.
- `ProcessRunsWithoutDevice` — multi-frame `Process()` pin that
  catches any new per-frame hook (Phase N added two:
  `BeginFrame`/`EndFrame` on the CB ring).

### Registration

`Tests/CMakeLists.txt` — all three files registered alongside the
Phase M test files.

## Files touched

```
SparkEngine/Source/Graphics/PostProcessingPipeline.h        (N1, N2)
SparkEngine/Source/Graphics/PostProcessingPipeline.cpp      (N1, N2)
Tests/TestRTHandleSystem.cpp                                (N1, new — 14 portable tests)
Tests/TestConstantBufferRing.cpp                            (N2, new — 9 Windows-gated tests)
Tests/TestPostProcessingPipelinePhaseN.cpp                  (N1+N2, new — 6 portable integration tests)
Tests/CMakeLists.txt                                        (N1, N2)
.claude/knowledge/engine-next-steps-phase-n-2026-04-11.md   (new)
.claude/index.md                                            (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4483 passed, 0 failed, 1 warned**
  (pre-existing `LoadTest_Severe_EntityFlood` known-flaky), total
  119366 assertions (+77 from Phase M baseline of 119289; the +22
  test delta is 14 `RTHandleSystem_*` + 2 `BufferedRTHandle_*` +
  6 `PhaseN_*` integration tests; the 9 `ConstantBufferRing_*`
  tests only register on Windows).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase O

Phase N closes two more Tier 2 graphics orphans, bringing the
running total across I→N to **twelve orphans activated**. Remaining
candidates, in roughly ascending order of scope:

- **`ShaderVariantSystem.h`** (391 lines) — keyword-based shader
  permutation management. Would slot into the existing `Shader.h`
  / `ShaderCompilation` pipeline as the variant cache layer.
- **`PersistentMaterialCB.h`** (220 lines) — persistent constant
  buffer with dirty tracking. One per material family. Integration
  surface is `MaterialSystem` or a per-material wrapper.
- **`DenoiserInterface.h`** (251 lines) — abstract denoiser plugin
  interface. Needs a concrete denoiser implementation to plug in.
- **`UICompositor.h`** (231 lines) — may be superseded by
  `Engine/UI`. Needs a wire-or-delete decision after an audit of
  the current UI path.
- **`FastNoise2SIMD.h`** (749 lines) — SIMD procedural noise. Good
  terrain / foliage fit but existing systems have their own noise
  generators — activation would be a parallel utility, not a
  replacement.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope, parked until real-time GI is a priority.

**Tightest fit for Phase O:** `ShaderVariantSystem` — it has the
clearest integration surface (the shader cache / compilation
pipeline), and the variant-key hashing math is easy to cover with
portable tests. `PersistentMaterialCB` could ride alongside if the
`MaterialSystem` has a clean hook point.

## Design notes for follow-ups

- **`RTHandleSystem` is secretly portable.** The header claims a
  "Graphics utility" home but has zero D3D11 dependency — the
  format field is `uint32_t`, and nothing in the allocation /
  resize / dynamic-scale paths touches GPU state. This pattern is
  worth looking for in other "graphics" orphans: some of them are
  pure bookkeeping classes that only look Windows-only because
  they live in the Graphics folder. When a future orphan has no
  `#include <d3d11.h>` and no `ComPtr` members, try compiling it
  on Linux before assuming it needs a guard.
- **Lifecycle-only activation is a valid step.** Phase N's
  `ConstantBufferRing` is live (`BeginFrame`/`EndFrame` every
  frame) but no pass actually sub-allocates from it yet — the
  existing per-pass `Map`/`Unmap` path on `m_constantBuffer` still
  runs. Activation does not mean "use the feature everywhere";
  it means "the feature has a real lifecycle home." Sub-
  allocation can land in a follow-up as a one-line-per-pass diff.
  This keeps activation diffs small and the risk of breaking
  working code low.
- **Accessor fallbacks keep tests portable.** The two
  `ConstantBufferRing` accessors
  (`GetConstantBufferRingCapacity`, `GetConstantBufferRingPeakUsage`)
  are compiled on every platform but return 0 on non-Windows. This
  lets the portable `TestPostProcessingPipelinePhaseN.cpp` file
  call them without platform guards, pinning the "must not crash"
  contract without demanding a D3D11 device. Phase J, K, M, and N
  all use the same pattern; new orphan accessors should follow it.
- **Grow-only allocation is the right default.** `RTHandleSystem`
  refuses to shrink texture allocations on window-size decrease so
  resizing to a smaller viewport and back does not reallocate. This
  is the Unity HDRP contract and it avoids a common performance
  cliff. Any future pipeline that adds GPU-side RT allocation must
  preserve the grow-only semantic.
