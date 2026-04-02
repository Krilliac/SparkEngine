# Mesh Shaders

SparkEngine provides a mesh shader rendering pipeline for meshlet-based geometry using D3D12 Shader Model 6.5. Amplification shaders handle LOD selection and coarse culling, while mesh shaders perform per-meshlet processing with fine-grained backface and frustum culling. The system falls back to the traditional vertex pipeline on D3D11 or unsupported hardware.

**Source:** `SparkEngine/Source/Graphics/MeshShaderPipeline.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestMeshShaderPipeline.cpp` (9 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Meshlet Data Structures](#meshlet-data-structures)
  - [GPUMeshlet](#gpumeshlet)
  - [GPUMeshletGroup](#gpumeshletgroup)
  - [MeshletMesh](#meshletmesh)
- [Pipeline Stages](#pipeline-stages)
- [Building Meshlets](#building-meshlets)
- [Rendering](#rendering)
- [Performance Statistics](#performance-statistics)
- [API Reference](#api-reference)
- [Usage Example](#usage-example)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

Traditional vertex shaders process one vertex at a time with no awareness of the surrounding geometry. Mesh shaders process entire meshlets (small clusters of triangles) at once, enabling GPU-driven culling at the meshlet level without CPU intervention.

```
┌─────────────────────────────────────────────────────────┐
│                Traditional Pipeline                      │
│  CPU: for each object → draw call                       │
│  GPU: VS → Rasterizer → PS                             │
│                                                         │
│                Mesh Shader Pipeline                      │
│  CPU: DispatchMesh(groupCount)                          │
│  GPU: AS → MS → Rasterizer → PS                        │
│                                                         │
│  ┌──────────────────┐   ┌──────────────────┐            │
│  │ Amplification     │   │ Mesh Shader       │           │
│  │ Shader (AS)       │──│ (MS)              │           │
│  │                   │   │                   │           │
│  │ • LOD selection   │   │ • Load meshlet    │           │
│  │ • Coarse frustum  │   │ • Backface cull   │           │
│  │   cull per group  │   │ • Frustum cull    │           │
│  │ • Dispatch visible│   │ • Emit triangles  │           │
│  │   meshlets        │   │                   │           │
│  └──────────────────┘   └──────────────────┘            │
│         ↑                        ↑                       │
│    MeshletAS.hlsl          MeshletMS.hlsl                │
└─────────────────────────────────────────────────────────┘
```

---

## Meshlet Data Structures

### GPUMeshlet

A single meshlet — a small cluster of vertices and triangles:

```cpp
struct GPUMeshlet
{
    uint32_t vertexOffset;      // Start index in unique vertex array
    uint32_t triangleOffset;    // Start index in primitive index array
    uint32_t vertexCount;       // Vertices in this meshlet (max 64)
    uint32_t triangleCount;     // Triangles in this meshlet (max 124)

    float boundCenter[3];       // Bounding sphere center
    float boundRadius;          // Bounding sphere radius

    float coneAxis[3];          // Normal cone axis
    float coneCutoff;           // cos(half_angle + 90°), negative = valid
};
```

The **normal cone** enables GPU-side backface culling: if the entire meshlet faces away from the camera, it can be skipped without testing individual triangles. `coneCutoff` encodes the cone's half-angle; negative values indicate a valid cone.

### GPUMeshletGroup

Groups of meshlets for LOD selection by the amplification shader:

```cpp
struct GPUMeshletGroup
{
    uint32_t meshletOffset;     // First meshlet index in this group
    uint32_t meshletCount;      // Meshlets in this group
    float lodError;             // Screen-space error for this LOD
    float parentLodError;       // Parent group's error (for smooth transitions)

    float boundCenter[3];       // Group bounding sphere center
    float boundRadius;          // Group bounding sphere radius
};
```

### MeshletMesh

A complete mesh converted to meshlet format:

```cpp
struct MeshletMesh
{
    std::string name;
    std::vector<GPUMeshlet> meshlets;
    std::vector<GPUMeshletGroup> groups;
    std::vector<uint32_t> uniqueVertexIndices;  // Meshlet-local → mesh-global
    std::vector<uint32_t> primitiveIndices;     // Packed triangle indices
    uint32_t totalVertices;
    uint32_t totalTriangles;
};
```

---

## Pipeline Stages

### 1. Amplification Shader (AS)

One thread group per meshlet group. Each group:
- Evaluates LOD by comparing `lodError` against the group's screen-space size
- Performs coarse frustum culling using the group's bounding sphere
- Dispatches visible meshlets to the mesh shader stage

### 2. Mesh Shader (MS)

One thread group per meshlet. Each meshlet:
- Loads its vertices from the unique vertex index array
- Performs **backface culling** using the normal cone (if `coneCutoff < 0`)
- Performs **fine frustum culling** using the bounding sphere
- Emits visible triangles to the rasterizer

### 3. Fallback Path

On D3D11 or when mesh shaders are unavailable, the system falls back to traditional `DrawIndexed` calls using the original vertex/index buffers.

---

## Building Meshlets

Convert a traditional mesh to meshlet format:

```cpp
auto& pipeline = MeshShaderPipeline::GetInstance();

MeshletMesh meshletMesh = pipeline.BuildMeshlets(
    vertexPositions,       // float3 per vertex
    vertexCount,
    indexBuffer,
    indexCount,
    64,                    // max vertices per meshlet (default)
    124                    // max triangles per meshlet (default)
);
```

The build process:
1. Groups triangles into meshlets of at most 64 vertices and 124 triangles
2. Computes bounding spheres for frustum culling
3. Computes normal cones for backface culling
4. Builds unique vertex index and packed primitive index arrays
5. Groups meshlets for LOD selection

---

## Rendering

```cpp
pipeline.RenderMeshletMesh(
    meshletMesh,
    worldViewProjection,   // float[16]
    worldMatrix,           // float[16]
    cameraPosition         // float[3]
);
```

Internally dispatches `DispatchMesh()` with one group per meshlet group.

---

## Performance Statistics

```cpp
struct MeshShaderStats
{
    uint32_t totalMeshlets;
    uint32_t visibleMeshlets;
    uint32_t culledByBackface;
    uint32_t culledByFrustum;
    uint32_t totalTriangles;
    uint32_t dispatchCount;
    bool meshShadersAvailable;
};

const auto& stats = pipeline.GetStats();
```

---

## API Reference

| Method | Description |
|--------|-------------|
| `GetInstance()` | Singleton access |
| `CheckMeshShaderSupport()` | Static check for hardware support |
| `Initialize(device)` | Init pipeline (returns false if fallback) |
| `Shutdown()` | Release resources |
| `BuildMeshlets(verts, vCount, indices, iCount, ...)` | Convert mesh to meshlet format |
| `RenderMeshletMesh(mesh, wvp, world, camPos)` | Render via mesh shaders |
| `IsUsingMeshShaders()` | True if mesh shaders active (not fallback) |
| `GetStats()` | Performance statistics |
| `Console_GetStatus()` | Formatted debug string |

---

## Usage Example

```cpp
using namespace Spark::Graphics;

auto& pipeline = MeshShaderPipeline::GetInstance();

if (pipeline.Initialize(d3d12Device))
{
    LOG_INFO("Mesh shaders enabled");
}
else
{
    LOG_INFO("Falling back to vertex pipeline");
}

// Convert static meshes at load time
MeshletMesh buildingMesh = pipeline.BuildMeshlets(
    building.positions.data(), building.vertexCount,
    building.indices.data(), building.indexCount);

LOG_INFO("Building: {} meshlets, {} triangles",
         buildingMesh.meshlets.size(), buildingMesh.totalTriangles);

// Each frame:
pipeline.RenderMeshletMesh(buildingMesh, wvp, world, camPos);

const auto& stats = pipeline.GetStats();
LOG_INFO("Visible: {}/{} meshlets (backface: {}, frustum: {})",
         stats.visibleMeshlets, stats.totalMeshlets,
         stats.culledByBackface, stats.culledByFrustum);
```

---

## Integration

- **MeshClusterSystem**: Provides meshlet-level data for GPU-driven rendering. See [GPU-Driven Rendering](GPU-Driven-Rendering)
- **D3D12 Backend**: Mesh shaders require D3D12 SM6.5 support. See [D3D12 Backend](D3D12-Backend)
- **Asset Pipeline**: Meshlet conversion can be done at asset import time. See [Asset Pipeline](Asset-Pipeline)
- **LOD System**: Meshlet groups integrate with the existing mesh LOD system. See [Rendering and Graphics](Rendering-and-Graphics)

---

## See Also

- [GPU-Driven Rendering](GPU-Driven-Rendering) — Compute-based culling for traditional geometry
- [D3D12 Backend](D3D12-Backend) — D3D12 feature tiers and mesh shader capability
- [Rendering and Graphics](Rendering-and-Graphics) — Overall rendering architecture
- [RHI Abstraction Layer](RHI-Abstraction-Layer) — Cross-backend abstraction
