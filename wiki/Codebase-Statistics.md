# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated 2026-04-03.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | 227,389 |
| **SparkEditor/Source** | 81,794 |
| **GameModules** | 56,786 |
| **Tests** | 78,814 |
| **SparkConsole/src** | 1,858 |
| **SparkShaderCompiler/src** | 533 |
| **Total C++ (excl. ThirdParty)** | **~447,174** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | 667 |
| Implementation files (.cpp) | 725 |
| HLSL shader files | 32 |
| GLSL shader files | 14 |
| AngelScript files (.as) | 1 |
| Test files (.cpp) | 244 |
| Wiki pages (.md) | 88 |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~243 |
| Average lines per .h file | ~167 |
| Largest codebase section | Graphics (~90,000 lines — 40% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | 83,986 | 41.3% |
| Engine (all subsystems) | 60,445 | 29.7% |
| Utils | 25,128 | 12.4% |
| Core | 10,670 | 5.2% |
| Physics | 9,806 | 4.8% |
| Audio | 4,553 | 2.2% |
| Input | 3,244 | 1.6% |
| SceneManager | 1,886 | 0.9% |
| Enums | 1,407 | 0.7% |
| Game | 1,256 | 0.6% |
| Camera | 868 | 0.4% |

### Engine Subsystems (SparkEngine/Source/Engine/)

| Subsystem | Lines | Key Classes |
|-----------|------:|-------------|
| AI | 12,372 | AISystem, BehaviorTree, NavMesh, Perception, Steering |
| Networking | 10,585 | NetworkManager, AreaServer, WorldServer, Replication |
| ECS | 7,529 | World, SystemManager, 80+ Components, 24 Systems |
| Animation | 6,037 | AnimationSystem, StateMachine, IK, Blending |
| Gameplay | 3,821 | WeaponManager, Inventory, Quests |
| Scripting | 2,604 | AngelScriptEngine, HotReload, VisualScript |
| SaveSystem | 2,455 | SaveSystem, Serialization, Compression |
| UI | 1,658 | UISystem, Widgets |
| World | 1,556 | WorldOriginSystem, DayNight, Weather |
| Dialogue | 1,340 | DialogueSystem, DialogueTree |
| Cinematic | 1,134 | Sequencer, Timeline |
| Modding | 1,106 | ModLoader, ModManager |
| 2D | 979 | SpriteRenderer, Physics2D |
| Coroutine | 785 | CoroutineScheduler |
| Replay | 701 | ReplaySystem |
| Streaming | 658 | SeamlessAreaManager, SceneTransition |
| Tween | 507 | TweenSystem, Easing |
| Destruction | 495 | DestructionSystem |
| Localization | 427 | LocalizationManager |
| Mobile | 421 | MobilePlatform |
| Events | 362 | EventBus, EventSystem |
| Loading | 349 | LoadingScreen |
| VR | 292 | VRSystem (OpenXR stub) |

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
| Editor panel header files | 57 |
| Editor panel classes | 57 |
| Total editor lines | 81,794 |
| Console command registrations | 200+ |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | 244 |
| TEST() definitions | 3,119 |
| Subsystems covered | All major |
| CI configurations | 14 jobs |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | 21 |
| ENABLE_* feature toggles | 13 |
| CI jobs | 14 |
| Supported compilers | MSVC v143/v144, GCC 13+, Clang 17+, Apple Clang, MinGW-w64 |
| Platforms | Windows, Linux, macOS (experimental) |

## Third-Party Dependencies

### Git Submodules (6)

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
| OpenGLDevice.cpp | 1,921 |
| SparkEngine.cpp | 1,767 |
| VulkanDevice.cpp | 1,571 |
| D3D12Device.cpp | 1,511 |
| GraphicsEngine.cpp | 1,499 |
| D3D11Device.cpp | 1,430 |
| GraphicsDeviceResources.cpp | 1,319 |
| LightingSystem.cpp | 1,283 |
| SaveSystem.cpp | 1,275 |

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
| RenderGraph.h | 1,124 |
| JsonUtils.h | 963 |
| BasisTranscoder.h | 912 |
| ECSystems.h | 847 |
| MeshClusterSystem.h | 819 |
| SVGRenderer.h | 803 |
| PhysicsTypes.h | 779 |
| FastNoise2SIMD.h | 749 |
| GraphicsEngine.h | 711 |

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
| VisualScriptPanel.cpp | 1,844 |
| EditorUI.cpp | 1,608 |
| CollaborativeEditSession.cpp | 1,382 |
| PerformanceProfiler.cpp | 1,332 |
| EditorTheme.cpp | 1,325 |
| LevelStreamingSystem.cpp | 1,272 |
| MaterialEditor.cpp | 1,269 |
| InspectorPanel.cpp | 1,262 |
| GameViewPanel.cpp | 1,172 |

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
