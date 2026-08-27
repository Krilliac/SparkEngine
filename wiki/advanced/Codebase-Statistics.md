# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-08-27.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 308080 |
| **SparkEditor/Source** | 108813 |
| **GameModules** | 142900 |
| **External services** | 10426 |
| **Asset pipeline** | 2399 |
| **Tests** | 164187 |
| **SparkConsole/src** | 1633 |
| **SparkShaderCompiler/src** | 588 |
| **Total C++ (excl. ThirdParty)** | **~747768** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 997 |
| Implementation files (.cpp) | 1461 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 573 |
| Wiki pages (.md) | 198 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~705 |
| Average lines per .h file | ~580 |
| Largest codebase section | Graphics (119374 lines — 38% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 119374 | 38.7% |
| Engine (all subsystems) | 85648 | 27.8% |
| Utils | 41153 | 13.3% |
| Core | 29328 | 9.5% |
| Physics | 10814 | 3.5% |
| Audio | 6090 | 1.9% |
| Input | 3953 | 1.2% |
| SceneManager | 2241 | 0.7% |
| Enums | 1423 | 0.4% |
| Game | 2794 | 0.9% |
| Camera | 999 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| Networking | 14176 |
| AI | 13490 |
| ECS | 8381 |
| Gameplay | 7755 |
| Animation | 6700 |
| Scripting | 5093 |
| SaveSystem | 2826 |
| UI | 2522 |
| Streaming | 2009 |
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
| Component header files | 17 |
| Component struct definitions | 75 |
| ECS systems | 25 |
| Execution order | Physics → Animation → AI → Audio → Lifecycle → Render |

## Editor Metrics

| Metric | Count |
|--------|------:|
| Editor panel classes | 65 |
| Total editor lines | 108813 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 573 |
| TEST() definitions | 6844 |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 30 |
| ENABLE_* feature toggles | 22 |
| Game modules | 11 |
| SDK public headers | 15 |
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
| `ModuleManager.cpp` | 1777 |
| `EngineSettings.cpp` | 1623 |
| `PostProcessingPipeline.cpp` | 1602 |
| `D3D12Device.cpp` | 1593 |
| `SaveSystem.cpp` | 1558 |
| `D3D11Device.cpp` | 1543 |
| `AngelScriptEngine.cpp` | 1473 |
| `CrashHandler.cpp` | 1469 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `GraphicsEngine.h` | 1260 |
| `JsonUtils.h` | 1087 |
| `EngineSettings.h` | 1080 |
| `NetworkManager.h` | 887 |
| `ECSystems.h` | 846 |
| `PhysicsTypes.h` | 828 |
| `MeshClusterSystem.h` | 824 |
| `SVGRenderer.h` | 803 |
| `DataTableSystem.h` | 800 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `ProjectManager.cpp` | 2482 |
| `EditorUI.cpp` | 2247 |
| `JSONSceneSerializer.cpp` | 2010 |
| `VisualScriptPanel.cpp` | 1773 |
| `BuildPipeline.cpp` | 1659 |
| `CollaborativeEditSession.cpp` | 1613 |
| `PerformanceProfiler.cpp` | 1609 |
| `EditorTheme.cpp` | 1587 |
| `HierarchyPanel.cpp` | 1524 |
| `ProjectSettingsPanel.cpp` | 1501 |

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
