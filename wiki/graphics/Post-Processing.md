# Post-Processing

SparkEngine provides a configurable post-processing pipeline that chains 14 screen-space effects in a fixed order. Each effect is a self-contained pass with its own settings struct, enable/disable state, and per-pass performance metrics. The pipeline manages render target ping-ponging between passes automatically using two `R16G16B16A16_FLOAT` textures, and integrates with the editor through the **PostProcessingPanel**.

**Source Files:**
- `SparkEngine/Source/Graphics/PostProcessingPipeline.h` -- Pipeline class (pass orchestration, render target management)
- `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp` -- Pipeline implementation
- `SparkEngine/Source/Graphics/PostProcessingTypes.h` -- All settings structs, enums, and metric types
- `SparkEngine/Source/Graphics/PostProcessingEffects.h` -- Backward-compatibility re-export of types
- `SparkEditor/Source/Panels/PostProcessingPanel.h` -- Editor panel for post-processing settings

## Pipeline Architecture

All enabled passes execute in the fixed order defined by the `PostProcessPass` enum. Disabled passes are skipped with zero cost. Between each pass, the pipeline swaps source and destination render targets (ping-pong).

```
Scene Color (HDR input)
       |
       v
+------+------+------+------+------+------+------+------+------+------+------+------+------+------+
| 1    | 2    | 3    | 4    | 5    | 6    | 7    | 8    | 9    |10    |11    |12    |13    |14    |
|Bloom |Auto  |Tone  |Color |FXAA  |Depth |Motion|Vign- |Chrom.|Film  |Lens  |Light |Lens  |Sharp-|
|      |Expos.|map   |Grade |      |OfFld |Blur  |ette  |Aberr.|Grain |Dist. |Shaft |Flare |en    |
+------+------+------+------+------+------+------+------+------+------+------+------+------+------+
       |
       v
Final Output (LDR)
```

### Pass Order

| # | Pass | Category | Description |
|---|------|----------|-------------|
| 1 | Bloom | HDR | Bright pixel extraction + multi-pass blur + composite |
| 2 | AutoExposure | HDR | Luminance histogram eye adaptation |
| 3 | Tonemapping | HDR | HDR-to-LDR conversion (ACES, Filmic, Neutral, Reinhard) |
| 4 | ColorGrading | Color | Lift/Gamma/Gain, temperature, tint, hue shift |
| 5 | FXAA | Anti-Aliasing | Fast Approximate Anti-Aliasing |
| 6 | DepthOfField | Lens | Bokeh blur based on focal distance and aperture |
| 7 | MotionBlur | Temporal | Per-pixel velocity-based blur |
| 8 | Vignette | Lens | Screen-edge darkening |
| 9 | ChromaticAberration | Lens | RGB channel separation at edges |
| 10 | FilmGrain | Cinematic | Animated noise overlay |
| 11 | LensDistortion | Lens | Barrel/pincushion distortion |
| 12 | LightShafts | Volumetric | God rays from bright light sources |
| 13 | LensFlare | Lens | Ghost images and halo from bright sources |
| 14 | Sharpen | Output | Contrast-adaptive sharpening (CAS) |

### Render Target Strategy

The pipeline allocates two `R16G16B16A16_FLOAT` textures at the viewport resolution. Each pass reads from one texture and writes to the other. The `SwapTargets()` call alternates which texture is source and which is destination. On viewport resize, `Resize()` recreates both targets.

## Effects Reference

### Bloom (`BloomSettings`)

Extracts bright pixels above a luminance threshold, applies multi-pass Gaussian blur, and composites the result back. Supports a soft knee around the threshold for smooth falloff.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `threshold` | float | 1.0 | Luminance cutoff for bright pixel extraction |
| `softThreshold` | float | 0.5 | Soft knee width around threshold [0, 1] |
| `intensity` | float | 0.8 | Final bloom composite strength |
| `radius` | float | 4.0 | Blur radius in texels |
| `iterations` | int | 5 | Downscale/blur passes [1, 8] |
| `scatter` | float | 0.7 | Energy scatter between blur passes [0, 1] |
| `highQuality` | bool | true | 13-tap dual filter (true) vs 5-tap (false) |

### AutoExposure (`AutoExposureSettings`)

Measures scene luminance via a histogram and smoothly adapts exposure over time, simulating human eye adaptation between bright and dark environments.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `minExposure` | float | 0.25 | Minimum EV (prevents over-darkening) |
| `maxExposure` | float | 4.0 | Maximum EV (prevents over-brightening) |
| `adaptSpeedUp` | float | 2.0 | Bright-to-dark adaptation speed (EV/s) |
| `adaptSpeedDown` | float | 1.0 | Dark-to-bright adaptation speed (EV/s) |
| `targetLuminance` | float | 0.18 | Middle-grey key value |
| `histogramMin` | float | -8.0 | Log2 luminance histogram lower bound |
| `histogramMax` | float | 4.0 | Log2 luminance histogram upper bound |
| `compensationEV` | float | 0.0 | Manual EV compensation offset |

### Tonemapping (`TonemappingSettings`)

Converts the HDR scene to LDR display range. Four tonemapping operators are available via the `TonemapOperator` enum:

| Operator | Description |
|----------|-------------|
| `ACES` | Academy Color Encoding System -- filmic, industry standard (default) |
| `Filmic` | Uncharted 2 filmic curve (John Hable) |
| `Neutral` | Minimal color shift, balanced contrast |
| `Reinhard` | Simple luminance-based Reinhard |

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `op` | TonemapOperator | ACES | Active tonemapping operator |
| `exposure` | float | 1.0 | Pre-tonemap exposure multiplier |
| `whitePoint` | float | 11.2 | White point for Filmic/Reinhard |
| `contrast` | float | 1.0 | Post-tonemap contrast [0.5, 2.0] |
| `saturation` | float | 1.0 | Post-tonemap saturation [0, 2] |

### ColorGrading (`ColorGradingSettings`)

Professional color correction using Lift/Gamma/Gain (shadows/midtones/highlights) plus global adjustments for temperature, tint, hue, saturation, brightness, and contrast.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `lift` | XMFLOAT3 | (0, 0, 0) | Shadow color offset |
| `gamma` | XMFLOAT3 | (1, 1, 1) | Midtone power curve |
| `gain` | XMFLOAT3 | (1, 1, 1) | Highlight multiplier |
| `temperature` | float | 0.0 | White balance [-1=cool, 1=warm] |
| `tint` | float | 0.0 | Green-magenta shift [-1, 1] |
| `hueShift` | float | 0.0 | Global hue rotation in degrees [-180, 180] |
| `saturation` | float | 1.0 | Global saturation [0=mono, 2=oversaturated] |
| `brightness` | float | 0.0 | Global brightness offset [-1, 1] |
| `contrast` | float | 1.0 | Global contrast [0.5, 2.0] |

### FXAA (`FXAASettings`)

Fast Approximate Anti-Aliasing -- a single-pass post-process AA that smooths jagged edges based on luminance contrast. Lightweight alternative to MSAA with no geometry cost.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `edgeThreshold` | float | 0.166 | Minimum luminance edge detection [0.063, 0.333] |
| `edgeThresholdMin` | float | 0.0833 | Darkest edge threshold |
| `subpixelQuality` | float | 0.75 | Sub-pixel AA quality [0=off, 1=max] |
| `qualityPreset` | int | 12 | Quality iterations [10=low, 29=ultra] |

### DepthOfField (`DepthOfFieldSettings`)

Physically-based depth of field with configurable focal plane, aperture, and bokeh shape. Uses the scene depth buffer to compute per-pixel circle of confusion.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `focalDistance` | float | 10.0 | Focus plane distance in meters |
| `focalLength` | float | 50.0 | Lens focal length in mm |
| `aperture` | float | 2.8 | F-stop (lower = more blur) |
| `nearBlurStart` / `End` | float | 0.5 / 2.0 | Near blur distance range |
| `farBlurStart` / `End` | float | 20.0 / 100.0 | Far blur distance range |
| `maxBokehSize` | float | 8.0 | Maximum bokeh diameter in pixels |
| `blurSamples` | int | 16 | Blur kernel samples |
| `useCircularBokeh` | bool | true | Circular (true) vs hexagonal (false) |
| `bokehBrightness` | float | 1.0 | Brightness threshold for bokeh highlights |

### MotionBlur

Per-pixel motion blur using temporal velocity data. This pass has no dedicated settings struct -- it uses velocity buffer data from the `TemporalEffects` system.

### Vignette (`VignetteSettings`)

Darkens screen edges to draw attention to the center. Supports configurable color, center position, and shape.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `intensity` | float | 0.3 | Darkening strength [0, 1] |
| `smoothness` | float | 0.5 | Edge softness [0, 1] |
| `roundness` | float | 1.0 | Shape (1=circular, 0=rectangular) |
| `color` | XMFLOAT3 | (0, 0, 0) | Vignette color (default: black) |
| `center` | XMFLOAT2 | (0.5, 0.5) | Center in UV space |

### ChromaticAberration (`ChromaticAberrationSettings`)

Simulates lens imperfection by separating RGB channels, with stronger effect at screen edges.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `intensity` | float | 0.5 | Separation amount [0, 3] |
| `radialFalloff` | float | 1.0 | Edge emphasis [0=uniform, 2=strong edge] |
| `channelOffsets` | XMFLOAT3 | (1, 0, -1) | R, G, B offset multipliers |

### FilmGrain (`FilmGrainSettings`)

Animated noise overlay that simulates cinematic film grain. Supports monochrome and colored modes.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `intensity` | float | 0.15 | Grain visibility [0, 1] |
| `size` | float | 1.6 | Grain particle size |
| `speed` | float | 1.0 | Animation speed |
| `luminanceContribution` | float | 0.8 | Luminance influence on grain [0, 1] |
| `colored` | bool | false | Color noise (true) vs monochrome (false) |

### LensDistortion (`LensDistortionSettings`)

Barrel or pincushion distortion simulating real lens imperfections.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `barrelDistortion` | float | 0.0 | [-1=pincushion, 1=barrel] |
| `zoomCompensation` | float | 1.0 | Zoom to compensate for distortion |
| `center` | XMFLOAT2 | (0.5, 0.5) | Distortion center in UV space |
| `cubicDistortion` | float | 0.0 | Higher-order distortion term |

### LightShafts (`LightShaftSettings`)

Screen-space god rays via radial blur from a light source position. Uses ray marching with configurable density and decay.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `lightScreenPos` | XMFLOAT2 | (0.5, 0.3) | Light source screen position |
| `density` | float | 1.0 | Ray density [0, 1] |
| `weight` | float | 0.01 | Intensity per sample |
| `decay` | float | 0.97 | Intensity decay per step [0, 1] |
| `exposure` | float | 1.0 | Final exposure multiplier |
| `sampleCount` | int | 64 | Ray marching samples |
| `color` | XMFLOAT3 | (1.0, 0.95, 0.8) | Shaft color |

### LensFlare (`LensFlareSettings`)

Generates ghost images and halo rings from bright light sources in the scene.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `threshold` | float | 0.8 | Brightness threshold for flare trigger |
| `intensity` | float | 0.5 | Flare overall intensity |
| `ghostCount` | int | 5 | Number of ghost images |
| `ghostSpacing` | float | 0.3 | Distance between ghosts |
| `ghostThreshold` | float | 10.0 | Brightness for ghost generation |
| `haloRadius` | float | 0.6 | Halo ring radius |
| `haloThickness` | float | 0.1 | Halo ring width |
| `chromaticDistortion` | float | 2.5 | Color separation in flare |

### Sharpen (`SharpenSettings`)

Contrast-adaptive sharpening inspired by AMD FidelityFX CAS. Applied last in the chain to counteract any softening from earlier passes.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `amount` | float | 0.5 | Sharpening strength [0, 1] |
| `threshold` | float | 0.05 | Edge threshold (avoids noise amplification) |
| `adaptiveSharpening` | bool | true | CAS mode (AMD FidelityFX style) |

## Usage Example

```cpp
#include "Graphics/PostProcessingPipeline.h"

using namespace Spark::Graphics;

// Create and initialize the pipeline
PostProcessingPipeline pipeline;
pipeline.SetDevice(device, context);
pipeline.Initialize(1920, 1080);

// Enable and configure bloom
pipeline.SetEffectEnabled(PostProcessPass::Bloom, true);
pipeline.GetBloomSettings().threshold = 0.9f;
pipeline.GetBloomSettings().intensity = 1.0f;

// Enable tonemapping with ACES
pipeline.SetEffectEnabled(PostProcessPass::Tonemapping, true);
pipeline.GetTonemappingSettings().op = TonemapOperator::ACES;
pipeline.GetTonemappingSettings().exposure = 1.2f;

// Enable vignette
pipeline.SetEffectEnabled(PostProcessPass::Vignette, true);
pipeline.GetVignetteSettings().intensity = 0.4f;

// Enable depth of field
pipeline.SetEffectEnabled(PostProcessPass::DepthOfField, true);
pipeline.GetDOFSettings().focalDistance = 15.0f;
pipeline.GetDOFSettings().aperture = 2.8f;

// Each frame, after scene rendering:
pipeline.SetInputSRV(sceneColorSRV);
pipeline.SetDepthSRV(sceneDepthSRV);
pipeline.Process(deltaTime);
pipeline.Render();

// Handle viewport resize
pipeline.Resize(newWidth, newHeight);

// Query per-pass performance
auto metrics = pipeline.GetPassMetrics();
for (const auto& pm : metrics)
{
    if (pm.isEnabled)
    {
        Logger::Info("{}: {:.2f}ms", pm.name, pm.timeMs);
    }
}
```

### Spatial post-process volumes (Phase K)

`VolumeManager`, owned by the pipeline, lets level designers author
global or local volumes that override a subset of the effect settings
— only the fields whose `overrideState` was set ever touch the live
settings, so hand-authored values survive volume-less frames. Push the
camera position once per frame and `Process()` blends the stack:

```cpp
using namespace Spark::Graphics;

auto& volumes = pipeline.GetVolumeManager();

// Author a global fallback — always applies, lowest priority:
if (Volume* global = volumes.CreateVolume("global_defaults"))
{
    global->isGlobal = true;
    global->priority = 0;
    if (auto* ex = global->AddComponent<ExposureVolumeComponent>())
    {
        ex->compensationEV.value = 0.0f;
        ex->compensationEV.overrideState = true;
    }
}

// Author a cave volume — overrides bloom + colour grading when the
// camera is inside the AABB, fading in over `blendDistance` metres:
if (Volume* cave = volumes.CreateVolume("cave"))
{
    cave->isGlobal      = false;
    cave->boundsMin     = {-40.0f, -5.0f, -40.0f};
    cave->boundsMax     = { 40.0f, 20.0f,  40.0f};
    cave->blendDistance = 3.0f;
    cave->priority      = 10;

    auto* bloom = cave->AddComponent<BloomVolumeComponent>();
    bloom->intensity.value = 0.2f;
    bloom->intensity.overrideState = true;

    auto* grade = cave->AddComponent<ColorGradingVolumeComponent>();
    grade->temperature.value = -0.15f;
    grade->temperature.overrideState = true;
    grade->saturation.value = 0.8f;
    grade->saturation.overrideState = true;
}

// Per frame:
pipeline.SetCameraPosition(camera.worldPosition);
pipeline.Process(deltaTime);   // runs VolumeManager::Update + ApplyVolumeStack
```

## Console Commands

Post-processing console commands are registered in `AdvancedConsoleCommands.cpp` (FPS game module):

```
pp_list              # List all post-processing effects and their on/off state
exposure <value>     # Set light shaft exposure value
hdr <on/off>         # Enable/disable HDR rendering
```

The `pp_list` command calls `PostProcessingPipeline::Console_ListEffects()`, which prints each pass name and its enabled/disabled state along with the total active pass count.

## Performance Metrics

Each pass records its GPU execution time in `m_passTimings[]`. Call `GetPassMetrics()` to retrieve a vector of `PassMetrics` structs:

```cpp
struct PassMetrics
{
    std::string name;    // Pass name (e.g., "Bloom", "FXAA")
    float timeMs = 0.0f; // GPU time in milliseconds
    bool isEnabled = false;
};
```

The `GetActivePassCount()` method returns how many passes were active in the last frame.

## Editor Integration

The **PostProcessingPanel** (`SparkEditor::PostProcessingPanel`) exposes bloom, tonemapping, fog, sky, and wind parameters through ImGui controls. It reads from and writes to the scene's `EnvironmentSettings`, which are serialized to scene files. The `LightingTools` panel also provides post-processing controls for tonemapping, exposure, bloom, contrast, saturation, and brightness as part of the lighting preset system.

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Graphics/PostProcessingPipeline.h` | Pipeline class declaration |
| `SparkEngine/Source/Graphics/PostProcessingPipeline.cpp` | Pipeline implementation (pass execution, GPU resources) |
| `SparkEngine/Source/Graphics/PostProcessingTypes.h` | All enums, settings structs, and metric types |
| `SparkEngine/Source/Graphics/PostProcessingEffects.h` | Backward-compatibility re-export header |
| `SparkEditor/Source/Panels/PostProcessingPanel.h` | Editor panel declaration |
| `SparkEditor/Source/Panels/PostProcessingPanel.cpp` | Editor panel ImGui implementation |
| `SparkEditor/Source/Lighting/LightingTools.cpp` | Lighting preset post-processing integration |
| `GameModules/SparkGameFPS/Source/Console/AdvancedConsoleCommands.cpp` | Console command registration |

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- GraphicsEngine and rendering pipeline overview
- [Render Graph](Render-Graph.md) -- Render graph pass scheduling system
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) -- Shader compilation and management
- [Camera System](../subsystems/Camera-System.md) -- Camera settings affecting depth of field
- [Editor Panels](../gameplay-tools/SparkEditor.md) -- PostProcessingPanel and other editor UI
