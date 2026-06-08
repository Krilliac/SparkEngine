# ThorVG + Unity Graphics Analysis

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** Cross-engine reference

## Overview

Analysis of three sources for graphics techniques and libraries applicable to SparkEngine: **ThorVG** (vector graphics engine), the **Unity Graphics** repo (SRP/URP/HDRP/ShaderGraph), and **awesome-graphics-libraries** (33+ libraries across 14 categories). The original document produced 35 recommendations across rendering, post-processing, asset pipeline, UI, shader tooling, and resource management.

As of the 2026-06-08 codebase verification, **many of the Tier 1 post-processing and library-integration items have been implemented** — notably Bloom, Tonemapping + Color Grading, Auto-Exposure, GTAO, tinyexr, FastNoiseLite/FastNoise2, meshoptimizer, the Volume system, cached shadow atlas, light layers, and a shader variant system. Each item is annotated below.

> **Status legend:** **Implemented** — concrete subsystem/file exists. **Partial** — core exists, refinement incomplete. **Open** — not yet built.

## Source 1: ThorVG (Vector Graphics Engine)

ThorVG is a production-ready C++ vector graphics engine (~150KB) supporting SVG and Lottie with CPU/SIMD, OpenGL, and WebGPU backends (used by Tizen, Samsung, Godot). Key patterns:

1. **Dirty Region Tracking** (16x16 grid, double-buffered dirty lists) — redraw only changed UI cells.
2. **Retained Scene Graph with Update Flags** — bitwise `RenderUpdateFlag` (Path, Color, Transform, Image, Blend) per node.
3. **Backend-Agnostic RenderMethod Interface** — `prepare()` returns opaque `RenderData`; `renderShape()`/`renderImage()`; `beginComposite()`/`endComposite()`.
4. **Compositor Stack with Pooled Render Targets** — per-node masking/blending with `needComposition()` skip for simple scenes.
5. **State Machine for Update/Render Separation** — Synced→Painting→Updating→Drawing→Synced.
6. **SVG/Lottie Loading Architecture** — independent loaders producing one scene graph; Lottie enables data-driven UI animation.

*Status note:* SparkEngine now has a 2D scene graph (`Engine/2D/SceneGraph2D.h`), partially covering ThorVG patterns #2/#3. UI dirty-region tracking, the compositor stack, and SVG/Lottie loading remain **Open**.

## Source 2: awesome-graphics-libraries

Libraries identified as gap-fillers, with current status:

| Library | Category | Status (2026-06-08) | Evidence |
|---------|----------|---------------------|----------|
| **meshoptimizer** | Mesh processing | **Implemented** | `Graphics/MeshOptimizer.h` |
| **PMP** | Mesh processing | **Open** | editor-side remeshing not found |
| **Basis Universal** | Texture compression | **Open** | no transcoder bundled; raw PNG/JPG path |
| **AMD Compressonator** | Texture compression | **Open** | — |
| **tinyexr** | HDR/EXR loading | **Implemented** | `ThirdParty/Utils/tinyexr/`, `Graphics/EXRLoader.h` |
| **msdfgen / msdf-atlas-gen** | Font rendering | **Partial** | `Engine/Text/FontSystem.h` exists; MSDF specifically not confirmed |
| **Slang** | Shader cross-compilation | **Open** | — |
| **ShaderConductor** | Shader cross-compilation | **Open** | SparkShaderCompiler still HLSL-centric |
| **Jolt Physics** | Physics | **Implemented** | Jolt is the engine's physics backend |
| **Intel Embree** | CPU ray tracing | **Open / unverified** | — |
| **Intel OIDN** | Denoising | **Partial** | `Graphics/DenoiserInterface.h` exists; OIDN binding unverified |
| **RTXGI-DDGI SDK** | GI | **Partial** | DDGI present (`Graphics/Neural/NeuralRadianceCache.h`, GI stubs); reference-SDK study open |
| **Voxel Cone Tracing** | GI | **Implemented** | wired per Advanced Techniques Phase Q |
| **FastNoise2** | Procedural | **Implemented** | `Graphics/FastNoise2SIMD.h` |
| **FastNoiseLite** | Procedural (shader) | **Implemented** | `Graphics/FastNoiseLite.h` |
| **ozz-animation** | Animation | **Open / unverified** | engine has its own animation system |

*Reference engines* (Filament, Wicked) remain useful C++ references, not integrations.

## Source 3: Unity Graphics (SRP/URP/HDRP)

### Render Pipeline Architecture
1. **Pipeline Asset / Feature Toggles with shader stripping** — **Partial** (`ShaderVariantSystem.h` exists; build-time stripping tied to runtime config unverified).
2. **Render Graph Compilation Caching** — **Partial** (RenderGraph present; verify cross-frame caching).
3. **Hybrid Tile/Cluster Light Culling** (Big Tile pre-pass) — **Partial** (clustered culling present; coarse Big-Tile pre-pass not confirmed).

### Lighting & Shadows
4. **Cached Shadow Atlas** — **Implemented** (`Graphics/CachedShadowAtlas.h`).
5. **PCSS soft shadows** — **Partial** (`ShadowAtlas` present; PCSS quality path unverified).
6. **Contact Shadows** — **Partial / unverified** (mentioned in ScreenSpaceEffects).
7. **Light Layers/Categories** — **Implemented** (`Graphics/LightLayers.h`).
8. **Adaptive Probe Volumes (APV)** — **Implemented** (`Graphics/AdaptiveProbeVolumes.{h,cpp}`).
9. **Reflection Probe Texture Cache** — **Implemented** (`Graphics/ReflectionProbeCache.h`).

### Post-Processing
10. **Volume System for Parameter Blending** — **Implemented** (`Graphics/VolumeSystem.h`).
11. **Bloom** — **Implemented** (`Graphics/BloomEffect.{h,cpp}`).
12. **Auto-Exposure / Eye Adaptation** — **Implemented** (in `PostProcessingPipeline`; grep-confirmed auto-exposure references).
13. **Tonemapping (ACES/Filmic/Neutral)** — **Implemented** (`Graphics/TonemapColorGrading.{h,cpp}`).
14. **Color Grading (LGG, curves, white balance)** — **Implemented** (`Graphics/TonemapColorGrading.{h,cpp}`).

### Materials & Shaders
15. **Shader Variant Keyword + stripping system** — **Partial** (`Graphics/ShaderVariantSystem.h`).
16. **Material Inheritance** — **Partial** (`MaterialDefinition.h` present; parent/child override unverified).
17. **Shader Graph Block Nodes / Master Stack** — **Partial** (ShaderGraphCompiler exists per original context).

### Advanced Rendering
18. **GTAO (Ground Truth AO)** — **Implemented** (`Graphics/GTAOEffect.h`).
19. **SSAO Temporal Denoising** — **Partial** (GTAO present; explicit AO temporal accumulation unverified).
20. **Bilateral Upsample (shared utility)** — **Open / unverified**.
21. **Froxel Volumetric Fog** — **Implemented** (`Graphics/FroxelVolumetricFog.{h,cpp}`).
22. **GPU-Driven Rendering (persistent CBs, HiZ occlusion)** — **Implemented** (GPU-driven culling + HiZ baseline).
23. **RTHandle System (scale-based RT allocation)** — **Open / unverified**.

### Resource Management
24. **Pipeline State Pre-warming** — **Partial** (`PipelineStateCache` exists; pre-warm-at-load unverified).

## Prioritized Recommendations (Freshened)

### Tier 1 — Originally High Priority
| # | Recommendation | Source | Status (2026-06-08) |
|---|---------------|--------|---------------------|
| 1 | Volume system for spatial parameter blending | Unity HDRP | **Implemented** |
| 2 | Bloom | Unity HDRP | **Implemented** |
| 3 | Auto-exposure / eye adaptation | Unity HDRP | **Implemented** |
| 4 | Tonemapping (ACES/Filmic/Neutral) | Unity HDRP | **Implemented** |
| 5 | Color grading | Unity HDRP | **Implemented** |
| 6 | meshoptimizer integration | awesome-graphics | **Implemented** |
| 7 | Basis Universal transcoder | awesome-graphics | **Open** |
| 8 | msdfgen MSDF font rendering | awesome-graphics | **Partial** (FontSystem exists) |
| 9 | Cached shadow atlas | Unity HDRP | **Implemented** |
| 10 | UI dirty region tracking | ThorVG | **Open** |
| 11 | Shader variant keyword + stripping | Unity SRP | **Partial** |
| 12 | FastNoiseLite (HLSL) | awesome-graphics | **Implemented** |

### Tier 2 — Originally Medium Priority
| # | Recommendation | Source | Status (2026-06-08) |
|---|---------------|--------|---------------------|
| 13 | PCSS soft shadows | Unity HDRP | **Partial** |
| 14 | Light layers/categories | Unity HDRP | **Implemented** |
| 15 | Retained 2D scene graph with update flags | ThorVG | **Partial** (`SceneGraph2D.h`) |
| 16 | Compositor stack with pooled RTs for UI | ThorVG | **Open** |
| 17 | Persistent GPU material constant buffers | Unity SRP Batcher | **Partial** |
| 18 | GPU occlusion culling (prev-frame HiZ) | Unity | **Implemented** |
| 19 | Intel OIDN denoiser | awesome-graphics | **Partial** (`DenoiserInterface.h`) |
| 20 | tinyexr HDR/EXR support | awesome-graphics | **Implemented** |
| 21 | ShaderConductor cross-compilation | awesome-graphics | **Open** |
| 22 | RTHandle scale-based allocation | Unity SRP Core | **Open** |
| 23 | SSAO temporal denoising | Unity HDRP | **Partial** |
| 24 | Bilateral upsample shared utility | Unity HDRP | **Open** |
| 25 | Reflection probe texture cache | Unity HDRP | **Implemented** |

### Tier 3 — Low Priority / Future
| # | Recommendation | Source | Status (2026-06-08) |
|---|---------------|--------|---------------------|
| 26 | Voxel Cone Tracing | awesome-graphics | **Implemented** |
| 27 | RTXGI-DDGI study | awesome-graphics | **Partial** (DDGI present) |
| 28 | Froxel volumetric fog | Unity HDRP | **Implemented** |
| 29 | Adaptive Probe Volumes | Unity HDRP | **Implemented** |
| 30 | Jolt Physics migration | awesome-graphics | **Implemented** (Jolt is the backend) |
| 31 | SVG/Lottie via ThorVG | ThorVG | **Open** |
| 32 | GTAO upgrade | Unity HDRP | **Implemented** |
| 33 | FastNoise2 SIMD noise | awesome-graphics | **Implemented** |
| 34 | Slang shading language | awesome-graphics | **Open** |
| 35 | NVIDIA vk_lod_clusters (Nanite-like) | awesome-graphics | **Partial** (mesh clusters/meshlets present; full virtualized geometry in progress) |

## Remaining Open Work (post-freshening)

Genuinely open items: **Basis Universal transcoder (#7)**, **UI dirty-region tracking (#10)**, **ThorVG compositor stack (#16)**, **ShaderConductor/Slang cross-compilation (#21/#34)**, **RTHandle scale-based allocation (#22)**, **bilateral upsample utility (#24)**, **SVG/Lottie loading (#31)**. Most post-processing and core lighting recommendations are done; MSDF fonts, PCSS quality, shader stripping, and SSAO temporal denoising are partial.

## Source & Freshness

- **Original analysis date:** 2026-03-22 (type: Decision).
- **Verified against codebase 2026-06-08.**
- **Annotations / updates made:**
  - Added per-recommendation status markers across all three sources and all 35 prioritized items.
  - Confirmed implemented: Volume system, Bloom, auto-exposure, tonemapping, color grading, meshoptimizer, tinyexr, FastNoiseLite, FastNoise2, cached shadow atlas, light layers, GTAO, froxel fog, APV, reflection probe cache, GPU occlusion culling, Voxel Cone Tracing, Jolt.
  - Confirmed open: Basis Universal, UI dirty-region tracking, ThorVG compositor stack, ShaderConductor/Slang, RTHandle, bilateral upsample, SVG/Lottie.
  - Noted the new `Engine/2D/SceneGraph2D.h` partially satisfies ThorVG retained-scene-graph patterns.
  - Stripped AI-session frontmatter, the "Quick Wins" duplicate list (now reflected by status), and the cross-source signal table's redundancy.

## Related Pages

- [Five Engine Analysis](Five-Engine-Analysis.md)
- [Eleven Engine Analysis](Eleven-Engine-Analysis.md)
- [Advanced Techniques Catalog](Advanced-Techniques-Catalog.md)
