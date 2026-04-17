# GPU-Driven Rendering

SparkEngine implements a GPU-driven rendering pipeline that performs frustum and hierarchical Z-buffer (HiZ) occlusion culling entirely on the GPU via compute shaders, then issues geometry draws through indirect dispatch. This eliminates CPU-side per-object visibility checks for scenes with thousands of instances.

**Source:** `SparkEngine/Source/Graphics/GPUDrivenRenderer.h`, `SparkEngine/Source/Graphics/GPUOcclusionCulling.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestGPUDrivenRenderer.cpp` (13 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Pipeline](#pipeline)
- [GPUDrivenRenderer](#gpudrivenrenderer)
  - [Cull Settings](#cull-settings)
  - [Statistics](#statistics)
  - [GPU Instance Data](#gpu-instance-data)
  - [API Reference](#api-reference)
- [GPU Occlusion Culling](#gpu-occlusion-culling)
  - [HiZ Pyramid](#hiz-pyramid)
  - [Occlusion Testing](#occlusion-testing)
  - [Batch Culling](#batch-culling)
- [Usage Example](#usage-example)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

Traditional rendering tests each object's visibility on the CPU, which becomes a bottleneck with thousands of draw calls. GPU-driven rendering uploads all instance bounding boxes to the GPU, runs a compute shader to cull invisible instances, and writes draw arguments directly — the CPU never touches per-instance visibility.

```
┌─────────────────────────────────────────────────────────────┐
│ Frame N-1: Render scene → depth buffer                      │
└───────────────────────────┬─────────────────────────────────┘
                            │
                            v
┌─────────────────────────────────────────────────────────────┐
│ BeginFrame(): Build HiZ mip chain from depth buffer         │
│                                                             │
│  Mip 0 (full res) → Mip 1 (half) → ... → Mip N (1x1)     │
│  Each mip stores the MAX depth of the 2x2 parent region    │
└───────────────────────────┬─────────────────────────────────┘
                            │
                            v
┌─────────────────────────────────────────────────────────────┐
│ CullAndDraw():                                              │
│  1. Upload instance AABBs to GPU structured buffer          │
│  2. Dispatch cull compute shader:                           │
│     - Frustum plane test                                    │
│     - HiZ occlusion test (project AABB → screen rect →     │
│       sample HiZ at appropriate mip)                        │
│  3. Write indirect draw args for visible instances          │
│  4. DrawIndexedInstancedIndirect                            │
└─────────────────────────────────────────────────────────────┘
```

---

## Pipeline

Each frame follows two stages:

### Stage 1: BeginFrame

Builds the HiZ mip chain from the previous frame's depth buffer. Each mip level stores the maximum depth of its parent 2x2 region, creating a conservative depth pyramid.

### Stage 2: CullAndDraw

1. **Upload**: Instance AABBs are uploaded to a GPU structured buffer
2. **Cull dispatch**: A compute shader tests each AABB against:
   - **Frustum planes**: 6-plane frustum test eliminates objects fully outside the view
   - **HiZ occlusion**: Projects the AABB to a screen-space rectangle, selects the HiZ mip level matching the rectangle size, and compares the AABB's nearest depth against the stored maximum depth
3. **Indirect args**: Visible instances write their draw arguments to an indirect buffer
4. **Draw**: `DrawIndexedInstancedIndirect` renders all visible geometry in one call

A 1-frame-deferred readback avoids CPU-GPU sync stalls when reading back statistics.

---

## GPUDrivenRenderer

### Cull Settings

```cpp
struct CullSettings
{
    bool enableFrustumCull = true;  // Frustum plane culling
    bool enableHiZCull = true;      // Hierarchical Z-buffer occlusion
    bool freezeCulling = false;     // Debug: freeze at current camera
};
```

### Statistics

```cpp
struct CullStatistics
{
    uint32_t totalInstances;    // Submitted for culling
    uint32_t visibleInstances;  // Passed all tests
    uint32_t culledByFrustum;   // Removed by frustum
    uint32_t culledByHiZ;       // Removed by occlusion
};
```

### GPU Instance Data

```cpp
struct alignas(16) GPUInstanceAABB
{
    float minX, minY, minZ;
    float padding0;
    float maxX, maxY, maxZ;
    float padding1;
};
```

16-byte aligned for GPU structured buffer compatibility.

### API Reference

| Method | Description |
|--------|-------------|
| `Initialize(device, context, maxInstances)` | Create GPU resources. Default max: 8192 instances |
| `Shutdown()` | Release all GPU resources |
| `BeginFrame(depthSRV, width, height)` | Build HiZ mip chain from depth buffer |
| `CullAndDraw(aabbs, count, view, proj, ib, vb, stride, indexCount)` | Cull and draw |
| `GetVisibleCount()` | Visible instances from last frame |
| `GetStatistics()` | Full `CullStatistics` struct |
| `GetSettings()` / `SetSettings()` | Read/write `CullSettings` |
| `Console_GetStatus()` | Formatted debug string |

---

## GPU Occlusion Culling

The `GPUOcclusionCuller` provides a standalone HiZ occlusion testing API, usable independently of the full GPU-driven renderer.

### HiZ Pyramid

```cpp
struct HiZPyramid
{
    static constexpr int kMaxMips = 12;  // Up to 4096x4096

    void Build(const float* depthBuffer, uint32_t w, uint32_t h);
    float Sample(int mip, uint32_t x, uint32_t y) const;
};
```

The pyramid stores maximum depth values at progressively coarser resolutions. A bounding box is occluded if its nearest depth is greater than the HiZ value at its screen extent.

### Occlusion Testing

```cpp
bool IsVisible(float screenMinX, float screenMinY,
               float screenMaxX, float screenMaxY,
               float nearDepth) const;
```

Projects screen-space bounds and tests against the appropriate HiZ mip level.

### Batch Culling

```cpp
std::vector<uint32_t> CullBatch(
    const std::vector<OcclusionAABB>& objects,
    const float* viewProjMatrix) const;
```

Tests multiple world-space AABBs at once, returning indices of visible objects.

---

## Usage Example

```cpp
auto& renderer = GPUDrivenRenderer::GetInstance();
renderer.Initialize(device, context, 16384);

// Each frame:
renderer.BeginFrame(depthSRV, screenWidth, screenHeight);

// Prepare instance AABBs
std::vector<GPUInstanceAABB> aabbs = BuildAABBs(sceneObjects);

renderer.CullAndDraw(
    aabbs.data(), static_cast<uint32_t>(aabbs.size()),
    viewMatrix, projMatrix,
    indexBuffer, vertexBuffer, vertexStride, totalIndexCount);

// Check stats
const auto& stats = renderer.GetStatistics();
LOG_INFO("Visible: {}/{} (frustum culled: {}, HiZ culled: {})",
         stats.visibleInstances, stats.totalInstances,
         stats.culledByFrustum, stats.culledByHiZ);
```

---

## Integration

- **GraphicsEngine**: Can be used alongside or instead of CPU frustum culling
- **MeshClusterSystem**: Provides meshlet-level AABBs for fine-grained culling. See [Mesh Shaders](Mesh-Shaders.md)
- **GPU Scene Buffer**: Shared GPU buffer for instance transforms and material IDs
- **Platform**: Requires D3D11 compute shaders. CPU reference implementation available via `GPUOcclusionCuller`

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Overall rendering architecture
- [Mesh Shaders](Mesh-Shaders.md) — Meshlet-based rendering with per-meshlet culling
- [GPU Particles](GPU-Particles.md) — Related GPU compute pipeline
- [Clustered Lighting](Clustered-Lighting.md) — 3D frustum grid for light culling
