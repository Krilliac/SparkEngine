# Shadow System

SparkEngine's shadow rendering pipeline combines a priority-based shadow atlas, temporal shadow caching, and Percentage-Closer Soft Shadows (PCSS) for realistic, distance-dependent soft shadows. The system supports directional, point, and spot light shadows with variable-resolution tiles allocated from a single atlas texture.

**Source:** `SparkEngine/Source/Graphics/ShadowAtlas.h`, `CachedShadowAtlas.h`, `PCSSshadows.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestShadowAtlas.cpp` (7 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Shadow Atlas](#shadow-atlas)
  - [Tile Allocation](#tile-allocation)
  - [GPU Resources](#gpu-resources)
  - [Atlas Metrics](#atlas-metrics)
- [Cached Shadow Atlas](#cached-shadow-atlas)
  - [Change Detection](#change-detection)
  - [Dynamic vs Cached Regions](#dynamic-vs-cached-regions)
- [PCSS Algorithm](#pcss-algorithm)
  - [Three-Step Process](#three-step-process)
  - [Sample Patterns](#sample-patterns)
  - [HLSL Integration](#hlsl-integration)
- [Settings](#settings)
  - [PCSS Settings](#pcss-settings)
- [Performance](#performance)
- [Code Example](#code-example)
- [Source Files](#source-files)
- [See Also](#see-also)

---

## Overview

```
┌────────────────────────────────────────────────────────────────┐
│                       Shadow Pipeline                          │
│                                                                │
│  LightingSystem                                                │
│       │                                                        │
│       ▼                                                        │
│  CachedShadowAtlas                                             │
│  ┌──────────────────┐  ┌──────────────────┐                    │
│  │  Dynamic Atlas    │  │  Cached Atlas     │                   │
│  │  (ShadowAtlas)    │  │  (ShadowAtlas)    │                   │
│  │  Re-render/frame  │  │  Render once,     │                   │
│  │                   │  │  reuse until stale │                  │
│  └────────┬──────────┘  └────────┬──────────┘                  │
│           └──────────┬───────────┘                              │
│                      ▼                                         │
│            PCSSShadowEvaluator                                 │
│            Blocker search → Penumbra → PCF                     │
│                      │                                         │
│                      ▼                                         │
│              Final shadow factor [0..1]                        │
└────────────────────────────────────────────────────────────────┘
```

The shadow system is composed of three cooperating classes:

| Class | Responsibility |
|-------|----------------|
| `ShadowAtlas` | Subdivides a square depth texture into variable-size tiles, assigns tiles to lights by priority |
| `CachedShadowAtlas` | Wraps two `ShadowAtlas` instances (dynamic + cached), skips re-rendering unchanged shadows |
| `PCSSShadowEvaluator` | Evaluates shadow factor using the PCSS algorithm with variable penumbra |

---

## Shadow Atlas

### Tile Allocation

`ShadowAtlas` manages a single large depth texture (default 4096x4096) divided into a grid of cells. Lights request tiles via `RequestTile(lightId, priority, desiredSize)`:

- **Priority-based sizing:** Higher-priority lights (closer, larger, more visible) receive larger tiles. The `GetDesiredTileSize()` heuristic maps priority > 0.8 to 1024px, > 0.4 to 512px, and below to 256px.
- **Variable tile sizes:** Tiles are always square and power-of-two, from `minTileSize` (default 256) up to the atlas size.
- **Grid occupancy:** An internal `std::vector<bool>` tracks which grid cells are occupied.
- **Eviction:** `EndFrame()` evicts tiles that have not been used for several frames, freeing space for new lights.
- **Persistence:** Tiles persist across frames for temporal stability -- a light keeps its tile as long as it remains active.

The `ShadowTile` struct stores:

| Field | Description |
|-------|-------------|
| `x`, `y` | Pixel offset within the atlas |
| `size` | Tile width/height (always square) |
| `lightId` | Owning light identifier |
| `priority` | Assignment priority (higher = more important) |
| `lastUsedFrame` | Frame index when last rendered |
| `active` | Whether the tile is in use this frame |

### GPU Resources

`CreateGPUResources(device)` allocates the atlas as an R32_FLOAT depth texture through the [RHI](RHI-Abstraction-Layer.md). `BindForShadowPass(cmdList, lightId)` sets the viewport to the tile region for a given light's shadow rendering pass, and `GetAtlasTexture()` exposes the atlas for sampling.

> **What actually ships (2026-09):** `ShadowAtlas::BindForShadowPass` and the GPU
> atlas texture (`CreateGPUResources`) have **no production caller**. The real
> shadow maps are per-light D32 depth textures in `LightingSystem::m_shadowMaps`,
> rasterized by a depth-only pass (basic VS, no PS; `MeshDrawCommand::castShadows`
> selects casters) on the Deferred path (via `LightingPass`) and the
> RenderGraphBased path (via `ShadowPass`) for ECS-submitted meshes. The default
> Forward pipeline has no shadow term, and neither pixel shader implements
> PCF/VSM/PCSS filtering yet. `LightingSystem::Update` calls
> `CachedShadowAtlas::RequestShadow` for every shadow-casting light and
> `RenderShadowMaps` calls `MarkRendered`, so the cache bookkeeping below is live
> while the atlas-backed *sampling* path described here is not.

### Atlas Metrics

`GetMetrics()` returns a `ShadowAtlasMetrics` snapshot:

| Field | Description |
|-------|-------------|
| `atlasSize` | Total atlas resolution in pixels |
| `totalTiles` | Number of allocated tiles |
| `activeTiles` | Tiles active this frame |
| `wastedPixels` | Unoccupied pixel area |
| `utilization` | Fraction of atlas area in use (0.0 -- 1.0) |

`Console_GetStatus()` returns a human-readable status string for the debug console.

---

## Cached Shadow Atlas

`CachedShadowAtlas` wraps two separate `ShadowAtlas` instances to avoid re-rendering shadows for static or unchanged lights.

### Dynamic vs Cached Regions

| Region | Atlas Size (default) | Re-rendered | Use Case |
|--------|---------------------|-------------|----------|
| Dynamic | 2048x2048 | Every frame | Moving lights, animated shadow casters |
| Cached | 4096x4096 | Only when stale | Static lights, fixed geometry |

### Change Detection

Each light tracked via a `ShadowCacheEntry` stores:

| Field | Description |
|-------|-------------|
| `lightStateHash` | FNV-1a hash of light position, direction, range, and spot angle |
| `casterSceneHash` | Hash of shadow casters in the light's frustum |
| `isStatic` | Whether the light is flagged as static |
| `needsUpdate` | Whether the shadow must be re-rendered this frame |

The per-frame workflow:

1. **`BeginFrame()`** -- Reset per-frame state, clear the render list.
2. **`RequestShadow(request)`** -- For each light, compute the state hash and compare against the cache entry. If unchanged and static, reuse the cached tile (cache hit). Otherwise, allocate a tile and add the light to the render list.
3. **Render** -- Iterate `GetShadowsToRender()` and render only the shadows that actually need updating.
4. **`MarkRendered(lightId)`** -- After rendering, mark the cache entry as up-to-date.
5. **`EndFrame()`** -- Evict stale tiles from both atlases.

`InvalidateShadow(lightId)` forces a specific light's shadow to re-render next frame. `InvalidateAll()` invalidates every cached shadow (e.g., after a scene reload).

`GetCachedRendersAvoided()` reports how many shadow renders were skipped this frame due to cache hits.

---

## PCSS Algorithm

Percentage-Closer Soft Shadows produce realistic soft shadows with distance-dependent penumbra -- shadows are sharp near contact points and softer further from the occluding geometry.

### Three-Step Process

```
Step 1: Blocker Search          Step 2: Penumbra Estimation      Step 3: PCF Filter
┌─────────────────────┐        ┌─────────────────────────┐      ┌──────────────────┐
│ Sample shadow map   │        │ penumbra = lightSize *  │      │ Sample shadow    │
│ around the point    │ ────► │ (receiver - blocker)    │ ──► │ map with variable│
│ to find average     │        │        / blocker        │      │ kernel size      │
│ blocker depth       │        │                         │      │ = penumbra       │
└─────────────────────┘        └─────────────────────────┘      └──────────────────┘
```

1. **Blocker search:** Sample the shadow map in a disk around the shading point using `blockerSearchSamples` Poisson samples. Collect depths that are closer than the receiver (i.e., occluders) and compute their average depth.

2. **Penumbra estimation:** Using the similar triangles formula:
   ```
   penumbra = lightSize * (receiverDepth - avgBlockerDepth) / avgBlockerDepth
   ```
   The result is clamped to `[minPenumbraSize, maxPenumbraSize]`.

3. **PCF filter:** Perform Percentage-Closer Filtering with `pcfSamples` Poisson samples at a radius determined by the estimated penumbra. The final shadow factor is the fraction of samples that pass the depth test.

### Sample Patterns

`PCSSSamplePattern` generates up to 64 sample points in two modes:

| Pattern | Description |
|---------|-------------|
| `GeneratePoisson(numSamples, seed)` | Golden-angle spiral with optional per-pixel rotation via seed |
| `GenerateGrid(gridSize)` | Regular NxN grid (fallback for debugging) |

When `useRotatedPoisson` is enabled, each pixel rotates the Poisson disk by a noise value derived from the seed, which reduces visible banding artifacts.

### HLSL Integration

`PCSSShadowEvaluator::GetHLSLCode()` returns a self-contained HLSL function `PCSShadow()` that implements the full three-step algorithm. The function signature:

```hlsl
float PCSShadow(Texture2D shadowMap, SamplerComparisonState shadowSampler,
                float2 uv, float depth, float lightSize, float2 texelSize);
```

This can be included directly in shadow sampling shaders. The HLSL version uses a 32-entry Poisson disk, 16 blocker search samples, and 32 PCF samples.

---

## Settings

### PCSS Settings

The `PCSSSettings` struct controls the shadow quality and behavior:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | `bool` | true | Enable/disable PCSS |
| `lightSize` | `float` | 1.0 | Light source size in world units. Larger values produce softer shadows. |
| `blockerSearchSamples` | `int` | 16 | Number of samples for the blocker depth search |
| `pcfSamples` | `int` | 32 | Number of samples for the final PCF filter |
| `maxPenumbraSize` | `float` | 32.0 | Maximum penumbra width in shadow map texels |
| `minPenumbraSize` | `float` | 1.0 | Minimum penumbra width (prevents fully hard edges) |
| `nearPlane` | `float` | 0.1 | Shadow frustum near plane |
| `shadowBias` | `float` | 0.002 | Depth bias to reduce self-shadowing (shadow acne) |
| `normalBias` | `float` | 0.5 | Normal-based offset to reduce peter-panning artifacts |
| `useRotatedPoisson` | `bool` | true | Rotate Poisson disk per pixel to reduce banding |

---

## Performance

Shadow rendering is often the most expensive part of a lighting pipeline. The SparkEngine shadow system provides several mechanisms to manage cost:

| Technique | Benefit |
|-----------|---------|
| **Priority-based atlas** | Low-priority lights get smaller (or no) shadow maps |
| **Temporal caching** | Static shadows render once and reuse across frames |
| **Variable penumbra** | PCSS uses fewer samples where shadows are sharp |
| **Eviction** | Tiles for off-screen or distant lights are automatically freed |
| **Tile persistence** | Avoids atlas fragmentation by keeping tiles stable across frames |

**Tuning guidelines:**

- Reduce `pcfSamples` for lower-end hardware (16 is a good minimum).
- Lower `maxPenumbraSize` to limit the maximum filter radius.
- Use the cached atlas for all static lights -- `GetCachedRendersAvoided()` should be high in a typical scene.
- Monitor `ShadowAtlasMetrics::utilization` -- if consistently above 0.9, increase the atlas size.

---

## Code Example

### Shadow atlas lifecycle

```cpp
#include "Graphics/CachedShadowAtlas.h"

using namespace Spark::Graphics;

// Initialize once after the RHI device comes up:
CachedShadowAtlas shadows;
shadows.Initialize(/*dynamic=*/2048, /*cached=*/4096, /*minTile=*/256);
shadows.CreateGPUResources(rhiDevice);
```

### Per-frame shadow pass

```cpp
shadows.BeginFrame();

for (const Light& light : activeLights)
{
    ShadowUpdateRequest req;
    req.lightId      = light.id;
    req.priority     = ComputeLightPriority(light, camera);
    req.isStatic     = light.isStatic;
    req.forceUpdate  = false;

    req.posX = light.position.x; req.posY = light.position.y; req.posZ = light.position.z;
    req.dirX = light.direction.x; req.dirY = light.direction.y; req.dirZ = light.direction.z;
    req.range     = light.range;
    req.spotAngle = light.spotAngle;

    shadows.RequestShadow(req);
}

// Only the list returned here actually needs a depth pass this frame —
// static lights whose state + caster hashes are unchanged are skipped.
// NOTE: the engine's LightingSystem renders into per-light depth textures here;
// BindForShadowPass / the atlas texture are not used by production code.
for (uint32_t lightId : shadows.GetShadowsToRender())
{
    shadows.BindForShadowPass(cmdList, lightId); // sets the tile viewport
    RenderShadowMap(cmdList, lightId);
    shadows.MarkRendered(lightId);
}

shadows.EndFrame();

// Every N frames log how many renders the cache saved:
Logger::Info("shadows: skipped {} cached renders this frame",
             shadows.GetCachedRendersAvoided());
```

### Invalidating a cached shadow

```cpp
// An object entered the light's frustum — force a one-shot re-render:
shadows.InvalidateShadow(streetLamp.shadowLightId);

// Scene reload — every cached entry is now stale:
shadows.InvalidateAll();
```

### PCSS configuration

```cpp
PCSSShadowEvaluator pcss;

PCSSSettings settings;
settings.enabled             = true;
settings.lightSize           = 2.0f;    // larger → softer edges far from contact
settings.blockerSearchSamples = 16;
settings.pcfSamples          = 32;
settings.minPenumbraSize     = 1.0f;
settings.maxPenumbraSize     = 32.0f;
settings.shadowBias          = 0.003f;
settings.normalBias          = 0.5f;
settings.useRotatedPoisson   = true;
pcss.SetSettings(settings);

// Inject the HLSL into your shadow sampling shader at shader-compile time:
const std::string hlsl = pcss.GetHLSLCode();
shaderSource += hlsl;   // adds the `PCSShadow()` function
```

Sampling in HLSL:

```hlsl
// Generated by PCSSShadowEvaluator::GetHLSLCode()
float PCSShadow(Texture2D shadowMap, SamplerComparisonState shadowSampler,
                float2 uv, float depth, float lightSize, float2 texelSize);

// In the lighting shader:
float s = PCSShadow(g_ShadowMap, g_ShadowCmpSampler,
                    shadowUV, linearDepth,
                    /*lightSize=*/2.0f, g_InvAtlasSize);
lighting *= s;
```

### Inspecting atlas utilisation

```cpp
const ShadowAtlasMetrics m = shadows.GetDynamicAtlas().GetMetrics();
Logger::Info("shadow atlas: {} tiles active, utilisation {:.1f}%",
             m.activeTiles, m.utilization * 100.0f);

// Human-readable dump (used by the `r.shadows.status` console command):
Logger::Info("{}", shadows.GetDynamicAtlas().Console_GetStatus());
```

---

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Graphics/ShadowAtlas.h` | `ShadowAtlas`, `ShadowTile`, `ShadowAtlasMetrics` |
| `SparkEngine/Source/Graphics/CachedShadowAtlas.h` | `CachedShadowAtlas`, `ShadowCacheEntry`, `ShadowUpdateRequest` |
| `SparkEngine/Source/Graphics/PCSSshadows.h` | `PCSSShadowEvaluator`, `PCSSSettings`, `PCSSSamplePattern` |
| `Tests/TestShadowAtlas.cpp` | Unit tests for `ShadowAtlas` (7 test cases) |

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Main rendering pipeline that integrates shadow passes
- [Clustered Lighting](Clustered-Lighting.md) -- Lighting system that samples the shadow atlas per light
- [RHI Abstraction Layer](RHI-Abstraction-Layer.md) -- Backend-agnostic texture and command list used by the atlas
- [GPU-Driven Rendering](GPU-Driven-Rendering.md) -- Indirect draw calls that interact with shadow culling
- [Performance Tips](../advanced/Performance-Tips.md) -- General performance guidance including shadow budget
