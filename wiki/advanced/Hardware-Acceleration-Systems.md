# Hardware Acceleration Systems

> **Audience:** Programmers
>
> **Thread Context:** GPU work is submitted from the main render thread. Async compute is D3D11 immediate-dispatch today, designed for D3D12/Vulkan async queues later. DXR TLAS is rebuilt per frame inside `GraphicsEngine::RenderScene()`.
>
> **Platform/Backend Scope:** D3D11/D3D12 primary on Windows. All D3D code is behind `#ifdef SPARK_PLATFORM_WINDOWS` with stub fallbacks. Mesh shaders are D3D12/Vulkan only (D3D11 falls back to traditional rendering). DirectStorage uses an async-I/O fallback on all platforms (SDK not linked). DXR is gated behind `SPARK_HARDWARE_RT`.

## Overview

SparkEngine implements GPU hardware acceleration across eight systems: GPU compute particles, GPU skinning, async compute scheduling, GPU-driven indirect rendering, DXR ray tracing, GPU clustered light culling, DirectStorage I/O, and a mesh-shader pipeline. Much of the infrastructure had been built but not wired; this work activated existing systems and added new GPU-driven capabilities.

## New systems implemented

All files confirmed present 2026-06-08.

| System | Header | Impl | Shaders | Tests |
|---|---|---|---|---|
| GPU Compute Particles | `Graphics/GPUParticleSystem.h` | `GPUParticleSystem.cpp` | `ParticleSimulate/Emit/BitonicSort.hlsl` | `TestGPUParticleSystem.cpp` (11) |
| GPU Skinning | `Graphics/GPUSkinning.h` | `GPUSkinning.cpp` | `SkinningCS.hlsl` | `TestGPUSkinning.cpp` (9) |
| Async Compute | `Graphics/AsyncComputeScheduler.h` | `AsyncComputeScheduler.cpp` | — | `TestAsyncComputeScheduler.cpp` (9) |
| GPU-Driven Renderer | `Graphics/GPUDrivenRenderer.h` | `GPUDrivenRenderer.cpp` | `GPUCull.hlsl`, `HiZBuild.hlsl` | `TestGPUDrivenRenderer.cpp` (12) |
| GPU Cluster Culling | `Graphics/GPUClusterCulling.h` | `GPUClusterCulling.cpp` | `ClusterCull.hlsl` | `TestGPUClusterCulling.cpp` (11) |
| DirectStorage | `Engine/Streaming/DirectStorageLoader.h` | `DirectStorageLoader.cpp` | — | `TestDirectStorageLoader.cpp` (11) |
| Mesh Shader Pipeline | `Graphics/MeshShaderPipeline.h` | `MeshShaderPipeline.cpp` | `MeshletMS/AS.hlsl` | `TestMeshShaderPipeline.cpp` (9) |

## Existing systems wired

- **DXR** — wired into `GraphicsEngine::RenderScene()`. Verified 2026-06-08 in `SparkEngine/Source/Graphics/GraphicsEngineWindows.cpp`: under `#ifdef SPARK_HARDWARE_RT`, the renderer rebuilds the TLAS each frame (`dxr.BuildTLAS(m_dxrInstances)`) and dispatches enabled RT effects (reflections, shadows, AO, GI). A `HybridRT` path selects `RayTracingBackend::HardwareDXR` when available.

> **Note on file layout:** the original audit placed the DXR hook in `GraphicsEngine.cpp`. The renderer has since been split per-platform; the DXR wiring now lives in `GraphicsEngineWindows.cpp` (with `GraphicsRenderPipelinesWindows.cpp` also referencing `SPARK_HARDWARE_RT`). Supporting RT code: `Graphics/RHI/DXRSupport.{h,cpp}`, `Graphics/HybridRT/HybridRTManager.cpp`, and `Graphics/RHI/Metal/MetalRayTracing.mm` for the Metal backend.

## Architecture decisions

1. All D3D11 code behind `#ifdef SPARK_PLATFORM_WINDOWS` with stub fallbacks.
2. GPU particles use append/consume structured buffers + bitonic sort.
3. GPU skinning stores bones as compact 4×3 matrices (48 bytes vs 64 for 4×4).
4. Async compute is D3D11 immediate-dispatch now, designed for D3D12/Vulkan async queues later.
5. GPU-driven renderer: HiZ + frustum-cull compute shader → visibility buffer → indirect draw.
6. DirectStorage exposes the full API with a transparent async-file-I/O fallback.
7. Mesh-shader pipeline includes a meshlet builder with greedy triangle packing.

## Key file locations

- Compute shaders: `Shaders/HLSL/Compute/`
- Mesh shaders: `Shaders/HLSL/MeshShaders/`
- C++ systems: `SparkEngine/Source/Graphics/`
- DirectStorage: `SparkEngine/Source/Engine/Streaming/`
- Tests: `Tests/` (7 files, ~72 tests)

## Notes

- `GPUParticleSystem.cpp` exceeds the ~500-line guideline but is one cohesive unit.
- The mesh-shader pipeline is D3D12/Vulkan only; D3D11 falls back to traditional rendering.
- DirectStorage currently uses the fallback path on all platforms — verified 2026-06-08 in `DirectStorageLoader.cpp`: comments note "async I/O fallback since full queue integration requires a D3D12 device" and "DirectStorage is Windows-only; Linux/macOS use async I/O fallback." The SDK is not linked.

## Source & Freshness

- Original entry: `.claude/knowledge/hardware-acceleration-systems.md` (last updated 2026-03-28).
- Verified against codebase 2026-06-08.

Status changes / verifications found during freshening:

- All 7 new-system header/impl pairs and all 7 test files confirmed present.
- **DXR wiring moved file:** now in `GraphicsEngineWindows.cpp` (the renderer was split per-platform), not `GraphicsEngine.cpp`. `BuildTLAS` per-frame call and `SPARK_HARDWARE_RT` gate confirmed.
- DirectStorage still on the fallback path (SDK not linked) — confirmed by in-file comments.
- Metal ray-tracing backend (`MetalRayTracing.mm`) now present alongside the DXR/HybridRT paths.
- No regressions; all named systems still present.

## Related Pages

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md)
- [Performance Profiling Guide](Performance-Profiling-Guide.md)
- [Performance Tips](Performance-Tips.md)
- [Benchmark Framework](Benchmark-Framework.md)
- [Stub & Abandoned Features](Stub-and-Abandoned-Features.md)
