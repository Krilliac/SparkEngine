# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-09-21.
This source inventory is not readiness evidence. The `stable-v1` Windows 11
x64 profile remains blocked and uncertified in `docs/site/readiness.json`.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 322012 |
| **SparkEditor/Source** | 103736 |
| **GameModules** | 142702 |
| **External services** | 11305 |
| **Asset pipeline** | 2524 |
| **Tests** | 185759 |
| **SparkConsole/src** | 1770 |
| **SparkShaderCompiler/src** | 691 |
| **Total C++ (excl. ThirdParty)** | **~782725** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hh/.hpp/.hxx/.inl) | 1067 |
| Implementation files (.c/.cc/.cpp/.cxx/.mm) | 1594 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test-bearing implementation files (.cpp/.mm) | 607 |
| Wiki pages (.md) | 201 |

### Largest Top-Level Source Section

Graphics contains 121327 lines, or 37% of `SparkEngine/Source`. This is a source-inventory measurement, not runtime coverage or support evidence.

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 121327 | 37.6% |
| Engine (all subsystems) | 89657 | 27.8% |
| Utils | 46144 | 14.3% |
| Core | 30924 | 9.6% |
| Physics | 10812 | 3.3% |
| Audio | 6884 | 2.1% |
| Input | 3997 | 1.2% |
| SceneManager | 2639 | 0.8% |
| Enums | 1383 | 0.4% |
| Game | 2855 | 0.8% |
| Camera | 999 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| Networking | 15168 |
| AI | 13490 |
| ECS | 8497 |
| Gameplay | 7925 |
| Animation | 6876 |
| Scripting | 5096 |
| SaveSystem | 3855 |
| UI | 2522 |
| Streaming | 2123 |
| Editor | 1737 |
| Cinematic | 1652 |
| World | 1604 |
| Modding | 1579 |
| Dialogue | 1425 |
| Persistence | 1412 |
| 2D | 1015 |
| Coroutine | 800 |
| Replay | 784 |
| Tween | 579 |
| Destruction | 559 |
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
| `*Panel.h` class inventory | 64 |
| Total editor lines | 103736 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 607 |
| TEST() definitions | 7394 |
| Configured sanitizer workflow lanes | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 33 |
| ENABLE_* feature toggles | 25 |
| Game modules | 11 |
| SDK public headers | 16 |
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
| `SaveSystem.cpp` | 2485 |
| `OpenGLDevice.cpp` | 2160 |
| `ModuleManager.cpp` | 2107 |
| `CrashHandler.cpp` | 2018 |
| `VulkanDevice.cpp` | 1991 |
| `D3D11Device.cpp` | 1991 |
| `EngineSettings.cpp` | 1849 |
| `NetworkConnection.cpp` | 1680 |
| `GameplayLifecycleShared.cpp` | 1653 |
| `PostProcessingPipeline.cpp` | 1602 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `Telemetry.h` | 1419 |
| `JsonUtils.h` | 1318 |
| `GraphicsEngine.h` | 1277 |
| `EngineSettings.h` | 1152 |
| `NetworkManager.h` | 943 |
| `RemoteDebugSystem.h` | 874 |
| `SaveSystem.h` | 866 |
| `ECSystems.h` | 846 |
| `PhysicsTypes.h` | 828 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `EditorUI.cpp` | 2872 |
| `ProjectManager.cpp` | 2589 |
| `JSONSceneSerializer.cpp` | 2010 |
| `VisualScriptPanel.cpp` | 1773 |
| `CollaborativeEditSession.cpp` | 1696 |
| `EditorTheme.cpp` | 1669 |
| `BuildPipeline.cpp` | 1659 |
| `HierarchyPanel.cpp` | 1651 |
| `PerformanceProfiler.cpp` | 1606 |
| `ProjectSettingsPanel.cpp` | 1519 |

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
