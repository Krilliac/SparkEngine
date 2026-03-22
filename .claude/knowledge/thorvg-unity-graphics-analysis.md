# ThorVG + Unity Graphics + Graphics Libraries Analysis

**Last updated:** 2026-03-22
**Type:** Decision
**Status:** Active
**Sources analyzed:** ThorVG (vector graphics engine), Unity Graphics repo (SRP/URP/HDRP/ShaderGraph), awesome-graphics-libraries (33+ libraries across 14 categories)

## Description

Comprehensive analysis of three major sources for graphics techniques and libraries applicable to SparkEngine. Identified 35 recommendations across rendering, post-processing, asset pipeline, UI, shader tooling, and resource management. Cross-referenced against SparkEngine's existing 112+ graphics files to avoid duplication.

## Context

SparkEngine has: D3D11 primary RHI with 5 experimental backends, RenderGraph, clustered forward lighting, PBR materials with ShaderGraphCompiler, ShadowAtlas, PostProcessing (10 effects), TemporalEffects/TAA, UpscalingSystem, HybridRT (DXR + SDF fallback), LightProbeSystem (L2 SH), VirtualTexture, ParticleSystem, DecalSystem, MeshLOD, OcclusionCulling, SpriteBatch (2D), UISystem, and 45+ working engine systems from prior analyses.

## Source 1: ThorVG (Vector Graphics Engine)

ThorVG is a production-ready C++ vector graphics engine (~150KB binary) supporting SVG and Lottie animation with multiple backends (CPU/SIMD, OpenGL, WebGPU). Used by Tizen, Samsung, and Godot.

### Key Patterns Discovered

1. **Dirty Region Tracking (16x16 grid, double-buffered dirty lists)** — Only redraws changed UI cells. SparkEngine's UISystem redraws all visible widgets every frame.
2. **Retained Scene Graph with Update Flags** — Bitwise `RenderUpdateFlag` enums (Path, Color, Transform, Image, Blend) track what changed per node. Renderer only reprocesses modified data.
3. **Backend-Agnostic RenderMethod Interface** — `prepare()` returns opaque `RenderData` handle; `renderShape()`/`renderImage()` draw; `beginComposite()`/`endComposite()` for masking. Each backend stores its own representation.
4. **Compositor Stack with Pooled Render Targets** — Per-node masking/blending with `needComposition()` skip when scene is simple (full opacity, no masks). Render target pool avoids allocation churn.
5. **State Machine for Update/Render Separation** — Synced→Painting→Updating→Drawing→Synced. Prevents race conditions between scene modification and rendering.
6. **SVG/Lottie Loading Architecture** — Independent loader modules producing same scene graph. Lottie enables data-driven UI animations.

## Source 2: awesome-graphics-libraries (33+ Libraries)

### Libraries Identified as Gap-Fillers

**Mesh Processing:**
- **meshoptimizer** — Vertex cache/fetch optimization, mesh simplification, meshlet generation, mesh compression. De facto standard (used by Filament, Bevy, NVIDIA). SparkEngine has MeshLOD but lacks meshlet generation and vertex optimization.
- **PMP Library** — Modern C++ mesh processing (remeshing, smoothing, parameterization). Lighter alternative to CGAL for editor-side mesh operations.

**Texture Compression:**
- **Basis Universal** — Supercompressed GPU textures (ETC1S/UASTC → transcode to BC1-7/ASTC/ETC/PVRTC at load time). Single compressed asset works across all RHI backends. SparkEngine currently loads raw PNG/JPG via stb_image. VRAM reduction of 4-8x.
- **AMD Compressonator** — GPU-accelerated BC1-7/ETC/ASTC compression. Alternative to Basis for direct D3D11/12 path.
- **tinyexr** — Single-header OpenEXR loader. Needed for HDR lightmaps, IBL probes, environment maps.

**Font Rendering:**
- **msdfgen + msdf-atlas-gen** — Multi-channel SDF font atlas generation. Resolution-independent text rendering with simple fragment shader. SparkEngine has no game-side text rendering system.

**Shader Cross-Compilation:**
- **Slang** — Modern shading language (NVIDIA/Khronos). Compiles to DXIL/SPIR-V/Metal/CUDA/WebGPU. Module system addresses shader permutation explosion.
- **ShaderConductor** — HLSL→SPIR-V/GLSL/MSL cross-compiler using DXC+SPIRV-Cross. Lighter alternative to Slang.

**Physics:**
- **Jolt Physics** — Modern multi-core rigid body physics (used in Horizon Forbidden West). Double-precision support relevant for SparkEngine's large-world origin rebasing.

**Ray Tracing & Denoising:**
- **Intel Embree** — CPU ray tracing kernels for lightmap baking, physics raycasts, editor tools.
- **Intel OIDN** — AI-powered denoiser. Feed noisy 1-4 spp DXR output, get clean result. CPU/GPU paths.

**Global Illumination:**
- **RTXGI-DDGI SDK** — Reference DDGI implementation. SparkEngine's `GlobalIllumination.h` stub already has DDGI structures matching this.
- **Voxel Cone Tracing** — GI without hardware RT. Clipmap variant compatible with SparkEngine's existing terrain clipmap architecture. Works on D3D11.

**Procedural Generation:**
- **FastNoise2** — SIMD-optimized node-graph noise (Simplex, cellular, value). Runtime SIMD level selection (SSE2→AVX-512).
- **FastNoiseLite** — Single-header with HLSL/GLSL versions. Runs directly in shaders for GPU-side procedural generation.

**Animation:**
- **ozz-animation** — High-performance skeletal animation (SoA layout, SIMD blending, animation compression). Could supplement SparkEngine's existing animation system.

**Reference Engines:**
- **Google Filament** — Excellent reference for clustered forward, IBL, PCSS shadows, post-processing. Similar architecture to SparkEngine.
- **Wicked Engine** — Closest open-source analog to SparkEngine. Good reference for compute particles, weather, terrain.

## Source 3: Unity Graphics (SRP/URP/HDRP)

### Render Pipeline Architecture

1. **Pipeline Asset / Feature Toggles** — Unity's HDRPAsset/URPAsset expose per-quality-level feature flags that control both runtime behavior AND shader compilation. Disabling a feature strips shader variants at build time. SparkEngine has CMake toggles but lacks runtime pipeline configurability tied to shader stripping.

2. **Render Graph Compilation Caching** — Unity caches compiled render graph between frames when structure unchanged. SparkEngine should verify its RenderGraph does this.

3. **Hybrid Tile/Cluster Light Culling** — HDRP uses coarse 64x64 Big Tile pre-pass before fine-grained tiled/clustered culling. Reduces work for many-light scenes.

### Lighting & Shadows

4. **Cached Shadow Atlas** — Separate atlas for static shadows rendered once and reused until invalidated. SparkEngine re-renders all shadows every frame.

5. **PCSS (Percentage Closer Soft Shadows)** — Variable penumbra soft shadows with distance-based softness. Dramatically better visual quality than PCF.

6. **Contact Shadows** — Screen-space depth ray marching for small-scale shadows. Up to 24 lights. Mentioned in SparkEngine's ScreenSpaceEffects.h but execution unclear.

7. **Light Layers/Categories** — Per-object bitmask controlling which lights affect it. Useful for art direction (character-only rim light, etc.).

8. **Adaptive Probe Volumes (APV)** — Brick-based hierarchy (4x4x4 probes), adaptive density based on geometry complexity, per-pixel SH sampling, streaming, virtual offsets to prevent light leaking. SparkEngine's LightProbeSystem uses uniform grid.

9. **Reflection Probe Texture Cache** — Cache cubemap renders for static probes. Re-render only when probe is marked dirty.

### Post-Processing

10. **Volume System for Parameter Blending** — Spatial interpolation framework. Volumes (global or local with blend distance) override parameters based on camera position. Enables different post-processing per area with smooth blending. Ties into SparkEngine's area streaming.

11. **Bloom** — HDR bloom with threshold, scatter, and anamorphic modes. Missing from SparkEngine's 10-effect chain.

12. **Auto-Exposure / Eye Adaptation** — Automatic brightness based on scene luminance. Histogram-based. Physical camera mode (ISO, aperture, shutter). Essential for HDR rendering.

13. **Tonemapping** — ACES, Filmic, Neutral tone mapping. HDR→LDR conversion. Currently missing.

14. **Color Grading** — Lift/Gamma/Gain, color curves, shadows/midtones/highlights, white balance, split toning. Currently missing.

### Materials & Shaders

15. **Shader Variant Keyword System** — `multi_compile` (always compiled) vs `shader_feature` (strip if unused) vs `dynamic_branch` (no variants, runtime branch). Pipeline-driven variant stripping. SparkEngine's SparkShaderCompiler lacks systematic keyword/stripping support.

16. **Material Inheritance** — Child materials override specific properties, inherit rest from parent. Valuable for large projects with hundreds of materials.

17. **Shader Graph Block Nodes** — Modular output system (Master Stack) more flexible than fixed SurfaceOutput. Sub-graphs for reusability. Custom function nodes for HLSL injection.

### Advanced Rendering

18. **GTAO (Ground Truth AO)** — More physically accurate than hemisphere SSAO. Multi-pass: compute→spatial denoise→temporal denoise→bilateral upsample.

19. **SSAO Temporal Denoising** — Accumulate SSAO across frames for stability. SparkEngine has blur but no explicit temporal accumulation for AO.

20. **Bilateral Upsample (shared utility)** — Half-res compute effects upsampled with edge-aware bilateral filter. Reused across SSAO, volumetric fog, SSR.

21. **Froxel Volumetric Fog** — 3D frustum-aligned voxel grid (240x135x64). Three passes: media injection → light scattering → temporal+spatial filtering. Accumulates ALL light types.

22. **GPU-Driven Rendering** — Persistent GPU material constant buffers (SRP Batcher pattern). Sort by shader variant to minimize state changes. GPU occlusion culling using previous frame's HiZ depth.

23. **RTHandle System** — Scale-based render texture allocation (fraction of reference resolution). Automatic adaptation to resolution changes. Native dynamic resolution support.

### Resource Management

24. **Pipeline State Pre-warming** — Pre-compile known-needed shader variants at load time rather than caching on first use.

## Prioritized Recommendations (Deduplicated Across All 3 Sources)

### Tier 1 — HIGH Priority (12 items)

| # | Recommendation | Source | Complexity | Already Exists? |
|---|---------------|--------|------------|-----------------|
| 1 | **Volume system for spatial parameter blending** | Unity HDRP | MEDIUM | No — flat PostProcessing config |
| 2 | **Bloom post-processing effect** | Unity HDRP | MEDIUM | No — missing from 10-effect chain |
| 3 | **Auto-exposure / eye adaptation** | Unity HDRP | MEDIUM | No |
| 4 | **Tonemapping (ACES/Filmic/Neutral)** | Unity HDRP | LOW | No |
| 5 | **Color grading (LGG, curves, adjustments)** | Unity HDRP | MEDIUM | No |
| 6 | **meshoptimizer integration** | awesome-graphics | LOW | No — MeshLOD has simplification but no meshlet gen/vertex optimization |
| 7 | **Basis Universal transcoder** | awesome-graphics | LOW | No — raw PNG/JPG loading only |
| 8 | **msdfgen MSDF font rendering** | awesome-graphics | MEDIUM | No game-side text rendering |
| 9 | **Cached shadow atlas** | Unity HDRP | MEDIUM | No — all shadows re-rendered every frame |
| 10 | **UI dirty region tracking** | ThorVG | MEDIUM | No — full redraw every frame |
| 11 | **Shader variant keyword + stripping system** | Unity SRP | MEDIUM | No systematic keyword management |
| 12 | **FastNoiseLite (HLSL)** | awesome-graphics | LOW | No GPU-side noise in shaders |

### Tier 2 — MEDIUM Priority (13 items)

| # | Recommendation | Source | Complexity | Already Exists? |
|---|---------------|--------|------------|-----------------|
| 13 | **PCSS soft shadows** | Unity HDRP | MEDIUM | ShadowAtlas exists but soft shadow quality unclear |
| 14 | **Light layers/categories** | Unity HDRP | LOW | No per-object light masking |
| 15 | **Retained 2D scene graph with update flags** | ThorVG | MEDIUM | No — 2D subsystem has physics only |
| 16 | **Compositor stack with pooled RTs for UI** | ThorVG | MEDIUM | No per-element compositing in UI |
| 17 | **Persistent GPU material constant buffers** | Unity SRP Batcher | MEDIUM | DrawSortKey exists but CB management unclear |
| 18 | **GPU occlusion culling (prev-frame HiZ)** | Unity GPU Resident Drawer | MEDIUM | OcclusionCulling exists, GPU path unclear |
| 19 | **Intel OIDN denoiser** | awesome-graphics | LOW | No — DXR output is raw |
| 20 | **tinyexr HDR/EXR support** | awesome-graphics | LOW | No OpenEXR loading |
| 21 | **ShaderConductor cross-compilation** | awesome-graphics | MEDIUM | SparkShaderCompiler is HLSL-only |
| 22 | **RTHandle scale-based allocation** | Unity SRP Core | MEDIUM | Fixed-size RT pool |
| 23 | **SSAO temporal denoising** | Unity HDRP | LOW | Blur exists but no temporal accumulation |
| 24 | **Bilateral upsample shared utility** | Unity HDRP | LOW | No shared half-res upsample |
| 25 | **Reflection probe texture cache** | Unity HDRP | MEDIUM | ProbeSystem exists but caching unclear |

### Tier 3 — LOW Priority / Future (10 items)

| # | Recommendation | Source | Complexity | Notes |
|---|---------------|--------|------------|-------|
| 26 | **Voxel Cone Tracing** | awesome-graphics | HIGH | GI on D3D11 without hardware RT |
| 27 | **RTXGI-DDGI study** | awesome-graphics | HIGH | GI stubs already exist; needs DXR path active |
| 28 | **Froxel volumetric fog** | Unity HDRP | HIGH | FogSystem is analytical only |
| 29 | **Adaptive Probe Volumes** | Unity HDRP | HIGH | LightProbeSystem uses uniform grid |
| 30 | **Jolt Physics migration** | awesome-graphics | HIGH | Bullet works; Jolt is modern successor |
| 31 | **SVG/Lottie via ThorVG** | ThorVG | LOW-HIGH | Resolution-independent UI + animations |
| 32 | **GTAO upgrade** | Unity HDRP | MEDIUM | Current SSAO uses hemisphere sampling |
| 33 | **FastNoise2 SIMD noise** | awesome-graphics | LOW | CPU-side procedural generation |
| 34 | **Slang shading language** | awesome-graphics | HIGH | Long-term multi-backend shader solution |
| 35 | **NVIDIA vk_lod_clusters (Nanite-like)** | awesome-graphics | HIGH | Future: mesh shaders + continuous LOD |

## Cross-Source Signal Analysis

Patterns that appeared across multiple sources carry more weight:

| Pattern | ThorVG | Unity | Libraries | Signal |
|---------|--------|-------|-----------|--------|
| Dirty tracking / incremental updates | Yes (UI grid) | Yes (APV, cached shadows) | — | Strong |
| Retained scene graph with change flags | Yes (RenderUpdateFlag) | Yes (Volume dirty flags) | — | Strong |
| Scale-based RT pooling | — | Yes (RTHandle) | — | Single |
| GPU texture compression | — | Yes (implicit) | Yes (Basis Universal) | Strong |
| Spatial parameter blending | — | Yes (Volume system) | — | Single but high-impact |
| Shader variant management | — | Yes (keyword/stripping) | Yes (Slang, ShaderConductor) | Strong |
| Font rendering (SDF) | — | — | Yes (msdfgen) | Single but gap-filling |
| Denoising for RT | — | — | Yes (OIDN) | Single |

## Quick Wins (Can Be Done in Days)

1. **FastNoiseLite HLSL** — Drop single header file into shader includes. Immediate GPU noise for terrain/clouds/water.
2. **tinyexr** — Drop single header. OpenEXR loading for lightmaps and IBL probes.
3. **Tonemapping** — Single compute/pixel shader pass. ACES is ~20 lines of HLSL.
4. **SSAO temporal denoising** — Add history buffer and reprojection to existing SSAO pipeline.
5. **Bilateral upsample utility** — Single shared compute shader for half-res effect upsampling.
6. **Light layers** — Add uint32_t bitmask to light and mesh components, AND in lighting shader.

## Notes

- All library recommendations use MIT/Apache/public domain licenses unless otherwise noted
- meshoptimizer, Basis Universal, msdfgen, FastNoiseLite, tinyexr are zero-dependency or near-zero-dependency
- Unity's C# implementation details are not directly portable but the architectural patterns and HLSL shaders are
- Filament and Wicked Engine serve as excellent C++ reference implementations for many Unity-equivalent techniques
- Prior analyses already recommended: job system (5 engines), prefabs (4), handle resources (4) — these remain unaffected
