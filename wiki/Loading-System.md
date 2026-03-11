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

## Console Commands

```
loading_status    # Show loading state and progress
```

---

## See Also

- [Asset Pipeline](Asset-Pipeline) — Asset loading functions used in tasks
- [UI System](UI-System) — Rendering loading screen UI
- [Scene Management](Scene-Management) — Level transitions triggering loading
