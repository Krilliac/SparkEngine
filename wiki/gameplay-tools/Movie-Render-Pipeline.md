# Movie Render Pipeline

Prototype job-state API for offline cinematic rendering. It does not currently produce image frames.

**Source:** `SparkEngine/Source/Engine/Rendering/MovieRenderPipeline.h`

## Overview

`MovieRenderPipeline` tracks render-job settings, warm-up and sub-frame counters, progress, and callbacks. Its `CaptureFrame()` currently constructs a planned filename and adds it to `outputFiles`; it does not read pixels, accumulate sub-frames, or write a file. A completed job therefore does **not** mean that images were rendered. Do not use this API as a production frame-output workflow.

Quality presets configure counters from Preview (1x AA, no motion blur) to Cinematic (32x AA, 16 motion blur sub-frames). `DeterministicTimeController` advances an internal fixed-time clock; it does not drive the engine world or rendering loop. Warm-up frames likewise advance only that internal state.

Settings expose PNG, EXR, and TGA choices and console-variable override values, but no encoder or override application is wired into this prototype. The engine does not initialize, update, or shut down this optional API automatically; an explicit caller owns its lifecycle.

## Architecture

```
MovieRenderPipeline (singleton)
  +-- MovieRenderJob (active job state)
  |     +-- MovieRenderSettings (resolution, frame range, quality)
  |     +-- progress, timing, output file list
  +-- DeterministicTimeController (internal fixed-time counter)
  +-- accumulation buffer placeholder (no pixel blending)
  +-- callbacks (per-frame, completion)
  +-- completed jobs history
```

### Render Loop

```
StartRender() --> [Warm-up counts] --> [Job-frame counts]
                                        |
                  For each frame:       |
                    For each sub-frame: |
                      Step(fixedDt)     |
                    Record planned path |
                  --> Complete/Finalize
```

## Key Classes

| Class | Description |
|-------|-------------|
| `MovieRenderPipeline` | Singleton tracking prototype job state; no pixel output |
| `MovieRenderSettings` | Stored job settings (resolution, quality, frame range) |
| `MovieRenderJob` | Job state, progress, and planned output paths |
| `DeterministicTimeController` | Advances an internal fixed-time counter |

## Explicit API use (state tracking only)

This example advances prototype job state, not the scene simulation or a real renderer. It will not create image files.

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
    // Planned path only; no file has been written
});

pipeline.StartRender(std::move(settings));

// In the main loop:
pipeline.Update(deltaTime);  // Uses fixed dt internally
float progress = pipeline.GetProgress();  // 0.0 to 1.0
pipeline.Shutdown();
```

## API Reference

### MovieRenderPipeline

| Method | Description |
|--------|-------------|
| `Initialize() / Shutdown()` | Lifecycle management |
| `StartRender(settings)` | Begin a prototype job-state sequence |
| `CancelRender()` | Cancel the active render |
| `Update(float dt)` | Advance internal job counters (explicit caller must invoke it) |
| `IsRendering()` | True while the internal job sequence is active |
| `GetProgress()` | Current progress [0, 1] |
| `GetCurrentJob()` | Access the active job state |
| `GetCompletedJobs()` | History of completed state sequences, not verified image files |
| `SetFrameCapturedCallback()` | Callback per planned output path; no pixels are captured |
| `SetRenderCompleteCallback()` | Callback on job completion |

### MovieRenderSettings

| Field | Default | Description |
|-------|---------|-------------|
| `width / height` | 1920x1080 | Intended output resolution; not applied to a renderer |
| `frameRate` | 30.0 | Target FPS |
| `startFrame / endFrame` | 0 / 300 | Frame range (inclusive) |
| `qualityPreset` | Standard | Preset for internal sub-frame counts |
| `outputFormat` | PNG | Intended format choice; not encoded |
| `aaSamples` | 1 | Internal sub-frame count; no temporal AA applied |
| `motionBlurSubFrames` | 1 | Internal sub-frame count; no motion blur applied |
| `warmUpFrames` | 0 | Internal warm-up step count; no scene simulation |
| `cvarOverrides` | empty | Stored CVar pairs; no overrides applied |

## Configuration

### Quality Presets

These values control the number of internal job-state steps only; they do not produce anti-aliasing or motion blur.

| Preset | AA Samples | Motion Blur Sub-frames |
|--------|-----------|----------------------|
| Preview | 1 | 1 |
| Standard | 4 | 2 |
| High | 8 | 4 |
| Cinematic | 32 | 16 |
| Custom | user-defined | user-defined |

## Related Systems

- [Cinematic System](Cinematic-Sequencer.md) -- Sequencer for driving camera and actors
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- the engine's actual rendering path
