# DXR Raytracing

## Overview

SparkEngine supports hardware-accelerated ray tracing via DirectX Raytracing (DXR) 1.1. Features include ray-traced reflections, soft shadows, ambient occlusion, and global illumination.

## Architecture

- **Namespace:** `Spark::Graphics`
- **Files:** `Graphics/RHI/DXRSupport.h/.cpp`
- **Dependencies:** D3D12 backend, DXR-capable GPU

## DXRManager

Singleton manager that handles the full DXR pipeline:

```cpp
auto& dxr = DXRManager::GetInstance();
dxr.Initialize(d3d12Device);  // Queries DXR tier

// Build acceleration structures
uint32_t blas = dxr.CreateBLAS(blasDesc);
dxr.BuildTLAS(instances);

// Dispatch ray tracing
dxr.TraceReflections(viewProj, cameraPos);
dxr.TraceShadows(lightDir);
dxr.TraceAmbientOcclusion(viewProj, cameraPos);
dxr.TraceGlobalIllumination(viewProj, cameraPos);
```

## Feature Flags

```cpp
enum class RTFeature : uint32_t {
    Reflections       = 1 << 0,
    Shadows           = 1 << 1,
    AmbientOcclusion  = 1 << 2,
    GlobalIllumination = 1 << 3,
    All               = 0xFFFFFFFF
};
```

## Acceleration Structures

- **BLAS (Bottom-Level):** Per-mesh geometry, supports update/refit for animation
- **TLAS (Top-Level):** Scene-wide instance list with transforms and hit group indices
- Memory tracked via `DXRStats::accelerationStructureMemory`

## Quality Presets

| Preset | Reflections SPP | Bounces | Shadows SPP | Render Scale |
|--------|----------------|---------|-------------|--------------|
| Low | 1 | 1 | 1 | 0.5x |
| Medium | 1 | 1 | 2 | 0.75x |
| High | 2 | 2 | 4 | 1.0x |
| Ultra | 4 | 3 | 8 | 1.0x |

## Inline Shaders

The implementation includes HLSL ray tracing shaders:
- **Reflection RayGen:** Traces reflection rays from G-Buffer normals
- **Shadow RayGen:** Soft shadow rays with jittered sampling
- **AO RayGen:** Cosine-weighted hemisphere sampling
- **GI RayGen:** Multi-bounce diffuse global illumination

## GPU Profiling

Timestamp queries measure per-feature timing:
```cpp
auto stats = dxr.GetStats();
stats.rtReflectionsTimeMs;
stats.rtShadowsTimeMs;
stats.rtAOTimeMs;
stats.rtGITimeMs;
```

## Console Commands

```
dxr.status          — Show DXR state
dxr.enable <feature> — Enable reflections/shadows/ao/gi
dxr.quality <preset> — Set low/medium/high/ultra
```
