# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase, generated from
the exact tracked source tree.
This source inventory is not readiness evidence. The `stable-v1` Windows 11
x64 profile remains blocked and uncertified in `docs/site/readiness.json`.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 332779 |
| **SparkEditor/Source** | 104717 |
| **GameModules** | 153776 |
| **External services** | 12036 |
| **Asset pipeline** | 2524 |
| **Tests** | 223201 |
| **SparkConsole/src** | 1800 |
| **SparkShaderCompiler/src** | 691 |
| **Total C++ (excl. ThirdParty)** | **~847959** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hh/.hpp/.hxx/.inl) | 1111 |
| Implementation files (.c/.cc/.cpp/.cxx/.mm) | 1729 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test-bearing implementation files (.cpp/.mm) | 685 |
| Wiki pages (.md) | 205 |

### Largest Top-Level Source Section

Graphics contains 124789 lines, or 37% of `SparkEngine/Source`. This is a source-inventory measurement, not runtime coverage or support evidence.

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 124789 | 37.4% |
| Engine (all subsystems) | 93735 | 28.1% |
| Utils | 46288 | 13.9% |
| Core | 32249 | 9.6% |
| Physics | 11077 | 3.3% |
| Audio | 6884 | 2.0% |
| Input | 4046 | 1.2% |
| SceneManager | 3827 | 1.1% |
| Enums | 1383 | 0.4% |
| Game | 2937 | 0.8% |
| Camera | 999 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| Networking | 15876 |
| AI | 13490 |
| ECS | 8618 |
| Gameplay | 7925 |
| Scripting | 7273 |
| Animation | 6850 |
| SaveSystem | 4295 |
| UI | 2522 |
| Streaming | 2137 |
| Editor | 1737 |
| Cinematic | 1652 |
| World | 1604 |
| Modding | 1601 |
| Dialogue | 1425 |
| Persistence | 1412 |
| 2D | 1015 |
| Coroutine | 841 |
| Replay | 784 |
| Tween | 579 |
| Destruction | 559 |
| Localization | 515 |
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
| Total editor lines | 104717 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 685 |
| TEST() definitions | 8005 |
| Configured sanitizer workflow lanes | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 34 |
| ENABLE_* feature toggles | 25 |
| Game modules | 11 |
| SDK public headers | 17 |
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
| `SaveSystem.cpp` | 2573 |
| `OpenGLDevice.cpp` | 2540 |
| `CrashHandler.cpp` | 2270 |
| `ModuleManager.cpp` | 2242 |
| `D3D11Device.cpp` | 2079 |
| `VulkanDevice.cpp` | 1982 |
| `EngineSettings.cpp` | 1875 |
| `NetworkConnection.cpp` | 1776 |
| `SceneManager.cpp` | 1756 |
| `AngelScriptEngine.cpp` | 1685 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `Telemetry.h` | 1419 |
| `JsonUtils.h` | 1321 |
| `GraphicsEngine.h` | 1293 |
| `OnlineServices.h` | 1225 |
| `EngineSettings.h` | 1149 |
| `NetworkManager.h` | 998 |
| `SaveSystem.h` | 936 |
| `PlatformDirectXMathStubs.h` | 892 |
| `ECSystems.h` | 864 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `EditorUI.cpp` | 2854 |
| `ProjectManager.cpp` | 2734 |
| `JSONSceneSerializer.cpp` | 2020 |
| `BuildPipeline.cpp` | 1729 |
| `CollaborativeEditSession.cpp` | 1696 |
| `EditorTheme.cpp` | 1669 |
| `VisualScriptPanel.cpp` | 1652 |
| `HierarchyPanel.cpp` | 1651 |
| `PerformanceProfiler.cpp` | 1606 |
| `ProjectSettingsPanel.cpp` | 1515 |

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
