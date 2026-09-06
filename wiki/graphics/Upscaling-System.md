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

## EASU Algorithm Details

The Edge Adaptive Spatial Upsampling pass is the core of FSR 1.0. It performs a single-pass directional upscale using a 12-tap filter kernel that adapts its shape based on local edge direction and gradient strength.

### How EASU Works

1. **Gradient Analysis:** For each output pixel, the shader samples a 4x4 neighbourhood in the source image and computes horizontal and vertical gradient magnitudes using finite differences.
2. **Edge Direction Estimation:** The ratio of gradients determines the dominant edge direction. This produces a continuous angle rather than a quantised set, allowing smooth adaptation.
3. **Kernel Shape Selection:** A Lanczos-2 kernel is stretched along the detected edge direction so that samples along the edge receive higher weight and samples across the edge receive lower weight. This preserves edge sharpness while avoiding ringing artefacts perpendicular to the edge.
4. **12-Tap Filtering:** The shaped kernel is evaluated at 12 sample positions surrounding the output pixel. Bilinear hardware filtering is used for each tap, effectively doubling the number of source texels considered.
5. **Output:** The weighted sum of the 12 taps produces the upscaled pixel value.

Key constants exposed in `UpscalingSystem.cpp`:

```cpp
// EASU constants packed into two float4 CBs
struct EASUConstants
{
    float inputSizeX, inputSizeY;   // Source resolution
    float outputSizeX, outputSizeY; // Target resolution
    float rcpInputSizeX, rcpInputSizeY;
    float rcpOutputSizeX, rcpOutputSizeY;
};
```

The compute shader dispatches one thread per output pixel in 16x16 thread groups:

```cpp
uint32_t dispatchX = (outputWidth + 15) / 16;
uint32_t dispatchY = (outputHeight + 15) / 16;
context->Dispatch(dispatchX, dispatchY, 1);
```

## RCAS Algorithm Details

Robust Contrast Adaptive Sharpening runs as a second pass after EASU (or after temporal accumulation for FSR 2.0). It applies per-pixel adaptive sharpening that avoids amplifying noise or creating halos.

### How RCAS Works

1. **Local Contrast Measurement:** For each pixel, the shader reads the immediate 4-connected neighbours (up, down, left, right) and computes the minimum and maximum luma values.
2. **Sharpening Weight Calculation:** The weight is derived from `1.0 - saturate((maxLuma - minLuma) / maxLuma)`. High-contrast edges receive less sharpening to avoid overshoot; low-contrast areas receive more to recover detail lost during upscaling.
3. **Sharpening Application:** A negative-lobe unsharp mask is applied: `sharpenedPixel = pixel + weight * (pixel - average(neighbours))`.
4. **Clamp:** The result is clamped to the `[minNeighbour, maxNeighbour]` range to prevent ringing.

The sharpening strength is user-configurable:

```cpp
// 0.0 = maximum sharpening, 1.0 = no sharpening
settings.rcasAttenuation = 0.25f; // Recommended default
```

## Jitter Sequence Details

Temporal upscaling modes (FSR 2.0, DLSS, XeSS) require sub-pixel jitter applied to the projection matrix each frame. SparkEngine uses Halton sequences for quasi-random jitter with good coverage properties.

### Halton Sequence Generation

```cpp
// Generates element n of the Halton sequence with the given base
float UpscalingUtils::GenerateHaltonSequence(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f / static_cast<float>(base);
    uint32_t i = index;
    while (i > 0)
    {
        result += fraction * static_cast<float>(i % base);
        i /= base;
        fraction /= static_cast<float>(base);
    }
    return result;
}
```

The jitter pattern uses base-2 for X and base-3 for Y, cycling over 16 frames (configurable via `settings.jitterPhaseCount`):

```cpp
uint32_t phase = frameIndex % settings.jitterPhaseCount;
float jitterX = UpscalingUtils::GenerateHaltonSequence(phase + 1, 2) - 0.5f;
float jitterY = UpscalingUtils::GenerateHaltonSequence(phase + 1, 3) - 0.5f;

// Apply to projection matrix (in NDC pixel units)
projMatrix[2][0] += jitterX * 2.0f / renderWidth;
projMatrix[2][1] += jitterY * 2.0f / renderHeight;
```

The jitter offset must also be passed to the upscaling shader so it can unjitter the current frame during temporal accumulation. Forgetting to unjitter is the most common integration bug and results in a persistent blurry or shimmering image.

## Render Resolution Calculation

The render resolution for each quality preset is calculated from the output (display) resolution and the render scale factor:

```cpp
// Formula used internally
uint32_t renderWidth  = std::max(1u, static_cast<uint32_t>(outputWidth * renderScale));
uint32_t renderHeight = std::max(1u, static_cast<uint32_t>(outputHeight * renderScale));
```

Concrete examples for a 3840x2160 (4K) output:

| Quality Preset | Render Scale | Render Resolution | Pixel Count Ratio |
|---------------|-------------|-------------------|-------------------|
| Ultra Performance | 0.33 | 1267x713 | 11% |
| Performance | 0.50 | 1920x1080 | 25% |
| Balanced | 0.58 | 2227x1253 | 34% |
| Quality | 0.67 | 2573x1447 | 45% |
| Ultra Quality | 0.77 | 2957x1663 | 59% |
| Native | 1.00 | 3840x2160 | 100% |

The `GetRenderResolution()` method returns the calculated pair and also updates internal constant buffers so the compute shaders receive the correct dimensions.

## Temporal Accumulation Details

FSR 2.0 and other temporal modes maintain a history buffer that accumulates detail over multiple frames. The process each frame is:

1. **Reproject History:** Use motion vectors to warp the previous frame's accumulated buffer to the current frame's viewpoint.
2. **Neighbourhood Clamp:** Compute a colour-space bounding box (AABB in YCoCg space) from the current frame's local neighbourhood. Clamp the reprojected history sample to this box to reject ghosting from disoccluded or fast-moving regions.
3. **Disocclusion Detection:** Compare the current depth buffer against the reprojected depth. Pixels with large depth discontinuities are marked as disoccluded and receive reduced history weight.
4. **Blend:** Lerp between the current jittered sample and the clamped history. Typical blend factor is 0.05-0.10 for the current frame (i.e., 90-95% history weight). Disoccluded pixels use a much higher current-frame weight (0.5-1.0).
5. **Sharpen (RCAS):** Apply the RCAS pass on the accumulated result to restore fine detail.
6. **Store:** Write the blended result into the history buffer for the next frame.

```cpp
// Internal temporal accumulation parameters
struct TemporalAccumulationParams
{
    float historyWeight       = 0.95f;  // Weight for history sample
    float disocclusionWeight  = 0.50f;  // Weight when disocclusion detected
    float depthThreshold      = 0.01f;  // Depth delta for disocclusion
    float velocityWeight      = 1.0f;   // Motion vector confidence scale
};
```

## Motion Vector Requirements

Temporal upscaling requires per-pixel motion vectors rendered to a dedicated R16G16_FLOAT render target during the G-buffer pass. Motion vectors encode the screen-space displacement from the current frame to the previous frame in UV coordinates.

Requirements for correct motion vectors:

- **Camera motion:** All objects must account for the change in view-projection matrix between the current and previous frame.
- **Object motion:** Dynamic objects must additionally encode their per-vertex displacement due to animation or physics movement.
- **Jitter removal:** Motion vectors must be computed using unjittered projection matrices. Jittered matrices cause the motion vector to include the jitter delta, confusing temporal accumulation.
- **Format:** R16G16_FLOAT (16-bit per component) is sufficient for most cases. R32G32_FLOAT provides higher precision for very large resolutions or extreme motion.

```cpp
// In the motion vector pixel shader
float2 currentNDC  = currentClipPos.xy / currentClipPos.w;
float2 previousNDC = previousClipPos.xy / previousClipPos.w;
float2 motionVector = (previousNDC - currentNDC) * float2(0.5, -0.5); // UV space
```

## Dynamic Resolution Scaling Interaction

The upscaling system can work alongside dynamic resolution scaling (DRS) to further improve performance. When DRS is active, the render scale is adjusted each frame based on GPU frame time:

```cpp
UpscalingSettings settings;
settings.mode = UpscalingMode::FSR2;
settings.quality = UpscalingQuality::Quality;       // Base render scale = 67%
settings.enableDynamicResolution = true;
settings.drsMinScale = 0.50f;                        // Never go below 50%
settings.drsMaxScale = 1.00f;                        // Never exceed native
settings.drsTargetFrameTimeMs = 16.67f;              // Target 60 FPS

upscaling.SetSettings(settings);
```

When DRS adjusts the render scale, the EASU/temporal accumulation constants are updated automatically. The history buffer is invalidated when the render resolution changes by more than 10% to avoid accumulation artefacts from mismatched resolutions.

## Integration with Quality Presets

Quality presets in SparkEngine bundle upscaling settings with other rendering parameters for a consistent experience:

```cpp
// In QualityPresets.h
struct QualityPreset
{
    UpscalingMode upscalingMode;
    UpscalingQuality upscalingQuality;
    float rcasAttenuation;
    ShadowQuality shadowQuality;
    TextureQuality textureQuality;
    // ... other settings
};

static constexpr QualityPreset kLowPreset = {
    UpscalingMode::FSR1,
    UpscalingQuality::Performance,
    0.20f,
    ShadowQuality::Low,
    TextureQuality::Low
};

static constexpr QualityPreset kUltraPreset = {
    UpscalingMode::FSR2,
    UpscalingQuality::Quality,
    0.25f,
    ShadowQuality::Ultra,
    TextureQuality::Ultra
};
```

No vendor upscaler SDK is linked: `IsFSR2Available()`, `GetDLSSFeatureInfo().isAvailable`, and `GetXeSSFeatureInfo().isAvailable` all report `false`, and selecting FSR2/DLSS/XeSS logs a warning once and runs the built-in SparkSR path instead. `Console_GetStatus` reports that availability truthfully.

The substitution is now visible rather than implied: `UpscalingSystem::GetEffectiveMode()` returns the mode that actually runs, and the status line reads `Mode: DLSS (running SparkSR)`. Report the effective mode when you quote what is active — a requested mode is not evidence that a vendor upscaler ran.

## Integration with Render Pipeline

The upscaling system is **designed** as a post-processing step after tonemapping and before the final UI overlay. **It is not executed in the frame path today:** the only production uses of `UpscalingSystem` are `Initialize`/`Shutdown` in `GraphicsEngineWindowsInit.cpp` and `GraphicsEngineLinux.cpp`; nothing calls `Execute*` or `GetRenderResolution()` per frame. The diagram shows the intended placement:

```
Scene Render (at render resolution)
    -> G-Buffer Pass (+ motion vectors)
    -> Lighting Pass
    -> Volumetrics
    -> Tonemapping
    -> Upscaling (EASU+RCAS or Temporal) <- UpscalingSystem::Execute()
    -> UI Overlay (at display resolution)
    -> Present
```

The upscaling system manages its own intermediate render targets (history buffer, EASU output) and only requires the caller to provide the tonemapped colour SRV and an output UAV at display resolution.

## ECS Integration and Editor UI (not implemented)

> **Not implemented (verified 2026-09):** no `UpscalingSettingsComponent` exists in
> `SparkEngine/Source`, `RenderSystem` does not read one, and SparkEditor has no `UpscalingSettingsPanel`
> or "Upscaling Settings" window. Earlier revisions of this page described both as if they shipped.
> Upscaling is configured only through the `UpscalingSystem` C++ API above, and that system is not
> executed in the frame path (see "Integration with Render Pipeline"). Treat a per-camera component and
> an editor panel as design intent, not as available features.

## Performance Tips

- **FSR 1.0** is the cheapest option (single spatial pass) and is recommended for very low-end hardware or when motion vectors are not available.
- **FSR 2.0** provides the best quality-per-cost for AMD and older NVIDIA hardware but requires accurate motion vectors.
- **DLSS** leverages dedicated Tensor Cores on NVIDIA RTX GPUs and produces the best quality at the lowest render scales, but is vendor-locked.
- **XeSS** uses XMX cores on Intel Arc GPUs for best quality but also has an effective DP4a fallback path on other vendors.
- The RCAS pass cost is negligible (under 0.1ms at 4K) and should almost always be enabled.
- When using temporal modes, ensure the jitter phase count matches or exceeds the number of frames the history buffer accumulates. A phase count of 16 is recommended for most scenarios.
- Avoid enabling both the engine's built-in TAA and temporal upscaling simultaneously, as double temporal accumulation produces excessive blur and ghosting.

## Testing

5 unit tests in `Tests/TestUpscalingSystem.cpp` covering quality presets, render resolution calculation, input requirements, FSR constants, and default settings.

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Main rendering pipeline documentation
- [Post-Processing](Post-Processing.md) -- Other post-processing effects (bloom, DOF, colour grading)
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) -- How compute shaders are compiled and dispatched
- [Quality Presets](../advanced/Configuration-Reference.md) -- Engine-wide quality preset system
- [Entity-Component-System](../subsystems/Entity-Component-System.md) -- ECS architecture and component reference
