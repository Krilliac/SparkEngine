# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-09-03.
This source inventory is not readiness evidence. The `stable-v1` Windows 11
x64 profile remains blocked and uncertified in `docs/site/readiness.json`.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 314312 |
| **SparkEditor/Source** | 109384 |
| **GameModules** | 143038 |
| **External services** | 11271 |
| **Asset pipeline** | 2399 |
| **Tests** | 172072 |
| **SparkConsole/src** | 1633 |
| **SparkShaderCompiler/src** | 588 |
| **Total C++ (excl. ThirdParty)** | **~765973** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hh/.hpp/.hxx/.inl) | 1051 |
| Implementation files (.c/.cc/.cpp/.cxx/.mm) | 1536 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test-bearing implementation files (.cpp/.mm) | 577 |
| Wiki pages (.md) | 198 |

### Largest Top-Level Source Section

Graphics contains 119569 lines, or 38% of `SparkEngine/Source`. This is a source-inventory measurement, not runtime coverage or support evidence.

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 119569 | 38.0% |
| Engine (all subsystems) | 87759 | 27.9% |
| Utils | 44429 | 14.1% |
| Core | 29978 | 9.5% |
| Physics | 10814 | 3.4% |
| Audio | 6090 | 1.9% |
| Input | 3953 | 1.2% |
| SceneManager | 2241 | 0.7% |
| Enums | 1423 | 0.4% |
| Game | 2794 | 0.8% |
| Camera | 999 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| Networking | 14945 |
| AI | 13490 |
| ECS | 8447 |
| Gameplay | 7755 |
| Animation | 6705 |
| Scripting | 5093 |
| SaveSystem | 3534 |
| UI | 2522 |
| Streaming | 2016 |
| Editor | 1737 |
| Cinematic | 1652 |
| World | 1588 |
| Dialogue | 1425 |
| Modding | 1377 |
| Persistence | 1267 |
| 2D | 1015 |
| Coroutine | 800 |
| Replay | 731 |
| Tween | 579 |
| Destruction | 545 |
| Localization | 515 |
| Events | 473 |
| Mobile | 452 |
| Loading | 386 |
| Physics | 377 |
| VR | 329 |

## ECS Architecture Metrics

| Metric | Count |
|--------|------:|
| Concrete component-group headers (`Engine/ECS/Components/*Components.h`) | 17 |
| Struct declarations in those headers | 79 |
| ECS systems | 25 |
| Execution order | Physics → Animation → AI → Audio → Lifecycle → Render |

Component provenance: this declaration inventory scans only the concrete
`*Components.h` files and matches whitespace-tolerant `struct` declarations.
It does not measure registration, runtime use, support, or readiness.

## Editor Metrics

| Metric | Count |
|--------|------:|
| `*Panel.h` class inventory | 65 |
| Total editor lines | 109384 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 577 |
| TEST() definitions | 6991 |
| Configured sanitizer workflow lanes | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 30 |
| ENABLE_* feature toggles | 22 |
| Game modules | 11 |
| SDK public headers | 15 |
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
| `OpenGLDevice.cpp` | 2160 |
| `CrashHandler.cpp` | 2093 |
| `SaveSystem.cpp` | 2067 |
| `VulkanDevice.cpp` | 1991 |
| `ModuleManager.cpp` | 1848 |
| `NetworkConnection.cpp` | 1680 |
| `EngineSettings.cpp` | 1623 |
| `PostProcessingPipeline.cpp` | 1602 |
| `D3D12Device.cpp` | 1593 |
| `D3D11Device.cpp` | 1543 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `Telemetry.h` | 1412 |
| `GraphicsEngine.h` | 1260 |
| `JsonUtils.h` | 1087 |
| `EngineSettings.h` | 1080 |
| `NetworkManager.h` | 920 |
| `ECSystems.h` | 846 |
| `PhysicsTypes.h` | 828 |
| `MeshClusterSystem.h` | 824 |
| `SVGRenderer.h` | 803 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `ProjectManager.cpp` | 2482 |
| `EditorUI.cpp` | 2247 |
| `JSONSceneSerializer.cpp` | 2010 |
| `VisualScriptPanel.cpp` | 1773 |
| `CollaborativeEditSession.cpp` | 1670 |
| `BuildPipeline.cpp` | 1659 |
| `PerformanceProfiler.cpp` | 1609 |
| `EditorTheme.cpp` | 1587 |
| `HierarchyPanel.cpp` | 1524 |
| `ProjectSettingsPanel.cpp` | 1503 |

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
