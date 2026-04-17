# Movie Render Pipeline

Offline cinematic rendering pipeline for high-quality frame output with deterministic time stepping and temporal accumulation.

**Source:** `SparkEngine/Source/Engine/Rendering/MovieRenderPipeline.h`

## Overview

The Movie Render Pipeline renders cinematic sequences offline at arbitrary resolution and quality settings. Unlike real-time rendering, it steps the simulation deterministically at a fixed timestep, accumulates multiple sub-frames for anti-aliasing and motion blur, and writes each completed frame to disk.

The pipeline supports quality presets ranging from fast Preview (1x AA, no motion blur) to Cinematic (32x AA, 16 motion blur sub-frames). A `DeterministicTimeController` overrides the engine's delta time during rendering to ensure frame-perfect reproducibility. Optional warm-up frames allow particle systems, physics, and other time-dependent effects to settle before recording begins.

Output formats include PNG (8-bit RGBA), EXR (32-bit HDR), and TGA (uncompressed). Console variable overrides let you force maximum quality settings during the render without changing the game's runtime configuration.

## Architecture

```
MovieRenderPipeline (singleton)
  +-- MovieRenderJob (active job state)
  |     +-- MovieRenderSettings (resolution, frame range, quality)
  |     +-- progress, timing, output file list
  +-- DeterministicTimeController (fixed dt override)
  +-- accumulation buffer (sub-frame blending)
  +-- callbacks (per-frame, completion)
  +-- completed jobs history
```

### Render Loop

```
StartRender() --> [WarmUp frames] --> [Render frames]
                                        |
                  For each frame:       |
                    For each sub-frame: |
                      Step(fixedDt)     |
                    CaptureFrame()      |
                  --> Complete/Finalize
```

## Key Classes

| Class | Description |
|-------|-------------|
| `MovieRenderPipeline` | Singleton managing offline render jobs |
| `MovieRenderSettings` | Configuration for a render job (resolution, quality, frame range) |
| `MovieRenderJob` | Active or completed job with state, progress, and output files |
| `DeterministicTimeController` | Overrides engine time with fixed delta for reproducible simulation |

## Usage

```cpp
auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
pipeline.Initialize();

Spark::Rendering::MovieRenderSettings settings;
settings.width = 3840;
settings.height = 2160;
settings.frameRate = 24.0f;
settings.startFrame = 0;
settings.endFrame = 240;
settings.qualityPreset = Spark::Rendering::RenderQuality::Cinematic;
settings.outputFormat = Spark::Rendering::OutputFormat::EXR;
settings.warmUpFrames = 30;
settings.outputDirectory = "Renders/Scene01";

pipeline.SetFrameCapturedCallback([](int32_t frame, const std::string& path) {
    // Per-frame notification
});

pipeline.StartRender(std::move(settings));

// In the main loop:
pipeline.Update(deltaTime);  // Uses fixed dt internally
float progress = pipeline.GetProgress();  // 0.0 to 1.0
```

## API Reference

### MovieRenderPipeline

| Method | Description |
|--------|-------------|
| `Initialize() / Shutdown()` | Lifecycle management |
| `StartRender(settings)` | Begin an offline render job |
| `CancelRender()` | Cancel the active render |
| `Update(float dt)` | Per-frame update (call every frame) |
| `IsRendering()` | True if a render is in progress |
| `GetProgress()` | Current progress [0, 1] |
| `GetCurrentJob()` | Access the active job state |
| `GetCompletedJobs()` | History of completed jobs |
| `SetFrameCapturedCallback()` | Callback per captured frame |
| `SetRenderCompleteCallback()` | Callback on job completion |

### MovieRenderSettings

| Field | Default | Description |
|-------|---------|-------------|
| `width / height` | 1920x1080 | Output resolution |
| `frameRate` | 30.0 | Target FPS |
| `startFrame / endFrame` | 0 / 300 | Frame range (inclusive) |
| `qualityPreset` | Standard | Preview, Standard, High, Cinematic, Custom |
| `outputFormat` | PNG | PNG, EXR, TGA |
| `aaSamples` | 1 | Temporal AA samples (1, 4, 8, 16, 32) |
| `motionBlurSubFrames` | 1 | Motion blur sub-frames (1-16) |
| `warmUpFrames` | 0 | Simulation warm-up before recording |
| `cvarOverrides` | empty | CVar key-value pairs to override during render |

## Configuration

### Quality Presets

| Preset | AA Samples | Motion Blur Sub-frames |
|--------|-----------|----------------------|
| Preview | 1 | 1 |
| Standard | 4 | 2 |
| High | 8 | 4 |
| Cinematic | 32 | 16 |
| Custom | user-defined | user-defined |

## Related Systems

- [Cinematic System](Cinematic-Sequencer.md) -- Sequencer for driving camera and actors
- [Graphics Engine](../subsystems/Rendering-and-Graphics.md) -- RHI and rendering pipeline
- [Screen Capture](Movie-Render-Pipeline.md) -- Pixel readback for frame output
