# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-04-05.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 244202 |
| **SparkEditor/Source** | 82914 |
| **GameModules** | 57449 |
| **Tests** | 92628 |
| **SparkConsole/src** | 1858 |
| **SparkShaderCompiler/src** | 533 |
| **Total C++ (excl. ThirdParty)** | **~479584** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 701 |
| Implementation files (.cpp) | 790 |
| HLSL shader files | 32 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 297 |
| Wiki pages (.md) | 103 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~858 |
| Average lines per .h file | ~571 |
| Largest codebase section | Graphics (96517 lines — 39% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 96517 | 39.5% |
| Engine (all subsystems) | 73377 | 30.0% |
| Utils | 32473 | 13.2% |
| Core | 17339 | 7.1% |
| Physics | 10101 | 4.1% |
| Audio | 5533 | 2.2% |
| Input | 3345 | 1.3% |
| SceneManager | 1886 | 0.7% |
| Enums | 1423 | 0.5% |
| Game | 1272 | 0.5% |
| Camera | 868 | 0.3% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines |
|-----------|------:|
| AI | 13121 |
| Networking | 11626 |
| ECS | 8517 |
| Gameplay | 7264 |
| Animation | 6533 |
| Scripting | 4516 |
| UI | 2524 |
| SaveSystem | 2491 |
| World | 1588 |
| Cinematic | 1525 |
| Editor | 1468 |
| Dialogue | 1359 |
| Modding | 1250 |
| Streaming | 1141 |
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
| Editor panel classes | 57 |
| Total editor lines | 82914 |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 297 |
| TEST() definitions | 3662 |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 20 |
| ENABLE_* feature toggles | 12 |
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
| `SparkEngine.cpp` | 1834 |
| `VulkanDevice.cpp` | 1728 |
| `GraphicsEngine.cpp` | 1578 |
| `D3D12Device.cpp` | 1564 |
| `D3D11Device.cpp` | 1470 |
| `GraphicsDeviceResources.cpp` | 1319 |
| `LightingSystem.cpp` | 1283 |
| `SaveSystem.cpp` | 1275 |
| `TextureSystem.cpp` | 1263 |

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
| `PhysicsTypes.h` | 779 |
| `FastNoise2SIMD.h` | 749 |
| `GraphicsEngine.h` | 725 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| `VisualScriptPanel.cpp` | 1844 |
| `EditorUI.cpp` | 1673 |
| `CollaborativeEditSession.cpp` | 1382 |
| `PerformanceProfiler.cpp` | 1332 |
| `EditorTheme.cpp` | 1325 |
| `LevelStreamingSystem.cpp` | 1272 |
| `MaterialEditor.cpp` | 1269 |
| `InspectorPanel.cpp` | 1262 |
| `GameViewPanel.cpp` | 1172 |
| `LightingTools.cpp` | 1160 |

## Shader Inventory

| Type | Count | Location |
|------|------:|----------|
| HLSL shaders | 32 | `Shaders/HLSL/` (includes Compute, MeshShaders, RayTracing) |
| GLSL shaders | 14 | `Shaders/GLSL/` |
| Compiled bytecode (.cso) | varies | `Shaders/Compiled/` |

---

## See Also

- [Architecture Overview](Architecture-Overview) — Engine design and structure
- [Codebase Health](Codebase-Health) — System maturity status and known gaps
- [Testing](Testing) — Test suite details and CI integration
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — Build configuration
