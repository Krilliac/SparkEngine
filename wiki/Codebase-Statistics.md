# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-04-06.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 259418 |
| **SparkEditor/Source** | 85014 |
| **GameModules** | 57824 |
| **Tests** | 98579 |
| **SparkConsole/src** | 1858 |
| **SparkShaderCompiler/src** | 533 |
| **Total C++ (excl. ThirdParty)** | **~503226** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 735 |
| Implementation files (.cpp) | 826 |
| HLSL shader files | 38 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 323 |
| Wiki pages (.md) | 116 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~880 |
| Average lines per .h file | ~568 |
| Largest codebase section | Graphics (99796 lines — 38% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 99796 | 38.4% |
| Engine (all subsystems) | 79585 | 30.6% |
| Utils | 35687 | 13.7% |
| Core | 19309 | 7.4% |
| Physics | 10101 | 3.8% |
| Audio | 5547 | 2.1% |
| Input | 3876 | 1.4% |
| SceneManager | 1886 | 0.7% |
| Enums | 1423 | 0.5% |
| Game | 1272 | 0.4% |
| Camera | 868 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13129 |
| Networking | 11631 |
| ECS | 8523 |
| Gameplay | 7278 |
| Animation | 6538 |
| Scripting | 4539 |
| UI | 2524 |
| SaveSystem | 2491 |
| Streaming | 1763 |
| World | 1588 |
| Cinematic | 1525 |
| Editor | 1468 |
| Dialogue | 1359 |
| Modding | 1257 |
| 2D | 979 |
| Persistence | 943 |
| Coroutine | 785 |
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
| Total editor lines | 85014 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 323 |
| TEST() definitions | 3943 |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 23 |
| ENABLE_* feature toggles | 15 |
| Game modules | 10 |
| SDK public headers | 10 |
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
| `OpenGLDevice.cpp` | 1938 |
| `SparkEngine.cpp` | 1894 |
| `VulkanDevice.cpp` | 1728 |
| `GraphicsEngine.cpp` | 1695 |
| `D3D12Device.cpp` | 1564 |
| `D3D11Device.cpp` | 1470 |
| `GraphicsDeviceResources.cpp` | 1334 |
| `CrashHandler.cpp` | 1332 |
| `LightingSystem.cpp` | 1283 |
| `SaveSystem.cpp` | 1275 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| `RenderGraph.h` | 1124 |
| `JsonUtils.h` | 963 |
| `BasisTranscoder.h` | 912 |
| `UILayoutExtensions.h` | 857 |
| `ECSystems.h` | 847 |
| `MeshClusterSystem.h` | 819 |
| `SVGRenderer.h` | 803 |
| `DataTableSystem.h` | 799 |
| `PhysicsTypes.h` | 779 |
| `EngineSettings.h` | 758 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `VisualScriptPanel.cpp` | 1850 |
| `EditorUI.cpp` | 1673 |
| `CollaborativeEditSession.cpp` | 1382 |
| `EditorTheme.cpp` | 1341 |
| `PerformanceProfiler.cpp` | 1332 |
| `LevelStreamingSystem.cpp` | 1272 |
| `MaterialEditor.cpp` | 1269 |
| `InspectorPanel.cpp` | 1262 |
| `GameViewPanel.cpp` | 1172 |
| `LightingTools.cpp` | 1160 |

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
