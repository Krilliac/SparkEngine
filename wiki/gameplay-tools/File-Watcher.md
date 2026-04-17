# File Watcher

File and directory change notification system using timestamp polling, with callbacks for asset hot-reload, script hot-reload, and configuration reloading.

**Source:** `SparkEngine/Source/Utils/FileWatcher/FileWatcher.h`

## Overview

The File Watcher monitors files and directories for changes by polling filesystem timestamps at a configurable interval. When a file is created, modified, or deleted, the system fires a registered callback with a `FileChangeEvent` containing the file path, change type, and new size. This drives the engine's hot-reload pipeline for scripts, assets, data tables, and configuration files.

Directory watches scan the file tree on initialization to establish a baseline, then compare timestamps on each poll cycle. New files trigger `Created` events, changed timestamps trigger `Modified` events, and missing files trigger `Deleted` events. Watches can be filtered by file extension (e.g., only `.csv` or `.as` files) and optionally limited to non-recursive scanning. Individual file watches track a single path with the same create/modify/delete detection.

The system is designed to be called once per frame via `Update(deltaTime)`. The poll interval prevents excessive filesystem access -- typically set to 0.5-2.0 seconds depending on the use case. Each watch returns an ID that can be used to stop watching later.

## Key Classes

| Class / Struct | Description |
|---|---|
| `FileWatcher` | Singleton that manages watches and polls the filesystem each frame |
| `FileChangeEvent` | Event payload: full path, relative path, change type, new file size |
| `FileChangeType` | Enum: `Created`, `Modified`, `Deleted` |
| `WatchEntry` | Internal watch record: path, directory flag, extension filter, tracked files, callback |
| `TrackedFile` | Internal per-file state: path, last write time, size, existence flag |

## Usage

```cpp
auto& watcher = Spark::Utils::FileWatcher::GetInstance();
watcher.Initialize(1.0f);  // poll every 1 second

// Watch a directory for script changes (hot-reload)
auto scriptWatch = watcher.Watch("Assets/Scripts/", [](const Spark::Utils::FileChangeEvent& e) {
    if (e.type == Spark::Utils::FileChangeType::Modified)
        ReloadScript(e.path);
    else if (e.type == Spark::Utils::FileChangeType::Created)
        LoadNewScript(e.path);
}, ".as");  // only AngelScript files

// Watch a directory for texture changes
auto textureWatch = watcher.Watch("Assets/Textures/", [](const Spark::Utils::FileChangeEvent& e) {
    if (e.type != Spark::Utils::FileChangeType::Deleted)
        ReloadTexture(e.path);
}, ".png", /*recursive=*/true);

// Watch a single config file
auto configWatch = watcher.WatchFile("config.ini", [](const Spark::Utils::FileChangeEvent& e) {
    if (e.type == Spark::Utils::FileChangeType::Modified)
        ReloadConfig();
});

// In the main loop
void MainLoop(float deltaTime)
{
    watcher.Update(deltaTime);
    // ...
}

// Stop watching when no longer needed
watcher.Unwatch(scriptWatch);
```

## API Reference

### Lifecycle

| Method | Return | Description |
|---|---|---|
| `Initialize(pollIntervalSeconds)` | `void` | Initialize with the given poll interval (default 1.0s) |
| `Shutdown()` | `void` | Remove all watches and shut down |

### Watch Management

| Method | Return | Description |
|---|---|---|
| `Watch(dirPath, callback, extFilter, recursive)` | `uint32_t` | Watch a directory; returns watch ID |
| `WatchFile(filePath, callback)` | `uint32_t` | Watch a single file; returns watch ID |
| `Unwatch(watchId)` | `void` | Stop watching a path |

### Update and Query

| Method | Return | Description |
|---|---|---|
| `Update(deltaTime)` | `void` | Poll for changes; call once per frame |
| `GetWatchCount()` | `size_t` | Number of active watches |
| `GetTrackedFileCount()` | `size_t` | Total tracked files across all watches |
| `SetPollInterval(seconds)` | `void` | Change the poll interval (minimum 0.1s) |

### FileChangeEvent Fields

| Field | Type | Description |
|---|---|---|
| `path` | `string` | Full filesystem path to the changed file |
| `relativePath` | `string` | Path relative to the watched directory |
| `type` | `FileChangeType` | `Created`, `Modified`, or `Deleted` |
| `newSize` | `uint64_t` | New file size in bytes (0 if deleted) |

## Related Systems

- [AngelScript Scripting](../subsystems/Scripting-with-AngelScript.md) -- script hot-reload is triggered by FileWatcher
- [Asset Dependency Graph](Asset-Dependency-Graph.md) -- graph updates when assets change on disk
- [Asset Browser](SparkEditor.md) -- refreshes file listings on directory change events
- [Editor Automation](Editor-Automation.md) -- automation scripts can register file watches
