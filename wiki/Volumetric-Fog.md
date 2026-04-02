# Volumetric Fog

SparkEngine implements froxel-based (frustum-voxel) volumetric fog that subdivides the camera frustum into a 3D grid and computes light scattering per cell. The system supports local fog volumes, point light scattering, temporal filtering, and Beer-Lambert transmittance integration.

**Source:** `SparkEngine/Source/Graphics/FroxelVolumetricFog.h`
**Namespace:** `Spark::Graphics`

---

## Table of Contents

- [Overview](#overview)
- [Froxel Grid](#froxel-grid)
- [Three-Pass Pipeline](#three-pass-pipeline)
  - [Pass 1: Media Injection](#pass-1-media-injection)
  - [Pass 2: Light Scattering](#pass-2-light-scattering)
  - [Pass 3: Temporal Filtering](#pass-3-temporal-filtering)
  - [Integration](#integration-pass)
- [Configuration](#configuration)
- [Local Fog Volumes](#local-fog-volumes)
- [Querying Fog](#querying-fog)
- [GPU Resources](#gpu-resources)
- [API Reference](#api-reference)
- [Usage Example](#usage-example)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

Traditional distance-based fog applies a single density function along the view ray. Volumetric fog instead evaluates scattering and absorption at every point in a 3D grid aligned to the camera frustum, producing physically-based fog with light shafts, local density variations, and proper light interaction.

```
┌───────────────────────────────────────────────────────────┐
│                     Camera Frustum                        │
│                                                           │
│  Near ─────────────────────────────────────────── Far     │
│  ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐     │
│  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │     │
│  ├──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┼──┤     │
│  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │     │
│  └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘     │
│  <-- dense slices -->          <-- sparse slices -->      │
│  (logarithmic depth distribution)                         │
│                                                           │
│  Each cell (froxel) stores:                               │
│    - inscattered light (RGB)                              │
│    - extinction coefficient                               │
└───────────────────────────────────────────────────────────┘
```

---

## Froxel Grid

The frustum is divided into a 3D grid where depth slices use a blend between linear and logarithmic distribution, giving higher resolution near the camera where fog detail matters most.

| Default Setting | Value | Description |
|-----------------|-------|-------------|
| `gridWidth` | 160 | Froxels along X (~1080p / 12) |
| `gridHeight` | 90 | Froxels along Y (~1080p / 12) |
| `gridDepth` | 64 | Depth slices |
| `nearPlane` | 0.1 | Camera near plane |
| `farPlane` | 200.0 | Maximum fog distance |
| `logDistribution` | 0.5 | Blend: 0 = linear, 1 = logarithmic |

Total froxels at default: 160 x 90 x 64 = **921,600 cells**.

Depth slice conversion:
```cpp
float SliceToDepth(uint32_t slice) const;  // slice index → linear depth
float DepthToSlice(float depth) const;      // linear depth → fractional slice
```

---

## Three-Pass Pipeline

### Pass 1: Media Injection

```cpp
void InjectMedia(const FogVolume* volumes, int volumeCount);
```

Writes scattering and extinction coefficients into each froxel. Global density is applied everywhere; local `FogVolume` boxes override density in their region. Each froxel accumulates:
- **Extinction**: How much light is absorbed per unit distance
- **Inscattering**: Ambient light scattered into the viewing direction

### Pass 2: Light Scattering

```cpp
void ScatterLight(const FroxelLight* lights, int lightCount,
                  float cameraPosX, float cameraPosY, float cameraPosZ);
```

Evaluates point light contributions per froxel using the **Henyey-Greenstein phase function**:

```
P(cos θ) = (1 - g²) / (4π · (1 + g² - 2g·cos θ)^(3/2))
```

Where `g` (phaseG setting) controls scattering directionality:
- `g = 0`: Isotropic scattering (equal in all directions)
- `g > 0`: Forward scattering (light shafts toward camera)
- `g < 0`: Back scattering

### Pass 3: Temporal Filtering

```cpp
void TemporalFilter(float motionScale = 0.0f);
```

Blends the current frame's froxel grid with the previous frame using exponential history:

```
result = lerp(current, history, temporalBlend)
```

Motion rejection (`temporalRejectionThreshold`) discards history samples when the camera moves significantly, preventing ghosting artifacts.

### Integration Pass

```cpp
void Integrate();
```

Marches along the depth axis accumulating inscattering and transmittance via **Beer-Lambert law**:

```
T(d) = exp(-∫ extinction · ds)
fogColor = ∫ inscattering · T(s) · ds
finalColor = sceneColor · T(totalDepth) + fogColor
```

---

## Configuration

```cpp
struct FroxelFogSettings
{
    // Grid dimensions
    uint32_t gridWidth = 160;
    uint32_t gridHeight = 90;
    uint32_t gridDepth = 64;
    float nearPlane = 0.1f;
    float farPlane = 200.0f;
    float logDistribution = 0.5f;

    // Density
    float globalDensity = 0.02f;
    float globalScattering = 0.015f;
    float globalAbsorption = 0.005f;

    // Phase function
    float phaseG = 0.3f;   // Henyey-Greenstein asymmetry [-1, 1]

    // Ambient inscattering
    float ambientR = 0.05f;
    float ambientG = 0.06f;
    float ambientB = 0.08f;

    // Temporal filtering
    float temporalBlend = 0.9f;
    float temporalRejectionThreshold = 0.1f;
};
```

---

## Local Fog Volumes

Override fog density in specific regions of the scene:

```cpp
struct FogVolume
{
    float posX, posY, posZ;          // World-space center
    float extentX, extentY, extentZ; // Half-extents (box shape)
    float density;                    // Override density
    float scattering;                 // Override scattering
    float absorption;                 // Override absorption
};
```

Use local volumes for fog under bridges, mist in valleys, or smoke in rooms.

---

## Querying Fog

After integration, query the fog at any screen position:

```cpp
FroxelFogResult QueryFog(float u, float v, float depth) const;
```

Returns:

```cpp
struct FroxelFogResult
{
    float fogR, fogG, fogB;   // Inscattered light to blend in
    float transmittance;       // Scene color multiplier [0, 1]
};
```

Apply in the final compositing pass:
```hlsl
float3 finalColor = sceneColor * result.transmittance + float3(result.fogR, result.fogG, result.fogB);
```

---

## GPU Resources

The system creates GPU resources for use in compute shader pipelines:

```cpp
bool CreateGPUResources(Spark::RHI::IRHIDevice* device);
void UploadGridToGPU(Spark::RHI::IRHIDevice* device);
Spark::RHI::IRHITexture* GetFogVolumeTexture() const;
```

The GPU resource is a 3D texture (`gridWidth x gridHeight x gridDepth`, RGBA16F) containing the integrated froxel grid data.

---

## API Reference

| Method | Description |
|--------|-------------|
| `Initialize(settings)` | Create froxel grid buffers |
| `Shutdown()` | Release all buffers |
| `InjectMedia(volumes, count)` | Pass 1: Write density per froxel |
| `ScatterLight(lights, count, camPos)` | Pass 2: Evaluate light contributions |
| `TemporalFilter(motionScale)` | Pass 3: Blend with history |
| `Integrate()` | March depth axis, accumulate fog |
| `QueryFog(u, v, depth)` | Sample fog at screen position |
| `GetFroxelCount()` | Total grid cells |
| `CreateGPUResources(device)` | Create 3D texture for GPU pipeline |
| `UploadGridToGPU(device)` | Upload integrated grid to GPU |

---

## Usage Example

```cpp
using namespace Spark::Graphics;

FroxelVolumetricFog fog;

FroxelFogSettings settings;
settings.globalDensity = 0.03f;
settings.phaseG = 0.4f;       // moderate forward scattering
settings.farPlane = 150.0f;
fog.Initialize(settings);

// Each frame:
FogVolume swampFog;
swampFog.posX = 50.0f; swampFog.posY = 0.0f; swampFog.posZ = 30.0f;
swampFog.extentX = 20.0f; swampFog.extentY = 3.0f; swampFog.extentZ = 20.0f;
swampFog.density = 0.1f;
fog.InjectMedia(&swampFog, 1);

fog.ScatterLight(lights.data(), lights.size(), camX, camY, camZ);
fog.TemporalFilter(cameraMotionMagnitude);
fog.Integrate();

// In compositing shader:
auto result = fog.QueryFog(screenU, screenV, pixelDepth);
```

---

## Integration

- **FogSystem**: The simpler distance-based `FogSystem` provides basic height/distance fog; `FroxelVolumetricFog` is the physically-based alternative
- **LightingSystem**: Point lights from `LightingSystem` feed into the scattering pass
- **ClusteredLightCulling**: Can share the light list with clustered lighting for efficiency
- **Day-Night Cycle**: Ambient inscattering colors can be driven by time-of-day

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — Overall rendering pipeline
- [Clustered Lighting](Clustered-Lighting) — Light culling that shares data with fog scattering
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) — Weather system integration
- [Global Illumination](Global-Illumination) — Probe-based lighting that complements fog
