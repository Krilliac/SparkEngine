# Engine next-steps — Phase I (2026-04-11)

**Status:** Active. Phase I wires the first Tier 2 graphics orphan
(`GTAOEffect`) into the real render path as the 15th post-process pass.

## Context

Phases A–G closed out the foliage render pipeline, DXR wiring, editor
SelectionManager migration, and the DXC/FXC shader compile steps for the
foliage and ray-tracing families. The in-flight Phase H track (unified
shader compile coverage, foliage normal/ORM PBR, SteamTransport deletion,
NetworkDebugPanel producer wiring) is being handled separately.

Phase I picks up with the first concrete step in the much larger effort
to convert the 25-file Tier 2 graphics orphan library from "documented
intentional utility" into actively wired render features. `GTAOEffect.h`
was the obvious first target: it is self-contained, has no dependencies
on other orphans, ships a CPU reference implementation (`ComputeGTAO`)
that doubles as a test oracle, and slots cleanly into the existing
`PostProcessingPipeline` pattern that already hosts 14 sibling effects.

## Items closed

### I1 — Pipeline plumbing

**Problem:** `PostProcessingPipeline` had 14 passes wired; `GTAOEffect`
existed as a 447-line header-only utility with zero callers.

**Fix (3 files, ~90 lines):**

- `SparkEngine/Source/Graphics/PostProcessingTypes.h` — included
  `GTAOEffect.h` to pull in `GTAOSettings` without duplicating its POD
  definition, added `GTAO` as the first entry in `PostProcessPass` so AO
  modulates scene lighting at HDR resolution before Bloom extracts
  highlights. The header comment documents the ordering rationale.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.h` — added
  `GTAOSettings m_gtaoSettings`, `GetGTAOSettings() / const` accessor
  pair matching the other 13 accessors, and `ComPtr<ID3D11PixelShader>
  m_gtaoPS`.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp`:
  - `Shutdown()` resets `m_gtaoPS` alongside the other pixel shaders.
  - `GetPassMetrics()` static `passNames[]` array was reordered to
    match the new enum order with `GTAO` as the first element.
  - `CompileEffectShaders()` gains an inline GTAO pixel shader literal
    (matches the pattern used by all 14 other effects — consistent with
    the rest of the file rather than introducing a new
    `D3DCompileFromFile` path). The shader is added to the `ShaderDef
    shaders[]` compile table as the first entry.
  - `ProcessPass()` gains a `case PostProcessPass::GTAO:` that packs
    the 8 `GTAOSettings` fields plus a conservative default `projScale`
    (`1 / tan(30°) ≈ 1.732`) into the 4-float4 `PostProcessCB`. The
    default `projScale` is used until a camera feed is wired, mirroring
    the DOF/MotionBlur pattern.

### I2 — GTAO pixel shader

**Problem:** `GTAOEffect::GetHLSLShaderSource()` returns a compute
shader (RWTexture2D UAV output) that is not directly compatible with
`PostProcessingPipeline`'s PS + ping-pong RTV pattern.

**Fix (~95 lines in one new file + matching inline literal):**

- `Shaders/HLSL/GTAOPS.hlsl` (new) — pixel shader variant of the
  horizon-based AO algorithm. Samples `sceneTexture @ t0` and
  `depthTexture @ t1`, reconstructs view-space normals via depth
  derivatives (no separate normal buffer required for the first cut),
  marches `directions × stepsPerDirection` horizon samples, and outputs
  `float4(scene.rgb * ao, 1)` so the pass is a self-contained
  multiplicative composite with no external stage.

- The runtime path uses the identical source embedded as a string
  literal inside `CompileEffectShaders`, matching every other effect.
  The standalone `.hlsl` file exists so a future build-time shader
  compile step (Phase D DXC, Phase G FXC, or Phase H unified coverage)
  can validate the source offline and catch typos before runtime.

### I3 — Tests

**Problem:** `GTAOEffect` had no test file despite shipping a complete
CPU reference implementation perfect for headless validation.

**Fix (`Tests/TestGTAOEffect.cpp`, ~180 lines, 8 new tests):**

- `GTAO_DefaultSettings` — pins all 8 POD field defaults against the
  documented values so a future refactor of `GTAOSettings` trips a test
  before the CB-pack in `ProcessPass(GTAO)` diverges.
- `GTAO_InitializeAndShutdown` — lifecycle round-trip, verifies the AO
  buffer sizes up to `width × height` and releases on shutdown.
- `GTAO_InitializeRejectsBadDimensions` — 0, too-large, and boundary
  rejection (catches the `> 16384` guard in the header).
- `GTAO_OpenSkyFlatDepth_HasHighAO` — flat depth patch, flat normals,
  asserts interior AO > 0.5 (fully-unoccluded open-sky scene).
- `GTAO_ZeroDepth_IsFullyLit` — all-zero depth path hits the early-out
  branch and every pixel must be AO = 1.0.
- `GTAO_WallOccluder_LowersNearbyAO` — left-half wall vs. right-half
  far plane; pixels adjacent to the wall must have strictly ≤ AO than
  pixels at the far edge, proving horizon occlusion is detected.
- `GTAO_SpatialDenoise_PreservesConstantField` — the 3×3 cross-bilateral
  denoiser must not change a constant-value AO buffer (weight symmetry
  verification).
- `GTAO_HLSLSourcePointerShape` — the static compute shader source
  string must be non-null, non-trivial length, and contain `CSMain`,
  `numthreads`, and `GTAOConstants` — the contract for any future
  build-time shader walker.

Registered in `Tests/CMakeLists.txt` alongside the other foliage tests.

### I4 — Editor panel: deliberately out of scope

The plan proposed a GTAO UI block in `PostProcessingPanel.cpp`. On
inspection that panel operates on `Scene::environment`, not on
`PostProcessingPipeline` directly, so exposing GTAO there would have
required new fields on the scene environment struct — out of Phase I
scope. Consistent with many of the existing 14 pipeline effects that
similarly have no editor UI, Phase I ships without touching
`PostProcessingPanel.cpp`. The pass is still controllable
programmatically via `pipeline.SetEffectEnabled(PostProcessPass::GTAO,
true)` and `pipeline.GetGTAOSettings()`.

## Files touched

```
SparkEngine/Source/Graphics/PostProcessingTypes.h      (I1)
SparkEngine/Source/Graphics/PostProcessingPipeline.h   (I1)
SparkEngine/Source/Graphics/PostProcessingPipeline.cpp (I1 + inline shader for I2)
Shaders/HLSL/GTAOPS.hlsl                               (I2, new)
Tests/TestGTAOEffect.cpp                               (I3, new)
Tests/CMakeLists.txt                                   (I3 registration)
.claude/knowledge/engine-next-steps-phase-i-2026-04-11.md (new)
.claude/index.md                                       (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --config Release` — clean
  (SparkEngine + SparkTests). Only pre-existing ODR warnings
  (`InputManager`, `DebugHookManager`) appear, untouched by Phase I.
- `ctest` — **4370 passed, 0 failed, 1 warned** (pre-existing flaky
  `LoadTest`), total 118831+ assertions — up 17 from Phase G's 4353
  (8 new GTAO tests + 9 from unrelated upstream work).
- `clang-format` — clean on all changed code files.
- The new `Shaders/HLSL/GTAOPS.hlsl` is picked up automatically by the
  existing "top-level Shaders/HLSL/ tree" copy rule from Phase C.

## What the next session can pick up

Phase I deliberately touches one orphan. The remaining 24 Tier 2
graphics utilities are unchanged; the pattern Phase I establishes is:

1. Pick one utility with a CPU reference (or add one).
2. Wire into the existing subsystem that owns the pattern it belongs
   to (render pass → `PostProcessingPipeline`, culling → scene render
   path, GPU query → pipeline metrics, etc.).
3. Add a test file that exercises the CPU reference first; the real
   GPU device path stays in the Windows-only integration tests.
4. Avoid batch refactors — one utility per phase keeps the blast
   radius small.

Priority order (from highest immediate value):
- `GPUTimestampQuery.h` — wire per-pass GPU timing into
  `m_passTimings` which is currently CPU-timed only. Affects all 15
  passes, not just GTAO. Small scope, high value.
- `RenderTargetPool.h` — pooled allocation for transient render
  targets. Replaces ping-pong allocation; benefits every subsystem
  that uses transient targets, not just post-process.
- `SSAOTemporal.h` — temporal reprojection variant. Could eventually
  replace or complement GTAO.
- `BVHAccelerator.h` — hierarchical scene culling.

Each is its own phase — do not batch.

## Non-goals that stayed non-goals

- Real G-buffer normals (first cut uses `ddx`/`ddy` reconstruction).
- Temporal GTAO reprojection (separate utility).
- Replacing the existing SSAO (GTAO is additive, not replacement).
- Editor panel UI (scene-env architecture out of scope).
- Running the GPU pixel shader in CI (remains Windows-integration
  territory).
