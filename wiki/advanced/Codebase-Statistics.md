# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-04-18.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 281876 |
| **SparkEditor/Source** | 88841 |
| **GameModules** | 58618 |
| **Tests** | 135175 |
| **SparkConsole/src** | 1868 |
| **SparkShaderCompiler/src** | 533 |
| **Total C++ (excl. ThirdParty)** | **~566911** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 777 |
| Implementation files (.cpp) | 1078 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 482 |
| Wiki pages (.md) | 144 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~769 |
| Average lines per .h file | ~575 |
| Largest codebase section | Graphics (112754 lines — 40% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 112754 | 40.0% |
| Engine (all subsystems) | 80913 | 28.7% |
| Utils | 39892 | 14.1% |
| Core | 22772 | 8.0% |
| Physics | 10101 | 3.5% |
| Audio | 6032 | 2.1% |
| Input | 3895 | 1.3% |
| SceneManager | 1886 | 0.6% |
| Enums | 1423 | 0.5% |
| Game | 1272 | 0.4% |
| Camera | 868 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13238 |
| Networking | 12166 |
| ECS | 8551 |
| Gameplay | 7511 |
| Animation | 6561 |
| Scripting | 4543 |
| SaveSystem | 2461 |
| UI | 2437 |
| Streaming | 1914 |
| World | 1588 |
| Editor | 1556 |
| Cinematic | 1526 |
| Dialogue | 1384 |
| Modding | 1279 |
| Persistence | 994 |
| 2D | 979 |
| Coroutine | 786 |
| Replay | 707 |
| Tween | 523 |
| Destruction | 509 |
| Localization | 428 |
| Mobile | 424 |
| Physics | 381 |
| Events | 362 |
| Loading | 355 |
| VR | 329 |

## ECS Architecture Metrics

| Metric | Count |
|--------|------:|
| Component header files | 17 |
| Component struct definitions | 75 |
| ECS systems | 25 |
| Execution order | Physics → Animation → AI → Audio → Lifecycle → Render |

## Editor Metrics

| Metric | Count |
|--------|------:|
| Editor panel classes | 59 |
| Total editor lines | 88841 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 482 |
| TEST() definitions | 5930 |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 23 |
| ENABLE_* feature toggles | 15 |
| Game modules | 10 |
| SDK public headers | 12 |
| Supported compilers | MSVC v143/v144, GCC 13+, Clang 17+, Apple Clang, MinGW-w64 |
| Platforms | Windows, Linux, macOS (experimental) |

## Third-Party Dependencies

### Git Submodules

| Library | Path | Purpose |
|---------|------|---------|
| Dear ImGui | `ThirdParty/UI/imgui` | Immediate-mode GUI |
| EnTT | `ThirdParty/ECS/entt` | Entity Component System |
| Jolt Physics | `ThirdParty/Physics/JoltPhysics` | Physics engine |
| AngelScript | `ThirdParty/Scripting/angelscript-mirror` | Scripting VM |
| miniz | `ThirdParty/Utils/miniz` | Compression |
| Recast Navigation | `ThirdParty/AI/recastnavigation` | NavMesh pathfinding |

### Embedded Libraries

| Library | Purpose |
|---------|---------|
| DirectXTK | DirectX 11 toolkit |
| Assimp | 3D model import (FBX, glTF) |
| ImGuizmo | 3D editor gizmos |
| imnodes | Node graph editor |
| GLM | Math library |
| RapidJSON | JSON parsing |
| spdlog | Structured logging |
| stb | Image loading |
| miniaudio | Cross-platform audio fallback |

## Largest Files

### SparkEngine .cpp Files (by line count)

| File | Lines |
|------|------:|
| `OpenGLDevice.cpp` | 2103 |
| `VulkanDevice.cpp` | 1991 |
| `D3D12Device.cpp` | 1593 |
| `PostProcessingPipeline.cpp` | 1530 |
| `D3D11Device.cpp` | 1481 |
| `GraphicsEngineWindows.cpp` | 1481 |
| `EngineSettings.cpp` | 1478 |
| `GameplayLifecycleShared.cpp` | 1347 |
| `CrashHandler.cpp` | 1322 |
| `SaveSystem.cpp` | 1245 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1125 |
| `EngineSettings.h` | 1079 |
| `JsonUtils.h` | 963 |
| `GraphicsEngine.h` | 957 |
| `BasisTranscoder.h` | 912 |
| `ECSystems.h` | 849 |
| `MeshClusterSystem.h` | 824 |
| `SVGRenderer.h` | 803 |
| `DataTableSystem.h` | 800 |
| `PhysicsTypes.h` | 779 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `VisualScriptPanel.cpp` | 1773 |
| `PerformanceProfiler.cpp` | 1609 |
| `EditorUI.cpp` | 1515 |
| `ProjectSettingsPanel.cpp` | 1501 |
| `EditorTheme.cpp` | 1456 |
| `CollaborativeEditSession.cpp` | 1373 |
| `InspectorComponentRenderers_Reflected.cpp` | 1362 |
| `InspectorPanel.cpp` | 1288 |
| `LevelStreamingSystem.cpp` | 1272 |
| `MaterialEditor.cpp` | 1269 |

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
