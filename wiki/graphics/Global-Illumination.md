# Global Illumination

SparkEngine provides two complementary probe-based global illumination systems: **DDGI** (Dynamic Diffuse Global Illumination) for regular-grid irradiance probes, and **Adaptive Probe Volumes** (APV) for hierarchical brick-based probes with geometry-aware subdivision. Both use L2 spherical harmonics for compact irradiance storage.

**Source:** `SparkEngine/Source/Graphics/DDGIProbeSystem.h`, `SparkEngine/Source/Graphics/AdaptiveProbeVolumes.h`
**Namespace:** `Spark::Graphics`

---

## Table of Contents

- [Overview](#overview)
- [DDGI Probe System](#ddgi-probe-system)
  - [Grid Configuration](#grid-configuration)
  - [Probe Data (Spherical Harmonics)](#probe-data-spherical-harmonics)
  - [Probe Update Pipeline](#probe-update-pipeline)
  - [Probe Relocation](#probe-relocation)
  - [Irradiance Queries](#irradiance-queries)
  - [DDGI API](#ddgi-api)
- [Adaptive Probe Volumes](#adaptive-probe-volumes)
  - [Brick Hierarchy](#brick-hierarchy)
  - [LOD Levels](#lod-levels)
  - [Adaptive Subdivision](#adaptive-subdivision)
  - [Virtual Offsets](#virtual-offsets)
  - [Camera-Based Streaming](#camera-based-streaming)
  - [APV API](#apv-api)
- [DDGI vs APV](#ddgi-vs-apv)
- [GPU Resources](#gpu-resources)
- [Usage Examples](#usage-examples)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

Both systems compute indirect diffuse lighting by placing probe grids in the scene, capturing irradiance from ray-cast samples, and interpolating the results at shading time.

```
┌─────────────────────────────────────────────────────────────┐
│                     Scene Geometry                           │
│                                                             │
│   ● ─── ● ─── ● ─── ●       DDGI: Regular 3D grid         │
│   |     |     |     |       of probes at fixed spacing      │
│   ● ─── ● ─── ● ─── ●                                      │
│   |     |     |     |                                       │
│   ● ─── ● ─── ● ─── ●                                      │
│                                                             │
│   ┌────┐                     APV: Brick-based hierarchy     │
│   │●●●●│ ┌──┐               Fine bricks near geometry,     │
│   │●●●●│ │●●│               coarse bricks in open areas    │
│   │●●●●│ │●●│                                              │
│   │●●●●│ └──┘                                              │
│   └────┘                                                    │
│                                                             │
│   Each probe stores L2 SH (9 coefficients x 3 RGB channels)│
│   = 27 floats per probe                                     │
└─────────────────────────────────────────────────────────────┘
```

### Constants

| Constant | DDGI | APV |
|----------|------|-----|
| SH coefficients per channel | 9 (L2) | 9 (L2) |
| Floats per probe | 27 | 27 |
| Probes per brick | N/A | 64 (4x4x4) |

---

## DDGI Probe System

A regular 3D grid of irradiance probes with real-time updates via ray casting.

### Grid Configuration

```cpp
struct DDGISettings
{
    float spacingX = 2.0f, spacingY = 2.0f, spacingZ = 2.0f;
    uint32_t countX = 8, countY = 4, countZ = 8;
    float originX = 0.0f, originY = 0.0f, originZ = 0.0f;
    int raysPerProbe = 128;
    float hysteresis = 0.97f;
    float maxRayDistance = 50.0f;
    float normalBias = 0.25f;
    float relocationThreshold = 0.5f;
    float relocationMaxOffset = 1.0f;
};
```

| Setting | Default | Description |
|---------|---------|-------------|
| `spacing` | 2m | Distance between probes on each axis |
| `count` | 8x4x8 = 256 | Number of probes per axis |
| `raysPerProbe` | 128 | Rays cast per probe per update cycle |
| `hysteresis` | 0.97 | Temporal blend factor (higher = smoother, slower) |
| `maxRayDistance` | 50m | Ray trace distance limit |
| `normalBias` | 0.25m | Bias along surface normal to prevent self-shadowing |

### Probe Data (Spherical Harmonics)

Each probe stores irradiance as L2 spherical harmonics — 9 basis functions per color channel:

```cpp
struct DDGIProbe
{
    std::array<float, 9> r{};  // 9 SH coefficients for red
    std::array<float, 9> g{};  // 9 SH coefficients for green
    std::array<float, 9> b{};  // 9 SH coefficients for blue
    float offsetX, offsetY, offsetZ;  // Relocation offset
    bool relocated;
};
```

SH basis functions are evaluated using `EvaluateSHBasis()` with a normalized direction vector, producing 9 basis values that are dot-multiplied with the stored coefficients to reconstruct irradiance.

### Probe Update Pipeline

```cpp
// For each probe in the grid:
DDGIRayResult rays[128];
// ... cast rays using cosine-weighted hemisphere sampling ...

system.UpdateProbe(ix, iy, iz, rays, 128);
system.UpdateProbeBorders();
```

`UpdateProbe()` performs:
1. **SH projection**: Each ray's radiance is projected into SH basis functions using spherical Fibonacci sampling for quasi-uniform direction distribution
2. **Hysteresis blending**: New SH coefficients are blended with existing values: `new = lerp(current, incoming, 1 - hysteresis)`
3. **Border copy**: `UpdateProbeBorders()` copies edge texels for seamless hardware bilinear filtering in the GPU texture atlas

### Probe Relocation

Probes inside or very close to geometry produce biased irradiance. Relocation nudges them along the surface normal:

```cpp
system.RelocateProbe(ix, iy, iz,
    closestHitDistance, hitNormalX, hitNormalY, hitNormalZ);
```

Relocation is limited to `relocationMaxOffset` (default 1m) to keep probes near their grid position.

### Irradiance Queries

```cpp
DDGIIrradiance result = system.QueryIrradiance(
    worldX, worldY, worldZ,
    normalX, normalY, normalZ);
```

Performs trilinear interpolation of the 8 nearest probes. Each probe's SH is evaluated at the given surface normal before interpolation.

### DDGI API

| Method | Description |
|--------|-------------|
| `Initialize(settings)` | Create probe grid |
| `Shutdown()` | Release probe data |
| `GetProbePosition(ix, iy, iz)` | World position of a grid probe |
| `UpdateProbe(ix, iy, iz, rays, count)` | Update one probe with ray results |
| `UpdateProbeBorders()` | Copy border texels for filtering |
| `RelocateProbe(ix, iy, iz, ...)` | Push probe away from geometry |
| `QueryIrradiance(pos, normal)` | Trilinear SH interpolation |
| `GetProbeCount()` | Total probes (countX * countY * countZ) |
| `CreateGPUResources(device)` | Create structured buffer for SH data |
| `UploadProbeDataToGPU(device)` | Upload SH to GPU |

---

## Adaptive Probe Volumes

A brick-based hierarchical system that adapts probe density to scene geometry, inspired by Unity's Adaptive Probe Volumes.

### Brick Hierarchy

Probes are organized in **bricks** of 4x4x4 = 64 probes each. Bricks are sparse — only regions of interest are populated.

```cpp
struct APVBrick
{
    BrickID id;
    std::array<APVProbeData, 64> probes;  // 4x4x4
    float originX, originY, originZ;       // World-space corner
    float spacing;                         // Probe spacing within brick
    bool loaded;
    bool subdivided;
};
```

### LOD Levels

| LOD | Spacing | Use Case |
|-----|---------|----------|
| `Fine` (0) | 1m | Near geometry surfaces |
| `Medium` (1) | 2m | Transitional areas |
| `Coarse` (2) | 4m | Open areas, far from camera |

### Adaptive Subdivision

Coarse bricks near geometry can be subdivided into 8 finer bricks:

```cpp
system.SubdivideBrick(gridX, gridY, gridZ, BrickLOD::Coarse);
// Creates 8 Medium bricks; their probes are interpolated from the parent
```

The parent brick is marked as `subdivided` and its data is preserved for fallback. Child bricks inherit initial SH values via interpolation.

### Virtual Offsets

Like DDGI relocation, but per-probe within each brick:

```cpp
system.ApplyVirtualOffsets(brickId, geometryDistances, geometryNormals);
```

Probes too close to (or inside) geometry are pushed away along the surface normal by up to `virtualOffsetDistance` (default 0.25m).

### Camera-Based Streaming

```cpp
system.UpdateStreaming(cameraPosX, cameraPosY, cameraPosZ);
```

Loads bricks within `streamingDistance` of the camera and unloads distant ones, keeping memory usage bounded.

### APV API

| Method | Description |
|--------|-------------|
| `Initialize(settings)` | Set up with `APVSettings` |
| `Shutdown()` | Release all brick data |
| `CreateBrick(x, y, z, lod)` | Create a brick at grid position |
| `SubdivideBrick(x, y, z, parentLOD)` | Split into 8 finer bricks |
| `ApplyVirtualOffsets(id, distances, normals)` | Push probes from surfaces |
| `UpdateStreaming(camPos)` | Load/unload bricks by distance |
| `SetProbeData(id, px, py, pz, shR, shG, shB)` | Store baked SH data |
| `SampleIrradiance(pos, normal)` | Query irradiance at world position |
| `GetLoadedBrickCount()` | Active bricks in memory |
| `GetTotalProbeCount()` | brickCount * 64 |
| `CreateGPUResources(device)` | Create structured buffer for bricks |
| `UploadBrickDataToGPU(device)` | Upload active bricks to GPU |

---

## DDGI vs APV

| Feature | DDGI | Adaptive Probe Volumes |
|---------|------|----------------------|
| Grid type | Regular 3D | Sparse brick hierarchy |
| Probe density | Uniform everywhere | Adaptive (fine near geometry) |
| Memory | Fixed (countX * countY * countZ) | Dynamic (loaded bricks only) |
| Streaming | No | Yes (camera distance) |
| Subdivision | No | Yes (3 LOD levels) |
| Real-time update | Yes (per-probe ray cast) | Typically baked, can update |
| Best for | Small-medium dynamic scenes | Large worlds with baked lighting |
| Virtual offsets | Probe relocation | Per-probe virtual offsets |

**Use DDGI** for fully dynamic lighting in contained areas (e.g., interiors, arenas).
**Use APV** for large open worlds where baked GI with streaming is needed.

---

## GPU Resources

Both systems create GPU structured buffers for shader access:

```cpp
// DDGI
ddgi.CreateGPUResources(device);
ddgi.UploadProbeDataToGPU(device);
auto* probeBuffer = ddgi.GetProbeBuffer();  // SH data structured buffer

// APV
apv.CreateGPUResources(device);
apv.UploadBrickDataToGPU(device);
auto* brickBuffer = apv.GetBrickBuffer();   // Brick SH data
```

---

## Usage Examples

### DDGI Setup

```cpp
DDGIProbeSystem ddgi;

DDGISettings settings;
settings.countX = 12; settings.countY = 6; settings.countZ = 12;
settings.spacingX = 3.0f; settings.spacingY = 2.0f; settings.spacingZ = 3.0f;
settings.raysPerProbe = 64;
ddgi.Initialize(settings);

// Update loop: update a subset of probes each frame
for (int i = 0; i < probesPerFrame; ++i)
{
    auto [ix, iy, iz] = GetNextProbeToUpdate();
    DDGIRayResult rays[64];
    CastProbeRays(ddgi, ix, iy, iz, rays, 64);
    ddgi.UpdateProbe(ix, iy, iz, rays, 64);
}
ddgi.UpdateProbeBorders();

// Query in shader setup
DDGIIrradiance irr = ddgi.QueryIrradiance(
    fragWorldPos.x, fragWorldPos.y, fragWorldPos.z,
    fragNormal.x, fragNormal.y, fragNormal.z);
```

### APV Setup

```cpp
AdaptiveProbeVolumes apv;

APVSettings settings;
settings.streamingDistance = 80.0f;
settings.subdivisionGeometryThreshold = 1.5f;
apv.Initialize(settings);

// Create coarse bricks covering the scene
for (int z = -5; z <= 5; ++z)
    for (int y = 0; y <= 2; ++y)
        for (int x = -5; x <= 5; ++x)
            apv.CreateBrick(x, y, z, BrickLOD::Coarse);

// Subdivide near a building
apv.SubdivideBrick(0, 0, 0, BrickLOD::Coarse);

// Each frame
apv.UpdateStreaming(cameraPos.x, cameraPos.y, cameraPos.z);

// Query
APVIrradiance irr = apv.SampleIrradiance(
    worldX, worldY, worldZ, normalX, normalY, normalZ);
```

---

## Integration

- **LightingSystem**: GI results are combined with direct lighting in the deferred lighting pass
- **Render Graph**: DDGI probe updates can be scheduled as compute passes
- **Large World Support**: APV streaming pairs well with [Large World Support](../subsystems/Large-World-Support.md) for seamless area transitions
- **Day-Night Cycle**: DDGI probes update dynamically with changing light conditions

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Overall lighting architecture
- [Volumetric Fog](Volumetric-Fog.md) — Volumetric scattering that complements GI
- [Clustered Lighting](Clustered-Lighting.md) — Direct light culling system
- [Large World Support](../subsystems/Large-World-Support.md) — World streaming for APV integration
