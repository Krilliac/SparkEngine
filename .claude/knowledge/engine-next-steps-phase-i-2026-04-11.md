# Engine next-steps — Phase I (2026-04-11)

**Status:** Active. Wires the first of the 25-file Tier 2 graphics orphan
library into the actual render pipeline. Builds on Phases A–G which
closed out foliage rendering, DXR wiring, SelectionManager migration, and
the DXC/FXC shader compile steps.

## Item closed

### I1 — GTAO activated as the 15th post-process pass

**Problem:** `SparkEngine/Source/Graphics/GTAOEffect.h` (447 lines) was
a documented intentional-utility orphan from the April 10 audit
(`stub-and-abandoned-features-2026-04-10.md`). It ships a full CPU
reference implementation (`ComputeGTAO` + `SpatialDenoise`) plus an
embedded HLSL compute shader string via `GetHLSLShaderSource()` — all
working code with zero engine callers. Phase I converts it from "library
for future use" into an actively wired effect in
`PostProcessingPipeline`.

**Fix (6 files, ~250 lines):**

- `SparkEngine/Source/Graphics/PostProcessingTypes.h`
  - Added `#include "GTAOEffect.h"` so `GTAOSettings` flows through the
    canonical types header.
  - Added `GTAO` as the **first** entry of the `PostProcessPass` enum —
    the pipeline iterates in declaration order, so this slot executes
    before `Bloom`, which is the correct phase for an AO term (AO
    modulates HDR lighting before bloom extracts highlights).

- `SparkEngine/Source/Graphics/PostProcessingPipeline.h`
  - New `GTAOSettings m_gtaoSettings` member alongside the other 13
    settings structs.
  - `GetGTAOSettings() / const` accessor pair mirroring the existing
    13 accessors.
  - New `ComPtr<ID3D11PixelShader> m_gtaoPS` member.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp`
  - `Shutdown()` resets `m_gtaoPS` alongside the other effect shaders.
  - `GetPassMetrics()`: `"GTAO"` prepended to the `passNames` array so
    the enum/index/name triple stays aligned.
  - `CompileEffectShaders()`: new inline HLSL literal `gtaoPS` matching
    the existing 14-effect pattern — horizon-based GTAO with per-pixel
    `ddx/ddy`-reconstructed view normals and multiplicative scene
    composite (`float4(scene * ao, 1)`). Added as the first entry of
    the `ShaderDef shaders[]` compile table.
  - `ProcessPass(PostProcessPass::GTAO, dt)` case populates a
    `PostProcessCB` from `m_gtaoSettings`. Directions/steps are
    clamped to `[1, 16]` to guard the `[loop]` bounds. Uses a
    conservative `projScale = 1.732f` (1 / tan(30°), ≈60° vertical
    FOV) until a camera-feed wire lands in a later phase.

- `Shaders/HLSL/GTAOPS.hlsl` (new) — standalone `.hlsl` file with the
  exact same source as the inline literal. Shipped so the eventual
  unified shader compile step (Phase H's named Phase-H work) can
  validate it offline before it reaches a Windows runtime. The runtime
  path continues to use the inline literal — consistent with all 14
  sibling effects. This file is a validation oracle, not a runtime
  dependency.

- `Tests/TestGTAOEffect.cpp` (new, 8 tests, ~180 lines) — exercises the
  real `Spark::Graphics::GTAOEffect` class (not a reimplementation):
  - `GTAO_DefaultSettings` — pins the 8 `GTAOSettings` POD defaults
    so future drift gets caught before it breaks the pipeline CB.
  - `GTAO_InitializeAndShutdown` — happy-path lifecycle + AO buffer
    size.
  - `GTAO_InitializeRejectsBadDimensions` — zero and too-large
    dimensions are rejected.
  - `GTAO_OpenSkyFlatDepth_HasHighAO` — flat depth field → high AO.
  - `GTAO_ZeroDepth_IsFullyLit` — sky pixels early-out to AO = 1.0.
  - `GTAO_WallOccluder_LowersNearbyAO` — occluder lowers AO for
    adjacent pixels vs. far-from-wall pixels.
  - `GTAO_SpatialDenoise_PreservesConstantField` — bilateral denoiser
    does not move a constant field.
  - `GTAO_HLSLSourcePointerShape` — `GetHLSLShaderSource()` returns
    non-null, ≥100 chars, contains `CSMain`, `numthreads`,
    `GTAOConstants`.

- `Tests/CMakeLists.txt` — registers `TestGTAOEffect.cpp` alongside
  `TestFoliageRenderer.cpp` in the Tests source list.

## Explicitly NOT in Phase I (deferred deliberately)

- **Editor UI for GTAO.** The existing `SparkEditor/Source/Panels/
  PostProcessingPanel.cpp` operates on `Scene::environment`, not on
  `PostProcessingPipeline::Get*Settings()`. Adding a GTAO UI block
  there would require new `Scene::environment` fields, a scene format
  bump, and serializer changes — well out of Phase I scope. All 14
  existing pipeline-level effects are in the same situation; the
  scene-environment panel only exposes bloom/tonemap/fog/sky/wind.
- **Real per-pixel view-space normals.** The first cut reconstructs
  normals from depth derivatives (`ddx/ddy`). A G-buffer normal feed
  belongs in a future deferred-shading migration.
- **Temporal reprojection.** `SSAOTemporal.h` is a separate Tier 2
  orphan covering that; it lands in its own phase if needed.
- **Camera-feed `projScale`.** Hard-coded to `1.732f` (~60° vertical
  FOV) in `ProcessPass(GTAO)`. A future phase will wire the live
  camera FOV through `PostProcessingPipeline::SetDepthSRV` or a new
  `SetCameraParams` hook.

## Files touched

```
SparkEngine/Source/Graphics/PostProcessingTypes.h       (I1)
SparkEngine/Source/Graphics/PostProcessingPipeline.h    (I1)
SparkEngine/Source/Graphics/PostProcessingPipeline.cpp  (I1)
Shaders/HLSL/GTAOPS.hlsl                                (I1, new)
Tests/TestGTAOEffect.cpp                                (I1, new)
Tests/CMakeLists.txt                                    (I1)
.claude/knowledge/engine-next-steps-phase-i-2026-04-11.md (new)
.claude/index.md                                        (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager ODR warnings unchanged).
- `SparkTests` binary — **4370 passed, 0 failed, 1 warned** (pre-existing
  `LoadTest_FullEngine_3000Frames` known-flaky), total 118997 assertions
  (up from 118831 — +166 from Phase I).
- `clang-format --dry-run --Werror` on all touched files — clean.
- 8 GTAO tests verified passing via direct-binary filter:
  `GTAO_DefaultSettings`, `GTAO_InitializeAndShutdown`,
  `GTAO_InitializeRejectsBadDimensions`,
  `GTAO_OpenSkyFlatDepth_HasHighAO`, `GTAO_ZeroDepth_IsFullyLit`,
  `GTAO_WallOccluder_LowersNearbyAO`,
  `GTAO_SpatialDenoise_PreservesConstantField`,
  `GTAO_HLSLSourcePointerShape`.

## What's left for Phase J

Phase I is the first of 25 Tier 2 orphan-activation phases. Candidates
that can follow directly:

- **`SSAOTemporal.h`** — same post-process slot as GTAO, adds temporal
  reprojection. Could replace or complement GTAO for moving cameras.
- **`GPUTimestampQuery.h`** — replace `PostProcessingPipeline::
  m_passTimings[]` (currently CPU timed via `std::chrono`) with real
  GPU timestamp query data. Benefits all 15 passes at once.
- **`RenderTargetPool.h`** — replace the hardcoded 2-entry ping-pong
  allocation with a pooled allocator. Benefits every system that uses
  transient RTs, not just post-process.
- **`BVHAccelerator.h`** — hierarchical frustum / ray culling. Feeds the
  scene renderer, not post-process — a different integration surface.
- **`VoxelConeTracing.h`** — real-time GI via voxel cones. Large scope,
  likely multi-session, parked until GI becomes a priority.

None of these are blocked on Phase I. Any of them can be picked up
next. The I1 pattern (inline literal + standalone `.hlsl` file +
CPU-reference tests against the real header) is reusable verbatim for
any post-process orphan activation.

## Design notes for follow-ups

- **Enum ordering is execution ordering.** Adding passes to
  `PostProcessPass` is order-sensitive — the `Process()` loop iterates
  `0..Count-1` in declaration order, so a new pass slots into the
  execution chain at its enum position. AO goes first (HDR-lighting
  modulator). New effects should audit the enum position against the
  HDR → tonemap → LDR → present phase they belong in.
- **The CPU reference path is Phase I's test oracle.** `GTAOEffect`
  was a particularly easy first orphan to activate because it shipped
  its own CPU reference. Orphans that don't have one (e.g.
  `BVHAccelerator`) will need either a CPU reference built alongside
  the activation, or device-dependent integration tests that run only
  on Windows.
- **Inline literal vs standalone `.hlsl` file.** The rest of the
  pipeline uses inline literals for reasons of self-containment (one
  file = one effect including its shader). Phase I keeps that pattern
  but additionally drops a `Shaders/HLSL/GTAOPS.hlsl` copy for
  offline validation. Keep both in sync if the shader is ever edited.
