# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-04-09.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 264419 |
| **SparkEditor/Source** | 86744 |
| **GameModules** | 58451 |
| **Tests** | 107806 |
| **SparkConsole/src** | 1858 |
| **SparkShaderCompiler/src** | 533 |
| **Total C++ (excl. ThirdParty)** | **~519811** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 748 |
| Implementation files (.cpp) | 867 |
| HLSL shader files | 38 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 345 |
| Wiki pages (.md) | 125 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~860 |
| Average lines per .h file | ~569 |
| Largest codebase section | Graphics (101631 lines — 38% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 101631 | 38.4% |
| Engine (all subsystems) | 80172 | 30.3% |
| Utils | 36969 | 13.9% |
| Core | 20593 | 7.7% |
| Physics | 10101 | 3.8% |
| Audio | 5548 | 2.0% |
| Input | 3888 | 1.4% |
| SceneManager | 1886 | 0.7% |
| Enums | 1423 | 0.5% |
| Game | 1272 | 0.4% |
| Camera | 868 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13133 |
| Networking | 11853 |
| ECS | 8546 |
| Gameplay | 7511 |
| Animation | 6534 |
| Scripting | 4539 |
| UI | 2521 |
| SaveSystem | 2491 |
| Streaming | 1763 |
| World | 1588 |
| Cinematic | 1526 |
| Editor | 1495 |
| Dialogue | 1384 |
| Modding | 1263 |
| 2D | 979 |
| Persistence | 955 |
| Coroutine | 786 |
| Replay | 705 |
| Tween | 516 |
| Destruction | 509 |
| Localization | 428 |
| Mobile | 421 |
| Physics | 381 |
| Events | 362 |
| Loading | 354 |
| VR | 296 |

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
| Total editor lines | 86744 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 345 |
| TEST() definitions | 4434 |
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
| `OpenGLDevice.cpp` | 1959 |
| `SparkEngine.cpp` | 1936 |
| `VulkanDevice.cpp` | 1835 |
| `GraphicsEngine.cpp` | 1695 |
| `D3D12Device.cpp` | 1565 |
| `EngineSettings.cpp` | 1554 |
| `D3D11Device.cpp` | 1471 |
| `GraphicsDeviceResources.cpp` | 1335 |
| `CrashHandler.cpp` | 1322 |
| `LightingSystem.cpp` | 1283 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1124 |
| `EngineSettings.h` | 1079 |
| `JsonUtils.h` | 963 |
| `BasisTranscoder.h` | 912 |
| `ECSystems.h` | 849 |
| `MeshClusterSystem.h` | 819 |
| `SVGRenderer.h` | 803 |
| `DataTableSystem.h` | 800 |
| `PhysicsTypes.h` | 779 |
| `FastNoise2SIMD.h` | 749 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `VisualScriptPanel.cpp` | 1851 |
| `EditorUI.cpp` | 1674 |
| `PerformanceProfiler.cpp` | 1609 |
| `ProjectSettingsPanel.cpp` | 1501 |
| `EditorTheme.cpp` | 1456 |
| `CollaborativeEditSession.cpp` | 1384 |
| `LevelStreamingSystem.cpp` | 1272 |
| `MaterialEditor.cpp` | 1269 |
| `InspectorPanel.cpp` | 1262 |
| `GameViewPanel.cpp` | 1172 |

## Shader Inventory

| Type | Count | Location |
|------|------:|----------|
| HLSL shaders | 38 | `Shaders/HLSL/` (includes Compute, MeshShaders, RayTracing) |
| GLSL shaders | 14 | `Shaders/GLSL/` |
| Compiled bytecode (.cso) | varies | `Shaders/Compiled/` |

---

## See Also

- [Architecture Overview](Architecture-Overview) — Engine design and structure
- [Codebase Health](Codebase-Health) — System maturity status and known gaps
- [Testing](Testing) — Test suite details and CI integration
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — Build configuration
