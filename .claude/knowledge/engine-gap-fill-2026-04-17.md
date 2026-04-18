# Engine Gap-Fill Session — 3 Missing Features (2026-04-17)

**Type:** Decision + Observation
**Status:** Active

## Description

Cross-checked SparkEngine against modern engines (Unreal 5, Unity, Godot, Bevy, Wicked, Filament, O3DE, The Forge, Decima) to find genuinely missing features. Prior sessions had already implemented the bulk of the TOP-30 list from `eleven-engine-analysis.md`. The remaining gaps were:

1. **Volumetric clouds (Decima-style, GPU Pro 7)** — weather system had fog but no clouds
2. **HRTF binaural spatial audio** — audio had 3D pan/Doppler but no head-related transfer function
3. **Directional predictive streaming** — `SeamlessAreaManager` had velocity lookahead but no forward-cone bias (RAGE/Decima predictive streaming)

## Implementation

### VolumetricCloudSystem (`Graphics/VolumetricClouds.{h,cpp}`, ~460 lines)

- Two-scale noise model: 32³ Perlin-like base + 16³ Worley detail + 64² weather map
- Wind-drift advection (wind vector + time accumulator)
- Per-altitude density gradient with stratus/cumulus/cumulonimbus profiles (`HeightGradient`)
- Anvil flattening for convective types
- Ray integration with Henyey-Greenstein two-lobe phase function + silver lining bias
- Self-shadowing via short light-march towards the sun
- CPU reference path **plus** GPU resource creation (R8 3D base/detail + RG8 2D weather)
- Singleton; wired into `GameplayLifecycleShared.cpp` alongside `SkyAtmosphereSystem`
- 12 tests (`TestVolumetricClouds.cpp`)

### HRTFProcessor (`Audio/HRTFProcessor.{h,cpp}`, ~330 lines)

- Analytical model (no external HRIR dataset required):
  - **ITD** via Woodworth formula (`r·(sin θ + θ) / c`) → integer-sample delay line
  - **ILD** via cos²(θ/2) gain rolloff + elevation shelf + 1/d distance falloff
  - **Head-shadow** via one-pole LP on far ear, cutoff 22 kHz → 1 kHz at 90°
- Coordinate convention: left-handed (DirectXMath) — `right = up × forward`
- Per-block gain cross-fade prevents zipper noise under rapid source motion
- Pure DSP class, portable, no audio backend dependency
- Diagnostic `HRTFState` snapshot (azimuth, elevation, ITD samples, cutoff)
- 8 tests (`TestHRTFProcessor.cpp`)

### Directional predictive streaming (`Engine/Streaming/SeamlessAreaManager.{h,cpp}`)

- Added `StreamingConfig::directionalBias` and `directionalDotThreshold`
- New private `DirectionalEffectiveDistance(rawDist, area)` helper
- Maps dot-product `[threshold..1]` linearly to a distance reduction `[0..bias]`
- Applied at two places:
  - Radius eligibility test in `ProcessLoadQueue`
  - Tie-break in the load-queue sort comparator
- Default bias 0.4 — areas directly ahead load as if 40 % closer
- `bias = 0` → fully isotropic (backwards-compatible)
- Inspired by RAGE GTA V and Decima Horizon streaming
- 3 tests (`TestDirectionalStreaming.cpp`)

## Results

- **5614 / 5614 tests pass** (was 5611; +3 test files adding 23 tests)
- CMake auto-globs `SparkEngine/Source/**/*.{cpp,h}` — no manual source registration needed
- Added 3 entries to `Tests/CMakeLists.txt`
- `clang-format -i` applied to all new files (120-col limit, Allman braces preserved)

## Notes

- Each of the three systems deliberately stays on the **CPU reference + GPU feeder** pattern used by `FroxelVolumetricFog`, so unit tests can run headlessly without a device
- HRTF uses an analytical model rather than a measured HRIR dataset — swapping in MIT-KEMAR or IRCAM measured HRIRs is a drop-in replacement behind `Process()`
- Directional-bias defaults are conservative (bias 0.4, cone-threshold 0.25) — existing behaviour is unchanged for unaware callers
- No new globals — all three use service-locator / singleton pattern consistent with the rest of the engine

## Files Touched

```
SparkEngine/Source/Graphics/VolumetricClouds.h                    (new)
SparkEngine/Source/Graphics/VolumetricClouds.cpp                  (new)
SparkEngine/Source/Audio/HRTFProcessor.h                          (new)
SparkEngine/Source/Audio/HRTFProcessor.cpp                        (new)
SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.h         (modified)
SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.cpp       (modified)
SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp     (modified — wire clouds)
Tests/TestVolumetricClouds.cpp                                    (new)
Tests/TestHRTFProcessor.cpp                                       (new)
Tests/TestDirectionalStreaming.cpp                                (new)
Tests/CMakeLists.txt                                              (register tests)
```
