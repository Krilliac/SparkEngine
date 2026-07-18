# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-07-18.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 290784 |
| **SparkEditor/Source** | 97827 |
| **GameModules** | 125302 |
| **Tests** | 145586 |
| **SparkConsole/src** | 1571 |
| **SparkShaderCompiler/src** | 578 |
| **Total C++ (excl. ThirdParty)** | **~661648** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 897 |
| Implementation files (.cpp) | 1252 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 536 |
| Wiki pages (.md) | 184 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~750 |
| Average lines per .h file | ~580 |
| Largest codebase section | Graphics (116098 lines — 39% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 116098 | 39.9% |
| Engine (all subsystems) | 83687 | 28.7% |
| Utils | 39790 | 13.6% |
| Core | 24460 | 8.4% |
| Physics | 10808 | 3.7% |
| Audio | 6090 | 2.0% |
| Input | 3953 | 1.3% |
| SceneManager | 2142 | 0.7% |
| Enums | 1423 | 0.4% |
| Game | 1266 | 0.4% |
| Camera | 999 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13450 |
| Networking | 13015 |
| ECS | 8307 |
| Gameplay | 7631 |
| Animation | 6683 |
| Scripting | 5045 |
| SaveSystem | 2627 |
| UI | 2522 |
| Streaming | 1952 |
| Editor | 1712 |
| World | 1588 |
| Cinematic | 1542 |
| Dialogue | 1417 |
| Modding | 1361 |
| Persistence | 1201 |
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
| Total editor lines | 97827 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 536 |
| TEST() definitions | 6196 |
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
| `OpenGLDevice.cpp` | 2160 |
| `VulkanDevice.cpp` | 1991 |
| `GraphicsDeviceResourcesWindows.cpp` | 1904 |
| `D3D12Device.cpp` | 1593 |
| `GraphicsEngineWindows.cpp` | 1568 |
| `SparkEngineWindows.cpp` | 1562 |
| `D3D11Device.cpp` | 1543 |
| `PostProcessingPipeline.cpp` | 1538 |
| `EngineSettings.cpp` | 1522 |
| `AngelScriptEngine.cpp` | 1473 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `GraphicsEngine.h` | 1241 |
| `RenderGraph.h` | 1125 |
| `JsonUtils.h` | 1087 |
| `EngineSettings.h` | 1079 |
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
| `EditorUI.cpp` | 1686 |
| `PerformanceProfiler.cpp` | 1609 |
| `EditorTheme.cpp` | 1587 |
| `HierarchyPanel.cpp` | 1533 |
| `ProjectSettingsPanel.cpp` | 1501 |
| `CollaborativeEditSession.cpp` | 1497 |
| `InspectorPanel.cpp` | 1401 |
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
