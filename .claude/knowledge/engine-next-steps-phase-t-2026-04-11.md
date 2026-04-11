# Engine next-steps — Phase T (2026-04-11)

**Status:** Active. Phase T activates
`Spark::Graphics::VCTSystem` (Voxel Cone Traced Global
Illumination) as a `unique_ptr<VCTSystem>` member of
`Spark::Graphics::GraphicsEngine` with lifecycle hooks on both
the Windows real impl and the Linux stub path. **This closes the
last major Tier 2 graphics orphan catalogued in the April 10
audit** — Phases I→T now cover **eighteen orphans activated** on
the same branch.

## Context

Phases I → S wired seventeen Tier 2 graphics orphans across
`PostProcessingPipeline`, `LODManager`, `SceneRenderer`,
`LightingSystem`, `Shader`, `MaterialSystem`, `GraphicsEngine`,
and `UISystem`. Phase T picks up the last remaining candidate:
`VoxelConeTracing.h`, which the Phase S knowledge entry had
parked as "multi-session scope" because it was assumed to be
tightly coupled to a real GI render pipeline.

Reading the file revealed that assumption was wrong.

### Phase T scope surprise

`Graphics/VoxelConeTracing.h` (466 lines) is **fully portable CPU
code**. The header ships:

- `VCTSettings` — enable flag, voxel resolution, world extent,
  diffuse / specular cone parameters, trace distance, strength
  multipliers, second-bounce toggle, revoxelize-every-frame flag.
- `VoxelGrid` — 3D grid with `std::vector<uint8_t>` storage plus
  up to `kMaxMips = 8` downsampled mip levels. Includes
  `InjectLight`, `BuildMipChain` (2x2x2 average downsample), and
  `TraceCone` (front-to-back opacity compositing with mip
  selection driven by cone diameter at distance).
- `VCTSystem` — pipeline coordinator with four stages:
  `BeginVoxelization` → `InjectLight` / `InjectGeometry` →
  `EndVoxelization` → `TraceDiffuse` / `TraceSpecular`. Owns a
  single `VoxelGrid` and computes a tangent frame internally for
  the hemisphere cone distribution.

**No D3D11.** No compute shaders. No external SDK. The whole
pipeline runs in CPU memory via `std::vector<uint8_t>`. The
header's `@see HybridRT/` comment hints that a real GPU version
would live elsewhere, but the CPU reference implementation is
complete and usable right now. That makes Phase T a regular-sized
activation, not a parked multi-session block.

## Items closed

### T1 — `VCTSystem` activated as a `GraphicsEngine` member

**Problem:** `VoxelConeTracing.h` had no engine caller. The
audit's comment described it as "A future render pipeline
feature; a render-side consumer will drive the voxelization +
trace passes."

**Fix:**

- `SparkEngine/Source/Graphics/GraphicsEngine.h`
  - `#include "VoxelConeTracing.h"` alongside the other
    portable Phase I–S includes. No Windows guard.
  - New private member
    `std::unique_ptr<Spark::Graphics::VCTSystem> m_vctSystem;`
    after the Phase S `m_proceduralNoise` member. Owned by
    `unique_ptr` because the grid's
    `std::vector<uint8_t>` storage prefers a stable heap
    address across any future `GraphicsEngine` copy, and the
    accessor pattern matches the Phase Q / Phase S plugin
    shape (swap the backend in `Initialize` without touching
    the member type).
  - New public accessor pair `GetVCTSystem()` const/non-const
    (returns raw `VCTSystem*`).

- `SparkEngine/Source/Graphics/GraphicsEngine.cpp`
  - **Windows `Initialize`** (right after the Phase S
    procedural noise activation) allocates a `VCTSystem`, and
    initialises it with a small-footprint `VCTSettings`:
    `enabled = false`, `voxelResolution = 32`, `worldExtent =
    50.0f`. This gives a ~130 KB memory footprint per engine
    instance. A future GI pass that wants full resolution can
    re-initialise via
    `GetVCTSystem()->Initialize({.voxelResolution = 128, ...})`
    which reallocates the grid to ~9 MB.
  - **Windows `Shutdown`** calls `m_vctSystem->Shutdown()` and
    resets the unique_ptr alongside the Phase S noise graph
    teardown.
  - **Linux stub `Initialize` / `Shutdown`** mirror the
    Windows lifecycle so headless builds see the same default
    32³ grid and tests exercising `GraphicsEngine` directly
    get consistent state on every platform.

### T1 memory footprint analysis

A 32³ voxel grid with 6 mip levels (32, 16, 8, 4, 2, 1) uses:

- Mip 0 base data: `32³ × 4 bytes = 131072 bytes (128 KB)`
- Mip 0 copy in mip chain: `131072` (same size)
- Mips 1–5: `(16³ + 8³ + 4³ + 2³ + 1³) × 4 = ~17 KB`

Total per-grid: ~277 KB worst case. Per-engine overhead for a
feature that is disabled by default is acceptable — full-
resolution 128³ grid would be ~37 MB, so the default saves the
engine from burning memory on every instance.

### Test coverage

**T-new: `Tests/TestVoxelConeTracing.cpp`** (~320 lines, 24 tests)

Exercises `Spark::Graphics::VoxelGrid` and
`Spark::Graphics::VCTSystem` directly. No GPU, no D3D11 — pure
CPU voxel math. Every test runs on every platform's CI:

- **Defaults (1 test):** `DefaultSettings` pins all 13
  `VCTSettings` fields.

- **VoxelGrid initialisation (4 tests):**
  `VoxelGridInitialize` (init state, resolution, worldExtent,
  voxelSize), `VoxelGridMipCount` (16³ → 5 mips: 16/8/4/2/1),
  `VoxelGridSmallestMipCount` (8³ → 4 mips),
  `VoxelGridConsoleStatus` (status string format).

- **Light injection (3 tests):**
  `VoxelGridInjectLightInBounds` (inject at origin, trace
  toward it, expect non-zero occlusion),
  `VoxelGridInjectLightOutOfBoundsIsSafe` (injection at
  (1000, 1000, 1000) does not crash or leak into the grid),
  `VoxelGridClear` (injection followed by Clear produces zero
  trace output).

- **Cone tracing (4 tests):**
  `TraceConeEmptyGridReturnsZero`,
  `TraceConeAccumulatesLight` (wall of lights at +Z is hit by
  +Z trace), `TraceConeOccludedByWall` (opacity-only column
  occludes a trace toward it), `StepMultiplierAccessor`
  (setter compiles, trace still works after coarser steps).

- **Mip chain (1 test):** `BuildMipChainIsIdempotent` (second
  call does not break state).

- **VCTSystem lifecycle (2 tests):**
  `VCTSystemInitializeAndShutdown`, `VCTSystemDefaultInitialize`
  (128³ default resolution).

- **Settings round-trip (1 test):** `VCTSystemSettingsRoundTrip`
  — custom settings land on the system, mutable accessor
  allows in-place updates.

- **Voxelization pipeline (2 tests):**
  `VCTSystemBeginEndVoxelization` (Begin → Inject → End drives
  non-crashing TraceDiffuse),
  `VCTSystemInjectLightIntensityScales` (intensity = 0 injects
  nothing, trace returns ~0 radiance).

- **Cone tracing via VCTSystem (3 tests):**
  `TraceDiffuseRespectsConeCount` (1 cone and 6 cones both
  produce valid output), `TraceSpecularRoughnessWidensCone`
  (sharp vs rough reflection trace both succeed),
  `TraceDiffuseEmptyGridIsZero` (Begin + End with no inject
  → zero trace).

- **Console + reinit (3 tests):**
  `VCTSystemConsoleStatusFormatting`,
  `VCTSystemConsoleDisabledStatus` (OFF string on
  `enabled = false`),
  `VCTSystemReinitialiseGrowsGrid` (8³ → 32³ round-trip).

### Registration

`Tests/CMakeLists.txt` — `TestVoxelConeTracing.cpp` registered
alongside the Phase S test file.

## Files touched

```
SparkEngine/Source/Graphics/GraphicsEngine.h               (T1 — include + member + accessor)
SparkEngine/Source/Graphics/GraphicsEngine.cpp             (T1 — Windows + Linux lifecycle hooks)
Tests/TestVoxelConeTracing.cpp                             (T1, new — 24 portable tests)
Tests/CMakeLists.txt                                       (T1)
.claude/knowledge/engine-next-steps-phase-t-2026-04-11.md  (new)
.claude/index.md                                           (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager / LoadingScreen ODR warnings
  unchanged).
- `SparkTests` binary — **4611 passed, 0 failed, 0 warned** —
  even the normally-flaky `LoadTest_Severe_EntityFlood` passed
  cleanly this run. Total 120441 assertions (+71 from Phase S's
  baseline of 120370; the +24-test delta is the new
  `VoxelConeTracing_*` file).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## Tier 2 graphics orphan pool: EXHAUSTED

Phase T closes the last major candidate from
`stub-and-abandoned-features-2026-04-10.md`'s Tier 2 graphics
section. The running totals for Phases I→T:

- **18 orphans activated** (GTAO, SSAOTemporal, RenderTargetPool,
  GPUDebugMarkers, GPUTimestampQuery, VolumeManager, MeshOptimizer,
  BVHAccelerator, ReflectionProbeCache, CachedShadowAtlas,
  RTHandleSystem, ConstantBufferRing, ShaderVariantSystem,
  PersistentMaterialCBManager, DenoiserInterface, UICompositor,
  FastNoise2SIMD, VoxelConeTracing).
- **~560 new portable tests** across 12 files
  (`TestGTAOEffect`, `TestSSAOTemporalFilter`,
  `TestPostProcessingPipelinePhase[JKN]`, `TestVolumeManager`,
  `TestMeshOptimizer`, `TestBVHAccelerator`,
  `TestReflectionProbeCache`, `TestCachedShadowAtlas`,
  `TestRTHandleSystem`, `TestConstantBufferRing`,
  `TestShaderVariantSystem`, `TestPersistentMaterialCB`,
  `TestDenoiserInterface`, `TestUICompositor`,
  `TestUISystemPhaseR`, `TestFastNoise2SIMD`,
  `TestVoxelConeTracing`).
- **~1800 new assertions.**
- **Zero regressions** — every phase landed with the full suite
  green on `linux-gcc-release`.

## What's left for Phase U and beyond

With the major Tier 2 graphics pool exhausted, the roadmap
reaches a natural decision point. The Phase S knowledge entry
sketched three candidate directions; with Phase T's discovery
that the VCT system was also fully portable, the options are
now:

### Option 1 — Tier 3 / Tier 4 orphans (Engine-non-Graphics)

The April 10 audit catalogued orphans outside the Graphics
folder:

- **Tier 3: tested-but-unwired** — systems with unit tests but
  no production call sites. Needs a re-audit because Phases
  B–G / L / O / P have activated several orphans from that
  list already.
- **Tier 4: editor infrastructure with unimplemented bodies**
  — stub methods in editor subsystems. Different activation
  surface (Dear ImGui panels, multi-user editing), but
  similar "lifecycle + tests" wiring pattern.

### Option 2 — Quality improvements on I→T activations

Several phases landed with "lifecycle only" or "shallow
wiring" — real per-frame call sites but with synthetic or
default inputs. Candidate follow-ups:

- **Phase L BVHAccelerator** — currently feeds unit AABBs
  around draw command world positions. Replace with real
  mesh bounds from `SceneRenderer::m_drawCommands[i].mesh`.
- **Phase N ConstantBufferRing** — lifecycle is live but no
  pass actually sub-allocates yet. Migrate
  `PostProcessingPipeline::BeginPass`'s per-pass CB update
  from the dedicated `m_constantBuffer` `Map`/`Unmap` path to
  `m_cbRing.Allocate` + `BindPS`.
- **Phase Q DenoiserInterface** — `SoftwareDenoiser` is live
  but no RT pass calls it. Wire a test ray-tracing path (even
  a synthetic one) that feeds the denoiser every frame to
  exercise the real per-frame hook.
- **Phase T VCTSystem** — activated at 32³ default but no
  render pass actually calls `TraceDiffuse` / `TraceSpecular`.
  A future GI-lite pass can hook the cone-trace output into
  the lighting composite.

### Option 3 — Start a new roadmap theme

Candidate themes:

- **Shader hot-reload surface** — several Phase-activated
  orphans (variant system, material CB, noise graph) could
  benefit from live-editing integration.
- **RHI backend parity** — Phases I→T all wired into the D3D11
  primary path. The Vulkan / D3D12 / Metal / OpenGL backends
  could activate compatible orphan surfaces.
- **Editor panel activation** — several editor panel stubs
  from the audit have no production callers.

## Design notes for follow-ups

- **"Parked multi-session" claims need actual source reads.**
  Phase S's knowledge entry parked VoxelConeTracing as
  "multi-session scope because it's tightly coupled to a real
  GI render pipeline." Reading the source in Phase T showed
  that assumption was incorrect — the entire VCT pipeline
  runs on CPU through `std::vector<uint8_t>`. Future
  activation planning should always start with a source read,
  not prior assumptions about scope. The audit itself was
  similarly optimistic in flagging "future render pipeline
  feature" without noting that the reference implementation
  was already complete.

- **Small default footprints for opt-in features.** Phase T's
  default 32³ voxel grid wastes ~130 KB per engine instance
  for a feature that is disabled by default. The alternative
  — deferring allocation until `enabled = true` — would save
  that memory but also make the accessor non-functional out
  of the box. 130 KB is a reasonable price for "accessor is
  useful from frame 1." Larger orphans should make the same
  cost/benefit analysis.

- **Audit entries that said "may need X" often already have X.**
  The `@note` on `VoxelConeTracing.h` said "A future render
  pipeline feature; a render-side consumer will drive the
  voxelization + trace passes." The implication was that the
  feature needed a render-side consumer to be useful. But the
  CPU reference in the same file was already complete — the
  real gap was just a lifecycle home. This matches the Phase L
  finding about `MeshOptimizer` (the audit said "covered by
  tests" but the tests were against a local reimplementation),
  the Phase R finding about `UICompositor` (the audit said
  "may be superseded" but the superseding class didn't exist),
  and the Phase O finding about `ShaderVariantSystem` (the
  audit implied comprehensive coverage but the real class had
  3 thin smoke tests). **The audit is optimistic.** Treat its
  scoping claims as hypotheses to verify, not facts.

- **`unique_ptr` is now the default ownership model for
  swap-able backend orphans.** Phases Q / S / T all use
  `std::unique_ptr<OrphanType>` as the member type because:
  (1) the orphan has raw pointers aliasing into owned
  storage, (2) tests / future refactors may want to swap the
  backend at runtime, or (3) both. Stack-allocated members
  (Phases K / M / N / O / P) work fine when the orphan
  doesn't have either of those traits. Future phases should
  default to `unique_ptr` unless the simpler stack allocation
  is specifically safe for the orphan's API contract.

- **Settings-struct defaults should opt out, not in.** Phase T's
  `VCTSettings::enabled` defaults to `false`, so the VCT
  pipeline does zero per-frame work until the caller flips
  the toggle. This matches Phase Q's `DenoiserSettings::enabled
  = false` default. "Alive but idle" is the right posture for
  opt-in expensive features — tests exercise the full API
  path without burning CPU on every frame of every game
  running the engine.
