# Engine next-steps — Phase K (2026-04-11)

**Status:** Active. Phase K activates `Spark::Graphics::VolumeManager`
(the Unity-style spatial post-process volume system from the April 10
audit) as a live member of `PostProcessingPipeline`, with per-frame
`Update()` from `Process()` and an `ApplyVolumeStack()` binding that
pushes only genuinely-overridden fields into the existing effect
settings structs.

## Context

Phase I wired `GTAOEffect` as the 15th post-process pass. Phase J
added four more Tier 2 orphans (`SSAOTemporal`, `RenderTargetPool`,
`GPUDebugMarkers`, `GPUTimestampQuery`) on the same integration
surface, taking the pipeline to 16 passes with per-pass GPU markers
and timestamps. Phase K continues that narrative with the next
orphan that clearly belongs on the PostProcessingPipeline:
`VolumeSystem.h` — a 464-line header-only system that models
Unity-style post-process volumes (global + local with AABB + blend
distance) and blends exposure, bloom, color grading, and fog
parameters based on camera position.

`VolumeSystem.h` was catalogued in
`stub-and-abandoned-features-2026-04-10.md` as a Tier 2 "documented
intentional utility." Its CPU code was complete and shipped with
coverage via `TestGraphicsIntegration.cpp`, but no engine subsystem
instantiated it — no volumes meant no volume-driven parameter
blending, period. Phase K fixes that by giving the pipeline its own
`VolumeManager`, feeding it the camera position, and wiring the
blended stack into the live settings.

## Items closed

### K1 — `VolumeManager` activated as a per-pipeline spatial volume system

**Problem:** `Graphics/VolumeSystem.h` is a 464-line header with four
`VolumeComponent` subtypes (Exposure, Bloom, ColorGrading, Fog), a
`Volume` definition (global + local AABB with blend distance and
priority), and a `VolumeManager` that evaluates all active volumes
at a camera position and produces a blended `VolumeStack`. No engine
code ever instantiated it.

**Fix:**

- `SparkEngine/Source/Graphics/PostProcessingPipeline.h`
  - Unconditionally `#include "VolumeSystem.h"` alongside
    `SSAOTemporal.h`. Pure CPU — no `SPARK_PLATFORM_WINDOWS` guard.
  - New members:
    - `VolumeManager m_volumeManager` — lifecycle-owned by the pipeline.
    - `XMFLOAT3 m_cameraPosition = {0, 0, 0}` — the single signal the
      volume blend needs.
    - `bool m_volumeBlendEnabled = true` — lets callers run the volume
      evaluator without mutating the live pipeline settings (useful
      for preview/debug panels).
  - New public surface:
    - `GetVolumeManager()` const/non-const accessor pair.
    - `SetCameraPosition(const XMFLOAT3&)` + `GetCameraPosition()`.
    - `SetVolumeBlendEnabled(bool)` + `IsVolumeBlendEnabled()`.
    - `ApplyVolumeStack()` — documented public method so tests can
      exercise the binding in isolation.

- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp`
  - `Initialize()` calls `m_volumeManager.Initialize()` after the
    SSAOTemporal filter boot, unconditionally.
  - `Shutdown()` calls `m_volumeManager.Shutdown()` alongside the
    SSAOTemporal teardown.
  - `Process()` calls `m_volumeManager.Update(m_cameraPosition)` once
    per frame *before* any pass runs. If `m_volumeBlendEnabled` is
    true (the default), it then calls `ApplyVolumeStack()`. The
    evaluator always runs so query-only debug panels see the live
    stack even when the pipeline isn't writing it back to settings.
  - New `ApplyVolumeStack()` body maps the four volume component
    types onto the existing effect settings structs:
    - `stack.exposure.compensationEV` → `m_autoExposureSettings.compensationEV`
    - `stack.bloom.{intensity,threshold,softKnee,scatter}` →
      `m_bloomSettings.{intensity,threshold,softThreshold,scatter}`
    - `stack.colorGrading.{liftR/G/B, gainR/G/B, saturation, contrast, temperature, tint}`
      → `m_colorGradingSettings.{lift.xyz, gain.xyz, saturation, contrast, temperature, tint}`
    - `stack.fog.*` — no fog pass yet; component evaluated but its
      fields are intentionally dropped until a fog pass lands.
  - **Critical:** every field is gated on `stack.*.overrideState` so
    unoverridden fields leave the pipeline settings untouched. This
    lets callers hand-author settings that survive volume-less
    frames.

### K2 — `VolumeComponent::Interpolate` now propagates `overrideState`

**Problem:** The first `ApplyVolumeStack` implementation blindly
copied *every* field of the blended stack into the pipeline settings,
which meant that on a frame with a local volume that only overrode
`bloom.intensity`, the default values of `bloom.threshold`,
`bloom.softKnee`, `bloom.scatter`, `colorGrading.*`, `exposure.*`,
etc. would clobber the caller's hand-authored values. The test
`PhaseK_LocalVolumeBindsOnlyNearCamera` caught this: moving the
camera far from a local bloom volume reset the default intensity to
the `BloomVolumeComponent` default of 1.0 rather than keeping the
pipeline's own default.

Root cause: `VolumeComponent::Interpolate()` only updated `value`,
never `overrideState`. After `VolumeManager::Update()` the blended
stack had no way to say "this field was touched by a volume" vs
"this field is still at its component-constructor default."

**Fix:** All four `Interpolate` methods (`ExposureVolumeComponent`,
`BloomVolumeComponent`, `ColorGradingVolumeComponent`,
`FogVolumeComponent`) now set `overrideState = true` on every field
they lerp. `ColorGradingVolumeComponent::Interpolate` was tightened
with a local `SPARK_VOLUME_BLEND` helper macro to keep the 13 fields
readable. `ApplyVolumeStack` then skips any field whose
`overrideState` is still false — the pipeline settings only change
for fields a volume actually touched.

This is a correctness fix to the orphan header, not a "make it
compile" hack. The header is now semantically correct for any
consumer, not just Phase K.

## Tests

### K-new: `Tests/TestVolumeManager.cpp` (11 tests, ~200 lines)

Exercises the real `Spark::Graphics::VolumeManager` class directly —
no pipeline, no GPU. The tests pin the blend math so the pipeline
binding stays well-defined even if `VolumeSystem.h` is ever
refactored:

- `VolumeManager_InitializeAndShutdown` — lifecycle happy path.
- `VolumeManager_CreateAndRemoveVolume` — create/find/remove by name.
- `VolumeManager_UpdateBeforeInitializeIsSafe` — `Update()` early-outs
  when not initialised; no crash.
- `VolumeManager_GlobalVolumeAlwaysContributes` — a global volume
  applies even at `(1000, 1000, 1000)`.
- `VolumeManager_LocalVolumeInsideBoundsApplies` — AABB at origin +
  camera at origin → fully inside.
- `VolumeManager_LocalVolumeOutsideBoundsDoesNotApply` — tiny blend
  distance + far camera → no contribution, stack stays at default.
- `VolumeManager_LocalVolumeBlendDistanceSmoothsEdges` — camera half
  a blend-distance outside produces a strictly-fractional blend.
- `VolumeManager_HigherPriorityOverridesLower` — priority order
  resolves overlapping globals deterministically.
- `VolumeManager_NonOverriddenFieldsPassThrough` — color-grading
  volume overrides saturation only; contrast stays at the default.
- `VolumeManager_FogComponentBlends` — fog density + heightFalloff
  both land on the stack.
- `VolumeManager_Console_GetStatusListsAllVolumes` — status string
  includes all volume names and global/local tags.

### K-new: `Tests/TestPostProcessingPipelinePhaseK.cpp` (11 tests, ~220 lines)

Integration tests against the real `PostProcessingPipeline` — these
cover the portable accessor surface and run on Windows, Linux, and
macOS CI without a GPU:

**Lifecycle:**
- `PhaseK_VolumeManager_InitializedWithPipeline` — manager follows
  pipeline Initialize/Shutdown.
- `PhaseK_CameraPosition_Defaults` — starts at `(0, 0, 0)`.
- `PhaseK_SetCameraPosition_IsReadBack` — position setter+getter round-trip.
- `PhaseK_VolumeBlendEnabled_DefaultIsTrue` — blend flag defaults on,
  and toggles cleanly.

**ApplyVolumeStack binding:**
- `PhaseK_ApplyVolumeStack_BloomIntensityLandsOnSettings` — bloom
  intensity/threshold/scatter all land on `m_bloomSettings`.
- `PhaseK_ApplyVolumeStack_ColorGradingLiftLandsOnSettings` — 7-field
  color grading round-trip (lift triplet + saturation + contrast +
  temperature + tint).
- `PhaseK_ApplyVolumeStack_ExposureCompensationLandsOnSettings` —
  exposure compensation EV lands on auto-exposure settings.

**Process() drives the blend once per frame:**
- `PhaseK_ProcessDrivesVolumeBlend` — calling `Process()` updates the
  blend automatically.
- `PhaseK_ProcessRespectsVolumeBlendEnabled` — when blend is
  disabled, the pipeline's hand-authored settings survive a
  `Process()` call with an active volume.
- `PhaseK_VolumeStackEvaluatedEvenWhenBlendDisabled` — the manager's
  own stack still reflects the blended state so query-only callers
  see the right value.

**Spatial behaviour:**
- `PhaseK_LocalVolumeBindsOnlyNearCamera` — far camera leaves the
  pipeline defaults alone; moving the camera into the AABB applies
  the volume's intensity override.

### Registration

`Tests/CMakeLists.txt` — both new files registered alongside the
Phase J test files.

## Files touched

```
SparkEngine/Source/Graphics/PostProcessingPipeline.h       (K1)
SparkEngine/Source/Graphics/PostProcessingPipeline.cpp     (K1)
SparkEngine/Source/Graphics/VolumeSystem.h                 (K1, K2 — +<memory> include, Interpolate overrideState fix)
Tests/TestVolumeManager.cpp                                (K1, K2, new)
Tests/TestPostProcessingPipelinePhaseK.cpp                 (K1, new)
Tests/CMakeLists.txt                                       (K1)
.claude/knowledge/engine-next-steps-phase-k-2026-04-11.md  (new)
.claude/index.md                                           (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --target SparkEngine` — clean
  (pre-existing InputManager ODR warnings unchanged).
- `cmake --build build/linux-gcc-release --target SparkTests` — clean
  (pre-existing DebugHookManager/LoadingScreen ODR warnings unchanged).
- `SparkTests` binary — **4418 passed, 0 failed, 0 warned** (the
  normally-flaky `LoadTest_Severe_EntityFlood` also passed this run),
  total 119075 assertions (+54 from Phase J baseline of 119021;
  the +22-test delta is from 11 `VolumeManager_*` CPU tests + 11
  `PhaseK_*` integration tests).
- `clang-format --dry-run --Werror` on all touched files — clean
  after a single `clang-format -i` pass.

## What's left for Phase L

Phase K closes one of the ~20 remaining Tier 2 graphics orphans.
Candidates for the next phase, roughly in ascending order of scope:

- **`ConstantBufferRing.h`** (365 lines, Windows-only) — instantiate
  per-pipeline, `BeginFrame`/`EndFrame` alongside the GPU timer. Even
  without migrating the per-pass CB path, a live ring buffer gives
  future sub-allocation landings a zero-churn integration surface.
- **`RTHandleSystem.h`** (265 lines) — HDRP-style render-target
  handle abstraction. Can be instantiated in `GraphicsEngine` and
  used for the main scene colour/depth targets as an alternative to
  `RenderTargetPool`.
- **`ShaderVariantSystem.h`** (391 lines) — keyword-based shader
  permutation management. Would replace ad-hoc variant bookkeeping
  in the existing shader families.
- **`MeshOptimizer.h`** (474 lines, portable) — vertex-cache /
  overdraw mesh optimiser. Wire into `AssetPipeline::LoadMesh` or
  `FoliageImpostorBaker` at mesh-upload time. Already covered by
  `TestGraphicsIntegration.cpp` — activation just needs a caller.
- **`BVHAccelerator.h`** (444 lines, portable) — hierarchical
  frustum/ray culling. Instantiate in `GraphicsEngine` as a scene
  culling utility; feeding real scene data is Phase M+.
- **`ReflectionProbeCache.h`** (317 lines) — prefiltered env-map cache.
  Lighting-adjacent; pairs naturally with `CachedShadowAtlas.h`.
- **`CachedShadowAtlas.h`** (328 lines) — shadow atlas with per-light
  caching. Lighting-adjacent.
- **`VoxelConeTracing.h`** (463 lines) — voxel-cone-traced GI.
  Multi-session scope, parked until real-time GI is a priority.

The portable orphans (`MeshOptimizer`, `BVHAccelerator`) are the
tightest fit for the next phase: both are pure CPU code, both already
have some test coverage, both only need a caller to be "activated."

## Design notes for follow-ups

- **Override propagation is load-bearing.** The K2 fix — marking
  `overrideState = true` in every `Interpolate` path — is what makes
  "only apply touched fields" actually work. Any future
  `VolumeComponent` subclass must follow the same pattern or its
  fields will silently clobber pipeline settings on every
  non-contributing frame.
- **Evaluation and application are separable.** Phase K splits
  `Update()` (pure volume blend) from `ApplyVolumeStack()` (write to
  pipeline settings) so debug panels, savegame capture, and scene
  preview can query the stack without mutating runtime state. Keep
  this split when adding the fog pass or any future volume consumer.
- **Fog component is evaluated but not applied.** The fog fields are
  deliberately computed in the stack even though the pipeline has no
  fog pass. When the fog pass lands, its `ApplyVolumeStack` additions
  are a one-file diff: read `stack.fog.*` + write `m_fogSettings.*`
  inside `ApplyVolumeStack()`. No other layer changes.
- **The volume manager lives in the PostProcessingPipeline, not in
  Scene.** This was deliberate: `Scene` doesn't own any rendering
  state; post-process is the natural home. A future editor panel
  that shows the volume list in the scene hierarchy just reflects
  `pipeline->GetVolumeManager().Console_GetStatus()` alongside the
  scene graph.
