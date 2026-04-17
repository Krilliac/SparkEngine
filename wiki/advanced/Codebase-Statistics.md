# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-04-17.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 277841 |
| **SparkEditor/Source** | 88747 |
| **GameModules** | 58460 |
| **Tests** | 133854 |
| **SparkConsole/src** | 1868 |
| **SparkShaderCompiler/src** | 533 |
| **Total C++ (excl. ThirdParty)** | **~561303** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 764 |
| Implementation files (.cpp) | 1061 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 474 |
| Wiki pages (.md) | 137 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~775 |
| Average lines per .h file | ~580 |
| Largest codebase section | Graphics (110103 lines — 39% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 110103 | 39.6% |
| Engine (all subsystems) | 80475 | 28.9% |
| Utils | 39686 | 14.2% |
| Core | 22436 | 8.0% |
| Physics | 10101 | 3.6% |
| Audio | 5628 | 2.0% |
| Input | 3895 | 1.4% |
| SceneManager | 1886 | 0.6% |
| Enums | 1423 | 0.5% |
| Game | 1272 | 0.4% |
| Camera | 868 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13225 |
| Networking | 12065 |
| ECS | 8544 |
| Gameplay | 7511 |
| Animation | 6554 |
| Scripting | 4543 |
| UI | 2437 |
| SaveSystem | 2409 |
| Streaming | 1781 |
| World | 1588 |
| Editor | 1556 |
| Cinematic | 1526 |
| Dialogue | 1384 |
| Modding | 1263 |
| 2D | 979 |
| Persistence | 974 |
| Coroutine | 786 |
| Replay | 705 |
| Tween | 516 |
| Destruction | 509 |
| Localization | 428 |
| Mobile | 421 |
| Physics | 381 |
| Events | 362 |
| Loading | 355 |
| VR | 318 |

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
| Total editor lines | 88747 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 474 |
| TEST() definitions | 5867 |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 23 |
| ENABLE_* feature toggles | 15 |
| Game modules | 10 |
| SDK public headers | 11 |
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
| `OpenGLDevice.cpp` | 2100 |
| `VulkanDevice.cpp` | 1925 |
| `D3D12Device.cpp` | 1577 |
| `PostProcessingPipeline.cpp` | 1530 |
| `D3D11Device.cpp` | 1486 |
| `EngineSettings.cpp` | 1478 |
| `GraphicsEngineWindows.cpp` | 1435 |
| `GameplayLifecycleShared.cpp` | 1342 |
| `CrashHandler.cpp` | 1322 |
| `AngelScriptEngine.cpp` | 1232 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1125 |
| `EngineSettings.h` | 1079 |
| `JsonUtils.h` | 963 |
| `BasisTranscoder.h` | 912 |
| `GraphicsEngine.h` | 877 |
| `ECSystems.h` | 849 |
| `MeshClusterSystem.h` | 824 |
| `SVGRenderer.h` | 803 |
| `DataTableSystem.h` | 800 |
| `PhysicsTypes.h` | 779 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `EditorUI.cpp` | 1777 |
| `VisualScriptPanel.cpp` | 1773 |
| `PerformanceProfiler.cpp` | 1609 |
| `ProjectSettingsPanel.cpp` | 1501 |
| `EditorTheme.cpp` | 1456 |
| `CollaborativeEditSession.cpp` | 1384 |
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

- [Architecture Overview](Architecture-Overview) — Engine design and structure
- [Codebase Health](Codebase-Health) — System maturity status and known gaps
- [Testing](Testing) — Test suite details and CI integration
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — Build configuration
