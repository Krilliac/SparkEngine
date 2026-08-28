# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-08-28.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 311571 |
| **SparkEditor/Source** | 109363 |
| **GameModules** | 143040 |
| **External services** | 11178 |
| **Asset pipeline** | 2399 |
| **Tests** | 169307 |
| **SparkConsole/src** | 1633 |
| **SparkShaderCompiler/src** | 588 |
| **Total C++ (excl. ThirdParty)** | **~760308** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hh/.hpp/.hxx/.inl) | 1050 |
| Implementation files (.c/.cc/.cpp/.cxx/.mm) | 1533 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 575 |
| Wiki pages (.md) | 198 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~712 |
| Average lines per .h file | ~583 |
| Largest codebase section | Graphics (119441 lines — 38% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 119441 | 38.3% |
| Engine (all subsystems) | 87621 | 28.1% |
| Utils | 42446 | 13.6% |
| Core | 29486 | 9.4% |
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
| Networking | 14943 |
| AI | 13490 |
| ECS | 8447 |
| Gameplay | 7755 |
| Animation | 6700 |
| Scripting | 5093 |
| SaveSystem | 3471 |
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
| Total editor lines | 109363 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 575 |
| TEST() definitions | 6951 |
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
| `CrashHandler.cpp` | 2093 |
| `SaveSystem.cpp` | 2036 |
| `VulkanDevice.cpp` | 1991 |
| `ModuleManager.cpp` | 1777 |
| `NetworkConnection.cpp` | 1678 |
| `EngineSettings.cpp` | 1623 |
| `PostProcessingPipeline.cpp` | 1602 |
| `D3D12Device.cpp` | 1593 |
| `D3D11Device.cpp` | 1543 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1427 |
| `GraphicsEngine.h` | 1260 |
| `JsonUtils.h` | 1087 |
| `EngineSettings.h` | 1080 |
| `NetworkManager.h` | 920 |
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
