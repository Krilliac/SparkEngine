# Volumetric Clouds

SparkEngine ships a ray-marched volumetric cloud model inspired by the
Decima "Nubis" system and GPU Pro 7. Coverage, cloud type, and wetness
are driven by a 2D weather map; density is modulated by a low-frequency
Perlin base and detail-eroded by a high-frequency Worley noise;
lighting is evaluated with the Henyey-Greenstein phase function and a
multiple-scattering approximation.

**Source:** `SparkEngine/Source/Graphics/VolumetricClouds.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestVolumetricClouds.cpp`

---

## Table of Contents

- [Overview](#overview)
- [Two-scale density model](#two-scale-density-model)
- [Settings](#settings)
- [Lighting & Phase Function](#lighting--phase-function)
- [CPU reference vs GPU path](#cpu-reference-vs-gpu-path)
- [Weather map driver](#weather-map-driver)
- [Usage](#usage)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

`VolumetricCloudSystem` is a singleton. It owns three small CPU noise
textures (base 32³, detail 16³, weather 64²) plus optional RHI-backed
3D textures for the GPU raymarch.

```
┌──────────────────────┐      weather map     ┌───────────────────────┐
│   WeatherSystem      │─── coverage/type ───▶│ VolumetricCloudSystem │
│   (rain, humidity)   │                      │                       │
└──────────────────────┘                      │  base noise (32³)     │
                                              │  detail noise (16³)   │
┌──────────────────────┐    sun direction     │  weather map  (64²)   │
│ Day/Night Cycle      │─────────────────────▶│                       │
└──────────────────────┘                      │  CPU ray marcher      │
                                              │  GPU textures (RHI)   │
                                              └──────────┬────────────┘
                                                         │
                                                         ▼
                                                 Sky rendering pass
```

---

## Two-scale density model

Cloud density at a world-space point is evaluated as:

```
base      = SampleBaseNoise(p * baseNoiseScale)       // low-freq, Perlin-like
detail    = SampleDetailNoise(p * detailNoiseScale)   // high-freq, Worley-like
height    = HeightGradient(altitude, cloudType)       // stratus / cumulus / cumulonimbus
weather   = SampleWeatherMap(p.xz * weatherScale)     // coverage + type + rain
density   = saturate(base * weather.coverage - (1 - detail) * detailStrength) * height
```

The two-scale separation means coverage can change at weather-map rate
without forcing the detail noise to be retiled, and detail erosion can
sharpen cloud edges without affecting the coarse silhouette.

---

## Settings

`VolumetricCloudSettings` is a plain struct with the controls
operators want to touch:

| Field | Default | Meaning |
|-------|---------|---------|
| `coverage` | `0.55` | Global coverage multiplier (`[0..1]`) |
| `cloudType` | `0.5` | `0` stratus, `0.5` cumulus, `1` cumulonimbus |
| `density` | `0.04` | Peak extinction per metre |
| `anvilBias` | `0.1` | Flattens the top of cumulonimbus |
| `baseAltitude` | `1500 m` | Bottom of the cloud layer |
| `topAltitude` | `4000 m` | Top of the cloud layer |
| `baseNoiseScale` | `1 / 2048` | World-space frequency of the Perlin base |
| `detailNoiseScale` | `1 / 128` | World-space frequency of the Worley detail |
| `detailStrength` | `0.35` | How aggressively detail erodes the base |
| `weatherScale` | `1 / 32768` | World-space frequency of the weather map |
| `windDirection` + `windSpeed` | `(1,0,0) × 5 m/s` | UV advection of the noise textures |
| `phaseG0`, `phaseG1`, `phaseBlend` | `0.8, -0.3, 0.5` | Dual-lobe Henyey-Greenstein |
| `silverLining` | `0.6` | Artistic front-lit bias |
| `marchSteps` | `64` | Primary raymarch steps (GPU quality hint) |
| `lightSteps` | `6` | Light raymarch steps |
| `lightMarchDistance` | `600 m` | Shadow march depth toward the sun |
| `enabled` | `true` | Master switch |

---

## Lighting & Phase Function

Each primary march step estimates in-scattered light with a dual-lobe
Henyey-Greenstein phase function:

```
phase(cosθ) = lerp(HG(cosθ, g0), HG(cosθ, g1), blend)
```

`g0 ≈ 0.8` produces a sharp forward lobe that gives clouds their bright
silver edge when the sun is behind them; `g1 ≈ -0.3` produces the mild
back-scatter that keeps the shadowed side from going black. A short
secondary light march (`lightSteps` samples out to `lightMarchDistance`)
approximates in-scattering with a Beer-Lambert fall-off toward the sun.

---

## CPU reference vs GPU path

- **CPU path.** Everything the GPU does, the CPU also does — in
  `VolumetricCloudSystem::SampleAlongRay` — so the model can be
  unit-tested headlessly (no device required). `Tests/TestVolumetricClouds.cpp`
  exercises coverage, density, sample-along-ray, wind advection, and
  sun-direction invariants.
- **GPU path.** `CreateGPUResources(device)` allocates two 3D textures
  (base + detail noise) and a 2D texture (weather). `UploadToGPU(device)`
  pushes the latest CPU noise/weather grids; the compute/pixel shader
  samples them with the same formulas for full resolution per-pixel
  raymarch.

The GPU shader lives in the same author-time HLSL tree as the rest of
the sky pipeline — it consumes the textures exposed by
`GetBaseNoiseTexture()`, `GetDetailNoiseTexture()`, and
`GetWeatherMapTexture()`.

---

## Weather map driver

The weather map is a 64×64 RG texture where:

- **R** channel = local coverage override
- **G** channel = local cloud type

`SetWeather(coverage, cloudType, rain)` mixes the authored weather
texture with a dynamic overlay so `WeatherSystem` can push
rain/humidity/coverage from the simulation without stomping the
artist-authored map. The `rain` value also feeds the wetness darkening
factor applied at integration time.

---

## Usage

```cpp
#include "Graphics/VolumetricClouds.h"

auto& clouds = Spark::Graphics::VolumetricCloudSystem::GetInstance();

Spark::Graphics::VolumetricCloudSettings settings;
settings.coverage      = 0.55f;
settings.cloudType     = 0.7f;     // lean toward cumulonimbus
settings.windDirectionX = 1.0f;
settings.windSpeed     = 8.0f;
clouds.Initialize(settings);

// Per frame:
clouds.Update(dt);
clouds.SetSunDirection(sun.x, sun.y, sun.z);
clouds.SetWeather(weather.coverage, weather.cloudType, weather.rain);

// Optional CPU sample (for ambient probes, shadow queries, tests):
auto s = clouds.SampleAlongRay(origin.x, origin.y, origin.z,
                               dir.x,    dir.y,    dir.z,
                               20'000.0f);
sky.AddInscatter(s.luminanceR, s.luminanceG, s.luminanceB);
sky.MultiplyTransmittance(s.transmittance);
```

For the GPU path, create resources once after an RHI device is
available and re-upload whenever the CPU noise is regenerated:

```cpp
clouds.CreateGPUResources(rhiDevice);
// ... on settings change:
clouds.UploadToGPU(rhiDevice);
```

---

## Integration

- **Sky rendering** — `SkyAtmosphere` composites the inscattered cloud
  luminance and transmittance over the analytical sky model so clouds
  inherit the Preetham sun/horizon colour.
- **Volumetric fog** — `FroxelVolumetricFog` runs underneath the cloud
  layer and shares the sun direction + wetness inputs for consistent
  weather transitions.
- **Weather system** — `WeatherSystem::Tick` forwards
  `(coverage, cloudType, rain)` to `SetWeather` every frame.
- **Day/Night cycle** — the cycle pushes `SetSunDirection` + sun colour
  so a single update propagates to sky, clouds, and fog in lockstep.

---

## See Also

- [Sky and Atmosphere](Sky-and-Atmosphere.md) — base sky model
- [Volumetric Fog](Volumetric-Fog.md) — ground-level volumetrics
- [Day Night Cycle and Weather](../gameplay-tools/Day-Night-Cycle-and-Weather.md)
- `Tests/TestVolumetricClouds.cpp` — reference test suite
