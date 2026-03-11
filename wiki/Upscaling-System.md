# Upscaling System (DLSS/FSR/XeSS)

## Overview

SparkEngine's upscaling system provides a unified interface for spatial and temporal upscaling: AMD FSR 1.0/2.0, NVIDIA DLSS, and Intel XeSS.

## Architecture

- **Files:** `Graphics/UpscalingSystem.h` (header + inline), `Graphics/UpscalingSystem.cpp` (compute shaders + utilities)
- **Platform:** Windows (D3D11 compute shaders)

## Supported Modes

| Mode | Type | Inputs Required |
|------|------|-----------------|
| FSR 1.0 | Spatial | Color only |
| FSR 2.0 | Temporal | Color, depth, motion vectors, jitter |
| DLSS | Temporal | Color, depth, motion vectors, exposure, jitter |
| XeSS | Temporal | Color, depth, motion vectors, jitter |

## Quality Presets

| Quality | Render Scale | Use Case |
|---------|-------------|----------|
| Ultra Performance | 33% | 4K with low-end GPU |
| Performance | 50% | Best performance/quality balance |
| Balanced | 58% | Moderate quality uplift |
| Quality | 67% | High quality with some perf gain |
| Ultra Quality | 77% | Minimal quality loss |
| Native | 100% | Sharpening only |

## Usage

```cpp
UpscalingSystem upscaling;
upscaling.Initialize(device, context, 1920, 1080);

UpscalingSettings settings;
settings.mode = UpscalingMode::FSR1;
settings.quality = UpscalingQuality::Quality;
upscaling.SetSettings(settings);

auto [renderW, renderH] = upscaling.GetRenderResolution();
// Render scene at renderW x renderH...
upscaling.Execute(colorSRV, outputUAV);
```

## Compute Shaders

### FSR 1.0
- **EASU (Edge Adaptive Spatial Upsampling):** 12-tap Lanczos filter with edge detection
- **RCAS (Robust Contrast Adaptive Sharpening):** Per-pixel adaptive sharpening

### FSR 2.0 / Temporal
- Temporal accumulation with motion vector reprojection
- Jitter-aware sampling using Halton sequences
- Depth-based disocclusion detection

## Utility Functions

```cpp
// Halton jitter for TAA/temporal upscaling
float jitter = UpscalingUtils::GenerateHaltonSequence(frameIndex, 2);

// SDK availability detection
bool hasDLSS = UpscalingUtils::DetectDLSSAvailability();
bool hasXeSS = UpscalingUtils::DetectXeSSAvailability();
```

## Testing

5 unit tests in `Tests/TestUpscalingSystem.cpp` covering quality presets, render resolution calculation, input requirements, FSR constants, and default settings.
