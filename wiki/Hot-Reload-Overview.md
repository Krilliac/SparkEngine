# Hot Reload Overview

SparkEngine supports hot-reloading across multiple subsystems: shaders, scripts, game modules, materials, and assets. This page provides a unified overview of all hot-reload capabilities, how they work, and how to use them during development.

**Relevant sources:**
- `SparkEngine/Source/Graphics/Shader.h` (shader hot-reload)
- `SparkEngine/Source/Engine/Scripting/ScriptHotReload.h` (script hot-reload)
- `SparkEngine/Source/Core/ModuleHotReload.h` (module hot-reload)
- `SparkEngine/Source/Graphics/MaterialSystem.h` (material hot-reload)
- `SparkEngine/Source/Graphics/AssetPipeline.h` (asset hot-reload)

---

## Table of Contents

- [Overview](#overview)
- [Shader Hot Reload](#shader-hot-reload)
- [Script Hot Reload](#script-hot-reload)
- [Module Hot Reload](#module-hot-reload)
- [Material Hot Reload](#material-hot-reload)
- [Asset Hot Reload](#asset-hot-reload)
- [Comparison](#comparison)
- [Console Commands](#console-commands)
- [Best Practices](#best-practices)
- [See Also](#see-also)

---

## Overview

Hot-reloading allows developers to modify assets, code, and data while the engine is running, seeing changes immediately without restarting. Each subsystem uses a similar pattern: monitor files for changes, detect modifications, reload the resource, and swap it into the running engine.

```
┌──────────────────────────────────────────────────────────────┐
│                     Hot Reload Systems                        │
│                                                              │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐   │
│  │ Shader       │  │ Script        │  │ Module            │   │
│  │ Hot Reload   │  │ Hot Reload    │  │ Hot Reload        │   │
│  │              │  │               │  │                   │   │
│  │ FILETIME     │  │ File watcher  │  │ DLL timestamp     │   │
│  │ polling      │  │ + debounce    │  │ polling           │   │
│  │              │  │               │  │                   │   │
│  │ Recompile    │  │ Recompile     │  │ Unload → Reload   │   │
│  │ HLSL → swap  │  │ AS → swap     │  │ DLL → reinit      │   │
│  └─────────────┘  └──────────────┘  └───────────────────┘   │
│                                                              │
│  ┌─────────────┐  ┌──────────────┐                           │
│  │ Material     │  │ Asset         │                          │
│  │ Hot Reload   │  │ Hot Reload    │                          │
│  │              │  │               │                          │
│  │ Material     │  │ File change   │                          │
│  │ file watch   │  │ detection     │                          │
│  │ → reparse    │  │ → reimport    │                          │
│  └─────────────┘  └──────────────┘                           │
└──────────────────────────────────────────────────────────────┘
```

---

## Shader Hot Reload

Monitors shader source files (HLSL) for changes and recompiles them at runtime.

**How it works:**
1. `Shader` tracks each shader file's `FILETIME` (last write time)
2. Each frame (or on demand), `HotReloadShaders()` checks if any file has been modified
3. Modified shaders are recompiled via D3DCompile
4. On success, the old compiled shader is replaced with the new one
5. On failure, the old shader remains active and an error is logged

**Usage:**
```cpp
// Automatic: happens each frame if enabled
graphicsEngine.SetShaderHotReloadEnabled(true);

// Manual trigger:
graphicsEngine.HotReloadShaders();
```

**State preservation:** Shader hot-reload is seamless — constant buffers, render targets, and pipeline state are unaffected. Only the compiled shader bytecode is swapped.

---

## Script Hot Reload

Monitors AngelScript files for changes and recompiles modified scripts.

**How it works:**
1. `ScriptHotReloadManager` watches directories for file changes (Modified, Created, Deleted, Renamed)
2. A **debounce timer** (configurable, default ~200ms) prevents recompilation during rapid saves
3. When the debounce timer expires, the recompile callback fires
4. The script engine recompiles the modified file
5. A `RecompileResult` reports success/failure and error details

**Usage:**
```cpp
ScriptHotReloadManager hotReload;
hotReload.AddWatchDirectory("Scripts/");
hotReload.SetRecompileCallback([&](const std::string& file)
{
    scriptEngine.CompileFile(file);
});
hotReload.Start();

// In main loop:
hotReload.PollChanges();
```

**State preservation:** Script global variables and object state may be lost on reload. Design scripts to re-initialize from component data rather than relying on persistent script-side state.

**File change types:**

| Type | Action |
|------|--------|
| `Modified` | Recompile the changed file |
| `Created` | Compile and register the new script |
| `Deleted` | Unregister the script (existing instances keep running) |
| `Renamed` | Treated as delete + create |

---

## Module Hot Reload

Monitors game module DLLs for changes and reloads them via the `ModuleManager`.

**How it works:**
1. `ModuleHotReloadManager` polls module DLL/SO file timestamps
2. When a change is detected, the module is unloaded (`IModule::Shutdown`)
3. The new DLL is loaded and the module is re-initialized (`IModule::Initialize`)
4. A callback reports success or failure per module

**Usage:**
```cpp
ModuleHotReloadManager moduleReload;
moduleReload.SetCallback([](const std::string& name, bool success)
{
    if (success)
        LOG_INFO("Module '{}' reloaded", name);
    else
        LOG_ERROR("Module '{}' reload failed", name);
});

// In main loop:
moduleReload.PollChanges();
```

**State preservation:** Module state is lost on reload. The module must re-register its systems, components, and commands during `Initialize()`. Game state stored in ECS components persists across module reloads.

---

## Material Hot Reload

The `MaterialSystem` monitors material definition files for changes.

**How it works:**
1. Material files are tracked by timestamp
2. On change, the material definition is re-parsed
3. Shader permutations are recompiled if the material's shader references changed
4. Texture bindings are updated
5. Objects using the material see the new appearance immediately

**Trigger:** Automatic when shader hot-reload detects material-related changes, or via console command.

---

## Asset Hot Reload

The `AssetPipeline` detects changes to source asset files and re-imports them.

**How it works:**
1. Source assets (textures, models, audio) are monitored via the `LocalFileCache`
2. When a source file changes, the asset transitions to a "needs reimport" state
3. The reimport pipeline processes the asset (compression, format conversion, etc.)
4. The runtime resource is updated with the new data

**Best for:** Texture iteration, model tweaks, audio adjustments during development.

---

## Comparison

| System | Detection | Debounce | State Preserved | Latency |
|--------|-----------|----------|-----------------|---------|
| Shader | FILETIME polling | No | Full (seamless) | ~1 frame |
| Script | File watcher | Yes (~200ms) | Partial (globals lost) | ~200ms + compile |
| Module | DLL timestamp | No | None (re-initialize) | ~1s (DLL load) |
| Material | File timestamp | No | Full (seamless) | ~1 frame + compile |
| Asset | File cache | No | Full (resource swap) | Varies (reimport) |

---

## Console Commands

| Command | Description |
|---------|-------------|
| `shader_reload` | Force reload all modified shaders |
| `shader_reload_all` | Recompile all shaders regardless of timestamp |
| `material_reload <name>` | Reload a specific material |
| `asset_reimport <path>` | Force reimport a specific asset |

---

## Best Practices

### Designing Reload-Safe Code

1. **Store state in ECS components, not in reloadable code.** Script variables and module-side state will be lost. ECS components survive script and module reloads.

2. **Use `Initialize()`/`Shutdown()` for module lifecycle.** Modules must cleanly unregister everything in `Shutdown()` and re-register in `Initialize()`. Leaked registrations cause crashes on reload.

3. **Keep shader interfaces stable.** Changing constant buffer layouts requires all dependent code to update simultaneously. Add fields at the end of constant buffers when possible.

4. **Test with hot-reload during development.** Enable shader and script hot-reload by default in debug builds. Discovering reload issues early is cheaper than fixing them later.

5. **Don't rely on script constructor order.** After script hot-reload, objects may initialize in a different order. Use explicit initialization functions instead.

---

## See Also

- [Shader Pipeline](Shader-Pipeline) — Shader compilation and hot-reload details
- [Scripting with AngelScript](Scripting-with-AngelScript) — Script engine and hot-reload
- [Creating a Game Module](Creating-a-Game-Module) — Module lifecycle and DLL boundaries
- [Asset Pipeline](Asset-Pipeline) — Asset import and caching
- [SparkEditor](SparkEditor) — Editor integration with hot-reload
