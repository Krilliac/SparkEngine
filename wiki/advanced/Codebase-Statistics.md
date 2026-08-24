# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-08-24.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 62181 |
| **SparkEditor/Source** | 103884 |
| **GameModules** | 43104 |
| **Tests** | 6194 |
| **SparkConsole/src** | 1571 |
| **SparkShaderCompiler/src** | 578 |
| **Total C++ (excl. ThirdParty)** | **~217512** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 966 |
| Implementation files (.cpp) | 1415 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 554 |
| Wiki pages (.md) | 198 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~290 |
| Average lines per .h file | ~237 |
| Largest codebase section | Graphics (118680 lines — 190% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 118680 | 190.8% |
| Engine (all subsystems) | 85069 | 136.8% |
| Utils | 40653 | 65.3% |
| Core | 26792 | 43.0% |
| Physics | 10814 | 17.3% |
| Audio | 6090 | 9.7% |
| Input | 3953 | 6.3% |
| SceneManager | 2241 | 3.6% |
| Enums | 1423 | 2.2% |
| Game | 1734 | 2.7% |
| Camera | 999 | 1.6% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| Networking | 13806 |
| AI | 13490 |
| ECS | 8381 |
| Gameplay | 7652 |
| Animation | 6700 |
| Scripting | 5093 |
| SaveSystem | 2746 |
| UI | 2522 |
| Streaming | 2009 |
| Editor | 1737 |
| Cinematic | 1634 |
| World | 1588 |
| Dialogue | 1417 |
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
| Component header files | 17 |
| Component struct definitions | 75 |
| ECS systems | 25 |
| Execution order | Physics → Animation → AI → Audio → Lifecycle → Render |

## Editor Metrics

| Metric | Count |
|--------|------:|
| Editor panel classes | 64 |
| Total editor lines | 103884 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 554 |
| TEST() definitions | 6436 |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 26 |
| ENABLE_* feature toggles | 18 |
| Game modules | 11 |
| SDK public headers | 13 |
| Supported compilers | MSVC v143/v145, GCC 13+, Clang 17+, Apple Clang, MinGW-w64 |
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
| `OpenGLDevice.cpp` | 2160 |
| `VulkanDevice.cpp` | 1991 |
| `PostProcessingPipeline.cpp` | 1602 |
| `D3D12Device.cpp` | 1593 |
| `D3D11Device.cpp` | 1543 |
| `EngineSettings.cpp` | 1522 |
| `SaveSystem.cpp` | 1508 |
| `ModuleManager.cpp` | 1503 |
| `AngelScriptEngine.cpp` | 1473 |
| `CrashHandler.cpp` | 1456 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `GraphicsEngine.h` | 1260 |
| `JsonUtils.h` | 1087 |
| `EngineSettings.h` | 1079 |
| `NetworkManager.h` | 856 |
| `ECSystems.h` | 846 |
| `PhysicsTypes.h` | 828 |
| `MeshClusterSystem.h` | 824 |
| `SVGRenderer.h` | 803 |
| `DataTableSystem.h` | 800 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `ProjectManager.cpp` | 2161 |
| `EditorUI.cpp` | 2136 |
| `JSONSceneSerializer.cpp` | 2010 |
| `VisualScriptPanel.cpp` | 1773 |
| `PerformanceProfiler.cpp` | 1609 |
| `EditorTheme.cpp` | 1587 |
| `HierarchyPanel.cpp` | 1524 |
| `ProjectSettingsPanel.cpp` | 1501 |
| `CollaborativeEditSession.cpp` | 1498 |
| `InspectorPanel.cpp` | 1437 |

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
