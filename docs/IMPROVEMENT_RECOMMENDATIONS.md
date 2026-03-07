# SparkEngine — Recommended Improvements & Features

> Improvements and feature suggestions beyond the existing [Feature Roadmap](FEATURE_ROADMAP.md).
> Focused on code quality, developer experience, missing infrastructure, performance, and modern C++ practices.

**Last Updated:** 2026-03-07

---

## High Priority — Immediate Impact

### 1. Add clang-tidy and enforce clang-format in CI

**Why:** A `.clang-format` config exists but is not enforced — there's no CI check or pre-commit hook to catch style violations. No static analysis tooling (`clang-tidy`) exists at all.

- Add a `.clang-tidy` config with checks for common C++ pitfalls (`bugprone-*`, `modernize-*`, `performance-*`)
- Add a CI step that runs `clang-format --dry-run --Werror` to enforce the existing `.clang-format`
- Optionally add a pre-commit hook for local enforcement

| | |
|---|---|
| Complexity | Small |
| Files | New `.clang-tidy`, update `.github/workflows/build.yml` |

---

### 2. Eliminate global mutable state in the main entry point

**Why:** `SparkEngine.cpp` uses 5 global variables (`g_graphics`, `g_game`, `g_input`, `g_timer`, `g_console`). This makes testing impossible, initialization order fragile, and subsystem lifetime unclear. It also prevents running multiple engine instances (e.g., for editor + game preview).

- Create an `Engine` or `Application` class that owns all subsystems
- Pass subsystems via constructor/dependency injection instead of globals
- Use an initialization/shutdown sequence with explicit ordering

| | |
|---|---|
| Complexity | Medium |
| Files | `Spark Engine/Source/Core/SparkEngine.cpp`, new `Engine/Application.h/.cpp` |

---

### 3. Expand asset format support (beyond OBJ/TGA/WAV)

**Why:** The engine only loads OBJ meshes, TGA textures, and WAV audio. Modern games need glTF/FBX models, PNG/DDS/KTX textures, and OGG/MP3 audio. Assimp is already integrated but underutilized.

- Use Assimp for glTF 2.0 and FBX import (already a dependency)
- Add `stb_image` for PNG/JPG loading (`stb` already included)
- Add DDS/BC texture loading for GPU-compressed textures
- Add an asset cache/registry with UUID-based references

| | |
|---|---|
| Complexity | Medium |
| Files | `Spark Engine/Source/Graphics/AssetPipeline.h/.cpp`, `TextureSystem.h/.cpp` |

---

### 4. Add a resource/asset hot-reload system

**Why:** Currently only AngelScript supports hot-reload. Artists and developers need to iterate on shaders, textures, and materials without restarting the engine.

- File watcher (`ReadDirectoryChangesW` on Windows, `inotify` on Linux)
- Hot-reload for shaders (recompile HLSL on change)
- Hot-reload for textures (reload from disk)
- Hot-reload for materials (re-read JSON material definitions)

| | |
|---|---|
| Complexity | Medium |
| Files | New `Utils/FileWatcher.h/.cpp`, modifications to `Shader.cpp`, `TextureSystem.cpp`, `MaterialSystem.cpp` |

---

### 5. Add a proper memory allocation strategy

**Why:** The engine uses default `new`/`delete` everywhere with no tracking, pooling, or custom allocators. For a game engine, this leads to fragmentation and unpredictable performance.

- Add a tagged/scoped allocator for per-frame and per-system allocations
- Add memory tracking/budgets (how much VRAM, RAM each system uses)
- Pool allocators for frequently created/destroyed objects (particles, projectiles)
- The existing `ObjectPool` in tests suggests this pattern is wanted but not yet engine-wide

| | |
|---|---|
| Complexity | Large |
| Files | New `Utils/MemoryAllocator.h`, modifications across engine systems |

---

## Medium Priority — Quality & Polish

### 6. Replace catch-all exception handlers with specific error handling

**Why:** Multiple console commands use `catch (...)` which silently swallows all errors, making debugging extremely difficult (see `SparkEngine.cpp` lines 337–348, 394–419).

- Replace `catch (...)` with specific exception types
- Add structured error codes/results (use `std::expected` from C++23 or a custom `Result<T>` type)
- Ensure errors propagate meaningful messages to the console/log

| | |
|---|---|
| Complexity | Small |
| Files | `Spark Engine/Source/Core/SparkEngine.cpp`, various system files |

---

### 7. Add a centralized job/task system for multithreading

**Why:** 36 source files use raw `std::thread`/`std::mutex`. No centralized threading model means ad-hoc thread creation, potential contention, and hard-to-debug race conditions.

- Create a thread pool / job system with work stealing
- Define job priorities (render, physics, audio, background loading)
- Use C++20 `std::jthread` and `std::stop_token` for cooperative cancellation
- Migrate existing raw thread usage to the job system

| | |
|---|---|
| Complexity | Large |
| Files | New `Engine/Jobs/JobSystem.h/.cpp`, modifications to physics, audio, asset loading |

---

### 8. Add an integration test that boots the engine headlessly

**Why:** 37 unit test files exist but none initialize the engine subsystems together. A headless boot test catches initialization regressions immediately.

- Add a headless/null graphics backend (renders to an offscreen buffer or no-ops)
- Create an integration test that initializes all subsystems, runs 1 frame, and shuts down
- Run it in CI on every push

| | |
|---|---|
| Complexity | Medium |
| Files | New `Tests/TestEngineBootstrap.cpp`, possible null RHI backend |

---

### 9. Add GPU resource leak detection in debug builds

**Why:** D3D11 has `ID3D11Debug::ReportLiveDeviceObjects()` which reports leaked GPU resources. This is critical for catching resource leaks during development.

- Enable D3D11 debug layer in debug builds
- Call `ReportLiveDeviceObjects` on shutdown
- Add DXGI info queue message logging

| | |
|---|---|
| Complexity | Small |
| Files | `Spark Engine/Source/Graphics/GraphicsEngine.cpp` |

---

### 10. Wire up the existing settings.ini to actually drive engine initialization

**Why:** A `settings.ini` exists (`Spark Engine/Resources/Config/settings.ini`) with graphics, audio, controls, and game settings — but the engine ignores it. Window size is hardcoded to 1280x720 in `SparkEngine.cpp`, and settings are only changeable via console commands at runtime.

- Parse `settings.ini` at startup and apply values (window size, fullscreen, vsync, volumes, mouse sensitivity)
- Wire console commands (`gfx_vsync`, etc.) to write back to the INI file
- Auto-save modified settings on shutdown
- Add quality presets (Low/Medium/High/Ultra) that set multiple values at once

| | |
|---|---|
| Complexity | Small–Medium |
| Files | `Spark Engine/Source/Core/SparkEngine.cpp`, new or existing config parser, `Spark Engine/Resources/Config/settings.ini` |

---

### 11. Make the Linux build functional (not just a stub)

**Why:** The non-Windows `main()` just prints "Full engine features require the Windows DirectX 11 runtime" and exits. The RHI abstraction exists but the OpenGL backend is untested.

- Get the OpenGL backend functional (add GLAD, test basic rendering)
- Use SDL2 or GLFW for window creation on Linux
- Make at least core systems (ECS, physics, audio via miniaudio, scene management) work cross-platform

| | |
|---|---|
| Complexity | Large |
| Files | `Spark Engine/Source/Core/SparkEngine.cpp`, OpenGL backend files, `CMakeLists.txt` |

---

## Lower Priority — Nice-to-Have

### 12. Add profiling scopes with chrome://tracing export

**Why:** A profiler exists but there's no easy way to visualize frame timings. Chrome tracing JSON format is a zero-dependency way to get flame graphs.

- Add `SPARK_PROFILE_SCOPE("name")` macro that records timestamps
- Export to chrome://tracing JSON format on demand
- Visualize in Chrome or [Perfetto](https://ui.perfetto.dev/)

| | |
|---|---|
| Complexity | Small |
| Files | `Spark Engine/Source/Utils/Profiler.h` |

---

### 13. Add spatial partitioning (BVH or Octree)

**Why:** No spatial acceleration structure exists for culling or queries. As scene complexity grows, brute-force iteration over all objects becomes a bottleneck.

- BVH or Octree for static geometry
- Frustum culling using the spatial structure
- Spatial queries for gameplay (find entities in radius)

| | |
|---|---|
| Complexity | Medium |
| Files | New `Engine/Spatial/BVH.h/.cpp` or `Octree.h/.cpp` |

---

### 14. Add a plugin/module system

**Why:** All engine features are compiled into a monolithic binary. A plugin system would allow loading game-specific code as DLLs/shared libraries without recompiling the engine.

- Define a plugin interface (`IPlugin` with `OnLoad`, `OnUnload`, `OnUpdate`)
- Dynamic library loading (`LoadLibrary`/`dlopen`)
- Plugin manifest (JSON) for metadata and dependencies

| | |
|---|---|
| Complexity | Medium |
| Files | New `Engine/Plugin/` directory |

---

### 15. Add an undo/redo system for the editor

**Why:** The ImGui editor has no undo/redo. This is essential for a usable level editor — accidental changes can't be reverted.

- Command pattern with undo/redo stack
- Serializable commands for all editor operations (move, delete, property change)
- Keyboard shortcuts (Ctrl+Z / Ctrl+Y)

| | |
|---|---|
| Complexity | Medium |
| Files | `SparkEditor/Source/` — new `EditorCommand.h/.cpp` |

---

### 16. Add CMake presets for common configurations

**Why:** The build system has 30+ CMake options. New developers must read docs to know which options to set. CMake presets (`CMakePresets.json`) provide named configurations.

- Presets: `windows-debug`, `windows-release`, `linux-debug`, `linux-release`, `minimal`, `full`
- Include compiler flags, feature toggles, and generator settings

| | |
|---|---|
| Complexity | Small |
| Files | New `CMakePresets.json` |
