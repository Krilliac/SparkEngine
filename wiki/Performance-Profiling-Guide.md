# Performance Profiling Guide

This page explains how to use SparkEngine's built-in profiling tools to identify and resolve performance bottlenecks.

**Source:** `SparkEngine/Source/Utils/Profiler.h`, `ChromeTracing.h`, `MemoryDebugger.h`, `DebugOverlay.h`, `FrameInspector.h`

---

## Quick Start

Enable the profiler overlay in-game:

```cpp
auto& profiler = Profiler::GetInstance();
profiler.SetOverlayEnabled(true);
```

Or via the debug console:

```
profiler.overlay on
profiler.gpu on
```

---

## CPU Profiling

### Profile Categories

Every profiling sample is tagged with a category:

| Category | What it covers |
|----------|---------------|
| `Frame` | Overall frame timing |
| `Render` | Draw calls, state changes |
| `Physics` | Bullet simulation step |
| `Audio` | XAudio2 processing |
| `GameLogic` | ECS systems, gameplay |
| `Input` | Input polling and dispatch |
| `Particles` | Particle system updates |
| `UI` | ImGui layout and rendering |
| `Custom` | User-defined sections |

### Manual Instrumentation

```cpp
auto& profiler = Profiler::GetInstance();

// Begin/end a named section
profiler.BeginSection("PhysicsStep", ProfileCategory::Physics);
physicsSystem.Update(dt);
profiler.EndSection("PhysicsStep");
```

### Scoped Profiling (RAII)

```cpp
{
    PROFILE_SCOPE("RenderShadows", ProfileCategory::Render);
    RenderShadowMaps();
}  // Automatically ends when scope exits
```

### Reading Results

```cpp
float physicsMs = profiler.GetSectionTime("PhysicsStep");
float frameMs = profiler.GetFrameTime();
float fps = 1000.0f / frameMs;

// Get all section timings
auto sections = profiler.GetAllSections();
for (const auto& [name, timing] : sections) {
    // timing.lastMs, timing.avgMs, timing.maxMs, timing.category
}
```

---

## GPU Profiling

GPU timing uses D3D11 timestamp queries (Windows only):

```cpp
profiler.SetGPUProfilingEnabled(true);

// In rendering code
profiler.BeginGPUSection("ShadowPass");
RenderShadowMaps();
profiler.EndGPUSection("ShadowPass");

// Read results (available next frame due to GPU latency)
float shadowMs = profiler.GetGPUSectionTime("ShadowPass");
```

---

## Frame Timing History

The profiler maintains a rolling history of frame times:

```cpp
// Get the last N frame times
auto history = profiler.GetFrameTimeHistory();  // vector<float>

// Get statistics
float avgFrame = profiler.GetAverageFrameTime();  // ms
float minFrame = profiler.GetMinFrameTime();
float maxFrame = profiler.GetMaxFrameTime();
float p99Frame = profiler.GetPercentileFrameTime(99);
```

---

## Chrome Tracing Export

Export profiling data to Chrome's `chrome://tracing` format for detailed timeline analysis:

**Source:** `SparkEngine/Source/Utils/ChromeTracing.h`

```cpp
ChromeTracing tracer;

// Record events
tracer.BeginEvent("Update", "GameLogic");
// ... work ...
tracer.EndEvent("Update", "GameLogic");

// Export to file
tracer.ExportToFile("profile_capture.json");
```

Open the exported `.json` file in:
- Chrome: Navigate to `chrome://tracing` and load the file
- [Perfetto UI](https://ui.perfetto.dev): Drag and drop the file

### Capture Workflow

1. Start capture: `profiler.capture start`
2. Play through the problematic scenario
3. Stop capture: `profiler.capture stop`
4. File is saved to `profile_capture.json` in the working directory

---

## Memory Profiling

**Source:** `SparkEngine/Source/Utils/MemoryDebugger.h`

### Category Tracking

```cpp
auto stats = MemoryDebugger::GetInstance().GetCategoryStats();
for (const auto& cat : stats) {
    // cat.name: "Physics", "Rendering", "Audio", etc.
    // cat.currentBytes: currently allocated
    // cat.peakBytes: high-water mark
    // cat.totalAllocations: lifetime count
}
```

### Allocation Hotspots

Find the code locations that allocate most frequently:

```cpp
auto hotspots = MemoryDebugger::GetInstance().GetHotSpots(10);
for (const auto& spot : hotspots) {
    // spot.location: "Physics/RigidBody.cpp:42"
    // spot.count: number of allocations from this site
    // spot.totalBytes: total bytes allocated
}
```

### Leak Detection

At shutdown:

```cpp
SPARK_PRINT_LEAK_REPORT();
// Output:
//   [LEAK] 0x7fff1234: 256 bytes (Physics) at PhysicsBody.cpp:87
//   [LEAK] 0x7fff5678: 1024 bytes (Rendering) at Mesh.cpp:142
```

---

## Debug Overlay

The debug overlay renders real-time statistics as an ImGui window:

```cpp
DebugOverlay::GetInstance().SetEnabled(true);
```

### Displayed Information

| Section | Metrics |
|---------|---------|
| **Frame** | FPS, frame time (ms), min/max/avg |
| **CPU** | Per-category breakdown bar chart |
| **GPU** | Per-pass timing (shadow, geometry, post-process) |
| **Memory** | Total allocated, per-category breakdown |
| **Draw Calls** | Total draws, triangles, state changes |
| **Physics** | Active bodies, collision pairs, simulation time |

---

## Console Commands

All profiling tools are accessible via the debug console:

| Command | Description |
|---------|-------------|
| `profiler.overlay on/off` | Toggle the profiler overlay |
| `profiler.gpu on/off` | Toggle GPU timing queries |
| `profiler.capture start` | Begin Chrome Tracing capture |
| `profiler.capture stop` | End capture and save to file |
| `profiler.memory` | Print memory category summary |
| `profiler.leaks` | Print leak report |
| `profiler.hotspots [N]` | Show top N allocation hotspots |
| `profiler.reset` | Clear accumulated statistics |

---

## Common Bottleneck Patterns

### High Frame Time (> 16.6 ms for 60 FPS)

1. Check the **CPU category breakdown** — which system dominates?
2. If `Render` is high: check draw call count, reduce overdraw, enable frustum culling
3. If `Physics` is high: reduce active body count, increase fixed timestep, simplify collision shapes
4. If `GameLogic` is high: profile individual ECS systems, check for O(n^2) algorithms

### GPU Bound

1. Enable GPU profiling: `profiler.gpu on`
2. Check which pass is slowest (shadow, geometry, post-process)
3. Reduce resolution, disable expensive effects (SSAO, bloom), lower shadow quality

### Memory Growth

1. Run `profiler.memory` periodically to track category growth
2. Check `profiler.hotspots 20` for excessive allocation sites
3. Consider object pooling for frequently allocated types
4. Run `profiler.leaks` at shutdown to catch leaks

### Frame Spikes

1. Start a Chrome Tracing capture around the spike
2. Look for single-frame anomalies: GC pauses, asset loads, physics explosions
3. Use async loading for assets, spread heavy work across frames

---

## Integration with External Tools

### RenderDoc

SparkEngine's D3D11 renderer is compatible with [RenderDoc](https://renderdoc.org/) for GPU debugging:

1. Launch the engine through RenderDoc
2. Press F12 (default) to capture a frame
3. Inspect draw calls, shader state, and GPU resources

### Visual Studio Profiler

For detailed CPU profiling on Windows:

1. Open the solution in Visual Studio
2. Debug > Performance Profiler > CPU Usage
3. Run the scenario and analyze the call tree

### Valgrind (Linux)

For memory analysis on Linux:

```bash
valgrind --leak-check=full --track-origins=yes ./bin/SparkEngine
```

---

## Best Practices

1. **Profile before optimizing.** Measure first, then fix the actual bottleneck.
2. **Use Release builds for profiling.** Debug builds have different performance characteristics.
3. **Profile representative scenarios.** Test with real game content, not empty scenes.
4. **Track frame time, not FPS.** Frame time is linear and easier to reason about.
5. **Watch for spikes, not just averages.** A 1% spike can cause visible stuttering.
6. **Use Chrome Tracing for complex issues.** The timeline view reveals ordering and overlap.

See [Profiler and Debugging](Profiler-and-Debugging) for the full API reference.
