# Water Rendering

SparkEngine provides a water rendering system with Gerstner wave simulation for realistic ocean and lake surfaces. Water planes are tessellated grids whose vertices are displaced each frame by a sum of Gerstner wave components, with CPU-side height queries for gameplay interactions like buoyancy and splash effects.

**Source:** `SparkEngine/Source/Graphics/WaterRenderer.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestWaterRenderer.cpp` (6 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Water Settings](#water-settings)
- [Gerstner Waves](#gerstner-waves)
- [Water Plane Management](#water-plane-management)
- [Height Queries](#height-queries)
- [GPU Buffers](#gpu-buffers)
- [API Reference](#api-reference)
- [Usage Example](#usage-example)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

The water renderer creates tessellated grid meshes for each water body and animates them using Gerstner wave summation. Gerstner waves produce the characteristic peaked crests and flat troughs of real ocean waves, unlike simple sine waves.

```
┌──────────────────────────────────────────────────────────┐
│                    WaterRenderer                          │
│                                                          │
│  WaterSettings ──► GenerateDefaultWaves()                │
│                         │                                │
│                         ▼                                │
│  ┌─────────────────────────────────────────┐             │
│  │ Gerstner Wave Components (N waves)      │             │
│  │  direction, amplitude, wavelength,      │             │
│  │  speed, steepness                       │             │
│  └──────────────┬──────────────────────────┘             │
│                 │                                         │
│                 ▼                                         │
│  Update(dt) ──► ComputeGerstnerDisplacement(x, z, t)    │
│                 ComputeGerstnerNormal(x, z, t)           │
│                         │                                │
│                         ▼                                │
│  ┌─────────────────────────────────────────┐             │
│  │ WaterPlane (tessellated grid)           │             │
│  │  vertices[] updated each frame          │             │
│  │  basePositions[] for undisplaced grid   │             │
│  └─────────────────────────────────────────┘             │
│                                                          │
│  GetWaterHeight(x, z) ──► CPU height query for gameplay  │
└──────────────────────────────────────────────────────────┘
```

---

## Water Settings

```cpp
struct WaterSettings
{
    XMFLOAT3 waterColor = {0.0f, 0.3f, 0.5f};
    float opacity = 0.8f;
    float waveAmplitude = 0.3f;
    float waveFrequency = 1.0f;
    float waveSpeed = 1.0f;
    int waveCount = 4;
    float fresnelPower = 5.0f;
    float reflectionStrength = 0.5f;
    float refractionStrength = 0.3f;
    float specularPower = 64.0f;
    float rippleSpeed = 1.0f;
    bool enabled = true;
};
```

| Setting | Default | Description |
|---------|---------|-------------|
| `waterColor` | (0, 0.3, 0.5) | Base surface color (linear RGB) |
| `opacity` | 0.8 | 0 = fully transparent, 1 = fully opaque |
| `waveAmplitude` | 0.3 | Global amplitude scale for all waves |
| `waveFrequency` | 1.0 | Global frequency multiplier |
| `waveSpeed` | 1.0 | Global speed multiplier |
| `waveCount` | 4 | Number of Gerstner wave components |
| `fresnelPower` | 5.0 | Fresnel reflection exponent |
| `reflectionStrength` | 0.5 | Planar reflection blend factor |
| `refractionStrength` | 0.3 | Refraction distortion strength |
| `specularPower` | 64.0 | Specular highlight sharpness |
| `rippleSpeed` | 1.0 | Detail ripple animation speed |

---

## Gerstner Waves

Each wave component is an independent travelling wave:

```cpp
struct GerstnerWave
{
    XMFLOAT2 direction = {1.0f, 0.0f};  // Travel direction (XZ)
    float amplitude = 0.2f;              // Vertical displacement
    float wavelength = 10.0f;            // Crest-to-crest distance
    float speed = 1.0f;                  // Phase velocity
    float steepness = 0.5f;              // 0 = sine, 1 = sharp peak
};
```

The Gerstner wave equation displaces vertices both vertically and horizontally, creating the distinctive peaked-crest shape:

```
x' = x + Σ (Q_i * A_i * D_i.x * cos(w_i * dot(D_i, [x,z]) + φ_i * t))
z' = z + Σ (Q_i * A_i * D_i.y * cos(w_i * dot(D_i, [x,z]) + φ_i * t))
y' = Σ (A_i * sin(w_i * dot(D_i, [x,z]) + φ_i * t))
```

Where `Q` is steepness, `A` is amplitude, `D` is direction, `w = 2π/wavelength`, and `φ = speed * w`.

Default waves are auto-generated from `WaterSettings` with varied directions and wavelengths.

---

## Water Plane Management

Water bodies are represented as individual planes, each with its own tessellated grid:

```cpp
auto& water = WaterRenderer::GetInstance();
water.Initialize();

// Add a 100x100 meter lake centered at (0, 0, 0)
uint32_t lakeId = water.AddWaterPlane({0.0f, 0.0f, 0.0f}, {100.0f, 100.0f});

// Add a 50x50 river section
uint32_t riverId = water.AddWaterPlane({200.0f, -1.0f, 0.0f}, {50.0f, 50.0f});

// Remove a water plane
water.RemoveWaterPlane(riverId);
```

Each plane generates a grid with `gridResolution` vertices per side (default 32), for 32x32 = 1024 vertices per plane.

---

## Height Queries

Query the animated water surface height at any world position for gameplay logic:

```cpp
float height = water.GetWaterHeight(playerX, playerZ);

// Buoyancy check
if (playerY < height)
{
    ApplyBuoyancyForce(playerY - height);
}
```

Returns 0 if no water plane covers the queried position.

Access the full vertex data for custom rendering:

```cpp
std::span<const WaterVertex> verts = water.GetMeshVertices(lakeId);
for (const auto& v : verts)
{
    // v.position, v.normal, v.texCoord
}
```

---

## GPU Buffers

The renderer can create GPU vertex and index buffers for hardware rendering:

```cpp
water.CreateGPUBuffers(rhiDevice);

// After Update() each frame:
water.UpdateGPUBuffers(rhiDevice);

uint32_t vertCount = water.GetGPUVertexCount();
uint32_t idxCount = water.GetGPUIndexCount();
```

---

## API Reference

| Method | Description |
|--------|-------------|
| `GetInstance()` | Singleton access |
| `Initialize()` | Create renderer, generate default waves |
| `Shutdown()` | Release all planes and resources |
| `SetSettings(settings)` | Update water configuration |
| `GetSettings()` | Read current settings |
| `AddWaterPlane(center, size)` | Add a water body, returns ID |
| `RemoveWaterPlane(id)` | Remove a water body |
| `Update(deltaTime)` | Advance simulation, displace vertices |
| `GetWaterHeight(worldX, worldZ)` | CPU height query |
| `GetMeshVertices(id)` | Access animated vertex data |
| `GetWaterPlaneCount()` | Number of active water planes |
| `CreateGPUBuffers(device)` | Create GPU vertex/index buffers |
| `UpdateGPUBuffers(device)` | Upload displaced vertices to GPU |

---

## Usage Example

```cpp
using namespace Spark::Graphics;

auto& water = WaterRenderer::GetInstance();
water.Initialize();

WaterSettings settings;
settings.waveAmplitude = 0.5f;
settings.waveCount = 6;
settings.waveSpeed = 0.8f;
settings.waterColor = {0.02f, 0.15f, 0.3f};
water.SetSettings(settings);

uint32_t oceanId = water.AddWaterPlane({0.0f, 0.0f, 0.0f}, {500.0f, 500.0f});

// In render loop:
water.Update(deltaTime);

// Gameplay: check if boat is floating
float waterY = water.GetWaterHeight(boat.x, boat.z);
boat.y = waterY + boat.draft;
```

---

## Integration

- **Physics**: Use `GetWaterHeight()` for buoyancy forces in [Physics](Physics) simulations
- **Audio**: Trigger splash sounds when objects enter water. See [Audio](Audio)
- **Particle System**: Spawn spray particles at wave crests
- **Sky Atmosphere**: Water color is affected by sky reflection. See [Rendering and Graphics](Rendering-and-Graphics)

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — Overall rendering pipeline
- [Physics](Physics) — Buoyancy and fluid interaction
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Shoreline and riverbed terrain
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) — Weather affects wave intensity
