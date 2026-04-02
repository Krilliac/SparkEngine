# Clustered Lighting

SparkEngine uses GPU-based clustered light culling to efficiently determine which lights affect each region of the screen. The camera frustum is subdivided into a 3D grid of clusters, and each light is assigned to the clusters it overlaps, enabling O(1) light lookups per pixel during shading.

**Source:** `SparkEngine/Source/Graphics/ClusteredLightCulling.h`, `SparkEngine/Source/Graphics/ClusteredLightGPU.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestClusteredLightGPU.cpp` (7 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Cluster Grid](#cluster-grid)
- [Light Assignment](#light-assignment)
- [API Reference](#api-reference)
- [GPU Buffers](#gpu-buffers)
- [Usage Example](#usage-example)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

With many lights in a scene, testing every light for every pixel is prohibitively expensive. Clustered lighting divides the frustum into a 3D grid and pre-computes which lights affect each cell, so the pixel shader only evaluates a small subset of lights.

```
┌──────────────────────────────────────────────────────────┐
│                     Camera Frustum                       │
│                                                          │
│  Near ──────────────────────────────────────────── Far   │
│  ┌────┬────┬────┬────┬────┬────┬────┬────┐  ← Y slices │
│  │ C0 │ C1 │ C2 │ C3 │ C4 │ C5 │ C6 │ C7 │             │
│  ├────┼────┼────┼────┼────┼────┼────┼────┤              │
│  │ C8 │ C9 │C10 │C11 │C12 │C13 │C14 │C15 │  ← gridY   │
│  └────┴────┴────┴────┴────┴────┴────┴────┘              │
│  ↑                                         ↑             │
│  gridX                              gridZ (depth)        │
│                                                          │
│  Each cluster stores:                                    │
│    - light count                                         │
│    - list of light indices                               │
│                                                          │
│  Pixel shader: cluster = f(screenXY, depth)              │
│                lights = clusterLightList[cluster]        │
└──────────────────────────────────────────────────────────┘
```

---

## Cluster Grid

### Configuration

```cpp
struct ClusterConfig
{
    uint32_t gridX = 16;                  // Clusters along screen X
    uint32_t gridY = 8;                   // Clusters along screen Y
    uint32_t gridZ = 24;                  // Clusters along depth
    uint32_t maxLightsPerCluster = 200;   // Light index list capacity
    uint32_t maxTotalLights = 1000;       // Maximum active lights
};
```

Default grid: 16 x 8 x 24 = **3,072 clusters**.

### Cluster AABBs

Each cluster is represented as an axis-aligned bounding box in view space:

```cpp
struct ClusterAABB
{
    XMFLOAT3 minBounds;
    XMFLOAT3 maxBounds;
};
```

Cluster AABBs are rebuilt when the projection matrix changes (e.g., FOV or aspect ratio change).

---

## Light Assignment

### Light Data

```cpp
struct LightData
{
    XMFLOAT3 position;
    float radius;
    XMFLOAT3 color;
    float intensity;
    uint32_t type;  // 0 = point, 1 = spot
};
```

### Assignment Algorithm

`Update()` performs two steps:

1. **Rebuild grid** (`RebuildClusterGrid`): Compute cluster AABBs from the projection matrix, near/far planes, and grid dimensions. Depth slices use logarithmic distribution for higher near-camera resolution.

2. **Assign lights** (`AssignLightsToClusters`): For each light, transform its position to view space and test its bounding sphere against each cluster's AABB. If they intersect, the light's index is added to that cluster's light list.

The sphere-AABB intersection test is conservative — it may include a few extra lights per cluster, but never misses one.

---

## API Reference

| Method | Description |
|--------|-------------|
| `GetInstance()` | Singleton access |
| `Initialize(config)` | Create grid with given dimensions |
| `Shutdown()` | Release all resources |
| `AddLight(light)` | Add a light for the next Update |
| `ClearLights()` | Remove all lights |
| `Update(viewMatrix, projMatrix, nearZ, farZ)` | Rebuild grid and assign lights |
| `GetClusterCount()` | Total clusters (gridX * gridY * gridZ) |
| `GetActiveLightCount()` | Lights in the active list |
| `GetConfig()` | Current grid configuration |
| `GetClusterLightCount(index)` | Light count for a specific cluster |
| `GetClusterLightIndices(index)` | Light index array for a cluster |
| `Console_GetStatus()` | Formatted debug string |

---

## GPU Buffers

Upload cluster data for use in pixel shaders:

```cpp
culling.CreateGPUBuffers(rhiDevice);

// After Update() each frame:
culling.UploadToGPU(rhiDevice);

// Bind in shader
auto* clusterBuf = culling.GetClusterBuffer();       // Per-cluster light counts
auto* lightIdxBuf = culling.GetLightIndexBuffer();    // Flat light index array
```

The pixel shader determines its cluster from screen position and depth, reads the light count, and iterates only over assigned lights.

---

## Usage Example

```cpp
using namespace Spark::Graphics;

auto& culling = ClusteredLightCulling::GetInstance();

ClusterConfig config;
config.gridX = 16;
config.gridY = 8;
config.gridZ = 32;
config.maxLightsPerCluster = 128;
culling.Initialize(config);

// Each frame:
culling.ClearLights();

// Add scene lights
for (const auto& light : sceneLights)
{
    LightData ld;
    ld.position = light.GetPosition();
    ld.radius = light.GetRadius();
    ld.color = light.GetColor();
    ld.intensity = light.GetIntensity();
    ld.type = light.IsSpot() ? 1 : 0;
    culling.AddLight(ld);
}

// Build clusters
XMFLOAT4X4 view, proj;
XMStoreFloat4x4(&view, camera.GetViewMatrix());
XMStoreFloat4x4(&proj, camera.GetProjectionMatrix());
culling.Update(view, proj, camera.GetNearZ(), camera.GetFarZ());

// Upload to GPU
culling.UploadToGPU(rhiDevice);

LOG_INFO("Clustered lighting: {} lights in {} clusters",
         culling.GetActiveLightCount(), culling.GetClusterCount());
```

---

## Integration

- **LightingSystem**: Feeds lights into clustered culling each frame
- **LightManager**: Provides the scene's active light list
- **Deferred Lighting Pass**: The lighting pass uses cluster data to evaluate only relevant lights per pixel
- **Volumetric Fog**: The fog scattering pass can reuse the clustered light list. See [Volumetric Fog](Volumetric-Fog)
- **Forward Rendering**: Clustered lighting also works with forward rendering by reading cluster data in the forward pixel shader

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — Overall lighting pipeline
- [Volumetric Fog](Volumetric-Fog) — Light scattering uses the same light data
- [Global Illumination](Global-Illumination) — Indirect lighting complements direct clustered lighting
- [GPU-Driven Rendering](GPU-Driven-Rendering) — GPU culling for geometry (parallel concept)
