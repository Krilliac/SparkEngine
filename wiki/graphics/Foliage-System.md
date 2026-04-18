# Foliage System

SparkEngine's foliage system handles procedural and painted vegetation placement within scatter volumes. Each volume defines bounds, density, and a list of vegetation species with terrain constraints, scale randomization, and wind influence. Instance transforms are generated at placement time and rendered via GPU instancing for optimal performance.

**Source:** `SparkEngine/Source/Graphics/FoliageSystem.h`
**Namespace:** `Spark::Graphics`

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Species Configuration](#species-configuration)
- [Volume Placement](#volume-placement)
- [GPU Instancing](#gpu-instancing)
- [Code Example](#code-example)
- [Source Files](#source-files)
- [See Also](#see-also)

---

## Overview

The foliage system populates large outdoor environments with vegetation (grass, bushes, trees, flowers) without manually placing each instance. Artists define foliage volumes that specify where vegetation can appear, and the system procedurally scatters instances within those volumes based on species rules and terrain constraints.

```
┌────────────────────────────────────────────────────────────────┐
│                        Scene Setup                             │
│                                                                │
│  FoliageVolumeData               FoliageSpecies (per volume)   │
│  ┌─────────────────┐             ┌──────────────────────────┐  │
│  │ halfExtents      │             │ meshPath, materialPath    │  │
│  │ seed             │             │ density, scale range      │  │
│  │ globalDensityScale│            │ slope/altitude limits     │  │
│  │ enabled          │             │ windInfluence, cullDist   │  │
│  └────────┬─────────┘             └────────────┬─────────────┘  │
│           └───────────────┬────────────────────┘               │
│                           ▼                                    │
│                  ┌─────────────────┐                           │
│                  │ FoliageManager  │                           │
│                  │ (singleton)     │                           │
│                  │ AddVolume()     │                           │
│                  │ RemoveVolume()  │                           │
│                  └────────┬────────┘                           │
│                           │                                    │
│                           ▼                                    │
│                  Instance Transforms                           │
│                  → GPU Instance Buffer                         │
│                  → Draw Instanced                              │
└────────────────────────────────────────────────────────────────┘
```

Key features:

- **Procedural scattering** based on density, random seed, and terrain constraints
- **Terrain-aware placement** respecting slope angle and altitude limits
- **Surface alignment** to terrain normals with optional random Y-axis rotation
- **Distance culling** per species for LOD-friendly rendering
- **Wind sway** multiplier for integration with the wind system
- **Shadow casting** toggle per species
- **Deterministic** placement via configurable random seed

---

## Architecture

| Class/Struct | Responsibility |
|-------------|----------------|
| `FoliageManager` | Singleton that owns all foliage volumes and manages instance generation |
| `FoliageVolumeData` | Defines a region where vegetation is placed (bounds, seed, density scale) |
| `FoliageSpecies` | Describes a single vegetation type (mesh, material, density, constraints) |

`FoliageManager` is accessed via `FoliageManager::GetInstance()`. Call `Initialize()` at startup and `Shutdown()` at teardown.

---

## Species Configuration

Each `FoliageSpecies` defines a vegetation type with mesh, material, and placement rules:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | `std::string` | -- | Registry key — referenced by `FoliageVolumeDesc::speciesNames` |
| `meshPath` | `std::string` | -- | Mesh asset path for this species |
| `materialPath` | `std::string` | -- | Material asset path |
| `density` | `float` | 1.0 | Instances per square meter |
| `minScale` | `float` | 0.8 | Minimum random scale factor |
| `maxScale` | `float` | 1.2 | Maximum random scale factor |
| `minSlopeAngleDeg` | `float` | 0.0 | Minimum terrain slope for placement (degrees) |
| `maxSlopeAngleDeg` | `float` | 45.0 | Maximum terrain slope for placement (degrees) |
| `minAltitude` | `float` | -1000.0 | Minimum placement altitude |
| `maxAltitude` | `float` | 1000.0 | Maximum placement altitude |
| `alignToSurface` | `bool` | true | Align instance up-vector to terrain normal |
| `randomYaw` | `bool` | true | Apply random Y-axis rotation |
| `castShadows` | `bool` | true | Whether instances cast shadows |
| `windInfluence` | `float` | 1.0 | Wind sway multiplier [0, 2]. 0 = no wind, 2 = exaggerated sway. |
| `cullDistance` | `float` | 100.0 | Distance beyond which instances are culled |
| `billboardHeight` | `float` | 2.0 | Impostor billboard vertical size in metres (Phase F) |
| `billboardAspect` | `float` | 0.5 | Impostor billboard width/height ratio (Phase G) |

**Slope and altitude constraints** allow species to be restricted to specific terrain regions. For example, grass might be limited to slopes under 30 degrees, while mossy rocks could be placed only on steep slopes above 40 degrees.

**Scale randomization** between `minScale` and `maxScale` prevents uniform-looking vegetation. The random value is seeded deterministically from the volume seed and instance index.

---

## Volume Placement

A `FoliageVolumeData` defines the spatial bounds and global parameters for a scatter region:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `center` | `XMFLOAT3` | (0, 0, 0) | Volume center in world space |
| `halfExtents` | `XMFLOAT3` | (50, 50, 50) | XZ drives area, Y is the altitude clamp |
| `speciesNames` | `std::vector<std::string>` | `{}` | Species keys to scatter (must be pre-registered) |
| `seed` | `uint32_t` | 0 | Random seed for reproducible placement |
| `densityScale` | `float` | 1.0 | Multiplier applied to every species' density in this volume |
| `minSlopeAngleDeg` / `maxSlopeAngleDeg` | `float` | 0 / 90 | Volume-level slope override (merged with species) |
| `minAltitude` / `maxAltitude` | `float` | ±10000 | Volume-level altitude clamp (merged with species) |
| `alignToTerrain` | `bool` | true | Sample terrain height for Y; leave false for flat tests |
| `enabled` | `bool` | true | Inactive volumes generate no instances |

Volumes are registered with `FoliageManager::AddVolume(desc)`, which returns a non-zero volume ID on success (or `0` if no species were registered). The ID feeds `RemoveVolume`, `GetInstances`, and `GetVolumeDesc`.

The placement algorithm:

1. Iterate the volume bounds at intervals determined by species density
2. For each candidate point, sample the terrain height and normal
3. Reject points outside the species slope and altitude constraints
4. Compute a deterministic random scale and Y-rotation from the seed
5. If `alignToSurface` is true, orient the instance to match the terrain normal
6. Emit the instance transform (position, rotation, scale) to the instance buffer

The `globalDensityScale` multiplier allows runtime density adjustment (e.g., reducing foliage density on lower-end hardware).

---

## GPU Instancing

Foliage instances are rendered using GPU instancing to minimize draw calls. All instances of the same species sharing the same mesh and material are batched into a single instanced draw call.

The per-instance data typically includes:

- **World transform** (4x3 matrix: position, rotation, uniform scale)
- **Wind phase offset** (derived from position, so nearby instances sway together)

**Distance culling** is applied per-species using the `cullDistance` field. Instances beyond this distance are excluded from the instance buffer before upload to the GPU.

For large volumes, the system can subdivide the volume into chunks and cull entire chunks against the view frustum before processing individual instances.

---

## Code Example

### Registering species and scattering a volume

The manager is a singleton. Species are registered by name first; a
volume then references one or more of them via `speciesNames`. Every
`AddVolume` call immediately scatters instances against the active
`ClipmapTerrain` (or a flat plane when no terrain is attached).

```cpp
#include "Graphics/FoliageSystem.h"

using namespace Spark::Graphics;

auto& foliage = FoliageManager::GetInstance();
foliage.Initialize();

// --- Species registry ---------------------------------------------------
FoliageSpecies grass;
grass.name             = "grass";
grass.meshPath         = "Assets/Meshes/Grass_Clump.mesh";
grass.materialPath     = "Assets/Materials/Grass.mat";
grass.density          = 8.0f;    // instances / m²
grass.minScale         = 0.7f;
grass.maxScale         = 1.3f;
grass.minSlopeAngleDeg = 0.0f;
grass.maxSlopeAngleDeg = 30.0f;   // gentle slopes only
grass.windInfluence    = 1.5f;    // swayed heavily by wind
grass.cullDistance     = 50.0f;
grass.castShadows      = false;
foliage.RegisterSpecies(grass);

FoliageSpecies pine;
pine.name             = "pine";
pine.meshPath         = "Assets/Meshes/Pine_Tree.mesh";
pine.materialPath     = "Assets/Materials/Pine.mat";
pine.density          = 0.05f;    // 1 tree per 20 m²
pine.minScale         = 0.8f;
pine.maxScale         = 1.5f;
pine.maxSlopeAngleDeg = 25.0f;
pine.windInfluence    = 0.3f;
pine.cullDistance     = 200.0f;
pine.castShadows      = true;
foliage.RegisterSpecies(pine);

// --- Volume -------------------------------------------------------------
FoliageVolumeDesc desc;
desc.center       = {0.0f, 0.0f, 0.0f};
desc.halfExtents  = {100.0f, 50.0f, 100.0f}; // XZ drives area, Y clamps altitude
desc.seed         = 1337;                    // deterministic scatter
desc.densityScale = 1.0f;
desc.speciesNames = {"grass", "pine"};
desc.alignToTerrain = true;

const uint32_t volumeId = foliage.AddVolume(desc);
SPARK_ASSERT(volumeId != 0 && "no registered species or terrain missing");
```

### Per-frame update + instance readback

`Update(dt, cameraPos)` refreshes `distanceToCamera` on each instance
and toggles the `visible` flag against the per-species `cullDistance`.
`GetInstances(volumeId)` exposes the immutable instance list to any
consumer (renderer, editor gizmo, physics probe):

```cpp
// Once per frame, after the camera has moved:
foliage.Update(deltaTime, camera.GetPosition());

// Readback — e.g. feed the render consumer:
for (uint32_t id : foliage.GetVolumeIds())
{
    for (const FoliageInstance& inst : foliage.GetInstances(id))
    {
        if (!inst.visible)
            continue;
        renderer.EnqueueFoliage(inst.position, inst.normal,
                                inst.yawRadians, inst.scale,
                                inst.speciesIndex);
    }
}
```

### ECS-driven authoring

Placing volumes as components on ECS entities is the recommended
authoring path; `UpdateFromECS` processes every `FoliageVolumeComponent`
each frame, so saving the scene restores the same scatter:

```cpp
auto entity = world.CreateEntity();
auto& volume = world.AddComponent<FoliageVolumeComponent>(entity);
volume.halfExtents = {100.0f, 50.0f, 100.0f};
volume.seed        = 1337;
volume.speciesNames = {"grass", "pine"};
world.AddComponent<TransformComponent>(entity).position = {0, 0, 0};

foliage.UpdateFromECS(world); // typically called once per frame from Systems
```

### Teardown

```cpp
foliage.RemoveVolume(volumeId);   // also drops the instances it scattered
foliage.Shutdown();               // releases registry + all volumes
```

---

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Graphics/FoliageSystem.h` | `FoliageManager`, `FoliageSpecies`, `FoliageVolumeData` |

---

## See Also

- [Terrain and Procedural Generation](../gameplay-tools/Terrain-and-Procedural-Generation.md) -- Terrain system that provides height and normal data for foliage placement
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Instanced rendering pipeline used by the foliage system
- [GPU-Driven Rendering](GPU-Driven-Rendering.md) -- Indirect draw and GPU culling for large instance counts
- [Day Night Cycle and Weather](../gameplay-tools/Day-Night-Cycle-and-Weather.md) -- Wind system that drives foliage sway
- [Shadow System](Shadow-System.md) -- Shadow rendering for foliage instances with `castShadows` enabled
- [HLOD and World Partition](../gameplay-tools/HLOD-And-World-Partition.md) -- World partitioning that manages foliage LOD transitions
