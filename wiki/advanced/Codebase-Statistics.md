# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-08-23.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 292680 |
| **SparkEditor/Source** | 97889 |
| **GameModules** | 128722 |
| **Tests** | 145703 |
| **SparkConsole/src** | 1571 |
| **SparkShaderCompiler/src** | 578 |
| **Total C++ (excl. ThirdParty)** | **~667143** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 948 |
| Implementation files (.cpp) | 1389 |
| HLSL shader files | 42 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 536 |
| Wiki pages (.md) | 198 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~688 |
| Average lines per .h file | ~567 |
| Largest codebase section | Graphics (117091 lines — 40% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 117091 | 40.0% |
| Engine (all subsystems) | 84048 | 28.7% |
| Utils | 39925 | 13.6% |
| Core | 24867 | 8.4% |
| Physics | 10808 | 3.6% |
| Audio | 6090 | 2.0% |
| Input | 3953 | 1.3% |
| SceneManager | 2142 | 0.7% |
| Enums | 1423 | 0.4% |
| Game | 1266 | 0.4% |
| Camera | 999 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13490 |
| Networking | 13143 |
| ECS | 8307 |
| Gameplay | 7652 |
| Animation | 6700 |
| Scripting | 5093 |
| SaveSystem | 2627 |
| UI | 2522 |
| Streaming | 2009 |
| Editor | 1712 |
| World | 1588 |
| Cinematic | 1542 |
| Dialogue | 1417 |
| Modding | 1377 |
| Persistence | 1235 |
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
| Total editor lines | 97889 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 536 |
| TEST() definitions | 6199 |
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
| `D3D12Device.cpp` | 1593 |
| `D3D11Device.cpp` | 1543 |
| `PostProcessingPipeline.cpp` | 1539 |
| `EngineSettings.cpp` | 1522 |
| `AngelScriptEngine.cpp` | 1473 |
| `CrashHandler.cpp` | 1456 |
| `SaveSystem.cpp` | 1389 |
| `GameplayLifecycleShared.cpp` | 1386 |

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
