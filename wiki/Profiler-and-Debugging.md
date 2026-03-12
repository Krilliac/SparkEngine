# Profiler and Debugging

SparkEngine provides a suite of profiling and debugging tools for analyzing CPU and GPU performance, tracking memory usage, visualizing debug geometry, inspecting frame state, and generating trace exports for external analysis.

**Source:** `SparkEngine/Source/Utils/Profiler.h`, `CrashHandler.h`, `MemoryDebugger.h`, `DebugDraw.h`, `DebugOverlay.h`, `ChromeTracing.h`, `FrameInspector.h`

---

## Profiler

The `Profiler` class is a singleton providing per-system CPU timing, GPU timing queries (Windows/D3D11), memory allocation tracking, and frame timing history. It renders as an ImGui overlay when enabled.

### Profile Categories

The `ProfileCategory` enum classifies profiling samples:

| Category | Description |
|---|---|
| `Frame` | Overall frame timing |
| `Render` | Rendering / draw calls |
| `Physics` | Physics simulation |
| `Audio` | Audio processing |
| `GameLogic` | Gameplay / ECS logic |
| `Input` | Input polling and dispatch |
| `Particles` | Particle system updates |
| `UI` | UI layout and rendering |
| `Custom` | User-defined sections |

### CPU Timing

Record the wall-clock duration of any code section:

```cpp
auto& profiler = Profiler::GetInstance();

// Manual begin/end
profiler.BeginSection("PhysicsStep", ProfileCategory::Physics);
world.StepSimulation(dt);
profiler.EndSection("PhysicsStep");

// RAII scoped timer (automatic begin/end via destructor)
{
    ScopedProfileTimer timer("RenderPass", ProfileCategory::Render);
    renderer.DrawScene();
}
```

Frame boundaries must be marked each frame:

```cpp
profiler.BeginFrame();
// ... entire frame work ...
profiler.EndFrame();
```

### GPU Timing (Windows / D3D11)

On Windows builds, the profiler can issue D3D11 timestamp queries to measure GPU-side durations. This requires initialization with the D3D11 device and context:

```cpp
profiler.Initialize(device, context);  // Returns HRESULT

// During rendering
profiler.BeginGPUTimer("ShadowPass");
RenderShadows();
profiler.EndGPUTimer("ShadowPass");

// At end of frame, resolve pending queries
profiler.ResolveGPUQueries();

// Query result
double shadowMs = profiler.GetGPUTimeMs("ShadowPass");
```

The `GPUTimerQuery` struct wraps three `ID3D11Query` COM objects (begin timestamp, end timestamp, disjoint query) managed via `ComPtr`. A `pending` flag tracks whether the query has been issued but not yet resolved. GPU timing is compiled out on non-Windows platforms.

### Memory Tracking

Lightweight per-category memory counters suitable for Release builds:

```cpp
profiler.RecordAllocation("Textures", textureSize);
// ... later ...
profiler.RecordDeallocation("Textures", textureSize);
```

Each category tracks:
- `currentBytes` -- currently allocated bytes
- `peakBytes` -- high-water mark
- `totalAllocations` -- lifetime allocation count

### Frame Timing History

The `FrameTimingHistory` struct maintains a rolling buffer of 300 frame-time samples (approximately 5 seconds at 60 FPS). Each call to `Push(float timeMs)` writes into the circular buffer and recomputes `minTime`, `maxTime`, and `avgTime` over all non-zero entries. The profiler automatically pushes a sample every frame via `EndFrame()`.

### ProfileSample Struct

Each timed section produces a `ProfileSample`:

| Field | Type | Description |
|---|---|---|
| `name` | `std::string` | Section name |
| `category` | `ProfileCategory` | Classification |
| `startTimeMs` | `double` | Start time relative to frame |
| `durationMs` | `double` | Measured duration |
| `depth` | `int` | Nesting depth for hierarchical display |

### Query Results

```cpp
float frameMs   = profiler.GetFrameTimeMs();       // Average frame time
float fps        = profiler.GetFPS();               // Derived from average frame time
double sectionMs = profiler.GetSectionTimeMs("AI"); // Last recorded duration for named section
double catMs     = profiler.GetCategoryTimeMs(ProfileCategory::Render); // Per-category total
const FrameTimingHistory& history = profiler.GetFrameHistory();
```

### Display Control

```cpp
profiler.SetEnabled(true);          // Enable profiling data collection
profiler.Toggle();                  // Toggle enabled state
profiler.SetOverlayVisible(true);   // Show the ImGui profiler overlay
profiler.ToggleOverlay();           // Toggle overlay visibility
```

### Console Integration

The profiler exposes formatted report methods for the in-game console:

```cpp
std::string cpuReport = profiler.Console_GetReport();
std::string gpuReport = profiler.Console_GetGPUReport();
std::string memReport = profiler.Console_GetMemoryReport();
profiler.Console_ExportCSV("profiling_data.csv");
```

### Convenience Macros

When `PROFILING_ENABLED` is defined, the following macros are available. They compile to no-ops otherwise:

| Macro | Expands to |
|---|---|
| `PROFILE_SCOPE(name)` | `ScopedProfileTimer` with `Custom` category |
| `PROFILE_SCOPE_CAT(name, cat)` | `ScopedProfileTimer` with explicit category |
| `PROFILE_BEGIN(name)` | `Profiler::GetInstance().BeginSection(name)` |
| `PROFILE_END(name)` | `Profiler::GetInstance().EndSection(name)` |
| `PROFILE_GPU_BEGIN(name)` | `Profiler::GetInstance().BeginGPUTimer(name)` (Windows only) |
| `PROFILE_GPU_END(name)` | `Profiler::GetInstance().EndGPUTimer(name)` (Windows only) |

Usage:

```cpp
void UpdateAI(float dt)
{
    PROFILE_SCOPE_CAT("AI::Update", ProfileCategory::GameLogic);
    // AI logic...
}
```

---

## CrashHandler

The `CrashHandler` system installs platform-specific unhandled-exception handlers to generate crash reports with minidumps, screenshots, system information, and multi-thread call stacks.

### Configuration

```cpp
CrashConfig cfg;
cfg.dumpPrefix          = L"SparkEngine";                        // Minidump filename prefix
cfg.uploadURL           = "https://crashes.example.com/upload";  // Remote upload URL (empty = disabled)
cfg.captureScreenshot   = true;   // Capture last rendered frame
cfg.captureSystemInfo   = true;   // Collect OS, GPU, memory info
cfg.captureAllThreads   = true;   // Dump all thread call stacks
cfg.zipBeforeUpload     = true;   // Compress report before sending
cfg.triggerCrashOnAssert = false; // Whether Assert::Fail generates a full crash report
cfg.connectTimeoutSeconds = 5;    // HTTP timeout for uploads

// Optional: create a GitHub Issue with the crash report
cfg.githubRepo   = "owner/repo";
cfg.githubToken  = "ghp_...";
cfg.githubLabels = "crash-report";
cfg.githubAttachDump = true;

InstallCrashHandler(cfg);
```

### API

| Function | Description |
|---|---|
| `InstallCrashHandler(const CrashConfig&)` | Register the SEH filter (Windows) or signal handlers (Linux) |
| `TriggerCrashHandler(const char* msg)` | Called by `Assert::Fail`; generates a report if `triggerCrashOnAssert` is true |
| `SetAssertCrashBehavior(bool)` | Toggle assert-crash behavior at runtime without reinstalling |

Install the crash handler early in application startup, before graphics or audio initialization.

---

## MemoryDebugger

`Spark::MemoryDebugger` is a debug-build allocation tracker that records every allocation with call-site information and reports leaks at shutdown. It is thread-safe and intended for Debug builds only (the Profiler's lightweight counters serve Release builds).

### Features

- Per-allocation tracking with file, line, and function
- Leak detection and formatted report
- Per-category statistics with high-water marks
- Double-free and use-after-free detection
- Allocation hot-spot analysis (most frequently allocating call sites)

### Usage

```cpp
auto& md = Spark::MemoryDebugger::GetInstance();
md.SetEnabled(true);

// Track allocations (prefer the macros below)
void* ptr = malloc(1024);
md.RecordAlloc(ptr, 1024, "Textures", __FILE__, __LINE__, __FUNCTION__);

// Track deallocations -- returns false on double-free
bool ok = md.RecordFree(ptr);
free(ptr);

// At shutdown
md.PrintLeakReport();   // Formatted report to stderr
md.PrintSummary();      // Usage statistics summary
```

### Convenience Macros

These macros are active in Debug builds (`_DEBUG` or `DEBUG` defined) and compile to no-ops in Release:

```cpp
SPARK_TRACK_ALLOC(ptr, size, "Audio");   // RecordAlloc with __FILE__, __LINE__, __FUNCTION__
SPARK_TRACK_FREE(ptr);                   // RecordFree
SPARK_PRINT_LEAK_REPORT();               // PrintLeakReport
SPARK_PRINT_MEMORY_SUMMARY();            // PrintSummary
```

### Analysis API

```cpp
// Per-category statistics (sorted by currentBytes descending)
std::vector<Spark::MemoryCategoryStats> stats = md.GetCategoryStats();

// Top 20 allocation hot spots (sorted by count descending)
std::vector<Spark::AllocationHotSpot> spots = md.GetHotSpots(20);

// Leak report as a vector of LeakEntry
std::vector<Spark::LeakEntry> leaks = md.GetLeaks();

// Scalar queries
size_t current  = md.GetCurrentBytes();
size_t peak     = md.GetPeakBytes();
uint64_t allocs = md.GetTotalAllocationCount();
size_t active   = md.GetActiveAllocationCount();
uint64_t dblFrees = md.GetDoubleFreeCount();

// Reset all tracking data
md.Reset();
```

---

## DebugDraw

`Spark::DebugDrawManager` provides immediate-mode 3D debug shape rendering. Draw calls are batched per-frame and rendered as an overlay after the main scene pass. Submission is thread-safe.

### Supported Shapes

| Shape | Method | Key Parameters |
|---|---|---|
| Line | `DrawLine` | start, end, color, lifetime, thickness |
| Ray | `DrawRay` | origin, direction, length, color |
| Arrow | `DrawArrow` | start, end, headSize, color |
| Sphere | `DrawSphere` | center, radius, color, segments |
| AABB | `DrawAABB` | min, max, color |
| Capsule | `DrawCapsule` | base, radius, height, color |
| Cross | `DrawCross` | position, size, color |
| Cone | `DrawCone` | apex, direction, height, angle, color |
| Text3D | `DrawText3D` | position, text, color |
| Grid | `DrawGrid` | center, cellSize, cellCount, color |
| Axes | `DrawAxes` | position, length (draws R=X, G=Y, B=Z arrows) |

### Lifetime and Depth

- `lifetime = 0.0f` (default) -- shape is drawn for a single frame only.
- `lifetime > 0.0f` -- shape persists for the specified duration in seconds.
- `depthTested = true` (default) -- shape is occluded by scene geometry.
- `depthTested = false` -- shape renders on top of everything.

### Frame Management

Call `Flush(float deltaTime)` once per frame to advance timers and discard expired shapes:

```cpp
auto& dd = Spark::DebugDrawManager::GetInstance();
dd.Flush(deltaTime);

// Access batched requests for rendering
const auto& requests = dd.GetRequests();
```

### Usage Example

```cpp
// Visualize a raycast hit
DEBUG_DRAW_RAY(origin, direction, 100.0f, Spark::DebugColor::Red());
DEBUG_DRAW_SPHERE_T(hitPoint, 0.3f, Spark::DebugColor::Yellow(), 2.0f);

// Visualize a collision volume
DEBUG_DRAW_AABB(entity.GetAABBMin(), entity.GetAABBMax(), Spark::DebugColor::Green());

// Visualize an AI sight cone
DEBUG_DRAW_CONE(eyePos, lookDir, sightRange, fovAngle, Spark::DebugColor::Orange());
```

### Macros

Debug draw macros are active in Debug builds (or when `SPARK_DEBUG_DRAW_ALWAYS` is defined):

`DEBUG_DRAW_LINE`, `DEBUG_DRAW_LINE_T`, `DEBUG_DRAW_RAY`, `DEBUG_DRAW_RAY_T`, `DEBUG_DRAW_ARROW`, `DEBUG_DRAW_SPHERE`, `DEBUG_DRAW_SPHERE_T`, `DEBUG_DRAW_AABB`, `DEBUG_DRAW_AABB_T`, `DEBUG_DRAW_CAPSULE`, `DEBUG_DRAW_CROSS`, `DEBUG_DRAW_CONE`, `DEBUG_DRAW_TEXT3D`, `DEBUG_DRAW_GRID`, `DEBUG_DRAW_AXES`

The `_T` suffix variants accept an additional `lifetime` parameter for persistent shapes.

---

## DebugOverlay

`Spark::DebugOverlay` is a lightweight in-game HUD that displays key engine metrics without requiring the full editor or profiler UI. It renders as a transparent overlay on the game viewport.

### Overlay Sections

Each section can be individually toggled via `SetSection(OverlaySection, bool)`:

| Section | Content |
|---|---|
| `FPS` | FPS counter with color coding (green >= 60, yellow >= 30, red < 30) |
| `Timing` | Per-system timing breakdown (color-coded: red > 8ms, yellow > 4ms) |
| `Memory` | RAM and VRAM usage in MB |
| `Rendering` | Draw calls and triangle count |
| `Entities` | Total and visible entity counts |
| `Physics` | Active rigid body count |
| `Audio` | Active audio channel count |
| `Input` | Input state |
| `Camera` | Camera position and rotation |
| `Custom` | User-defined debug lines |

### Configuration

```cpp
auto& overlay = Spark::DebugOverlay::GetInstance();
overlay.SetEnabled(true);
overlay.SetPosition(Spark::OverlayPosition::TopRight);
overlay.SetOpacity(0.7f);
overlay.SetCompactMode(false);    // false = expanded, true = FPS only
overlay.SetSection(Spark::OverlaySection::Memory, true);
overlay.SetSection(Spark::OverlaySection::Camera, false);
```

### Feeding Data

Subsystems push their metrics each frame:

```cpp
overlay.SetDrawCalls(renderer.GetDrawCallCount());
overlay.SetTriangleCount(renderer.GetTriCount());
overlay.SetEntityCount(ecs.GetEntityCount());
overlay.SetMemoryUsageMB(memTracker.GetCurrentMB());
overlay.SetVRAMUsageMB(gpu.GetVRAMUsageMB());
overlay.SetSystemTime("Render", renderTimeMs);
overlay.SetSystemTime("Physics", physicsTimeMs);

// Custom debug text from any subsystem
overlay.AddCustomLine("Player HP", "100/100", {0, 1, 0, 1});
overlay.RemoveCustomLine("Player HP");
```

### Frame History

The overlay maintains 120 frame-time samples (~2 seconds at 60 FPS) in a `FrameTimeSample` array containing `frameTimeMs`, `cpuTimeMs`, and `gpuTimeMs` per sample. Access the history via `GetFrameHistory()` for rendering a mini-graph.

---

## ChromeTracing

`Spark::ChromeTracing` records profiling events and exports them in Chrome Trace Event Format (JSON). Output files can be loaded in `chrome://tracing` or [Perfetto](https://ui.perfetto.dev/).

### Recording

```cpp
auto& tracer = Spark::ChromeTracing::GetInstance();

// Start recording (clears previous events, pre-allocates 100K entries)
tracer.Start();

// Instrument code with scoped or manual events
{
    SPARK_TRACE_SCOPE("Render");
    // ... rendering ...
}

tracer.BeginEvent("Physics", "simulation");
StepPhysics(dt);
tracer.EndEvent("Physics", "simulation");

tracer.InstantEvent("PlayerDeath", "gameplay");

// Stop and save
tracer.Stop();
tracer.SaveToFile("trace.json");
```

### Event Types

| Method | Phase | Description |
|---|---|---|
| `BeginEvent(name, category)` | `B` | Begin a duration event |
| `EndEvent(name, category)` | `E` | End a duration event |
| `InstantEvent(name, category)` | `i` | Single-point event marker |

All events record a microsecond timestamp and the originating thread ID. The tracer is thread-safe.

### Macros

| Macro | Description |
|---|---|
| `SPARK_TRACE_SCOPE(name)` | RAII scoped duration event (default category) |
| `SPARK_TRACE_SCOPE_CAT(name, cat)` | RAII scoped duration event with explicit category |

### Utility

```cpp
size_t count = tracer.EventCount();  // Number of recorded events
tracer.Clear();                      // Discard all events without saving
```

---

## FrameInspector

`Spark::FrameInspector` provides frame-stepping, slow-motion, breakpoints, and state snapshot tools for debugging physics glitches, animation issues, and timing-dependent bugs.

### Frame States

| State | Behavior |
|---|---|
| `Running` | Normal execution at full speed |
| `Paused` | Game loop halted; no frames advance |
| `Stepping` | Advance N frames, then automatically pause |
| `SlowMotion` | Running at reduced time scale |

### Game Loop Integration

```cpp
auto& inspector = Spark::FrameInspector::GetInstance();

// In the main loop:
if (inspector.ShouldAdvance())
{
    float dt = inspector.GetEffectiveDeltaTime(rawDeltaTime);
    UpdateGame(dt);
    inspector.OnFrameEnd();
}
```

### Control

```cpp
inspector.Pause();             // Halt execution
inspector.Resume();            // Return to normal speed
inspector.StepFrame();         // Advance exactly 1 frame, then pause
inspector.StepFrames(10);      // Advance 10 frames, then pause
inspector.TogglePause();       // Toggle between paused and running
inspector.SetTimeScale(0.25f); // Quarter speed (enters SlowMotion state)
```

### Breakpoints

Pause execution at a specific frame number or when a condition is met:

```cpp
// Frame number breakpoint
inspector.AddBreakpointFrame(5000);
inspector.RemoveBreakpointFrame(5000);
inspector.ClearBreakpointFrames();

// Conditional breakpoint (checked every frame)
inspector.AddBreakCondition("LowFPS", []() {
    return Profiler::GetInstance().GetFPS() < 20.0f;
}, /*oneShot=*/true);

inspector.RemoveBreakCondition("LowFPS");
inspector.ClearBreakConditions();
```

### State Snapshots

Capture and compare engine state across frames:

```cpp
// Register state providers (once at startup)
inspector.RegisterStateProvider("PlayerPos", []() {
    auto pos = GetPlayerPosition();
    return std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z);
});

// Capture a snapshot each frame (up to 60 retained)
inspector.CaptureSnapshot(deltaTime, totalTime);

// Compare two snapshots
auto diffs = inspector.CompareSnapshots(0, 1);
for (const auto& diff : diffs)
    LOG_DEBUG("{}", diff);

// Access all snapshots
const auto& snapshots = inspector.GetSnapshots();
inspector.ClearSnapshots();
```

---

## Console Commands

The following console commands are available for runtime profiling and debugging:

| Command | Description |
|---|---|
| `profile_start` | Enable profiler data collection |
| `profile_stop` | Disable profiler data collection |
| `profile_report` | Print CPU timing report to console |
| `profile_gpu` | Print GPU timing report to console |
| `memory_info` | Print memory allocation report |
| `frame_time` | Print current frame timing statistics |

---

## Integration Patterns

### Typical Engine Loop Integration

```cpp
void EngineLoop()
{
    auto& profiler  = Profiler::GetInstance();
    auto& inspector = Spark::FrameInspector::GetInstance();
    auto& overlay   = Spark::DebugOverlay::GetInstance();
    auto& debugDraw = Spark::DebugDrawManager::GetInstance();

    while (running)
    {
        profiler.BeginFrame();

        if (inspector.ShouldAdvance())
        {
            float dt = inspector.GetEffectiveDeltaTime(rawDt);

            PROFILE_SCOPE_CAT("Physics", ProfileCategory::Physics);
            StepPhysics(dt);

            PROFILE_SCOPE_CAT("GameLogic", ProfileCategory::GameLogic);
            UpdateGameLogic(dt);

            PROFILE_SCOPE_CAT("Render", ProfileCategory::Render);
            RenderScene();

            inspector.OnFrameEnd();
        }

        debugDraw.Flush(rawDt);
        overlay.Update(rawDt);

        profiler.EndFrame();
#ifdef SPARK_PLATFORM_WINDOWS
        profiler.ResolveGPUQueries();
#endif
    }
}
```

### Profiler vs. MemoryDebugger

| Feature | Profiler | MemoryDebugger |
|---|---|---|
| **Purpose** | Lightweight per-category counters | Full allocation tracking with call sites |
| **Build target** | Release and Debug | Debug only |
| **Thread safety** | Not thread-safe | Thread-safe (mutex) |
| **Overhead** | Minimal | Significant |
| **Leak detection** | No | Yes |
| **Double-free detection** | No | Yes |
| **Hot-spot analysis** | No | Yes |

Use the Profiler's `RecordAllocation`/`RecordDeallocation` in production code for category-level memory budgets. Use `MemoryDebugger` during development to find leaks and allocation hot spots.

---

## Thread Safety

| System | Thread Safety |
|---|---|
| `Profiler` | Main thread only |
| `MemoryDebugger` | Thread-safe (mutex-protected) |
| `DebugDrawManager` | Thread-safe (mutex-protected submission) |
| `DebugOverlay` | Thread-safe for `SetSystemTime`, `AddCustomLine`, `BuildStatsLines` |
| `ChromeTracing` | Thread-safe (mutex-protected, records thread ID per event) |
| `FrameInspector` | Main thread only |
| `CrashHandler` | Called from exception context |

---

## See Also

- [Architecture Overview](Architecture-Overview) -- engine subsystem layout
- [Rendering and Graphics](Rendering-and-Graphics) -- D3D11 rendering pipeline and GPU resources
- [SparkConsole](SparkConsole) -- in-game console for running debug commands
- [SparkEditor](SparkEditor) -- ImGui editor with profiler integration
- [Testing](Testing) -- unit test framework and CTest integration
- [Troubleshooting](Troubleshooting) -- common issues and diagnostic steps
