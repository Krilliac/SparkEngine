# Decal System

SparkEngine's decal system provides deferred decal projection for FPS visual feedback -- bullet holes, scorch marks, blood splatters, and other environmental detail projected onto scene surfaces. Decals are surface-type aware, automatically selecting the right material based on whether a bullet hits concrete, metal, wood, or flesh.

**Source:** `SparkEngine/Source/Graphics/DecalSystem.h`

---

## Overview

The `DecalSystem` is a singleton that manages a fixed-size pool of projected decals. Each decal is an oriented bounding box (OBB) projected onto the surface at a hit point, combining albedo, normal, and roughness textures to blend seamlessly with the underlying geometry. The system integrates with the weapon/raycast pipeline: when a projectile hits a surface, the system spawns the appropriate decal based on the `DecalType` and `SurfaceType` at the impact point.

```
┌─────────────────────────────────────────────────────────┐
│                     Weapon System                        │
│           (raycast hit -> position, normal, surface)     │
├─────────────────────────────────────────────────────────┤
│                      DecalSystem                         │
│  ┌──────────────┬────────────────┬────────────────────┐ │
│  │ Material     │ Surface-Decal  │ Decal Pool         │ │
│  │ Registry     │ Mapping Table  │ (spawn, fade, cull)│ │
│  │              │                │                    │ │
│  │ RegisterMaterial()  RegisterSurfaceMapping()       │ │
│  │ GetMaterial()       SpawnDecal()  Update()         │ │
│  └──────────────┴────────────────┴────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│  DecalMaterial       Decal           SurfaceDecalMapping │
│  (textures,          (OBB, fade,     (surface -> material│
│   opacity, flags)     lifetime)       + size range)      │
├─────────────────────────────────────────────────────────┤
│              Deferred Rendering Pass                      │
│         (project OBBs into G-buffer)                     │
└─────────────────────────────────────────────────────────┘
```

### Namespace

All decal types reside in `namespace Spark::Graphics`.

---

## Decal Types and Surface Types

### DecalType

Identifies the category of decal to spawn. The system uses this together with `SurfaceType` to look up the correct material and size range.

| Value | Description |
|-------|-------------|
| `BulletHole` | Small impact mark from projectile hits |
| `ScorchMark` | Burn or explosion residue |
| `BloodSplatter` | Organic impact effect |
| `Footprint` | Character or creature footprints |
| `TireTrack` | Vehicle tire marks |
| `Crack` | Structural damage cracks |
| `Custom` | User-defined decal with explicit material |

### SurfaceType

Identifies the physical material of the surface that was hit. Different surfaces produce different decal sizes and materials (e.g., bullet holes are smaller on metal than on wood).

| Value | Description |
|-------|-------------|
| `Default` | Fallback surface type |
| `Concrete` | Stone, cement, brick |
| `Metal` | Steel, iron, aluminum |
| `Wood` | Timber, planks, furniture |
| `Glass` | Windows, bottles |
| `Dirt` | Soil, gravel, sand |
| `Flesh` | Organic targets |
| `Water` | Liquid surfaces |
| `Foliage` | Leaves, grass, vegetation |

### Default Surface Mappings

The system registers these mappings during `Initialize()`:

| Surface | Decal Type | Material | Size Range |
|---------|-----------|----------|------------|
| Concrete | BulletHole | `decal_bullet_hole` | 0.05 -- 0.15 |
| Metal | BulletHole | `decal_bullet_hole` | 0.03 -- 0.08 |
| Wood | BulletHole | `decal_bullet_hole` | 0.04 -- 0.12 |
| Default | ScorchMark | `decal_scorch` | 0.3 -- 0.8 |
| Default | BloodSplatter | `decal_blood` | 0.2 -- 0.5 |

Custom mappings can be registered at runtime via `RegisterSurfaceMapping()`.

---

## DecalMaterial Configuration

A `DecalMaterial` defines the visual appearance of a decal through PBR textures and blending controls:

```cpp
struct DecalMaterial
{
    std::string name;              // Unique identifier (e.g. "decal_bullet_hole")
    std::string albedoTexture;     // Color/albedo texture path
    std::string normalTexture;     // Normal map path (optional)
    std::string roughnessTexture;  // Roughness map path (optional)
    float opacity = 1.0f;          // Base opacity (0.0 - 1.0)
    bool affectsNormals = true;    // Whether to write to G-buffer normals
    bool affectsRoughness = true;  // Whether to write to G-buffer roughness
};
```

| Field | Purpose |
|-------|---------|
| `name` | Lookup key used by surface mappings and `SpawnDecalWithMaterial()` |
| `albedoTexture` | Diffuse color texture (e.g. `Textures/Decals/bullet_hole.dds`) |
| `normalTexture` | Perturbs surface normals for depth effect (e.g. indent around bullet hole) |
| `roughnessTexture` | Overrides surface roughness (e.g. scorch marks are rougher) |
| `opacity` | Base opacity before fade-out is applied |
| `affectsNormals` | Set to `false` for flat decals like scorch marks that should not alter normals |
| `affectsRoughness` | Set to `false` for decals that should preserve the underlying surface roughness |

### Default Materials

Three materials are registered during `Initialize()`:

| Name | Albedo | Normal | Affects Normals |
|------|--------|--------|----------------|
| `decal_bullet_hole` | `Textures/Decals/bullet_hole.dds` | `Textures/Decals/bullet_hole_normal.dds` | Yes |
| `decal_scorch` | `Textures/Decals/scorch_mark.dds` | -- | No |
| `decal_blood` | `Textures/Decals/blood_splatter.dds` | -- | No |

---

## Pooling and Lifecycle

The decal system uses a fixed-capacity object pool to avoid dynamic allocation during gameplay.

### Pool Behavior

1. **Spawn request** -- `SpawnDecal()` or `SpawnDecalWithMaterial()` is called.
2. **Find inactive slot** -- The pool is scanned for the first `active == false` entry.
3. **Grow if room** -- If no inactive slot exists and `m_decals.size() < m_maxDecals`, a new entry is appended.
4. **Recycle oldest** -- If the pool is full, the oldest active decal is immediately deactivated and its slot is reused.

### Fade-Out Timeline

Each decal follows a two-phase lifetime:

```
|<---- fadeTimer ---->|<-- fadeDuration -->|
       Full opacity         Fading            Deactivated
     (default 15s)       (default 3s)
```

- **`fadeTimer`** (default 15 seconds) -- The decal remains at full opacity.
- **`fadeDuration`** (default 3 seconds) -- Opacity linearly interpolates from full to zero.
- Once `age >= fadeTimer + fadeDuration`, the decal is marked `active = false` and its slot becomes available for reuse.

The current opacity at any point is computed by `Decal::GetCurrentOpacity()`:

```cpp
if (age < fadeTimer) return opacity;
float fadeProgress = min(1.0f, (age - fadeTimer) / fadeDuration);
return opacity * (1.0f - fadeProgress);
```

### Configuration

| Method | Default | Description |
|--------|---------|-------------|
| `SetMaxDecals(uint32_t)` | 512 | Maximum number of decals in the pool |
| `SetDefaultFadeTime(float)` | 15.0s | Seconds before fade-out begins |
| `SetDefaultFadeDuration(float)` | 3.0s | Seconds for the fade-out transition |
| `ClearAllDecals()` | -- | Immediately deactivates all decals |

---

## Code Example

### Spawning a decal from a weapon hit

```cpp
#include "Graphics/DecalSystem.h"

using namespace Spark::Graphics;

// After a raycast hit:
void OnWeaponHit(const XMFLOAT3& hitPos, const XMFLOAT3& hitNormal, SurfaceType surface)
{
    auto& decals = DecalSystem::GetInstance();

    // Automatic material selection based on surface type
    Decal* decal = decals.SpawnDecal(hitPos, hitNormal, DecalType::BulletHole, surface);

    // Or spawn with an explicit material and size
    Decal* custom = decals.SpawnDecalWithMaterial(hitPos, hitNormal, "decal_scorch", 0.5f);
}
```

### Registering a custom material and surface mapping

```cpp
// Register a new decal material
DecalMaterial laserBurn;
laserBurn.name = "decal_laser_burn";
laserBurn.albedoTexture = "Textures/Decals/laser_burn.dds";
laserBurn.normalTexture = "Textures/Decals/laser_burn_normal.dds";
laserBurn.affectsRoughness = true;
DecalSystem::GetInstance().RegisterMaterial(laserBurn);

// Map it to a surface + decal type combination
SurfaceDecalMapping mapping;
mapping.surface = SurfaceType::Metal;
mapping.decalType = DecalType::ScorchMark;
mapping.materialName = "decal_laser_burn";
mapping.sizeRange = {0.1f, 0.25f};
mapping.rotationVariance = MathUtils::PI;
DecalSystem::GetInstance().RegisterSurfaceMapping(mapping);
```

### Rendering loop integration

```cpp
// In engine initialization
DecalSystem::GetInstance().Initialize(512);

// In the main update loop
DecalSystem::GetInstance().Update(deltaTime);

// In the render pass — retrieve active decals for deferred projection
const auto& activeDecals = DecalSystem::GetInstance().GetActiveDecals();
for (const auto& decal : activeDecals)
{
    if (!decal.active) continue;
    XMMATRIX world = decal.GetWorldMatrix();
    float opacity = decal.GetCurrentOpacity();
    // Submit OBB to deferred decal pass...
}

// At shutdown
DecalSystem::GetInstance().Shutdown();
```

---

## Console Commands

The `DecalSystem` exposes three console integration methods:

| Method | Description |
|--------|-------------|
| `Console_GetStatus()` | Returns a status string with active/total decal count, material count, surface mapping count, and fade configuration |
| `Console_SetMaxDecals(uint32_t)` | Changes the maximum pool size at runtime |
| `Console_SpawnTestDecal(float x, float y, float z)` | Spawns a test bullet hole decal at the given world position (concrete surface, 0.15 size, upward normal) |

### Example console output

```
=== Decal System ===
Active: 12/128 (Max: 512)
Materials: 3
Surface Mappings: 5
Fade Time: 15s + 3s fade
```

---

## Editor

The **DecalEditorPanel** (`SparkEditor/Source/Panels/DecalEditorPanel.h`) provides an ImGui-based interface for authoring decal materials and surface mappings at edit time. It exposes:

- **Material list** -- Browse and select registered decal materials.
- **Material details** -- Edit albedo/normal/roughness texture paths, opacity, fade time, and the `affectsNormals`/`affectsRoughness` flags.
- **Surface mappings** -- Configure which surface types map to which decal materials.
- **Pool status** -- Displays active decal count and max pool size.

---

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Graphics/DecalSystem.h` | Enums, structs, and `DecalSystem` class declaration |
| `SparkEngine/Source/Graphics/DecalSystem.cpp` | Implementation (Windows + Linux stub) |
| `SparkEditor/Source/Panels/DecalEditorPanel.h` | Editor panel declaration |
| `SparkEditor/Source/Panels/DecalEditorPanel.cpp` | Editor panel implementation |
| `Tests/TestDecalSystem.cpp` | Unit tests (7 tests) |

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Parent rendering system and G-buffer pipeline
- [Physics](../subsystems/Physics.md) -- Surface type detection from physics materials
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Weapon system that triggers decal spawning
