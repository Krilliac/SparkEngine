# Orphaned Headers — Never Included Anywhere

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Mostly resolved (27 of 30 deleted)
**Severity:** High

## Description

30 header files exist in the source tree but are never `#include`d by any other file. They have no corresponding `.cpp` implementation files. These represent aspirational features that were designed but never integrated — pure dead code that adds confusion and maintenance burden.

## Evidence

Verified via `grep -r '#include.*<filename>'` across the entire codebase. Zero matches for each.

## Headers by Subsystem

### Graphics (19 files — largest category)

| File | Lines | Feature |
|------|-------|---------|
| `Graphics/InstanceRenderer.h` | 1,223 | GPU instancing |
| `Graphics/SkyAtmosphere.h` | 1,475 | Atmospheric sky rendering |
| `Graphics/WaterSystem.h` | 1,373 | Water rendering |
| `Graphics/GlobalIllumination.h` | 1,041 | DDGI/light probes |
| `Graphics/ShadowAtlas.h` | 873 | Shadow atlas management |
| `Graphics/DynamicQualityScaler.h` | 849 | Dynamic quality scaling |
| `Graphics/GPUParticleSystem.h` | 805 | GPU particle system |
| `Graphics/ResourceResidencyManager.h` | 902 | VRAM residency management |
| `Graphics/ShaderCacheWarming.h` | ~400 | Shader cache pre-compilation |
| `Graphics/OcclusionCulling.h` | ~400 | Occlusion culling |
| `Graphics/ForwardPlusLightCulling.h` | ~350 | Tile-based light culling |
| `Graphics/DeferredLightingPass.h` | ~350 | Deferred lighting |
| `Graphics/ScreenSpaceEffectsGPU.h` | ~400 | GPU screen-space effects |
| `Graphics/TransparencySorting.h` | ~200 | Transparency sorting |
| `Graphics/AdvancedBRDF.h` | ~300 | Advanced material BRDF |
| `Graphics/IBLGenerator.h` | ~400 | Image-based lighting |
| `Graphics/PBRUtils.h` | ~200 | PBR utilities |
| `Graphics/TessellationSystem.h` | ~200 | GPU tessellation |
| `Graphics/SkyboxRenderer.h` | ~300 | Skybox rendering |
| `Graphics/RHI/Metal/MetalDevice.h` | ~400 | Metal backend |

### Engine Systems (3 files)

| File | Lines | Feature |
|------|-------|---------|
| `Engine/ECS/Components/ConstraintComponent.h` | ~100 | Physics constraints |
| `Engine/Streaming/AsyncAssetLoader.h` | ~300 | Async asset loading |
| `Engine/World/DayNightCycle.h` | ~200 | Day/night cycle |

### Utils (3 files)

| File | Lines | Feature |
|------|-------|---------|
| `Utils/PerformanceStats.h` | ~200 | Performance statistics |
| `Utils/ObjectPool.h` | ~150 | Generic object pool |
| `Enums/EnumTests.h` | ~100 | Test enumerations (misplaced) |

### Meta/Architecture (2 files)

| File | Lines | Feature |
|------|-------|---------|
| `AllEnums.h` | ~50 | Master enum include (never used) |
| `SparkArchitecture.h` | ~50 | Master architecture include |

### SparkEditor (4 files)

| File | Lines | Feature |
|------|-------|---------|
| `Integration/SparkFutureIntegration.h` | ~200 | Future tech integration |
| `Core/DockPosition.h` | ~30 | Dock position enum |
| `Core/EditorIntegration.h` | ~150 | Editor architecture bridge |
| `Reflection/ComponentReflection.h` | ~300 | Component reflection |

## Estimated Total Dead Lines

~11,000+ lines across 30 files.

## Action

Per CLAUDE.md anti-bloat rules: *"Features built but not integrated count as bugs, not WIP."*

These should be deleted. If a feature is needed later, it can be rebuilt from git history — keeping unintegrated headers creates false confidence that the feature exists.

## Resolution (2026-03-16)

27 of 30 orphaned headers were deleted across two cleanup commits (~40K lines in the first pass, DayNightCycle.h in the second). Three files remain intentionally:
- `MetalDevice.h` (542 lines) — experimental Metal backend, expected orphan
- `PerformanceStats.h` (331 lines) — actually used by SparkGame module (not truly orphaned)
- `DayNightCycle.h` was deleted (test has standalone copy)

## Notes

- MetalDevice.h is expected to be orphaned (experimental backend)
- PerformanceStats.h was misidentified as orphaned — it's included by SparkGame/Source/Game/Game.h
