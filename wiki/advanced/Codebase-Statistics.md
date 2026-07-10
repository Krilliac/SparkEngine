# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-07-10.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 52,543 |
| **SparkEditor/Source** | 90,172 |
| **GameModules** | 86,021 |
| **Tests** | 5,807 |
| **SparkConsole/src** | 1,571 |
| **SparkShaderCompiler/src** | 578 |
| **Total C++ (excl. ThirdParty)** | **~236,692** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 830 |
| Implementation files (.cpp) | 1170 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 525 |
| Wiki pages (.md) | 184 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~290 |
| Average lines per .h file | ~217 |
| Largest codebase section | Graphics (114,830 lines — 218% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 114,830 | 218.5% |
| Engine (all subsystems) | 83,366 | 158.6% |
| Utils | 40,361 | 76.8% |
| Core | 23,868 | 45.4% |
| Physics | 10,648 | 20.2% |
| Audio | 6,090 | 11.5% |
| Input | 3,948 | 7.5% |
| SceneManager | 2,137 | 4.0% |
| Enums | 1,423 | 2.7% |
| Game | 1,266 | 2.4% |
| Camera | 963 | 1.8% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13,446 |
| Networking | 12,863 |
| ECS | 8,572 |
| Gameplay | 7,624 |
| Animation | 6,683 |
| Scripting | 4,950 |
| SaveSystem | 2,629 |
| UI | 2,461 |
| Streaming | 1,952 |
| Editor | 1,636 |
| World | 1,588 |
| Cinematic | 1,542 |
| Dialogue | 1,417 |
| Modding | 1,361 |
| Persistence | 1,066 |
| 2D | 1,010 |
| Coroutine | 800 |
| Replay | 731 |
| Tween | 570 |
| Destruction | 545 |
| Localization | 477 |
| Events | 473 |
| Mobile | 452 |
| Loading | 386 |
| Physics | 377 |
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
| Total editor lines | 90,172 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 525 |
| TEST() definitions | 6,070 |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 26 |
| ENABLE_* feature toggles | 18 |
| Game modules | 11 |
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
| `GraphicsEngineWindows.cpp` | 1568 |
| `D3D11Device.cpp` | 1543 |
| `PostProcessingPipeline.cpp` | 1538 |
| `EngineSettings.cpp` | 1522 |
| `AngelScriptEngine.cpp` | 1459 |
| `SaveSystem.cpp` | 1391 |
| `SparkEngineWindows.cpp` | 1374 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1125 |
| `EngineSettings.h` | 1079 |
| `GraphicsEngine.h` | 1059 |
| `JsonUtils.h` | 963 |
| `BasisTranscoder.h` | 912 |
| `ECSystems.h` | 846 |
| `PhysicsTypes.h` | 828 |
| `MeshClusterSystem.h` | 824 |
| `SVGRenderer.h` | 803 |
| `DataTableSystem.h` | 800 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `VisualScriptPanel.cpp` | 1773 |
| `EditorUI.cpp` | 1674 |
| `PerformanceProfiler.cpp` | 1609 |
| `EditorTheme.cpp` | 1587 |
| `ProjectSettingsPanel.cpp` | 1501 |
| `CollaborativeEditSession.cpp` | 1497 |
| `InspectorPanel.cpp` | 1401 |
| `InspectorComponentRenderers_Reflected.cpp` | 1362 |
| `HierarchyPanel.cpp` | 1339 |
| `LevelStreamingSystem.cpp` | 1272 |

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
