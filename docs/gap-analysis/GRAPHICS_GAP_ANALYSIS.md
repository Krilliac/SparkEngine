# SparkEngine Graphics/Rendering — Gap Analysis

> **Scope**: `SparkEngine/Source/Graphics/` and `SparkEngine/Source/Graphics/RHI/`
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Graphics/`.
> Each gap is assigned a severity: **Critical** (blocks rendering correctness), **Major** (significant missing feature), **Moderate** (partial implementation), **Minor** (polish / optimization).

---

## Critical Gaps

### GAP-G01 — RHI Abstraction Layer Is Largely Non-Functional Across Backends

**Files**:
- `Graphics/RHI/Vulkan/VulkanDevice.cpp` (gated behind `SPARK_VULKAN_SUPPORT`)
- `Graphics/RHI/OpenGL/OpenGLDevice.cpp` (gated behind `SPARK_OPENGL_SUPPORT`)
- `Graphics/RHI/D3D12/D3D12Device.h` (gated behind `_WIN32`)
- `Graphics/RHI/Metal/MetalDevice.h` (gated behind `__APPLE__`)
- `Graphics/RHI/RHIFactory.cpp`
- `Graphics/RHI/RHIBridge.cpp`

**Impact**: The engine declares a multi-backend RHI abstraction (D3D11, D3D12, Vulkan, OpenGL, Metal) but only D3D11 is wired into the actual `GraphicsEngine`. The `RHIFactory.cpp` has 9 stub returns, and `RHIBridge.cpp` has 12 stub returns. Vulkan and OpenGL backends have code behind compile-time guards that are never enabled in any CMake preset. D3D12 and Metal are header-only declarations with no `.cpp` implementations.

**Evidence**:
- `RHIFactory.cpp`: `CreateDevice()` returns `nullptr` for all backends except D3D11
- `RHIBridge.cpp`: 12 methods returning `false` / `nullptr` / empty vectors
- `VulkanDevice.cpp`: 77 stub pattern matches — extensive scaffolding but behind a never-enabled flag
- `OpenGLDevice.cpp`: 15 stub pattern matches
- `D3D12Device.h`: 8 stub pattern matches, no `.cpp` file exists
- `MetalDevice.h`: No `.mm` implementation file exists

**What is needed**: Either commit to the RHI abstraction (implement and test at least D3D11 + Vulkan backends) or remove the dead code to avoid maintenance burden. Currently the abstraction adds complexity without providing any cross-platform benefit.

---

### GAP-G02 — MaterialSystem Has 62 Stub/Default Returns

**Files**:
- `Graphics/MaterialSystem.h`
- `Graphics/MaterialSystem.cpp`

**Impact**: The MaterialSystem is the most stub-heavy file in the Graphics directory with 62 stub pattern matches. PBR material creation, texture binding, shader parameter setting, and material instance management are declared but largely return defaults or do nothing.

**Evidence**: 24 raw `printf` calls indicate debug-stage implementation. Material compilation, hot-reload, and shader permutation generation are declared in the header but non-functional.

**What is needed**: Implement PBR material pipeline: albedo/normal/metallic/roughness/AO texture binding, per-material constant buffers, shader permutation system, and material instancing.

---

### GAP-G03 — PostProcessing Pipeline is Fragmented Across Three Files

**Files**:
- `Graphics/PostProcessing.h` (legacy, monolithic)
- `Graphics/PostProcessingSystem.cpp` (11 stub returns)
- `Graphics/PostProcessingPipeline.h` (23 stub returns — newer, modular design)

**Impact**: Three overlapping post-processing implementations exist with no clear authority. `PostProcessingPipeline.h` declares a modern pass-based pipeline (bloom, tone-mapping, FXAA, chromatic aberration, vignette, DoF, motion blur) but has 23 stub returns. `PostProcessingSystem.cpp` has 11 additional stubs. The result is that no post-processing effects actually execute.

**What is needed**: Consolidate into a single `PostProcessingPipeline` with working implementations for at minimum: tone-mapping, bloom, FXAA, and exposure adaptation.

---

### GAP-G04 — LightingSystem Has 18 Stub Returns

**Files**:
- `Graphics/LightingSystem.h`
- `Graphics/LightingSystem.cpp`

**Impact**: The LightingSystem declares support for directional, point, and spot lights with shadow mapping, but the `.cpp` has 18 stub returns. Light culling, shadow map rendering, and light constant buffer updates are non-functional.

**What is needed**: Implement per-frame light buffer updates, basic shadow mapping for at least directional lights, and forward lighting pass integration.

---

## Major Gaps

### GAP-G05 — Shadow System Declared But Not Integrated

**Files**:
- `Graphics/ShadowAtlas.h` (15 stub returns)
- `Graphics/DeferredLightingPass.h` (10 stub returns)

**Impact**: A sophisticated shadow atlas system with priority-based updates, cascaded shadow maps, and static caching is fully designed in the header but has 15 stub returns. No shadow pass is actually rendered.

**What is needed**: Implement basic cascaded shadow maps for the directional light. The shadow atlas design is sound; implement `AllocateTile`, `RenderShadowMap`, and `BindShadowAtlas`.

---

### GAP-G06 — Global Illumination System is 100% Stubbed

**File**: `Graphics/GlobalIllumination.h` (19 stub returns)

**Impact**: Light probes, irradiance volumes, and DDGI are declared but entirely empty. No indirect lighting exists in the engine.

**What is needed**: Implement at minimum baked SH light probes with GPU buffer upload and shader sampling.

---

### GAP-G07 — Screen-Space Effects Are Stubbed

**Files**:
- `Graphics/ScreenSpaceEffects.h` (CPU-side, header-only)
- `Graphics/ScreenSpaceEffectsGPU.h` (13 stub returns — GPU compute path)
- `Graphics/OcclusionCulling.h` (20 stub returns)

**Impact**: SSAO, SSR (screen-space reflections), and occlusion culling are all declared but return defaults. These are essential for visual quality in an FPS engine.

**What is needed**: Implement SSAO (screen-space ambient occlusion) as the minimum viable screen-space effect. SSR and occlusion culling are lower priority.

---

### GAP-G08 — Forward+ / Clustered Rendering Paths Are Stubbed

**File**: `Graphics/ForwardPlusLightCulling.h` (18 stub returns)

**Impact**: `RenderPath::ForwardPlus` and `RenderPath::Clustered` are declared in the enum but the light culling compute shaders are entirely empty. Only basic forward rendering works.

**What is needed**: Implement tiled light culling compute shader for Forward+ to support many dynamic lights efficiently.

---

### GAP-G09 — TextureSystem Has Incomplete Streaming

**Files**:
- `Graphics/TextureSystem.h`
- `Graphics/TextureSystem.cpp` (15 stub returns)

**Impact**: Texture loading works for basic cases, but texture streaming (mip-level streaming, async loading, memory budget management) is declared but stubbed with 15 default returns.

**What is needed**: Implement async texture loading with priority queue and memory budget tracking.

---

### GAP-G10 — RenderTarget Management Has 27 Stub Returns

**Files**:
- `Graphics/RenderTarget.h`
- `Graphics/RenderTarget.cpp`

**Impact**: The render target system (used by shadow maps, post-processing, and off-screen rendering) has 27 stub returns in the `.cpp`, suggesting that MRT (multiple render target), depth stencil, and format negotiation are incomplete.

---

## Moderate Gaps

### GAP-G11 — AssetPipeline Has 25 Stub Returns

**Files**:
- `Graphics/AssetPipeline.h`
- `Graphics/AssetPipeline.cpp`

**Impact**: Mesh loading, texture importing, and asset caching are partially implemented but 25 operations return defaults. Model import from FBX/OBJ and material assignment are incomplete.

---

### GAP-G12 — Particle Systems Are Partially Implemented

**Files**:
- `Graphics/ParticleSystem.h` / `.cpp` (3 stubs in cpp)
- `Graphics/GPUParticleSystem.h` (2 stub returns — header-only)

**Impact**: CPU particle system has basic emission but GPU particle compute path is entirely empty.

---

### GAP-G13 — Advanced Rendering Features Are Header-Only Stubs

Multiple header-only files declare advanced features with no implementations:

| File | Stub Count | Feature |
|------|-----------|---------|
| `WaterSystem.h` | 28 | Ocean/water rendering |
| `SkyAtmosphere.h` | 20 | Physically-based sky/atmosphere |
| `IBLGenerator.h` | 22 | Image-based lighting |
| `TemporalEffects.h` | 15 | TAA, temporal upscaling |
| `InstanceRenderer.h` | 14 | GPU instanced rendering |
| `SkyboxRenderer.h` | 14 | Skybox/cubemap rendering |
| `UpscalingSystem.h` | 11 | DLSS/FSR integration |
| `TessellationSystem.h` | 7 | Hardware tessellation |
| `ShaderCacheWarming.h` | 6 | Shader pre-compilation |
| `RenderGraph.h` | 7 | Frame graph / render graph |
| `AdvancedBRDF.h` | 4 | Advanced BRDF models |
| `TransparencySorting.h` | 2 | OIT / transparency |

**Total**: ~150 stub methods across 12 header-only files.

**Impact**: These represent the "AAA features" roadmap but none are functional. The engine renders with basic forward shading only.

---

### GAP-G14 — DXR Raytracing Support is Stubbed

**Files**:
- `Graphics/RHI/DXRSupport.h` (1 stub)
- `Graphics/RHI/DXRSupport.cpp` (2 stubs)

**Impact**: DXR raytracing support is declared but not implemented. This is expected given the D3D11 primary target.

---

### GAP-G15 — Deferred Rendering Path is Stubbed

**File**: `Graphics/DeferredLightingPass.h` (10 stub returns)

**Impact**: The deferred lighting pass (G-buffer creation, lighting pass, light volume rendering) is entirely empty despite `RenderPath::Deferred` being a valid enum value.

---

## Minor Gaps

### GAP-G16 — MeshLOD Has Minimal Implementation

**Files**:
- `Graphics/MeshLOD.h` (2 stubs)
- `Graphics/MeshLOD.cpp` (2 stubs)

**Impact**: LOD selection and mesh simplification are declared but the transition logic is basic.

---

### GAP-G17 — DecalSystem Is Partially Implemented

**Files**:
- `Graphics/DecalSystem.h`
- `Graphics/DecalSystem.cpp` (6 stub returns)

**Impact**: Decal projection and rendering have basic structure but 6 operations return defaults.

---

### GAP-G18 — FrustumCulling Is Minimal

**File**: `Graphics/FrustumCulling.h` (1 stub)

**Impact**: Frustum culling logic exists but is basic. No spatial acceleration structure (octree, BVH) for large scene culling.

---

## Summary Table

| ID | Severity | Subsystem | Stub Count | Impact |
|---|---|---|---|---|
| GAP-G01 | Critical | RHI Multi-Backend | ~120 | Only D3D11 works |
| GAP-G02 | Critical | MaterialSystem | 62 | No PBR materials |
| GAP-G03 | Critical | PostProcessing | 34 | No post-processing |
| GAP-G04 | Critical | LightingSystem | 18 | Incomplete lighting |
| GAP-G05 | Major | Shadow System | 15 | No shadows |
| GAP-G06 | Major | Global Illumination | 19 | No indirect lighting |
| GAP-G07 | Major | Screen-Space Effects | 33 | No SSAO/SSR |
| GAP-G08 | Major | Forward+/Clustered | 18 | One render path only |
| GAP-G09 | Major | TextureSystem | 15 | No texture streaming |
| GAP-G10 | Major | RenderTarget | 27 | Incomplete RT management |
| GAP-G11 | Moderate | AssetPipeline | 25 | Incomplete asset loading |
| GAP-G12 | Moderate | Particles | 5 | No GPU particles |
| GAP-G13 | Moderate | Advanced Features | ~150 | 12 header-only stubs |
| GAP-G14 | Moderate | DXR Raytracing | 3 | Expected (DX11 target) |
| GAP-G15 | Moderate | Deferred Rendering | 10 | Deferred path unusable |
| GAP-G16 | Minor | MeshLOD | 4 | Basic LOD only |
| GAP-G17 | Minor | DecalSystem | 6 | Partial decals |
| GAP-G18 | Minor | FrustumCulling | 1 | No spatial acceleration |

---

## Aggregate Statistics

| Metric | Value |
|---|---|
| Total gaps identified | 18 |
| Critical | 4 |
| Major | 6 |
| Moderate | 4 |
| Minor | 4 |
| Total files in Graphics/ | 67 |
| Total stub occurrences | ~694 |
| Header-only stub files (no .cpp) | 12+ |
| Working render backend | D3D11 only |
| Non-functional render backends | 4 (D3D12, Vulkan, OpenGL, Metal) |

---

## Recommended Priority Order

1. **GAP-G02 + GAP-G04** — MaterialSystem + LightingSystem (unblocks PBR rendering)
2. **GAP-G05** — Shadow mapping (essential for FPS visual quality)
3. **GAP-G03** — PostProcessing consolidation (tone-mapping + bloom)
4. **GAP-G07** — SSAO (high visual impact, moderate complexity)
5. **GAP-G09 + GAP-G10** — TextureSystem + RenderTarget (infrastructure)
6. **GAP-G08** — Forward+ light culling (performance with many lights)
7. **GAP-G01** — RHI cleanup (either implement Vulkan or remove dead backends)
8. Everything else
