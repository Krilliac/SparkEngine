# Hardware Acceleration Plan

**Created:** 2026-03-28
**Branch:** `claude/hardware-acceleration-exploration-JxQNL`
**Status:** In Progress

## Executive Summary

SparkEngine has substantial hardware acceleration infrastructure already built but not fully wired.
This plan covers activating existing systems and adding new GPU-driven capabilities across
8 major workstreams. Priority is ordered by impact-to-effort ratio.

---

## Current State Audit

### Already Active
| System | Technology | Status |
|--------|-----------|--------|
| SIMD Math | DirectXMath (SSE2/AVX auto) | Pervasive across 250+ files |
| Multi-ISA Dispatch | `MultiISA.h` SSE2/SSE4/AVX/AVX2 | Framework active, hot paths use it |
| Job System | `JobSystem.h` thread pool + ParallelFor | Wired into engine startup |
| Parallel ECS | `ParallelSystemExecutor` | Concurrent system execution |
| Jolt Physics MT | Jolt internal job dispatch | SIMD + multithreaded broadphase |
| Hybrid RT (SDFGI) | Compute shaders for SDF tracing | `ENABLE_HYBRID_RT=ON` |
| FSR 1.0 Upscaling | EASU/RCAS compute shaders | Inline HLSL, functional |
| GPU Scene Buffer | `GPUSceneBuffer` structured buffer | Per-instance transforms on GPU |
| HiZ Occlusion | `GPUOcclusionCulling` | CPU reference impl, GPU types ready |

### Built But Disabled
| System | Lines | Blocker |
|--------|-------|---------|
| DXR 1.1 Ray Tracing | 45K | `ENABLE_DXR=ON` (SDFGI fallback active; DXR wired in on Windows) |
| FSR 2.0 / DLSS / XeSS | ~2K | Vendor SDK not linked |
| Mesh Cluster System | 820 | GPU rasterization path incomplete |
| Render Graph Async Compute | 1.7K | Pass type exists, no workloads assigned |

### Not Yet Implemented
| System | Impact | Effort |
|--------|--------|--------|
| GPU Compute Particles | High | 2-3 weeks |
| GPU Skinning | High | 2-3 weeks |
| Async Compute Queue | Medium-High | 2-3 weeks |
| GPU-Driven Indirect Rendering | Medium-High | 3-4 weeks |
| GPU Clustered Light Culling | Medium | 1-2 weeks |
| Mesh Shaders | Medium | 4-6 weeks |
| DirectStorage | Medium | 2 weeks |
| Hardware Video Decode | Low | 3-4 weeks |

---

## Implementation Plan

### Phase 1: GPU Compute Particles (Priority: Highest)

**Goal:** Move particle simulation from CPU to GPU compute shaders.

**Current state:** `ParticleSystem.cpp` (1,240 lines) runs all simulation on CPU.
`GPUParticleTypes.h` defines GPU-uploadable particle structures.

**New files:**
- `Shaders/HLSL/Compute/ParticleSimulate.hlsl` - Compute shader for particle update
- `Shaders/HLSL/Compute/ParticleEmit.hlsl` - Compute shader for particle emission
- `Shaders/HLSL/Compute/ParticleBitonicSort.hlsl` - Distance sorting for transparency
- `SparkEngine/Source/Graphics/GPUParticleSystem.h` - GPU particle manager
- `SparkEngine/Source/Graphics/GPUParticleSystem.cpp` - Implementation

**Architecture:**
```
CPU: EmitterDesc -> Emit params -> Dispatch emit CS
GPU: Emit CS -> Simulate CS -> Sort CS -> Indirect Draw
```

**Key data structures:**
- `RWStructuredBuffer<GPUParticle>` - Particle pool (position, velocity, life, size, color)
- `AppendStructuredBuffer<uint>` - Dead particle index list
- `RWBuffer<uint>` - Alive count + indirect args
- `RWStructuredBuffer<float>` - Sort keys (camera distance)

**Capacity:** 1M particles per system (vs ~10K CPU limit).

---

### Phase 2: GPU Skinning (Priority: High)

**Goal:** Offload skeletal mesh vertex skinning from VS to compute shader.

**Current state:** `AnimationSystem.cpp` (1,409 lines) computes bone matrices on CPU,
uploads to constant buffer, vertex shader applies skinning.

**New files:**
- `Shaders/HLSL/Compute/SkinningCS.hlsl` - Compute shader for vertex skinning
- `SparkEngine/Source/Graphics/GPUSkinning.h` - GPU skinning manager
- `SparkEngine/Source/Graphics/GPUSkinning.cpp` - Implementation

**Architecture:**
```
CPU: Evaluate animation -> Bone matrices to structured buffer
GPU: Compute shader reads source VB + bone matrices -> Writes skinned VB
     Vertex shader reads pre-skinned vertices (no skinning math)
```

**Benefits:**
- Decouples animation evaluation from vertex count
- Enables GPU-driven rendering of skinned meshes
- Constant buffer pressure reduced (no bone matrix array per draw)

---

### Phase 3: Async Compute Queue (Priority: Medium-High)

**Goal:** Run heavy compute workloads on async compute queue parallel to graphics.

**Current state:** `RenderGraph.h` defines `PassType::AsyncCompute` but no workloads use it.
D3D12 and Vulkan backends have queue separation capability.

**New files:**
- `SparkEngine/Source/Graphics/AsyncComputeScheduler.h` - Async compute manager
- `SparkEngine/Source/Graphics/AsyncComputeScheduler.cpp` - Implementation

**Workloads to move to async compute:**
1. HiZ pyramid generation (post depth prepass)
2. Clustered light culling
3. SDFGI probe updates
4. Particle simulation
5. GPU skinning

**Sync points:** Fence-based synchronization between compute and graphics queues.

---

### Phase 4: GPU-Driven Indirect Rendering (Priority: Medium-High)

**Goal:** Eliminate per-draw CPU overhead with GPU-generated draw commands.

**Current state:**
- `MeshClusterSystem.h` (820 lines) defines Nanite-style cluster DAG
- `GPUOcclusionCulling` has HiZ infrastructure
- `GPUSceneBuffer` has per-instance transforms
- `ExecuteIndirect`/`DrawIndexedInstancedIndirect` in RHI

**New files:**
- `Shaders/HLSL/Compute/GPUCull.hlsl` - Frustum + HiZ culling compute shader
- `Shaders/HLSL/Compute/BuildIndirectArgs.hlsl` - Indirect args generation
- `SparkEngine/Source/Graphics/GPUDrivenRenderer.h` - GPU-driven pipeline manager
- `SparkEngine/Source/Graphics/GPUDrivenRenderer.cpp` - Implementation

**Pipeline:**
```
1. Depth prepass (traditional)
2. Build HiZ pyramid (compute)
3. GPU frustum + occlusion cull (compute) -> visibility buffer
4. Compact visible instances (compute) -> indirect args buffer
5. ExecuteIndirect for all visible geometry (single call)
```

---

### Phase 5: DXR Integration into Render Loop (Priority: Medium)

**Goal:** Wire existing 45K-line DXR implementation into main render loop.

**Current state:** `DXRSupport.cpp/h` is complete, `ENABLE_DXR=ON` by default,
DXR wired into `GraphicsEngine::RenderScene()` behind `#ifdef SPARK_HARDWARE_RT`.

**Changes to existing files:**
- `GraphicsEngine.cpp` - Add DXR dispatch after G-buffer/lighting pass
- `RenderPipeline.cpp` - Add RT pass to render graph
- `DXRSupport.cpp` - Wire TLAS rebuild per frame

**No new files needed** - just wiring existing code.

---

### Phase 6: GPU Clustered Light Culling (Priority: Medium)

**Goal:** Move light-to-cluster assignment from CPU to compute shader.

**Current state:** `ClusteredLightCulling.cpp` (281 lines) does CPU-side assignment.

**New files:**
- `Shaders/HLSL/Compute/ClusterCull.hlsl` - Light assignment compute shader
- Update `ClusteredLightCulling.cpp` to dispatch compute instead of CPU loop

**Data flow:**
```
CPU: Upload light list to structured buffer
GPU: Compute shader assigns lights to 3D cluster grid
     Structured buffer output: per-cluster light indices
     Forward+ pixel shader samples cluster buffer
```

---

### Phase 7: DirectStorage Integration (Priority: Medium-Low)

**Goal:** GPU-direct asset decompression for streaming.

**New files:**
- `SparkEngine/Source/Engine/Streaming/DirectStorageLoader.h`
- `SparkEngine/Source/Engine/Streaming/DirectStorageLoader.cpp`

**Architecture:**
```
SSD -> DirectStorage Queue -> GPU Memory (bypass CPU)
Fallback: Traditional async file I/O on non-Win11 systems
```

---

### Phase 8: Mesh Shader Pipeline (Priority: Low)

**Goal:** Replace vertex pipeline with mesh/amplification shaders for D3D12.

**New files:**
- `Shaders/HLSL/MeshShaders/MeshletMS.hlsl` - Mesh shader
- `Shaders/HLSL/MeshShaders/MeshletAS.hlsl` - Amplification shader
- `SparkEngine/Source/Graphics/MeshShaderPipeline.h`
- `SparkEngine/Source/Graphics/MeshShaderPipeline.cpp`

**Requires:** D3D12 or Vulkan backend active. Falls back to traditional pipeline on D3D11.

---

## Test Plan

Each system gets dedicated tests in `Tests/`:

| System | Test File | Key Tests |
|--------|----------|-----------|
| GPU Particles | `TestGPUParticleSystem.cpp` | Emission, simulation, sorting, capacity |
| GPU Skinning | `TestGPUSkinning.cpp` | Bone matrix upload, vertex transform accuracy |
| Async Compute | `TestAsyncComputeScheduler.cpp` | Queue creation, fence sync, workload dispatch |
| GPU-Driven | `TestGPUDrivenRenderer.cpp` | Cull accuracy, indirect args generation |
| DXR Wiring | `TestDXRIntegration.cpp` | TLAS build, feature enable/disable |
| Cluster Cull | `TestGPUClusterCull.cpp` | Light assignment accuracy, boundary cases |
| DirectStorage | `TestDirectStorageLoader.cpp` | Fallback path, queue management |
| Mesh Shaders | `TestMeshShaderPipeline.cpp` | Meshlet generation, pipeline creation |

---

## CMake Integration

New build toggles:
```cmake
option(ENABLE_GPU_PARTICLES "Enable GPU compute particle simulation" ON)
option(ENABLE_GPU_SKINNING "Enable GPU compute vertex skinning" ON)
option(ENABLE_ASYNC_COMPUTE "Enable async compute queue scheduling" ON)
option(ENABLE_GPU_DRIVEN "Enable GPU-driven indirect rendering" ON)
option(ENABLE_DIRECTSTORAGE "Enable DirectStorage for GPU-direct I/O" OFF)
option(ENABLE_MESH_SHADERS "Enable mesh shader pipeline (D3D12/Vulkan)" OFF)
```

---

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| D3D11 lacks async compute | Fallback to immediate context dispatch |
| Compute shader debugging | GPU debug markers + PIX/RenderDoc integration |
| Particle sort perf | Bitonic sort is O(n log^2 n); radix sort if needed |
| Mesh shader adoption | Optional path, traditional pipeline always available |
| DirectStorage Win11-only | Transparent fallback to async file I/O |

---

## Success Metrics

- GPU particle system handles 500K+ particles at 60fps
- GPU skinning supports 100+ animated characters simultaneously
- GPU-driven rendering reduces draw call CPU time by 80%+
- Async compute improves GPU utilization by 15-25%
- All existing tests continue passing
