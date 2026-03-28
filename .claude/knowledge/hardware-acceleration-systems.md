# Hardware Acceleration Systems

**Last updated:** 2026-03-28
**Type:** Decision
**Status:** Active

## Description

Comprehensive hardware acceleration implementation across 8 systems:
GPU compute particles, GPU skinning, async compute scheduling, GPU-driven
indirect rendering, DXR integration, GPU clustered light culling,
DirectStorage I/O, and mesh shader pipeline.

## Context

SparkEngine had significant GPU acceleration infrastructure built but not wired.
This session activated existing systems and implemented new GPU-driven capabilities.

## Details

### New Systems Implemented

| System | Header | Impl | Shader | Tests |
|--------|--------|------|--------|-------|
| GPU Compute Particles | GPUParticleSystem.h | GPUParticleSystem.cpp | ParticleSimulate/Emit/BitonicSort.hlsl | TestGPUParticleSystem.cpp (11 tests) |
| GPU Skinning | GPUSkinning.h | GPUSkinning.cpp | SkinningCS.hlsl | TestGPUSkinning.cpp (9 tests) |
| Async Compute | AsyncComputeScheduler.h | AsyncComputeScheduler.cpp | — | TestAsyncComputeScheduler.cpp (9 tests) |
| GPU-Driven Renderer | GPUDrivenRenderer.h | GPUDrivenRenderer.cpp | GPUCull.hlsl, HiZBuild.hlsl | TestGPUDrivenRenderer.cpp (12 tests) |
| GPU Cluster Culling | GPUClusterCulling.h | GPUClusterCulling.cpp | ClusterCull.hlsl | TestGPUClusterCulling.cpp (11 tests) |
| DirectStorage | DirectStorageLoader.h | DirectStorageLoader.cpp | — | TestDirectStorageLoader.cpp (11 tests) |
| Mesh Shader Pipeline | MeshShaderPipeline.h | MeshShaderPipeline.cpp | MeshletMS/AS.hlsl | TestMeshShaderPipeline.cpp (9 tests) |

### Existing Systems Wired

- **DXR**: Wired into `GraphicsEngine::RenderScene()` between pipeline render and post-processing
- Build TLAS per frame, dispatch enabled RT effects (reflections, shadows, AO, GI)
- Gated behind `#ifdef SPARK_HARDWARE_RT`

### Architecture Decisions

1. All D3D11 code behind `#ifdef SPARK_PLATFORM_WINDOWS` with stub fallbacks
2. GPU particle system uses append/consume structured buffers + bitonic sort
3. GPU skinning stores bones as compact 4x3 matrices (48 bytes vs 64 for 4x4)
4. Async compute is D3D11 immediate dispatch now, designed for D3D12/Vulkan async queues later
5. GPU-driven renderer uses HiZ + frustum cull compute shader -> visibility buffer -> indirect draw
6. DirectStorage has full API with transparent async file I/O fallback
7. Mesh shader pipeline includes meshlet builder with greedy triangle packing

### Key File Locations

- Compute shaders: `Shaders/HLSL/Compute/` (7 files)
- Mesh shaders: `Shaders/HLSL/MeshShaders/` (2 files)
- C++ systems: `SparkEngine/Source/Graphics/` (14 new files)
- DirectStorage: `SparkEngine/Source/Engine/Streaming/` (2 new files)
- Tests: `Tests/` (7 new files, 72 total tests)
- Plan: `docs/plans/hardware-acceleration-plan.md`

## Notes

- Total new tests: 72 across 7 test files
- All existing 2,000+ tests continue passing
- GPUParticleSystem.cpp is 691 lines (above 500 guideline) but is one cohesive unit
- Mesh shader pipeline is D3D12/Vulkan only; D3D11 falls back to traditional rendering
- DirectStorage currently uses fallback path on all platforms (SDK not linked)
