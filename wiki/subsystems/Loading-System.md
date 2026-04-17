# Loading System

SparkEngine provides a loading screen framework with progress tracking, loading tips, and task-based asset loading. It works with the asset pipeline to display progress during level transitions.

**Source:** `SparkEngine/Source/Engine/Loading/LoadingScreen.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `LoadingScreen` | Manages loading tasks, progress, tips, and callbacks |
| `LoadingTask` | A single weighted task with an execute function |

## Loading States

```cpp
enum class LoadingState {
    Idle,      // No loading in progress
    Loading,   // Currently loading
    Completed, // All tasks finished
    Failed,    // One or more tasks failed
    Cancelled  // Loading was cancelled
};
```

## Quick Start

```cpp
LoadingScreen loader;
loader.SetBackgroundImage("Data/Textures/loading_bg.png");
loader.SetMinimumDisplayTime(1.5f);  // Prevent flash for fast loads
loader.AddLoadingTip("Press SPACE to jump");
loader.AddLoadingTip("Use cover to avoid enemy fire");

loader.BeginLoading("Level 1");
loader.AddTask("meshes",   0.4f, [](){ return LoadAllMeshes(); });
loader.AddTask("textures", 0.3f, [](){ return LoadAllTextures(); });
loader.AddTask("audio",    0.2f, [](){ return LoadAllAudio(); });
loader.AddTask("scripts",  0.1f, [](){ return CompileScripts(); });

loader.OnProgress([](float progress, const std::string& taskName) {
    UpdateProgressBar(progress);
    UpdateStatusText(taskName);
});

loader.OnComplete([](bool success) {
    if (success) StartLevel();
    else ShowErrorScreen();
});

loader.Execute();  // Runs all tasks, fires callbacks
```

## Task Weights

Task weights are relative. The progress percentage is calculated from completed weight divided by total weight:

```cpp
loader.AddTask("large_task", 0.7f, executeFunc);  // 70% of progress
loader.AddTask("small_task", 0.3f, executeFunc);   // 30% of progress
```

## Cancellation

```cpp
loader.Cancel();
// State becomes LoadingState::Cancelled
```

## API Reference

| Method | Description |
|--------|-------------|
| `BeginLoading(name)` | Start a new loading session |
| `AddTask(name, weight, func)` | Add a weighted loading task |
| `Execute()` | Run all tasks synchronously |
| `Cancel()` | Cancel current loading |
| `GetProgress()` | Get overall progress (0.0 - 1.0) |
| `GetState()` | Get current loading state |
| `GetCurrentTip()` | Get a random loading tip |
| `SetMinimumDisplayTime(sec)` | Set minimum display time |

## Async Loading with Background Threads

By default, `Execute()` runs tasks synchronously on the calling thread. For non-blocking loading, use `ExecuteAsync()` which dispatches tasks to a background thread pool while the main thread continues rendering the loading screen.

```cpp
loader.ExecuteAsync();  // Returns immediately, tasks run on background threads

// Main thread rendering loop
while (loader.GetState() == LoadingState::Loading)
{
    float dt = GetDeltaTime();
    loader.PollProgress();           // Sync progress from worker threads
    RenderLoadingScreen(loader);     // Render on main thread
    PresentFrame();
}
```

Internally, `ExecuteAsync()` uses a dedicated `std::thread` pool (default 2 workers, configurable via `SetWorkerThreadCount()`). Each `LoadingTask` is dispatched to the next available worker. Tasks that depend on GPU resources (e.g., texture upload) are automatically deferred to the main thread via a finalization queue that is drained during `PollProgress()`.

```cpp
loader.SetWorkerThreadCount(4);  // Use 4 background threads for loading
```

### Thread Safety During Async Loading

- Worker threads may only call thread-safe asset loading functions (file I/O, decompression, mesh parsing).
- GPU resource creation (texture uploads, buffer creation) must happen on the main thread. The loading system handles this automatically by queuing GPU work during `PollProgress()`.
- Progress callbacks are always invoked on the main thread during `PollProgress()`, never from a worker thread.

## Loading Screen Rendering Loop

The loading screen rendering loop runs on the main thread while background tasks execute. The `LoadingScreen` class provides built-in rendering support through the `RenderLoadingScreen()` helper, or you can query state and render manually.

```cpp
void GameLoadingLoop(LoadingScreen& loader)
{
    loader.ExecuteAsync();

    while (loader.GetState() == LoadingState::Loading)
    {
        float dt = GetDeltaTime();
        loader.PollProgress();

        // Begin frame
        auto& gfx = EngineContext::Get().GetGraphics();
        gfx.BeginFrame();
        gfx.ClearRenderTarget({0.0f, 0.0f, 0.0f, 1.0f});

        // Draw background image (stretched to viewport)
        loader.DrawBackground(gfx);

        // Draw progress bar
        float smoothProgress = loader.GetSmoothedProgress();
        DrawProgressBar(gfx, smoothProgress, {100, 650, 1080, 30});

        // Draw current task name
        DrawText(gfx, loader.GetCurrentTaskName(), {100, 690});

        // Draw rotating tips (auto-cycles every 5 seconds)
        DrawText(gfx, loader.GetCurrentTip(), {100, 720});

        // End frame
        gfx.EndFrame();
        gfx.Present();
    }
}
```

The loading screen maintains its own lightweight render path that does not depend on the full scene rendering pipeline. This ensures the loading screen can display even when the scene graph is being torn down and rebuilt.

## Progress Interpolation for Smooth Bars

Raw progress values jump in discrete steps as tasks complete, which creates a jarring visual experience. The loading system provides built-in progress smoothing via exponential interpolation.

```cpp
// GetSmoothedProgress() returns an interpolated value that
// smoothly catches up to the actual progress
float smoothed = loader.GetSmoothedProgress();

// Configure interpolation speed (default: 5.0)
loader.SetProgressSmoothingSpeed(8.0f);  // Faster catch-up

// Configure minimum progress rate (prevents stalling visually)
loader.SetMinimumProgressRate(0.01f);  // Always advance at least 1% per second
```

The smoothing algorithm works as follows:

1. Each frame, the displayed progress lerps toward the actual progress: `displayed += (actual - displayed) * speed * deltaTime`.
2. A minimum rate ensures the bar never appears stuck, even when a large task is processing.
3. When actual progress reaches 1.0, the displayed progress accelerates to catch up quickly, preventing a long tail at the end of loading.
4. The final jump from displayed to 1.0 is clamped to complete within 0.3 seconds maximum.

## Asset Priority Loading

Tasks can be assigned priority levels that control execution order within the async loading pipeline. Higher-priority tasks are dispatched first, which is useful for loading assets needed for the initial camera view before loading distant or off-screen assets.

```cpp
// Priority levels (higher value = loaded first)
loader.AddTask("player_model",  0.1f, LoadPlayerModel,  /*priority=*/100);
loader.AddTask("weapon_model",  0.1f, LoadWeaponModel,  /*priority=*/90);
loader.AddTask("nearby_env",    0.3f, LoadNearbyEnv,    /*priority=*/50);
loader.AddTask("distant_env",   0.3f, LoadDistantEnv,   /*priority=*/10);
loader.AddTask("ambient_audio", 0.2f, LoadAmbientAudio, /*priority=*/5);
```

Priority loading allows the engine to begin rendering a partial scene early. Combined with streaming, the player can start interacting with the level while low-priority assets finish loading in the background.

## Dependency Graphs Between Loading Tasks

Some assets depend on others (e.g., materials depend on textures, prefabs depend on meshes). The loading system supports explicit task dependencies to ensure correct ordering.

```cpp
auto texTask = loader.AddTask("textures", 0.3f, LoadTextures);
auto matTask = loader.AddTask("materials", 0.2f, LoadMaterials);
auto meshTask = loader.AddTask("meshes", 0.3f, LoadMeshes);
auto prefabTask = loader.AddTask("prefabs", 0.2f, LoadPrefabs);

// Materials depend on textures being loaded first
loader.AddDependency(matTask, texTask);

// Prefabs depend on both meshes and materials
loader.AddDependency(prefabTask, meshTask);
loader.AddDependency(prefabTask, matTask);
```

The dependency graph is validated at `Execute()` / `ExecuteAsync()` time. Circular dependencies are detected and cause a `LoadingState::Failed` transition with a descriptive error message. Independent branches of the dependency graph are executed in parallel when using async loading.

## Error Recovery and Retry Logic

When a loading task fails, the system supports configurable retry behavior before marking the entire loading session as failed.

```cpp
// Global retry settings
loader.SetMaxRetries(3);          // Retry failed tasks up to 3 times
loader.SetRetryDelay(0.5f);       // Wait 0.5 seconds between retries

// Per-task retry override
loader.AddTask("critical_data", 0.4f, LoadCriticalData, /*priority=*/100, /*maxRetries=*/5);

// Error callback (fires for each failure, even if retries remain)
loader.OnTaskFailed([](const std::string& taskName, int attempt, const std::string& error) {
    LogWarning("Task '{}' failed on attempt {}: {}", taskName, attempt, error);
});

// Final failure callback (fires only when retries are exhausted)
loader.OnComplete([](bool success) {
    if (!success)
    {
        auto errors = loader.GetFailedTasks();
        ShowErrorDialog(errors);
    }
});
```

For non-critical assets (e.g., cosmetic decals), tasks can be marked as optional so that their failure does not prevent the loading session from completing successfully:

```cpp
loader.AddTask("optional_decals", 0.05f, LoadDecals, /*priority=*/1, /*maxRetries=*/1, /*optional=*/true);
```

## Integration with Scene Transitions

The loading system integrates tightly with SparkEngine's scene management to provide seamless level transitions.

```cpp
void TransitionToLevel(const std::string& levelName)
{
    auto& sceneMgr = EngineContext::Get().GetSceneManager();

    // 1. Fade out current scene
    sceneMgr.BeginTransition(TransitionType::FadeToBlack, 0.5f);

    // 2. Set up loading screen
    LoadingScreen loader;
    loader.SetBackgroundImage(sceneMgr.GetLevelLoadingImage(levelName));
    loader.BeginLoading(levelName);

    // 3. Unload current scene (as a loading task)
    loader.AddTask("unload", 0.1f, [&]() { return sceneMgr.UnloadCurrentScene(); });

    // 4. Load new scene assets
    auto tasks = sceneMgr.CreateLoadingTasks(levelName);
    for (auto& t : tasks)
        loader.AddTask(t.name, t.weight, t.func);

    // 5. Initialize new scene
    loader.AddTask("init_scene", 0.1f, [&]() { return sceneMgr.InitializeScene(levelName); });

    // 6. Execute and transition
    loader.OnComplete([&](bool success) {
        if (success)
            sceneMgr.EndTransition(TransitionType::FadeFromBlack, 0.5f);
        else
            sceneMgr.ReturnToMainMenu();
    });

    loader.ExecuteAsync();
}
```

## Loading Screen Customization

The loading screen supports extensive visual customization including background images, animated elements, and rotating gameplay tips.

### Background Images

```cpp
// Static background
loader.SetBackgroundImage("Data/Textures/loading_bg.png");

// Per-level background (set from level metadata)
loader.SetBackgroundImage(levelData.loadingScreenImage);

// Animated background (cycles through images)
loader.SetBackgroundSlideshow({
    "Data/Textures/loading_01.png",
    "Data/Textures/loading_02.png",
    "Data/Textures/loading_03.png"
}, /*intervalSeconds=*/4.0f);
```

### Loading Tips Rotation

```cpp
// Add tips from a file
loader.LoadTipsFromFile("Data/Config/loading_tips.json");

// Configure tip rotation
loader.SetTipRotationInterval(5.0f);   // Change tip every 5 seconds
loader.SetTipFadeDuration(0.3f);       // Fade transition between tips

// Context-sensitive tips (filter by level or game mode)
loader.SetTipFilter("deathmatch");     // Only show deathmatch tips
```

### Animated Spinner and Progress Bar Styling

```cpp
// Custom progress bar appearance
loader.SetProgressBarColor({0.2f, 0.8f, 0.3f, 1.0f});    // Green bar
loader.SetProgressBarBackColor({0.1f, 0.1f, 0.1f, 0.8f}); // Dark background
loader.SetProgressBarPosition({100, 650, 1080, 30});        // x, y, width, height

// Animated loading spinner
loader.SetSpinnerTexture("Data/Textures/spinner.png");
loader.SetSpinnerSpeed(360.0f);  // Degrees per second
```

## Memory Budget During Loading

Loading large levels can cause memory spikes. The loading system supports memory budgeting to control peak memory usage during loading.

```cpp
// Set a memory budget for loading (in bytes)
loader.SetMemoryBudget(512 * 1024 * 1024);  // 512 MB budget

// Query current loading memory usage
size_t used = loader.GetLoadingMemoryUsage();
size_t budget = loader.GetMemoryBudget();

// When the budget is exceeded, the loader pauses task dispatch
// until completed tasks free enough memory. This prevents OOM
// situations on memory-constrained platforms.
```

The memory budget system works by tracking allocations made during loading tasks. When the budget threshold is reached (default 90%), new task dispatch is paused until in-flight tasks complete and release their temporary buffers. This is particularly important on consoles and mobile platforms where total available memory is limited.

## Streaming vs Batch Loading

SparkEngine supports two loading strategies that can be mixed within a single loading session.

### Batch Loading

Batch loading is the default mode. All tasks are queued and executed before the level begins. This is appropriate for competitive multiplayer where all assets must be ready before gameplay starts.

```cpp
loader.SetLoadingStrategy(LoadingStrategy::Batch);
// All tasks must complete before OnComplete fires
```

### Streaming Loading

Streaming mode allows gameplay to begin as soon as critical assets are loaded. Non-critical assets stream in during gameplay with minimal frame time impact.

```cpp
loader.SetLoadingStrategy(LoadingStrategy::Streaming);
loader.SetStreamingBudgetMs(2.0f);  // Max 2ms per frame for streaming work

// Mark tasks as critical (must complete before gameplay) or streamable
loader.AddTask("player", 0.2f, LoadPlayer, /*priority=*/100, /*critical=*/true);
loader.AddTask("terrain", 0.3f, LoadTerrain, /*priority=*/50, /*critical=*/true);
loader.AddTask("foliage", 0.2f, LoadFoliage, /*priority=*/10, /*critical=*/false);  // Streams in
loader.AddTask("decals",  0.1f, LoadDecals,  /*priority=*/5,  /*critical=*/false);  // Streams in
```

## Platform-Specific Loading Optimizations

The loading system adapts its behavior based on the target platform.

| Platform | Optimization |
|----------|-------------|
| **Windows (NVMe/SSD)** | 4 worker threads, large read buffers (4 MB), parallel decompression |
| **Windows (HDD)** | 2 worker threads, sequential reads to minimize seek time, smaller buffers |
| **Linux** | Uses `io_uring` for async file I/O when available, falls back to thread pool |
| **Console (future)** | Plans for platform-specific async I/O APIs (e.g., DirectStorage) |

```cpp
// Auto-detect storage type and configure accordingly
loader.AutoConfigureForPlatform();

// Or configure manually
loader.SetReadBufferSize(4 * 1024 * 1024);  // 4 MB read buffer
loader.SetUseAsyncIO(true);                   // Enable async file I/O
```

The loading system also supports I/O coalescing, where multiple small file reads are batched into a single large read when the files are physically adjacent on disk. This reduces the number of I/O operations and is especially beneficial for HDD-based systems.

## Console Commands

```
loading_status          # Show loading state and progress
loading_memory          # Show memory usage during loading
loading_tasks           # List all tasks with status and timing
loading_cancel          # Cancel current loading session
loading_set_threads N   # Set worker thread count
loading_strategy batch  # Switch to batch loading
loading_strategy stream # Switch to streaming loading
```

---

## See Also

- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) — Asset loading functions used in tasks
- [UI System](UI-System.md) — Rendering loading screen UI
- [Scene Management](Scene-Management.md) — Level transitions triggering loading
