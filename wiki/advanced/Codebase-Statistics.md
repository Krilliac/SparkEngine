# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase, generated from
the exact tracked source tree.
This source inventory is not readiness evidence. The `stable-v1` Windows 11
x64 profile remains blocked and uncertified in `docs/site/readiness.json`.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 338230 |
| **SparkEditor/Source** | 105249 |
| **GameModules** | 156968 |
| **External services** | 12571 |
| **Asset pipeline** | 2524 |
| **Tests** | 238499 |
| **SparkConsole/src** | 1857 |
| **SparkShaderCompiler/src** | 839 |
| **Total C++ (excl. ThirdParty)** | **~875466** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hh/.hpp/.hxx/.inl) | 1130 |
| Implementation files (.c/.cc/.cpp/.cxx/.mm) | 1754 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test-bearing implementation files (.cpp/.mm) | 703 |
| Wiki pages (.md) | 205 |

### Largest Top-Level Source Section

Graphics contains 123753 lines, or 36% of `SparkEngine/Source`. This is a source-inventory measurement, not runtime coverage or support evidence.

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 123753 | 36.5% |
| Engine (all subsystems) | 97838 | 28.9% |
| Utils | 47708 | 14.1% |
| Core | 32814 | 9.7% |
| Physics | 11077 | 3.2% |
| Audio | 6992 | 2.0% |
| Input | 4046 | 1.1% |
| SceneManager | 4100 | 1.2% |
| Enums | 1383 | 0.4% |
| Game | 2950 | 0.8% |
| Camera | 999 | 0.2% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| Networking | 18222 |
| AI | 13538 |
| ECS | 8611 |
| Gameplay | 7925 |
| Scripting | 7784 |
| Animation | 6850 |
| SaveSystem | 4743 |
| UI | 2522 |
| Streaming | 2236 |
| Modding | 1860 |
| Editor | 1737 |
| Cinematic | 1652 |
| World | 1604 |
| Persistence | 1564 |
| Dialogue | 1425 |
| 2D | 1015 |
| Coroutine | 841 |
| Replay | 833 |
| Localization | 605 |
| Tween | 579 |
| Destruction | 559 |
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
| Total editor lines | 105249 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 703 |
| TEST() definitions | 8211 |
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
| `SaveSystem.cpp` | 2642 |
| `OpenGLDevice.cpp` | 2545 |
| `ModuleManager.cpp` | 2544 |
| `CrashHandler.cpp` | 2507 |
| `NetworkConnection.cpp` | 2473 |
| `D3D11Device.cpp` | 2096 |
| `VulkanDevice.cpp` | 1983 |
| `EngineSettings.cpp` | 1875 |
| `SceneManager.cpp` | 1760 |
| `D3D12Device.cpp` | 1726 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `Telemetry.h` | 1419 |
| `JsonUtils.h` | 1361 |
| `GraphicsEngine.h` | 1293 |
| `NetworkManager.h` | 1233 |
| `OnlineServices.h` | 1225 |
| `EngineSettings.h` | 1149 |
| `SaveSystem.h` | 939 |
| `PlatformDirectXMathStubs.h` | 892 |
| `AngelScriptEngine.h` | 876 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `EditorUI.cpp` | 2890 |
| `ProjectManager.cpp` | 2790 |
| `JSONSceneSerializer.cpp` | 2020 |
| `CollaborativeEditSession.cpp` | 1968 |
| `BuildPipeline.cpp` | 1852 |
| `EditorTheme.cpp` | 1669 |
| `VisualScriptPanel.cpp` | 1668 |
| `HierarchyPanel.cpp` | 1651 |
| `PerformanceProfiler.cpp` | 1606 |
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
