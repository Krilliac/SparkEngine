# SparkEngine Graphics & Rendering — Gap Analysis

## Context

This analysis identifies gaps and missing features in SparkEngine's rendering and graphics systems. The engine has a mature foundation: DX11/Vulkan/OpenGL backends via an RHI abstraction, PBR materials, multiple render paths (Forward, Deferred, Forward+, Clustered), and extensive post-processing infrastructure. The gaps below are prioritized by impact for an FPS-focused engine.

---

## Critical Gaps (High Impact)

### 1. Post-Processing Pipeline Execution is Stubbed
- **Files**: `SparkEngine/Source/Graphics/PostProcessingPipeline.h` (lines 262, 342-346)
- `Render()` and `ProcessPass()` are **empty stubs** — the 10 declared effects (FXAA, DOF, Motion Blur, Vignette, Chromatic Aberration, Film Grain, Lens Distortion, Light Shafts, Lens Flare, Sharpen) have settings structs but no GPU execution
- `PostProcessingSystem.cpp` implements bloom/tone-mapping/color-grading, but the extended pipeline never runs
- **Impact**: No DOF, motion blur, god rays, lens effects, or FXAA despite full configuration UI

### 2. GPU Shaders for Temporal Effects Missing
- **File**: `SparkEngine/Source/Graphics/TemporalEffects.h` (568 lines)
- CPU-side jitter generation, history management, and variance clipping logic are complete
- No corresponding GPU shader code to actually resolve TAA or accumulate motion blur
- **Impact**: TAA and per-object motion blur are non-functional despite extensive CPU infrastructure

### 3. Occlusion Culling Not Implemented
- `GraphicsSettings` has an `enableOcclusionCulling` flag but no backend
- No occlusion queries, hierarchical-Z, or software rasterizer for visibility
- Only frustum culling is active (`FrustumCulling.h`)
- **Impact**: Indoor FPS scenes with many occluded objects will over-draw significantly

### 4. No GPU Instancing / Draw-Call Batching
- Each mesh is drawn individually — no instanced draw support
- No indirect draw dispatch or SRV-based instance buffers
- **Impact**: Severe CPU bottleneck for scenes with repeated geometry (foliage, debris, props)

### 5. D3D12 Backend Not Implemented
- Declared in `RHITypes.h` (line 31) but no `D3D12Device` exists
- Blocks DXR ray tracing (`DXRSupport.h` requires D3D12)
- Blocks mesh shaders, variable-rate shading, bindless resources (all have capability flags but no backend)
- **Impact**: Engine stuck on DX11-level GPU features on Windows

---

## Major Gaps (Medium-High Impact)

### 6. No Water Rendering System
- No water shaders, wave simulation, caustics, or refraction
- `DecalSystem` references a "Water" surface type but no volumetric water exists
- **Impact**: FPS levels with water bodies have no rendering solution

### 7. No Procedural Sky / Atmosphere Scattering
- `DayNightCycle.h` animates sun position and ambient color, but there is no sky dome shader
- No Rayleigh/Mie scattering, no procedural or volumetric clouds
- `WeatherSystem.h` defines weather types but doesn't render sky
- **Impact**: Sky is a flat color; no atmospheric depth

### 8. IBL / Environment Map Generation May Be Stubbed
- `LightingSystem.h` declares `GenerateIBLTextures()`, `GenerateIrradianceMap()`, `GeneratePrefilteredMap()` (lines 303, 425-427)
- Implementation status unclear — likely stubs for runtime convolution
- Without working IBL, PBR materials lack accurate environment reflections
- **Impact**: Metallic/glossy surfaces look flat without convolved environment maps

### 9. Screen-Space Effects GPU Integration Unclear
- `ScreenSpaceEffects.h` defines SSAO (kernel generation, quality presets) and SSR (ray marching settings)
- Whether these actually dispatch compute/pixel shaders on the GPU needs verification
- **Impact**: If stubbed, no ambient occlusion or screen-space reflections

### 10. Forward+ / Clustered Light Culling Incomplete
- `RenderForwardPlus()` and `RenderClustered()` are declared in `GraphicsEngine.h` (lines 644-647)
- Tile/cluster light lists and GPU structures need verification
- **Impact**: With 32+ point lights, forward rendering without culling is too expensive

---

## Moderate Gaps

### 11. Metal Backend Not Implemented
- Declared in RHI enum but no `MetalDevice` exists
- macOS rendering relies on OpenGL (deprecated by Apple)
- **Impact**: macOS support is fragile long-term

### 12. No DLSS / FSR Upsampling
- No references to NVIDIA DLSS or AMD FSR anywhere in the codebase
- TAA infrastructure could serve as a base for temporal upscaling
- **Impact**: No AI-based or temporal upscaling for performance recovery

### 13. No Global Illumination Solution
- IBL provides indirect specular (if working), but there is no diffuse GI
- No light probes, irradiance volumes, DDGI, or voxel-based GI
- SSAO partially compensates but is not GI
- **Impact**: Indoor scenes lack bounced light, feel flat

### 14. Advanced Material Features Lack GPU Shaders
- `MaterialSystem.h` declares subsurface scattering, clearcoat, anisotropy, transmission, sheen, iridescence
- These need corresponding BRDF shader terms — unclear if pixel shaders implement them
- **Impact**: Advanced material knobs exist in the editor but may not render

### 15. No Shadow Atlas / Streaming Shadows
- Each shadow-casting light appears to get its own shadow map
- No shared shadow atlas, no cached/streaming shadows for static geometry
- **Impact**: Many shadow-casting lights consume excessive VRAM

### 16. Deferred Rendering Lighting Pass Unclear
- G-Buffer creation (4 MRT) is set up in `GraphicsEngine`
- Whether `RenderLightingPass()` actually accumulates lighting from the G-Buffer needs verification
- **Impact**: Deferred path may not produce correct output

---

## Minor / Nice-to-Have Gaps

### 17. No Skybox / Cubemap Rendering
- No explicit skybox draw call or cubemap sampling in the render pipeline

### 18. No Transparency Sorting
- Materials support alpha blending modes but no back-to-front sort pass is visible

### 19. Geometry / Tessellation Shaders Underused
- `ShaderType` enum includes Geometry/Hull/Domain but these shader stages aren't exercised

### 20. No GPU-Driven Particle Update
- Particle system is CPU-driven (`ParticleSystem.h/cpp`)
- Compute shader infrastructure exists but isn't used for particles

### 21. No Render Graph / Frame Graph
- Render passes are hardcoded in `GraphicsEngine`; no declarative render graph for automatic resource barriers/aliasing

### 22. No Shader Permutation Cache Warming
- Shader variants compile on demand — no precompilation/warmup pass to avoid hitches

---

## Verification Plan

To confirm which "partially implemented" systems actually work end-to-end:

1. **Build the engine**: `cmake --preset windows-release && cmake --build build --config Release`
2. **Run with console**: Enable `ENABLE_GRAPHICS=ON` and launch the editor or game
3. **Test render paths**: Use console commands to switch between Forward/Deferred/Forward+ and verify output
4. **Test post-processing**: Toggle bloom, tone mapping, and check if DOF/motion blur/FXAA produce visible changes
5. **Test TAA**: Enable TAA via console and verify jitter + history resolve
6. **Test shadows**: Place multiple lights and verify CSM/PCF/VSM output
7. **Test SSAO/SSR**: Enable screen-space effects and verify visual output
8. **Run graphics tests**: `cd build && ctest --output-on-failure -R "PostProcessing|Light|Frustum|MeshLOD|ScreenSpace|Temporal|Fog|Weather"`

---

## Summary Table

| # | Gap | Severity | Category |
|---|-----|----------|----------|
| 1 | Post-processing pipeline stubbed | Critical | Rendering |
| 2 | TAA/motion blur GPU shaders missing | Critical | Temporal |
| 3 | Occlusion culling unimplemented | Critical | Performance |
| 4 | No instancing/batching | Critical | Performance |
| 5 | D3D12 backend missing | Critical | Platform |
| 6 | No water rendering | Major | Content |
| 7 | No procedural sky/atmosphere | Major | Content |
| 8 | IBL generation possibly stubbed | Major | Lighting |
| 9 | Screen-space effects GPU unclear | Major | Rendering |
| 10 | Forward+/Clustered culling incomplete | Major | Performance |
| 11 | Metal backend missing | Moderate | Platform |
| 12 | No DLSS/FSR | Moderate | Performance |
| 13 | No global illumination | Moderate | Lighting |
| 14 | Advanced BRDF terms unimplemented | Moderate | Materials |
| 15 | No shadow atlas | Moderate | Shadows |
| 16 | Deferred lighting pass unclear | Moderate | Rendering |
| 17 | No skybox rendering | Minor | Content |
| 18 | No transparency sorting | Minor | Rendering |
| 19 | Geometry/tessellation unused | Minor | Rendering |
| 20 | CPU-only particles | Minor | Performance |
| 21 | No render graph | Minor | Architecture |
| 22 | No shader cache warming | Minor | Performance |
