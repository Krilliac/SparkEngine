# Sky and Atmosphere

SparkEngine provides an analytical sky rendering system based on the Preetham 1999 paper ("A Practical Analytic Model for Daylight"). The `SkyAtmosphereSystem` computes physically-plausible sky colors from any view direction using Perez distribution coefficients derived from atmospheric turbidity and sun position.

**Source:** `SparkEngine/Source/Graphics/SkyAtmosphere.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestSkyAtmosphere.cpp` (5 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Sky Settings](#sky-settings)
- [Preetham Sky Model](#preetham-sky-model)
- [Integration with Day/Night Cycle](#integration-with-daynight-cycle)
- [GPU Rendering](#gpu-rendering)
- [Code Example](#code-example)
- [Source Files](#source-files)
- [See Also](#see-also)

---

## Overview

The sky atmosphere system is a singleton (`SkyAtmosphereSystem::GetInstance()`) that evaluates sky luminance for any view direction given the current sun position and atmospheric turbidity. The system is primarily CPU-side -- computed colors can feed a skybox, be passed to shaders as uniform data, or be sampled for ambient lighting.

```
┌──────────────────────┐     sun direction      ┌──────────────────────┐
│   TimeOfDaySystem    │ ─────────────────────► │  SkyAtmosphereSystem │
│   (day/night cycle)  │     turbidity           │  Preetham/Perez      │
└──────────────────────┘                         │  model evaluation    │
                                                 └──────────┬───────────┘
                                                            │
                        ┌───────────────────────────────────┤
                        │                                   │
                ┌───────▼───────┐                   ┌───────▼───────┐
                │ Skybox Render │                   │ Ambient Light │
                │ (6 face       │                   │ Probe colors  │
                │  colors)      │                   │               │
                └───────────────┘                   └───────────────┘
```

Key capabilities:

- **Analytical sky color** from any view direction via `ComputeSkyColor()`
- **Sun disc color and intensity** via `ComputeSunColor()`
- **Zenith and horizon colors** for ambient lighting or fog blending
- **HDR exposure control** for tone mapping integration
- **GPU constant buffer** for shader-side sky evaluation
- **Cubemap face colors** rendered via `RenderGPU()` for skybox display

---

## Architecture

| Class/Struct | Responsibility |
|-------------|----------------|
| `SkyAtmosphereSystem` | Singleton manager. Computes Perez coefficients, evaluates sky color, manages GPU resources. |
| `SkySettings` | Configuration struct: turbidity, sun intensity, sun direction, exposure, ground albedo. |
| `SkyColor` | Linear RGB color with `Lerp()` and scalar multiply. |
| `PerezCoefficients` | Five coefficients (A-E) for one channel of the Perez distribution function. |

---

## Sky Settings

The `SkySettings` struct controls all atmospheric parameters:

| Field | Type | Default | Range | Description |
|-------|------|---------|-------|-------------|
| `turbidity` | `float` | 2.0 | 1 -- 10 | Atmospheric haze. 1 = perfectly clear, 10 = heavy haze/smog. |
| `sunIntensity` | `float` | 20.0 | -- | Brightness multiplier for direct sunlight. |
| `sunDirection` | `XMFLOAT3` | (0, -0.5, 0.5) | Normalized | Direction toward the sun. |
| `exposure` | `float` | 1.0 | -- | HDR exposure control for tone mapping. |
| `groundAlbedo` | `float` | 0.3 | 0 -- 1 | Ground reflectance for multi-scattering approximation. |
| `enabled` | `bool` | true | -- | Master enable/disable switch. |

Lower turbidity produces a deep blue sky with a sharp sun disc. Higher turbidity washes out the sky toward white/yellow and broadens the circumsolar glow.

---

## Preetham Sky Model

The implementation follows the Preetham 1999 analytical model using the Perez luminance distribution function. For each color channel (Y luminance, x chrominance, y chrominance in CIE Yxy color space):

1. **Perez coefficients (A-E)** are derived from the current turbidity value via `RecalculateCoefficients()`. These control:
   - **A** -- Darkening or brightening of the horizon
   - **B** -- Luminance gradient near the horizon
   - **C** -- Relative intensity of the circumsolar region
   - **D** -- Width of the circumsolar region
   - **E** -- Relative backscattered light

2. **Zenith values** are computed from sun elevation and turbidity via `ComputeZenithValues()`.

3. **Sky evaluation** at a view direction uses:
   - `theta` -- angle from zenith to the sample direction
   - `gamma` -- angle between the sample direction and the sun direction
   - The ratio `F(theta, gamma) / F(0, thetaSun)` normalizes the distribution relative to the zenith.

The static method `EvaluatePerez()` computes the distribution value for a given (theta, gamma) pair and coefficient set.

---

## Integration with Day/Night Cycle

The sky system integrates with the [Day/Night Cycle and Weather](Day-Night-Cycle-and-Weather) systems:

- **Sun direction** is typically driven by `TimeOfDaySystem`, which updates the sun angle each frame based on the in-game clock.
- **Turbidity** can be modulated by `WeatherSystem` -- clear weather uses low turbidity, overcast or polluted conditions use high turbidity.
- **Exposure** can be linked to auto-exposure or post-processing tone mapping.
- **`Update(deltaTime)`** optionally animates the sun position if linked to a time-of-day source.

The `FogSystem` can sample `GetHorizonColor()` to match fog color to the sky at the horizon, creating seamless blending between distant geometry and the sky.

---

## GPU Rendering

`RenderGPU(deltaTime)` creates a GPU constant buffer containing the Perez coefficients, zenith values, sun direction, and exposure. It then samples the Preetham model at six cardinal directions (+X, -X, +Y, -Y, +Z, -Z) to produce cubemap-ready colors stored internally.

```cpp
// Access the computed skybox face colors
const SkyColor* faces = sky.GetSkyboxColors();
// faces[0] = +X, faces[1] = -X, faces[2] = +Y, etc.
```

The constant buffer (`m_gpuConstantBuffer`) is created through the [RHI](RHI-Abstraction-Layer) abstraction, making the system backend-agnostic.

---

## Code Example

```cpp
// Get the singleton
auto& sky = Spark::Graphics::SkyAtmosphereSystem::GetInstance();
sky.Initialize();

// Configure for a clear afternoon
SkySettings settings;
settings.turbidity = 3.0f;
settings.sunIntensity = 22.0f;
settings.sunDirection = {0.0f, -0.7f, 0.7f}; // Sun at ~45 degrees
settings.exposure = 1.2f;
settings.groundAlbedo = 0.3f;
sky.SetSettings(settings);

// Query sky color for a specific direction
SkyColor overhead = sky.ComputeSkyColor({0.0f, 1.0f, 0.0f}); // Looking up
SkyColor horizon  = sky.GetHorizonColor();
SkyColor sunDisc  = sky.ComputeSunColor();

// Use horizon color for fog blending
fogSystem.SetColor(horizon.r, horizon.g, horizon.b);

// Per-frame update
sky.Update(deltaTime);
sky.RenderGPU(deltaTime);

// Cleanup
sky.Shutdown();
```

---

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Graphics/SkyAtmosphere.h` | `SkyAtmosphereSystem`, `SkySettings`, `SkyColor`, `PerezCoefficients` |
| `Tests/TestSkyAtmosphere.cpp` | Unit tests (5 test cases) |

---

## See Also

- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) -- Time-of-day and weather systems that drive sun direction and turbidity
- [Volumetric Fog](Volumetric-Fog) -- Fog system that samples horizon color for seamless sky-fog blending
- [Rendering and Graphics](Rendering-and-Graphics) -- Graphics engine and skybox rendering
- [RHI Abstraction Layer](RHI-Abstraction-Layer) -- Backend-agnostic GPU resource creation
- [Clustered Lighting](Clustered-Lighting) -- Lighting pipeline that uses sun intensity from the sky system
