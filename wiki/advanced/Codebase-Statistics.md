# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase, generated from
the exact tracked source tree.
This source inventory is not readiness evidence. The `stable-v1` Windows 11
x64 profile remains blocked and uncertified in `docs/site/readiness.json`.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 339308 |
| **SparkEditor/Source** | 105121 |
| **GameModules** | 158486 |
| **External services** | 12596 |
| **Asset pipeline** | 2524 |
| **Tests** | 244255 |
| **SparkConsole/src** | 1857 |
| **SparkShaderCompiler/src** | 839 |
| **Total C++ (excl. ThirdParty)** | **~883713** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hh/.hpp/.hxx/.inl) | 1145 |
| Implementation files (.c/.cc/.cpp/.cxx/.mm) | 1779 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test-bearing implementation files (.cpp/.mm) | 712 |
| Wiki pages (.md) | 205 |

### Largest Top-Level Source Section

Graphics contains 124245 lines, or 36% of `SparkEngine/Source`. This is a source-inventory measurement, not runtime coverage or support evidence.

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 124245 | 36.6% |
| Engine (all subsystems) | 97278 | 28.6% |
| Utils | 48607 | 14.3% |
| Core | 32946 | 9.7% |
| Physics | 11077 | 3.2% |
| Audio | 6992 | 2.0% |
| Input | 4046 | 1.1% |
| SceneManager | 4159 | 1.2% |
| Enums | 1383 | 0.4% |
| Game | 2950 | 0.8% |
| Camera | 999 | 0.2% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| Networking | 18262 |
| AI | 13559 |
| ECS | 8611 |
| Gameplay | 7925 |
| Scripting | 7762 |
| Animation | 6852 |
| SaveSystem | 4087 |
| UI | 2522 |
| Streaming | 2236 |
| Modding | 1860 |
| Editor | 1737 |
| Cinematic | 1652 |
| World | 1604 |
| Persistence | 1564 |
| Dialogue | 1448 |
| 2D | 1015 |
| Coroutine | 841 |
| Replay | 833 |
| Localization | 605 |
| Destruction | 591 |
| Tween | 579 |
| Events | 492 |
| Mobile | 452 |
| Loading | 386 |
| Physics | 377 |
| VR | 329 |

## ECS Architecture Metrics

| Metric | Count |
|--------|------:|
| Concrete component-group headers (`Engine/ECS/Components/*Components.h`) | 17 |
| Struct declarations in those headers | 81 |
| ECS systems | 25 |
| Execution order | Physics → Animation → AI → Audio → Lifecycle → Render |

Component provenance: this declaration inventory scans only the concrete
`*Components.h` files and matches whitespace-tolerant `struct` declarations.
It does not measure registration, runtime use, support, or readiness.

## Editor Metrics

| Metric | Count |
|--------|------:|
| `*Panel.h` class inventory | 64 |
| Total editor lines | 105121 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 712 |
| TEST() definitions | 8292 |
| Configured sanitizer workflow lanes | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 33 |
| ENABLE_* feature toggles | 24 |
| Game modules | 11 |
| SDK public headers | 18 |
| Documented build compiler paths | MSVC v143/v145, GCC 13+, Clang 17+, Apple Clang, MinGW-w64 |
| Platforms | Windows, Linux, macOS (experimental) |

## Third-Party Dependencies

### Audited Dependency Inventory

`ThirdParty/dependencies.lock` is the authoritative manifest. This selected
inventory is implementation evidence, not support certification.

| Library | Path | Purpose |
|---------|------|---------|
| Dear ImGui | `ThirdParty/UI/imgui` | Immediate-mode GUI |
| EnTT | `ThirdParty/ECS/entt` | Entity Component System |
| Jolt Physics | `ThirdParty/Physics/JoltPhysics` | Physics engine |
| AngelScript | `ThirdParty/Scripting/angelscript-mirror` | Scripting VM |
| miniz | `ThirdParty/Utils/miniz` | Compression |
| Recast Navigation | `ThirdParty/AI/recastnavigation` | NavMesh pathfinding |
| SDL2 | `ThirdParty/SDL2` | Experimental non-Windows window/input path |
| tinyobjloader | `ThirdParty/Utils/tinyobjloader` | OBJ import |
| stb_image | `ThirdParty/Utils/stb` | Image import |
| cgltf | `ThirdParty/Utils/cgltf` | glTF import |
| miniaudio | `ThirdParty/Audio/miniaudio` | Linked XAudio2-stub implementation surface; not the active audio-factory fallback |
| nlohmann/json | `ThirdParty/Utils/json` | JSON parsing when available |
| tinyexr | `ThirdParty/Utils/tinyexr` | EXR import |
| zstd | `ThirdParty/Utils/zstd` | Compression path |
| VulkanMemoryAllocator | `ThirdParty/VulkanMemoryAllocator` | Experimental Vulkan allocation path |
| glad | `ThirdParty/glad` | Experimental OpenGL loader |

## Largest Files

### SparkEngine .cpp Files (by line count)

| File | Lines |
|------|------:|
| `SaveSystem.cpp` | 2642 |
| `ModuleManager.cpp` | 2546 |
| `OpenGLDevice.cpp` | 2545 |
| `CrashHandler.cpp` | 2514 |
| `NetworkConnection.cpp` | 2483 |
| `D3D11Device.cpp` | 2135 |
| `VulkanDevice.cpp` | 1983 |
| `EngineSettings.cpp` | 1875 |
| `D3D12Device.cpp` | 1817 |
| `SceneManager.cpp` | 1760 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `Telemetry.h` | 1419 |
| `JsonUtils.h` | 1361 |
| `GraphicsEngine.h` | 1341 |
| `NetworkManager.h` | 1252 |
| `OnlineServices.h` | 1225 |
| `EngineSettings.h` | 1149 |
| `SaveSystem.h` | 939 |
| `PlatformDirectXMathStubs.h` | 892 |
| `AngelScriptEngine.h` | 876 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `ProjectManager.cpp` | 2790 |
| `EditorUI.cpp` | 2723 |
| `JSONSceneSerializer.cpp` | 2020 |
| `CollaborativeEditSession.cpp` | 1968 |
| `BuildPipeline.cpp` | 1852 |
| `EditorTheme.cpp` | 1669 |
| `VisualScriptPanel.cpp` | 1668 |
| `PerformanceProfiler.cpp` | 1606 |
| `HierarchyPanel.cpp` | 1583 |
| `ProjectSettingsPanel.cpp` | 1517 |

## Shader Inventory

| Type | Count | Location |
|------|------:|----------|
| HLSL shaders | 42 | `Shaders/HLSL/` (includes Compute, MeshShaders, RayTracing) |
| GLSL shaders | 14 | `Shaders/GLSL/` |
| Compiled bytecode (.cso) | varies | `Shaders/Compiled/` |

---

## See Also

- [Architecture Overview](../getting-started/Architecture-Overview.md) — Engine design and structure
- [Codebase Health](Codebase-Health.md) — System maturity status and known gaps
- [Testing](Testing.md) — Test suite details and CI integration
- [Build System and CMake Modules](Build-System-and-CMake-Modules.md) — Build configuration
