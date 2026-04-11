# Engine next-steps — Phase Q (2026-04-11)

**Status:** Active. Phase Q activates
`Spark::Graphics::DenoiserInterface` (the `IDenoiser` abstract
interface plus the bundled `SoftwareDenoiser` joint-bilateral
fallback implementation) as a per-instance `unique_ptr` member of
`Spark::Graphics::GraphicsEngine` with lifecycle hooks on both
the Windows real impl and the Linux stub path.

## Context

Phases I → P wired fourteen Tier 2 graphics orphans across the
post-process, LOD, scene, lighting, shader, and material surfaces.
Phase Q picks up the denoiser plugin interface catalogued in the
April 10 audit:

`Graphics/DenoiserInterface.h` (254 lines) is a plugin-style
interface designed for future OIDN / OptiX / neural denoiser
integration. The file contains:

- `IDenoiser` — abstract base (`Initialize`, `Shutdown`,
  `SetColorInput`, `SetAlbedoGuide`, `SetNormalGuide`, `Execute`,
  `GetOutput`, `GetBackend`, `GetLastExecutionTimeMs`).
- `DenoiserSettings` — enabled flag, backend, quality preset,
  albedo/normal guide toggles, temporal flag, blend factor.
- `DenoiserBuffer` — image descriptor (float* data, width,
  height, channels, rowStride).
- `DenoiserBackend` / `DenoiserQuality` enum classes.
- `SoftwareDenoiser` — complete concrete implementation using a
  joint bilateral filter with optional albedo + normal guide
  weighting. Pure CPU (no SDK, no D3D11), works on every
  platform.

The audit noted the denoiser was catalogued as "intentional
reusable utility" with no engine caller. Phase Q gives it a
production home without requiring any concrete third-party SDK.

Key insight: the `SoftwareDenoiser` ships *in the same header*
as the interface, which means activation doesn't need a separate
concrete implementation — the bundled fallback is the default
backend, and a future OIDN / OptiX integration can hot-swap the
`m_denoiser` unique_ptr inside `GraphicsEngine::Initialize`
without any header reshuffling.

## Items closed

### Q1 — `DenoiserInterface` activated as a `GraphicsEngine` member

**Problem:** `IDenoiser` / `SoftwareDenoiser` had no engine caller.
The class ships complete functionality — joint bilateral filter
with optional per-pixel guide weighting, settings struct, backend
enum — but no `GraphicsEngine`, post-process pipeline, or ray-
tracing subsystem instantiated it.

**Fix:**

- `SparkEngine/Source/Graphics/GraphicsEngine.h`
  - `#include "DenoiserInterface.h"` alongside the other
    post-process / RHI includes. Portable — no Windows guard
    needed.
  - New private member
    `std::unique_ptr<Spark::Graphics::IDenoiser> m_denoiser;`
    immediately after the existing Windows-guarded renderer
    integration block. The `unique_ptr` lets a future OIDN /
    OptiX swap happen by replacing the pointee inside
    `GraphicsEngine::Initialize` — no member-type change, no
    ABI break.
  - New public accessor pair
    `GetDenoiser()` const/non-const (returns raw pointer).
  - New `GetDenoiserBackend()` accessor — returns the current
    backend enum, or `DenoiserBackend::None` when
    `m_denoiser == nullptr` (pre-Initialize or post-Shutdown).

- `SparkEngine/Source/Graphics/GraphicsEngine.cpp`
  - **Windows `Initialize`** (line ~497, right before the final
    `SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit)`) allocates a
    `SoftwareDenoiser` via `std::make_unique`, configures
    default `DenoiserSettings` (backend = Software, quality =
    Balanced, `enabled = false` so no CPU work runs until a
    ray-tracing pass toggles it on), and calls
    `m_denoiser->Initialize(settings)`.
  - **Windows `Shutdown`** (right after the existing renderer
    integration system teardowns) calls `m_denoiser->Shutdown()`
    and resets the unique_ptr.
  - **Linux stub `Initialize`** (line ~1482) mirrors the same
    allocation + configuration so headless builds see the same
    state as the Windows path.
  - **Linux stub `Shutdown`** (line ~1513) mirrors the teardown.
  - New `GetDenoiserBackend()` definition in **both** branches
    (the `GraphicsEngine.cpp` file has duplicated Windows and
    Linux implementations, so any new method needs a matching
    body on both sides). The two bodies are identical one-liners
    that forward to `m_denoiser->GetBackend()` with a null
    guard.

Every `GraphicsEngine` instance now has a live denoiser on
every platform. A future ray-traced AO / GI / reflections pass
only needs to call `GetDenoiser()->SetColorInput(...)` /
`Execute()` / `GetOutput()` to hit the filter.

### Test coverage

**Q-new: `Tests/TestDenoiserInterface.cpp`** (~280 lines, 14 tests)

Exercises the real `Spark::Graphics::SoftwareDenoiser` and the
`IDenoiser` polymorphic contract it implements. No GPU, no
D3D11, no external SDK — pure CPU bilateral filtering. Every
test runs on every platform's CI:

- **Settings defaults:** `DefaultSettings` — pins the six
  `DenoiserSettings` fields (enabled=false, backend=Software,
  quality=Balanced, useAlbedoGuide=true, useNormalGuide=true,
  temporal=false, blendFactor=0.1).

- **Lifecycle:** `SoftwareInitializeAndShutdown`,
  `SoftwareExecuteBeforeInitializeFails`,
  `SoftwareExecuteWithoutInputFails`.

- **Passthrough / constant-field:**
  `SoftwareConstantImageRoundTrip` — a flat RGB image round-
  trips through the filter virtually unchanged (bilateral on a
  constant field is ~identity).

- **Variance reduction:** `SoftwareReducesVarianceOnNoisyInput`
  — pseudo-random deterministic noise gets filtered to a
  strictly lower variance. The first cut of this test used a
  checkerboard pattern which defeats the joint filter (every
  neighbor has uniform color delta), so Phase Q uses a hash-
  based bipolar generator instead.

- **Optional guide buffers:** `SoftwareAcceptsAlbedoGuide`,
  `SoftwareAcceptsNormalGuide` (constant normal does not
  perturb constant input), `SoftwareWithBothGuidesSucceeds`.

- **Polymorphic dispatch:** `PolymorphicDispatch` — creates a
  `std::unique_ptr<IDenoiser>` holding a `SoftwareDenoiser`,
  runs the full pipeline via the virtual interface, and
  verifies the backend enum, output dimensions.

- **Metrics:** `LastExecutionTimeIsNonNegative` — pre-execute
  time must be valid (>=0).

- **Settings variations:** `QualityPresetsAreAccepted` (Fast
  and High quality presets both succeed), `TemporalSettingAccepted`
  (temporal + custom blend factor work end-to-end).

- **Repeatability:** `RepeatedExecuteProducesSameOutput` —
  running `Execute()` twice on the same input produces bit-
  identical output (deterministic filter).

### Registration

`Tests/CMakeLists.txt` — `TestDenoiserInterface.cpp` registered
alongside the Phase P test file.

## Files touched

```
SparkEngine/Source/Graphics/GraphicsEngine.h               (Q1 — include + member + accessors)
SparkEngine/Source/Graphics/GraphicsEngine.cpp             (Q1 — Windows + Linux lifecycle hooks)
Tests/TestDenoiserInterface.cpp                            (Q1, new — 14 portable tests)
Tests/CMakeLists.txt                                       (Q1)
.claude/knowledge/engine-next-steps-phase-q-2026-04-11.md  (new)
.claude/index.md                                           (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4537 passed, 0 failed, 1 warned**
  (pre-existing `LoadTest_Severe_EntityFlood` known-flaky), total
  120154 assertions (+611 from Phase P's baseline of 119543; the
  +14-test delta is from the new `DenoiserInterface_*` file and
  the large assertion bump reflects the per-pixel checks in the
  constant-image / guide / variance / repeatability tests).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase R

Phase Q closes one more Tier 2 orphan, bringing the running total
across I→Q to **fifteen orphans activated**. Remaining candidates,
in roughly ascending order of scope:

- **`UICompositor.h`** (231 lines) — may be superseded by
  `Engine/UI`. Needs a wire-or-delete audit: grep the existing
  UI path for consumers, compare the class API to the current
  UI rendering surface, decide whether to wire the compositor
  as a UI-side layer or delete the orphan outright. If it's a
  delete, the phase is smaller but still meaningful (removes
  dead code from the audit).
- **`FastNoise2SIMD.h`** (749 lines) — SIMD procedural noise.
  Activation would make it an optional backend alongside the
  existing terrain / foliage noise. Phase R could wire it as an
  opt-in `GraphicsEngine::GetProceduralNoise()` accessor so
  downstream callers can hit the SIMD path when they want it,
  without replacing the existing noise generators.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope. The class is large and tightly coupled
  to a real GI pipeline; activation without an actual GI render
  path would just mean "lifecycle only" like Phase N did for
  `ConstantBufferRing`.

**Tightest fit for Phase R:** `UICompositor` — the wire-or-delete
decision is a single session of work. If wire: follow the Phase M /
Phase P pattern and hook it into an existing UI subsystem's
Initialize / Shutdown. If delete: remove the header, strip any
stale `@see` references in other headers, update the audit entry.

## Design notes for follow-ups

- **Plugin interfaces with bundled fallbacks are one-step
  activations.** `DenoiserInterface.h` ships the abstract
  `IDenoiser` *and* a complete concrete `SoftwareDenoiser` in
  the same header. That pattern — interface + default
  implementation in one file — makes activation a single
  `unique_ptr<IDenoiser>` member with no header reshuffling.
  Future orphan plugin interfaces (render graph passes, asset
  importers, post-process effects) should follow the same
  pattern so the activation surface stays trivial.
- **`std::unique_ptr` is the right ownership model for
  swap-able backends.** Phase Q uses `unique_ptr<IDenoiser>`
  instead of a stack-allocated `SoftwareDenoiser` member
  specifically so a future Phase can replace the pointee with
  an OIDN / OptiX / neural implementation in a one-line diff
  inside `GraphicsEngine::Initialize`. Stack allocation would
  force a member type change for every future backend swap.
- **Duplicated Windows/Linux `.cpp` files need duplicated method
  bodies.** `GraphicsEngine.cpp` has two full `Initialize` /
  `Shutdown` implementations — one inside the `#ifdef
  SPARK_PLATFORM_WINDOWS` block, one inside the `#else` Linux
  stub. Any new method added to the class via the header needs
  a *matching definition in both branches* or the Linux build
  fails with unresolved symbols. Phase Q's `GetDenoiserBackend()`
  has identical one-line bodies in both branches. Future phases
  that add methods to `GraphicsEngine` must do the same.
- **Deterministic noise beats checkerboard patterns in tests.**
  The first cut of `SoftwareReducesVarianceOnNoisyInput` used a
  checkerboard noise pattern, which produced zero variance
  reduction because every neighbor in the bilateral kernel had
  the same uniform color delta — the joint filter's color
  weight killed the smoothing entirely. The fix uses a hash-
  based pseudo-random bipolar generator (deterministic across
  platforms, irregular across neighbors). Future signal-
  processing tests should prefer hash-based PRN over regular
  patterns so the underlying filter behaviour is actually
  exercised.
- **`enum class` values need casts for EXPECT_EQ.** The test
  framework's `EXPECT_EQ` macro streams both operands to
  `std::ostream` for diagnostic output, which fails to compile
  for `enum class` without a user-defined `operator<<`. Phase
  Q worked around this by casting both sides to `int` in the
  four `EXPECT_EQ(..., DenoiserBackend::Software)` call sites.
  Future tests against `enum class` values should use the same
  cast pattern until the test framework grows an ostream shim
  for strongly-typed enums.
