# Rendering Pipeline Feature Status

**Last updated:** 2026-03-22
**Type:** Observation
**Status:** Resolved
**Severity:** High

## Description

Comprehensive audit of all 26 rendering features to determine what actually works vs header-only scaffolding. The D3D11 core pipeline is solid with 17 working features, and 12 systems have type definitions but need GPU implementations. Terrain rendering exists via ClipmapTerrain (added 2026-03-21). **Direction (2026-03-22): all 12 stubs will be fully implemented with GPU backends — no deletion.**

---

## Working Features (17 systems)

| Feature | Key Files | Evidence |
|---------|-----------|----------|
| D3D11 core rendering | GraphicsEngine.cpp (4.6K) | Device/swap chain init at SparkEngine.cpp:611, EndFrame() presents |
| Forward rendering | GraphicsEngine.cpp:646 | `RenderForward()` called based on pipeline mode |
| Deferred rendering | GraphicsEngine.cpp:695 | G-Buffer (4 MRTs + depth), `LightingPass()` |
| Forward+ / clustered | GraphicsEngine.cpp:740 | `RenderForwardPlus()` with tile-based light culling |
| Material system (PBR) | MaterialSystem.cpp (151K) | Initialized at GraphicsEngine::Initialize():251 |
| Lighting system | LightingSystem.cpp (73K) | Initialized at GraphicsEngine::Initialize():254, directional/point/spot + shadows |
| Texture system | TextureSystem.cpp (49K) | Initialized at GraphicsEngine::Initialize():245, anisotropic, VRAM budget |
| Shader system | Shader.cpp (75K) | D3D compilation, constant buffers, hot-reload |
| Asset pipeline | AssetPipeline.cpp (72K) | Initialized at GraphicsEngine::Initialize():257 |
| Particle system | ParticleSystem.cpp (36K) | GPU-friendly emitters with blending, collision |
| Decal system | DecalSystem.cpp (20K) | Deferred decal rendering (albedo/normal/emission) |
| Mesh LOD | MeshLOD.cpp (23K) | Distance-based LOD, edge collapse simplification |
| Upscaling (FSR/DLSS) | UpscalingSystem.cpp (39K) | FSR 1.0/2.0, DLSS, XeSS with quality presets |
| ECS render system | ECSystems.h:186-230 | Calls SubmitMeshForRendering() per visible entity |
| RHI bridge layer | RHIBridge.cpp (17K) | Multi-API abstraction with shader cache |
| D3D11 RHI backend | D3D11Device.cpp (1.3K) | Primary backend, implements IRHIDevice |
| 2D sprite rendering | Systems2D.h | SpriteRenderer via ECS integration |

### Post-processing (CREATED but execution path unclear)

- `PostProcessingPipeline.h` (1.1K) — created at GraphicsEngine.cpp:112
- Effects defined: FXAA, DOF, Motion Blur, Vignette, Chromatic Aberration, Film Grain, Lens Distortion, Light Shafts, Lens Flare, Sharpen
- `TemporalEffects.h` (940) — TAA, temporal stability, frame history
- `RenderPostProcessing()` exists at GraphicsEngine.cpp:4264 but call path unclear

---

## Built-Not-Wired (1 system)

| Feature | Key Files | Lines | Status |
|---------|-----------|-------|--------|
| DXR ray tracing | DXRSupport.cpp (45K) | ~2,000 | Complete BLAS/TLAS, reflections, GI, shadows — ENABLE_DXR=OFF by default |

---

## Header-Only Stubs (12 systems, ~15K dead lines)

These have full type definitions and interface stubs but NO actual GPU work:

| Feature | File | Lines | What Exists |
|---------|------|-------|-------------|
| Sky/atmosphere | SkyAtmosphere.h | 1,500 | Preetham/Bruneton model stubs, LUT infrastructure |
| Water system | WaterSystem.h | 1,400 | Gerstner waves, planar reflections stubs |
| Global illumination | GlobalIllumination.h | 1,000 | Light probes, SH (L2), DDGI stubs |
| Shadow atlas | ShadowAtlas.h | 873 | Priority-based tile management stubs |
| Deferred lighting pass | DeferredLightingPass.h | 632 | G-Buffer accumulation (fully defined inline) |
| Forward+ light culling | ForwardPlusLightCulling.h | 635 | Tile-based culling (fully defined inline) |
| Screen-space effects GPU | ScreenSpaceEffectsGPU.h | ~500 | Compute shader infrastructure |
| Render graph | RenderGraph.h | 1,700 | Complete DSL framework, not integrated |
| Resource residency mgr | ResourceResidencyManager.h | 902 | GPU memory streaming stubs |
| Dynamic quality scaler | DynamicQualityScaler.h | 849 | Dynamic resolution scaling stubs |
| Occlusion culling | OcclusionCulling.h | 695 | Occlusion query infrastructure stubs |
| Instance renderer | InstanceRenderer.h | 1,200 | Instance rendering interface stubs |

**SSR**: Settings struct in PostProcessingPipeline.h but no ray-marching implementation.
**Volumetric fog**: Settings struct only, no compute implementation.

---

## Missing (1 system)

| Feature | Status |
|---------|--------|
| Terrain rendering | NO terrain system files anywhere in codebase |

---

## RHI Backend Status

| Backend | Files | Status |
|---------|-------|--------|
| D3D11 | D3D11Device.cpp (1.3K) | WORKING — primary backend |
| D3D12 | D3D12Device.cpp (1.8K) | STUBS — not default |
| Vulkan | VulkanDevice.cpp (2.2K) | STUBS — not default |
| Metal | MetalDevice.h (542) | STUBS — no .cpp |
| OpenGL | OpenGLDevice.cpp (1.6K) | STUBS — not default |

---

## Key Findings

1. **Core D3D11 pipeline is solid** — device, swap chain, render targets, state management all work
2. **12 header-only rendering systems** add ~15K lines of dead code that creates false confidence
3. **DXR is feature-complete** (45K lines) but disabled by default and not in main render loop
4. **No terrain rendering** — critical gap for an engine claiming open-world support
5. **Post-processing created but render path unclear** — effects defined but execution uncertain
6. **Header-only systems need D3D11 binding** to become functional (shaders, CBs, RT setup)
