# Engine next-steps — Phase CC (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Wire two more SparkEngine singleton orphans surfaced by a
broader sweep after Phase BB. Theme 3D continuation.

---

## Phase CC targets

The Phase BB sweep (`grep static.*GetInstance() --include='*.h' | filter
by external call sites` against SparkEngine alone) found two
orphans. Phase CC widens the same sweep to include the `Tests` and
`GameModules` directories as exclusion paths, and lands two more:

1. **`Spark::GPUStallProfiler`** — `Utils/GPUStallProfiler.h`
   (237 LOC, header-only). Tracks CPU/GPU frame-time overlap and
   classifies each frame as CPU-bound, GPU-bound, Balanced, or
   Bubble. Only reference in the engine: the test file
   (`Tests/TestGPUStallProfiler.cpp`) which exercises the real class
   already — this is a tests-but-no-integration orphan, not a fake-
   coverage one.

2. **`Spark::Graphics::AsyncComputeScheduler`** —
   `Graphics/AsyncComputeScheduler.h` (198 LOC) +
   `AsyncComputeScheduler.cpp` (282 LOC). Manages compute workloads
   with a D3D11 synchronous-immediate fallback; designed so D3D12 /
   Vulkan backends can plug in. Only reference in the engine: the
   test file (`Tests/TestAsyncComputeScheduler.cpp`).

## Wire-up

### `GPUStallProfiler`

Added to the RenderingAndUtility block of
`GameplayLifecycleShared.cpp`:

```cpp
Spark::GPUStallProfiler::GetInstance().Initialize();
```

and to the shutdown block:

```cpp
Spark::GPUStallProfiler::GetInstance().Shutdown();
```

`BeginCPUWork` / `EndCPUWork` / `RecordGPUFrameTime` / `EndFrame`
are intentionally **not** pumped from the lifecycle. Those calls
belong in the render loop (specifically around the main-thread frame
submission path) and will land when a real profiler feed is wired
— for example from `GraphicsEngine::BeginFrame` /
`GraphicsEngine::EndFrame`. Phase CC lands the seam so the profiler
is always reachable and in a clean state.

### `AsyncComputeScheduler`

Added to the same lifecycle block:

```cpp
Spark::Graphics::AsyncComputeScheduler::GetInstance().Initialize();
```

and to the shutdown block:

```cpp
Spark::Graphics::AsyncComputeScheduler::GetInstance().Shutdown();
```

`SubmitComputeWork` / `Flush` / `BeginFrame` / `WaitForCompletion`
are called from the real D3D11 render path on Windows when a
consumer lands; Phase CC only lands the lifecycle seam.
`SetDeviceContext` is skipped because (a) the D3D11 device lives
behind a Windows-only platform guard and (b) adding the set-context
call from this TU would create an engine→D3D11 backend dependency
in the shared lifecycle file. A follow-up phase can wire
`SetDeviceContext` from `D3D11Device::Initialize` directly.

## Files touched (code)

- `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp`:
  - `#include "Utils/GPUStallProfiler.h"`
  - `#include "Graphics/AsyncComputeScheduler.h"`
  - Initialize block: `GPUStallProfiler::GetInstance().Initialize()`
    and `AsyncComputeScheduler::GetInstance().Initialize()`.
  - Shutdown block: both singletons released first (before
    `DynamicQualityScaler::Reset()` from Phase BB) because they
    depend on the render loop being alive.

## Files touched (tests)

- `Tests/TestGPUStallProfilerPhaseCC.cpp` (new) — 10 real-class
  tests covering: singleton stability, Initialize clears history,
  SimulateFrame populates history (via CPU busy-loop + RecordGPUFrameTime),
  CPU-bound classification (CPU >> GPU), GPU-bound classification
  (GPU >> CPU), Balanced classification (both at comparable busy
  levels — tolerance for clock jitter), Unknown on a zero-duration
  frame, `GetDistribution` percentages sum to 100 over a mixed
  history, Shutdown is safe and reinit works, PresentTime captured
  in timeline.

  Note: the percentages are reported in **0–100 form**, not 0–1 —
  the first version of the test expected fractions and failed with
  `total = 99` (the sum was `99.0 + 0.0 + 0.0 + 0.0 = 99.0`). Fixed
  to `EXPECT_NEAR(total, 100.0f, 0.5f)`.

- `Tests/TestAsyncComputeSchedulerPhaseCC.cpp` (new) — 12 real-class
  tests covering: singleton stability, Initialize/Shutdown toggle
  `IsInitialized`, idempotent Shutdown, `SubmitComputeWork` returns
  valid handle, handles are monotonically unique, `pendingItems`
  grows with submissions, Flush without a device context is safe
  (the Linux path), Flush bumps `totalDispatches`, BeginFrame resets
  per-frame counter, WaitForCompletion is safe anytime,
  Console_GetStatus returns non-empty, Submit-before-Initialize is
  safe (returns invalid handle or accepts work — either is allowed
  as long as it doesn't crash).

- `Tests/CMakeLists.txt` — added both new test files to the main
  source list.

## Full suite

**4751 passed, 0 failed, 4751 total** on `linux-gcc-release` — +22
from Phase CC, +167 cumulative since Phase U baseline (4605 → 4751
net Linux-active gain).

## Playbook notes

1. **Percentages vs fractions trap.** `GetDistribution` returns
   percentages in 0–100 form, not fractions in 0–1 form. My first
   test assumed fractions and failed with a sum of 99. Always read
   the real class implementation before asserting on numeric
   ranges — don't infer from the struct field name alone.

2. **Don't wire backend-specific calls from the shared lifecycle
   TU.** `AsyncComputeScheduler::SetDeviceContext(ID3D11DeviceContext*)`
   is Windows-only. Calling it from `GameplayLifecycleShared.cpp`
   would require guarding the call with `#ifdef SPARK_PLATFORM_WINDOWS`
   and including `<d3d11.h>`, creating an engine→D3D11 dependency
   in the shared TU. Leave backend wiring for the backend's own
   Initialize function.

3. **Tests-but-no-integration is a legitimate orphan category.**
   `GPUStallProfiler` and `AsyncComputeScheduler` both had existing
   test files against the real class — they weren't fake-coverage
   like Phase BB's `ScriptHookManager`. But the engine never
   instantiated them outside the test harness. Phase CC adds the
   engine lifecycle seam without touching the test files, and
   additionally writes new Phase-CC tests to lock down behaviours
   the existing tests didn't cover.

## Cross-references

- Phase BB: [engine-next-steps-phase-bb-2026-04-11.md](engine-next-steps-phase-bb-2026-04-11.md)
- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
