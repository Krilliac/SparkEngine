# SparkEngine — Recommended Improvements & Features

> Improvements and feature suggestions beyond the existing [Feature Roadmap](FEATURE_ROADMAP.md).
> Focused on code quality, developer experience, missing infrastructure, performance, and modern C++ practices.

**Last Updated:** 2026-03-07

---

## High Priority — Immediate Impact

### 1. Remove `using namespace DirectX;` from all header files

**Why:** 57+ header files contain `using namespace DirectX;`, including `GraphicsEngine.h`, `PhysicsSystem.h`, `Components.h`, `NavMesh.h`, `BehaviorTree.h`, `SceneManager.h`, and critically `framework.h` (line 51) which propagates to every translation unit. This pollutes the global namespace, creates collision risks with STL/third-party names, and violates every modern C++ style guide. One file also has `using namespace std;`.

- Replace with explicit `DirectX::` qualification or scoped `using` declarations inside functions/methods
- Remove `using namespace std;` from `RampObject.cpp`
- Add a clang-tidy check to prevent reintroduction

| | |
|---|---|
| Complexity | Medium (mechanically straightforward, but touches 57+ files) |
| Files | All headers with `using namespace DirectX;`, especially `framework.h`, `GraphicsEngine.h`, `PhysicsSystem.h`, `Components.h` |

---

### 2. Split monolithic header files into header/implementation pairs

**Why:** The engine has 42,006 lines in headers vs 45,079 in `.cpp` files — nearly 1:1, which is pathologically header-heavy. The worst offenders: `PhysicsSystem.h` (1,805 lines), `Components.h` (1,299 lines), `BehaviorTree.h` (1,057 lines), `AnimationSystem.h` (992 lines), `SaveSystem.h` (851 lines), `Player.h` (784 lines), `NavMesh.h` (770 lines). Every translation unit that includes these parses thousands of lines, inflating compile times and making incremental rebuilds slow.

- Move implementation bodies to corresponding `.cpp` files
- Keep only declarations, templates, and inline functions in headers
- Do this system-by-system with test validation after each

| | |
|---|---|
| Complexity | Large |
| Files | `PhysicsSystem.h`, `Components.h`, `BehaviorTree.h`, `AnimationSystem.h`, `SaveSystem.h`, `Player.h`, `NavMesh.h`, `SceneManager.h` |

---

### 3. Add clang-tidy and enforce clang-format in CI

**Why:** A `.clang-format` config exists but is not enforced — there's no CI check or pre-commit hook to catch style violations. No static analysis tooling (`clang-tidy`) exists at all.

- Add a `.clang-tidy` config with checks for common C++ pitfalls (`bugprone-*`, `modernize-*`, `performance-*`)
- Add a CI step that runs `clang-format --dry-run --Werror` to enforce the existing `.clang-format`
- Optionally add a pre-commit hook for local enforcement

| | |
|---|---|
| Complexity | Small |
| Files | New `.clang-tidy`, update `.github/workflows/build.yml` |

---

### 4. Eliminate global mutable state — create an Application class

**Why:** `SparkEngine.cpp` uses 5 global variables (`g_graphics`, `g_game`, `g_input`, `g_timer`, `g_console`) plus numerous `GetInstance()` singletons (`AnimationManager`, `AudioMixer`, `MusicManager`, `SimpleConsole`, etc.). `Game::Initialize()` takes raw pointers to `GraphicsEngine*` and `InputManager*` while the same objects are also globals — creating ambiguity about ownership and lifetime.

- Create an `Engine` or `Application` class that owns all subsystems
- Pass subsystems via constructor/dependency injection instead of globals
- Use an initialization/shutdown sequence with explicit ordering
- Migrate `GetInstance()` singletons to the service registry over time

| | |
|---|---|
| Complexity | Large (incremental migration recommended) |
| Files | `Spark Engine/Source/Core/SparkEngine.cpp/.h`, `Game/Game.cpp`, every file referencing `g_graphics`, `g_game`, `g_input`, `g_timer`, or `GetInstance()` |

---

### 5. Add a `Result<T>` error handling type

**Why:** The codebase mixes HRESULT returns (138 occurrences across 15 files), bool returns, null-pointer checking, and `catch (...)` blocks. `SparkError.h` macros handle error *reporting* but not *propagation*. Functions like `Game::Initialize()` return HRESULT on Windows which doesn't work cross-platform.

- Introduce `Spark::Result<T>` (wrap C++23 `std::expected` or use `tl::expected` polyfill)
- Replace `catch (...)` blocks with specific exception types (see `SparkEngine.cpp` lines 337–348, 394–419)
- New code uses `Result<T>`, old code migrates gradually

| | |
|---|---|
| Complexity | Medium |
| Files | New `Utils/Result.h`, then gradually `AssetPipeline.h`, `SaveSystem.h`, `SparkEngine.cpp` |

---

### 6. Expand asset format support (beyond OBJ/TGA/WAV)

**Why:** The engine only loads OBJ meshes, TGA textures, and WAV audio. OBJ is a legacy format lacking skeletal animation, PBR materials, and scene hierarchy. Assimp and stb are already integrated but underutilized.

- Use Assimp for glTF 2.0 and FBX import (already a dependency)
- Add `stb_image` for PNG/JPG loading (`stb` already included)
- Add DDS/BC texture loading for GPU-compressed textures
- Add an asset cache/registry with UUID-based references

| | |
|---|---|
| Complexity | Medium |
| Files | `Spark Engine/Source/Graphics/AssetPipeline.h/.cpp`, `TextureSystem.h/.cpp` |

---

## Medium Priority — Quality & Polish

### 7. Add a centralized job/task system for multithreading

**Why:** 36 source files use raw `std::thread`/`std::mutex` — `SceneManager` (async loading), `TextureSystem` (streaming), `AssetPipeline` (loading), `ConsoleProcessManager`, and `InputManager` all spawn threads independently. None coordinate, pool, or limit thread creation. `PhysicsSystem.h` explicitly documents it is "not thread-safe."

- Create a thread pool / job system with work stealing
- Define job priorities (render, physics, audio, background loading)
- Use C++20 `std::jthread` and `std::stop_token` for cooperative cancellation
- Migrate existing raw thread usage to the job system

| | |
|---|---|
| Complexity | Large |
| Files | New `Engine/Jobs/JobSystem.h/.cpp`, modifications to `TextureSystem.h`, `AssetPipeline.h`, `SceneManager.cpp` |

---

### 8. Add precompiled headers (PCH) for build time reduction

**Why:** With 125 header files and 75 cpp files, many heavy headers are included repeatedly. `Platform.h` (789 lines) is included by nearly everything. `<entt/entt.hpp>` is a large single-include header. CMake 3.16+ (already the minimum) supports `target_precompile_headers()` natively.

- Create a `pch.h` with stable, frequently-included headers: `Platform.h`, `<string>`, `<vector>`, `<memory>`, `<unordered_map>`, `<functional>`, `<mutex>`, `<DirectXMath.h>`, `<entt/entt.hpp>`
- Configure via CMake `target_precompile_headers()`

| | |
|---|---|
| Complexity | Small |
| Files | New `Spark Engine/Source/pch.h`, update `CMakeLists.txt` |

---

### 9. Add a per-frame linear (bump) allocator

**Why:** The `MemoryDebugger` tracks allocations but the engine has no mechanism to *avoid* heap allocations for frame-scoped data. `malloc`/`free` overhead and fragmentation are significant at 60+ FPS. The `ObjectPool` helps for reusable objects, but many temporary allocations (debug strings, render lists, particle spawn lists) are use-once-per-frame.

- Per-frame linear allocator that resets at end of frame
- ~100 lines of allocator code, thread-local for safety
- Use for graphics command building, particle system, debug draw

| | |
|---|---|
| Complexity | Medium |
| Files | New `Utils/FrameAllocator.h`, consumers in graphics/particles/debug |

---

### 10. Add GPU timestamp profiling alongside CPU profiling

**Why:** The `Profiler.h` already includes `<d3d11.h>` and `ComPtr`, suggesting GPU profiling was planned. Currently only CPU-side `std::chrono` timing is implemented. For a deferred renderer with PBR, post-processing, and particles, GPU bottlenecks are common and cannot be identified from CPU timers alone.

- D3D11 timestamp queries (`D3D11_QUERY_TIMESTAMP` + `D3D11_QUERY_TIMESTAMP_DISJOINT`)
- Handle 1–2 frame GPU query readback latency
- Display GPU timings alongside CPU timings in the debug overlay

| | |
|---|---|
| Complexity | Medium |
| Files | `Spark Engine/Source/Utils/Profiler.h`, `Graphics/GraphicsEngine.cpp` |

---

### 11. Wire up the existing settings.ini to actually drive engine initialization

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

### 12. Add integration tests that exercise real system interactions

**Why:** The 37 test files are all unit tests that test systems in isolation — the test suite doesn't link against actual engine libraries (no Bullet, no D3D11, no audio). Tests include engine headers directly but cannot test real physics simulation, GPU rendering, or asset loading. The most complex and bug-prone code paths are completely untested.

- Add a headless/null graphics backend (WARP software adapter on Windows, or no-op)
- Create integration tests: physics + ECS sync, asset pipeline loading real OBJ files, scene manager round-trip
- Run in CI on every push

| | |
|---|---|
| Complexity | Medium–Large |
| Files | `Tests/CMakeLists.txt`, new integration test files |

---

### 13. Add GPU resource leak detection in debug builds

**Why:** D3D11 has `ID3D11Debug::ReportLiveDeviceObjects()` which reports leaked GPU resources. This is critical for catching resource leaks during development.

- Enable D3D11 debug layer in debug builds
- Call `ReportLiveDeviceObjects` on shutdown
- Add DXGI info queue message logging

| | |
|---|---|
| Complexity | Small |
| Files | `Spark Engine/Source/Graphics/GraphicsEngine.cpp` |

---

### 14. Add AddressSanitizer/ThreadSanitizer CI builds

**Why:** With 36 files using `std::thread`/`std::mutex`, manual buffer management (fixed-size `char[1024]`/`char[2048]` in `SparkError.h`), and `vsnprintf` calls, runtime sanitizers would catch real bugs that static analysis misses. TSan would catch data races in ad-hoc threading. ASan would catch buffer overflows in the crash handler and logging system.

- Add GCC/Clang CI variants with `-fsanitize=address,undefined`
- Add a separate `-fsanitize=thread` build
- Fix the issues that surface

| | |
|---|---|
| Complexity | Small |
| Files | `.github/workflows/build.yml` |

---

### 15. Cross-platform math type abstraction

**Why:** Nearly every system uses `DirectX::XMFLOAT3`, `DirectX::XMMATRIX`, etc. directly. On Linux, these types do not exist — the entire ECS, physics, AI, and graphics subsystems are Windows-only at the *source* level. `Platform.h` handles Win32 types but not math types. This is the single largest blocker for real cross-platform compilation.

- Create `Spark::Math` namespace with `Vector3`, `Matrix4x4`, `Quaternion` types
- Use DirectXMath on Windows, GLM (already a dependency) on Linux/macOS
- Can start with `typedef` aliases and migrate incrementally

| | |
|---|---|
| Complexity | Large (touches 50+ files, but can be done incrementally) |
| Files | New `Utils/SparkMath.h`, then every file using DirectXMath directly |

---

## Lower Priority — Nice-to-Have

### 16. Add a resource/asset hot-reload system

**Why:** Currently only AngelScript supports hot-reload. Shader iteration requires a full restart. The existing `ENABLE_HOT_RELOAD` CMake option and `HOT_RELOAD_ENABLED` define appear limited to scripts.

- File watcher (`ReadDirectoryChangesW` on Windows, `inotify` on Linux)
- Hot-reload for shaders (recompile HLSL on change via existing `d3dcompiler` linkage)
- Hot-reload for textures and materials

| | |
|---|---|
| Complexity | Medium |
| Files | New `Utils/FileWatcher.h/.cpp`, `Shader.h/.cpp`, `TextureSystem.cpp`, `MaterialSystem.cpp` |

---

### 17. Add profiling scopes with chrome://tracing export

**Why:** A profiler exists but there's no easy way to visualize frame timings. Chrome tracing JSON format is zero-dependency and gives flame graphs.

- Add `SPARK_PROFILE_SCOPE("name")` macro that records timestamps
- Export to chrome://tracing JSON format on demand
- Visualize in Chrome or [Perfetto](https://ui.perfetto.dev/)

| | |
|---|---|
| Complexity | Small |
| Files | `Spark Engine/Source/Utils/Profiler.h` |

---

### 18. Add spatial partitioning (BVH or Octree)

**Why:** No spatial acceleration structure exists for culling or queries. As scene complexity grows, brute-force iteration over all objects becomes a bottleneck.

- BVH or Octree for static geometry
- Frustum culling using the spatial structure
- Spatial queries for gameplay (find entities in radius)

| | |
|---|---|
| Complexity | Medium |
| Files | New `Engine/Spatial/BVH.h/.cpp` or `Octree.h/.cpp` |

---

### 19. Add an undo/redo system for the editor

**Why:** The ImGui editor has no undo/redo. This is essential for a usable level editor — accidental changes can't be reverted.

- Command pattern with undo/redo stack
- Serializable commands for all editor operations (move, delete, property change)
- Keyboard shortcuts (Ctrl+Z / Ctrl+Y)

| | |
|---|---|
| Complexity | Medium |
| Files | `SparkEditor/Source/` — new `EditorCommand.h/.cpp` |

---

### 20. Add CMake presets for common configurations

**Why:** The build system has 30+ CMake options. `generate.bat`/`generate.sh` only cover one configuration each. CI workflows duplicate configuration flags. `CMakePresets.json` (CMake 3.19+) provides a single source of truth.

- Presets: `windows-debug`, `windows-release`, `linux-gcc-debug`, `linux-clang-release`, `ci-windows`, `ci-linux-asan`
- Include compiler flags, feature toggles, and generator settings

| | |
|---|---|
| Complexity | Small |
| Files | New `CMakePresets.json` |

---

### 21. Fix UUID generation to use standards-based approach

**Why:** The current UUID generator in `Utils/UUID.h` seeds from `high_resolution_clock` and uses `std::mt19937_64` — not cryptographically secure, with potential collisions if two processes start within the same clock tick. Uses a static mutex for every generation, creating contention under load.

- Use proper UUID v4 (RFC 4122) or OS-provided generation (`UuidCreate` on Windows, `/dev/urandom` on Linux)
- Remove the per-generation mutex contention

| | |
|---|---|
| Complexity | Small |
| Files | `Spark Engine/Source/Utils/UUID.h` |

---

### 22. Add a plugin/module system

**Why:** All engine features are compiled into a monolithic binary. A plugin system would allow loading game-specific code as DLLs/shared libraries without recompiling the engine.

- Define a plugin interface (`IPlugin` with `OnLoad`, `OnUnload`, `OnUpdate`)
- Dynamic library loading (`LoadLibrary`/`dlopen`)
- Plugin manifest (JSON) for metadata and dependencies

| | |
|---|---|
| Complexity | Medium |
| Files | New `Engine/Plugin/` directory |

---

## Summary Table

| # | Recommendation | Priority | Complexity | Category |
|:---:|---|:---:|:---:|---|
| 1 | Remove `using namespace` from headers | High | Medium | Code Quality |
| 2 | Split monolithic headers | High | Large | Build Times / Architecture |
| 3 | Enforce clang-format + add clang-tidy in CI | High | Small | Developer Experience |
| 4 | Replace globals/singletons with Application class | High | Large | Architecture |
| 5 | `Result<T>` error handling type | High | Medium | Robustness |
| 6 | Expand asset format support (glTF, PNG, DDS) | High | Medium | Feature |
| 7 | Centralized job/task system | Medium | Large | Performance |
| 8 | Precompiled headers (PCH) | Medium | Small | Build Times |
| 9 | Per-frame linear allocator | Medium | Medium | Performance |
| 10 | GPU timestamp profiling | Medium | Medium | Developer Experience |
| 11 | Wire up settings.ini to engine initialization | Medium | Small–Medium | Developer Experience |
| 12 | Integration tests (real system interactions) | Medium | Medium–Large | Robustness |
| 13 | GPU resource leak detection (D3D11 debug layer) | Medium | Small | Developer Experience |
| 14 | AddressSanitizer / ThreadSanitizer CI builds | Medium | Small | Security / Robustness |
| 15 | Cross-platform math type abstraction | Medium | Large | Architecture |
| 16 | Shader/texture hot-reload | Lower | Medium | Developer Experience |
| 17 | Chrome tracing profiler export | Lower | Small | Developer Experience |
| 18 | Spatial partitioning (BVH/Octree) | Lower | Medium | Performance |
| 19 | Editor undo/redo system | Lower | Medium | Feature |
| 20 | CMake presets | Lower | Small | Developer Experience |
| 21 | Fix UUID generation (RFC 4122) | Lower | Small | Robustness |
| 22 | Plugin/module system | Lower | Medium | Architecture |
