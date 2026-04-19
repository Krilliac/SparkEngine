# Architecture Overview

SparkEngine follows a modular, service-oriented architecture where the engine executable loads game logic as dynamically linked modules (DLLs on Windows, shared libraries on Linux).

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│              SparkEngine (Main Executable)                   │
│  • Initializes all subsystems via EngineContext              │
│  • Loads game modules dynamically (DLL/SO)                  │
│  • Runs the main loop                                       │
│  • Manages lifecycle (Initialize → MainLoop → Shutdown)     │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│              EngineContext (Service Locator)                  │
│                                                              │
│  Named getters:                                              │
│  • GetGraphics()      → GraphicsEngine*                      │
│  • GetInput()         → InputManager*                        │
│  • GetTimer()         → Timer*                               │
│  • GetEventBus()      → EventBus*                            │
│  • GetAudio()         → AudioEngine*                         │
│  • GetPhysics()       → PhysicsSystem*                       │
│  • GetAnimation()     → AnimationSystem*                     │
│  • GetAI()            → AISystem*                            │
│  • GetNetwork()       → NetworkManager*                      │
│  • GetSceneManager()  → SceneManager*                        │
│  • GetScriptEngine()  → AngelScriptEngine*                   │
│  • GetSaveSystem()    → SaveSystem*                          │
│  • GetCoroutineScheduler() → CoroutineScheduler*             │
│                                                              │
│  Generic registry:                                           │
│  • RegisterSystem<T>(ptr) / GetSystem<T>()                   │
│  • RegisterSubsystem<T>(ptr, DependsOn<...>{}, init, shut)  │
│  • InitializeAll() / ShutdownAll()                           │
└─────────────────────────────────────────────────────────────┘
                            │
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
     ┌──────────────┐ ┌──────────┐ ┌──────────────┐
     │ SparkGame.dll│ │Module2.dll│ │ Module3.dll  │
     │ (IModule)    │ │ (IModule) │ │  (IModule)   │
     └──────────────┘ └──────────┘ └──────────────┘
```

## Module System

Game logic is compiled as separate shared libraries that the engine loads at runtime. Each module implements the `Spark::IModule` interface:

```cpp
class IModule
{
public:
    virtual ModuleInfo GetModuleInfo() const = 0;  // Name, version, load order
    virtual bool OnLoad(IEngineContext* context) = 0;  // Initialization
    virtual void OnUnload() = 0;                     // Cleanup
    virtual void OnUpdate(float deltaTime) = 0;      // Per-frame logic
    virtual void OnRender() {}                       // Optional rendering
    virtual void OnResize(int w, int h) {}           // Optional resize handling
};
```

**Lifecycle order:**
1. `CreateModule()` -- Allocate module instance (DLL export)
2. `GetModuleInfo()` -- Read metadata, validate SDK version
3. `OnLoad(context)` -- Module receives the engine context
4. `OnUpdate()` / `OnRender()` -- Called every frame
5. `OnUnload()` -- Cleanup before DLL unload
6. `DestroyModule()` -- Deallocate module instance (DLL export)

Multiple modules can be loaded simultaneously and are initialized in load-order (lower `ModuleInfo::loadOrder` values first, default is 1000). Shutdown happens in reverse order.

**Module discovery** (in order of priority):
1. Command-line argument: `-game <path>`
2. `spark.modules.json` manifest next to the executable
3. Directory scan for `*Game*.dll` or `*Module*.dll`

See [Creating a Game Module](Creating-a-Game-Module.md) for a step-by-step guide.

## Service Locator Pattern

### IEngineContext Interface

The `IEngineContext` interface provides centralized access to engine subsystems:

```cpp
class IEngineContext
{
public:
    virtual GraphicsEngine*     GetGraphics()    = 0;
    virtual InputManager*       GetInput()       = 0;
    virtual Timer*              GetTimer()       = 0;
    virtual EventBus*           GetEventBus()    = 0;
    virtual AudioEngine*        GetAudio()       = 0;
    virtual PhysicsSystem*      GetPhysics()     = 0;
    virtual AnimationSystem*    GetAnimation()   = 0;
    virtual AISystem*           GetAI()          = 0;
    virtual NetworkManager*     GetNetwork()     = 0;
    virtual SceneManager*       GetSceneManager()= 0;
    virtual AngelScriptEngine*  GetScriptEngine()= 0;
    virtual SaveSystem*         GetSaveSystem()  = 0;
    virtual CoroutineScheduler* GetCoroutineScheduler() = 0;
    virtual uint32_t            GetEngineVersion() const = 0;
    virtual uint32_t            GetSDKVersion() const = 0;
    virtual bool                IsHeadless() const = 0;
};
```

### EngineContext Implementation

**Source:** `SparkEngine/Source/Core/EngineContext.h`

The concrete `EngineContext` class implements `IEngineContext` using a unified generic registry as the single source of truth. Named getters delegate to this registry internally.

#### Type ID System

Instead of RTTI (`std::type_index`), the context uses a compile-time type ID that works with incomplete (forward-declared) types:

```cpp
using TypeId = const void*;

template <typename T> TypeId GetTypeId()
{
    static const char id = 0;
    return &id;
}
```

#### Generic System Registry

```cpp
// Register any subsystem by type (non-owning pointer)
template <typename T> void RegisterSystem(T* system);

// Retrieve a subsystem by type (nullptr if not registered)
template <typename T> T* GetSystem() const;
```

This allows game modules to register and retrieve custom subsystems beyond the named getters:

```cpp
context->RegisterSystem<MyCustomManager>(&myManager);
auto* mgr = context->GetSystem<MyCustomManager>();
```

#### Dependency-Aware Subsystem Registration

The `RegisterSubsystem()` method supports declaring dependencies between subsystems for ordered initialization and shutdown:

```cpp
template <typename... Deps>
struct DependsOn {};

struct SubsystemEntry
{
    TypeId type;
    std::string name;
    std::vector<TypeId> dependencies;
    std::function<bool()> initFn;      // Called during InitializeAll()
    std::function<void()> shutdownFn;  // Called during ShutdownAll()
    bool initialized = false;
};
```

Usage:

```cpp
// Register with dependencies
ctx->RegisterSubsystem<AudioEngine>(&audio,
    DependsOn<Timer, Spark::EventBus>{},
    []{ return audio.Initialize(); },
    []{ audio.Shutdown(); });

ctx->RegisterSubsystem<PhysicsSystem>(&physics,
    DependsOn<Timer>{},
    []{ return physics.Initialize(); },
    []{ physics.Shutdown(); });

// Initialize all in topological (dependency) order
ctx->InitializeAll();

// Shutdown in reverse order
ctx->ShutdownAll();
```

`InitializeAll()` performs a topological sort of subsystem entries based on declared dependencies. It returns `false` if a dependency cycle is detected or any subsystem fails to initialize.

#### EngineContext API Summary

| Method | Description |
|--------|-------------|
| `Get()` | Global singleton accessor |
| `GetOwned()` | Access owning `unique_ptr` (init/shutdown only) |
| `RegisterSystem<T>(ptr)` | Register subsystem in generic registry |
| `GetSystem<T>()` | Retrieve subsystem from generic registry |
| `RegisterSubsystem<T>(ptr, deps, init, shutdown)` | Register with dependency metadata |
| `InitializeAll()` | Init all subsystems in topological order |
| `ShutdownAll()` | Shut down in reverse order |
| `GetInitOrder()` | Get computed init order (debugging) |
| `GetSubsystemCount()` | Number of registered subsystem entries |
| Named getters/setters | `GetGraphics()`, `SetGraphics()`, etc. |

## Subsystem Architecture

```
┌──────────────────┬──────────────────┬──────────────────┐
│   RENDERING      │    PHYSICS       │      AUDIO       │
│                  │                  │                  │
│ GraphicsEngine   │ PhysicsSystem    │ AudioEngine      │
│ RHI (DX11,      │ Jolt Physics     │ XAudio2 / mini   │
│   Vulkan, GL)   │ Collision        │ 3D Spatial       │
│ PBR Materials   │ Raycasting       │ Object Pool      │
│ Post-Processing │ Rigid Bodies     │ Doppler          │
├──────────────────┼──────────────────┼──────────────────┤
│   SCRIPTING      │    INPUT & UI    │    CORE & ECS    │
│                  │                  │                  │
│ AngelScript VM   │ InputManager     │ EnTT ECS         │
│ Hot Reload       │ Gamepad Support  │ SceneManager     │
│ Engine Bindings  │ ImGui Editor     │ AssetPipeline    │
│ Visual Script    │ Input Actions    │ EngineContext     │
├──────────────────┼──────────────────┼──────────────────┤
│   GAMEPLAY       │    AI & NAV      │   NETWORKING     │
│                  │                  │                  │
│ PlayerController │ BehaviorTree     │ UDP Client/Srv   │
│ WeaponSystem     │ NavMesh (A*)     │ Prediction       │
│ VehicleSystem    │ Perception       │ Lag Compensation │
│ Inventory/Quest  │ Steering         │ Entity Replication│
├──────────────────┼──────────────────┼──────────────────┤
│   PROCEDURAL     │    ANIMATION     │    UTILITIES     │
│                  │                  │                  │
│ Noise (Perlin+)  │ Skeletal Anim    │ CrashHandler     │
│ Erosion / WFC    │ IK (3 solvers)   │ Console (200+)   │
│ Mesh Generation  │ State Machines   │ Profiler / Debug │
│ Terrain Height   │ Multi-layer Blend│ JobSystem        │
└──────────────────┴──────────────────┴──────────────────┘
```

### Subsystem Details

| Subsystem | Key Class | Description |
|-----------|-----------|-------------|
| **Rendering** | `GraphicsEngine` | DX11 primary, Vulkan/GL stubs; PBR materials, shadow maps, post-processing pipeline |
| **Physics** | `PhysicsSystem` | Jolt Physics wrapper; rigid bodies, collision detection, raycasting, trigger volumes |
| **Audio** | `AudioEngine` | XAudio2 on Windows, miniaudio fallback; 3D spatial audio, object pool, Doppler |
| **ECS** | `World`, `SystemManager` | EnTT-backed entity registry; POD components, ordered system execution |
| **Scene** | `SceneManager` | JSON scene files, hierarchy management, prefabs, async loading |
| **Events** | `EventBus` | Type-safe pub/sub with 24+ built-in event types, queued dispatch |
| **Animation** | `AnimationManager` | Skeleton/clip cache, state machines, multi-layer blend, 3 IK solvers |
| **AI** | `AISystem` | Behavior trees, NavMesh pathfinding (A*), perception, steering behaviors |
| **Scripting** | `AngelScriptEngine` | AngelScript VM, engine bindings, hot-reload in editor |
| **Input** | `InputManager` | Keyboard, mouse, gamepad; action bindings, input mapping |
| **Networking** | `NetworkManager` | UDP client/server, entity replication, lag compensation, client prediction |
| **Area Servers** | `AreaServer` / `WorldServer` | Scalable multiplayer with per-area server processes ([docs](../subsystems/Area-Server-Architecture.md)) |
| **Large Worlds** | `WorldOriginSystem` / `SeamlessAreaManager` | Origin rebasing and seamless area streaming ([docs](../subsystems/Large-World-Support.md)) |
| **Collaboration** | `CollaborativeEditSession` | Multi-user editor sessions with node locking ([docs](../subsystems/Collaborative-Editing.md)) |
| **Save** | `SaveSystem` | Game state serialization with compression |
| **Procedural** | -- | Noise generators (Perlin, Simplex), hydraulic erosion, Wave Function Collapse |

## ECS Data Flow

SparkEngine uses the **EnTT** library for its [Entity Component System](../subsystems/Entity-Component-System.md). The data flow each frame follows a fixed execution order:

```
Frame Start
    │
    ├── 1. Input        — InputManager::Update() captures keyboard/mouse/gamepad
    ├── 2. Scripts       — AngelScriptEngine calls Update() on entity scripts
    ├── 3. Physics       — PhysicsUpdateSystem steps Jolt simulation (fixed timestep)
    ├── 4. Animation     — AnimationUpdateSystem evaluates state machines, IK
    ├── 5. AI            — AIUpdateSystem ticks behavior trees, pathfinding
    ├── 6. Particles     — ParticleUpdateSystem spawns and simulates particles
    ├── 7. Audio         — AudioUpdateSystem updates 3D source positions
    ├── 8. Lifecycle     — LifecycleSystem processes death events
    ├── 9. Projectiles   — ProjectileSystem advances projectile movement
    ├── 10. Decals       — DecalSystem manages decal lifetimes
    └── 11. Rendering    — RenderSystem submits draw calls to GPU
         │
         ├── Shadow pass
         ├── G-Buffer pass
         ├── Lighting pass
         ├── Post-processing
         └── UI / HUD overlay
```

Components are pure POD structs (state only, no behavior). Systems query and mutate components each frame. Systems communicate indirectly through shared components, never by calling each other directly.

## Project Structure

```
SparkEngine/
├── SparkEngine/              # Main engine executable + core library
│   └── Source/
│       ├── Audio/            # XAudio2 3D audio engine
│       ├── Camera/           # First-person camera controller
│       ├── Console/          # Debug console integration
│       ├── Core/             # Entry point, Platform.h, EngineContext.h
│       ├── Engine/
│       │   ├── AI/           # Behavior trees, NavMesh, perception, steering
│       │   ├── Animation/    # Skeletal animation, IK, state machines
│       │   ├── Cinematic/    # Cinematic sequencer
│       │   ├── Coroutine/    # Coroutine scheduler
│       │   ├── Destruction/  # Destructible objects
│       │   ├── Dialogue/     # Dialogue system
│       │   ├── ECS/          # EnTT components and systems
│       │   │   ├── Components/  # Component headers by category
│       │   │   ├── Systems/     # ECSystems.h, Systems2D.h
│       │   │   └── ReactiveSystem.h
│       │   ├── Events/       # Publish/subscribe event bus
│       │   ├── Gameplay/     # WeaponManager, weapon definitions
│       │   ├── Localization/ # Localization system
│       │   ├── Modding/      # Mod loading system
│       │   ├── Networking/   # UDP multiplayer, AreaServer, WorldServer
│       │   ├── Procedural/   # Noise, erosion, mesh generation, WFC
│       │   ├── Replay/       # Replay recording/playback
│       │   ├── SaveSystem/   # Serialization, compression
│       │   ├── Scripting/    # AngelScript VM, hot-reload, visual scripting
│       │   ├── Stats/        # Achievement system
│       │   ├── Streaming/    # SeamlessAreaManager, SceneTransitionManager
│       │   ├── UI/           # UI system
│       │   ├── VR/           # VR system (stub)
│       │   └── World/        # WorldOriginSystem, day/night, weather
│       ├── Game/             # Player, weapons, vehicles, HUD, terrain
│       ├── Graphics/         # DX11 renderer, RHI, PBR, post-processing
│       │   ├── DecalSystem.h
│       │   ├── FogSystem.h
│       │   ├── GPUParticleSystem.h
│       │   ├── LightingSystem.h
│       │   ├── MaterialSystem.h
│       │   ├── ParticleSystem.h
│       │   ├── PostProcessingSystem.h
│       │   ├── TessellationSystem.h
│       │   ├── TextureSystem.h
│       │   ├── UpscalingSystem.h
│       │   ├── WaterSystem.h
│       │   └── WeatherSystem.h
│       ├── Input/            # Keyboard, mouse, gamepad
│       ├── Physics/          # Jolt Physics integration
│       ├── SceneManager/     # Scene hierarchy, serialization
│       └── Utils/            # Logging, profiler, crash handler, JobSystem
├── SparkEditor/              # ImGui-based visual editor (Windows only)
├── GameModules/              # Game modules
│   ├── SparkGame/            # Default game module (DLL)
│   └── SparkGameMMO/         # MMO game module (DLL)
├── SparkConsole/             # Standalone debug console
├── SparkShaderCompiler/      # Offline shader compilation tool
├── SparkBuild/               # Terminal-UI CMake configurator (C++17, in-tree)
├── SparkSDK/                 # Public SDK headers for module development
├── Templates/                # Game module templates (EmptyProject)
├── ThirdParty/               # Git submodules (15 libraries)
├── Assets/                   # Models, Scenes, Scripts
├── Shaders/                  # HLSL, GLSL, Compiled bytecode
├── Tests/                    # 244 test files, 3,119 test cases
├── Tools/                    # CLI tools (spark-cli)
├── docs/                     # API docs, gap analysis, roadmap
├── wiki/                     # Wiki documentation pages
├── cmake/                    # CMake helper modules
└── CMakeLists.txt            # Main build configuration (1000+ lines)
```

## Key Architectural Patterns

| Pattern | Usage | Implementation |
|---------|-------|---------------|
| **Service Locator** | Centralized subsystem access | `EngineContext` with `TypeId`-keyed registry |
| **ECS** | Entity-Component-System | EnTT registry, POD components, system functions |
| **Dynamic Module Loading** | Game logic in DLLs/SOs | `IModule` interface, `CreateModule`/`DestroyModule` exports |
| **Event Bus** | Decoupled system communication | Type-safe pub/sub via `std::type_index`, synchronous dispatch |
| **RHI Abstraction** | Graphics backend selection | `RHIFactory` selects D3D11, Vulkan, or OpenGL |
| **Object Pooling** | Resource reuse | Audio sources, particles, projectiles |
| **Factory Pattern** | Backend instantiation | `RHIFactory`, `Primitives::Create*()` |
| **Singleton** | Global access for managers | `AnimationManager`, `AudioEngine`, `Profiler` via `GetInstance()` |
| **Dependency Injection** | Ordered init/shutdown | `DependsOn<>` template, topological sort in `InitializeAll()` |

## Thread Safety Rules

| Subsystem | Thread Safety | Notes |
|-----------|--------------|-------|
| `SimpleConsole` | Thread-safe | Mutex-protected command buffer |
| `PhysicsSystem` | Main thread only | Jolt Physics; supports multithreaded job dispatch |
| `GraphicsEngine` | Main thread render | `std::atomic` frame state for synchronization |
| `NetworkManager` | Queue mutex | Thread-safe message I/O and handler registration |
| `EventBus` | Thread-safe subscribe/unsubscribe | Callbacks execute on publishing thread |
| `QueuedEventBus` | Thread-safe queue | Dispatch on main thread via `DispatchAll()` |
| `AnimationSystem` | Main thread only | State machines and IK must run on main thread |
| `EnTT Registry` | NOT thread-safe | All World operations from main thread only |
| `SceneManager` | Main thread only | Async load transitions on main thread |

## Build System

CMake-based with 30+ toggleable feature flags. See [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) for details.

Key build targets:

| Target | Type | Description |
|--------|------|-------------|
| `SparkEngineLib` | Static library | All engine subsystems |
| `SparkEngine` | Executable | Runtime host |
| `SparkGame` | Shared library | Default game module |
| `SparkEditor` | Executable | ImGui editor (Windows) |
| `SparkTests` | Executable | Unit test runner |

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| EnTT | 3.x | Entity Component System |
| Jolt Physics | 5.x | Physics simulation |
| Dear ImGui | Docking branch | Editor UI |
| AngelScript | 2.x | Gameplay scripting |
| Assimp | 5.x | 3D model import (FBX, glTF, OBJ) |
| GLM | 0.9+ | Math library |
| RapidJSON | 1.1+ | JSON parsing |
| spdlog | 1.x | Structured logging |
| stb | -- | Image loading (stb_image) |
| miniaudio | 0.11+ | Cross-platform audio fallback |
| DirectXTK | -- | DirectX 11 toolkit (Windows) |
| ImGuizmo | -- | 3D editor gizmos |
| imnodes | -- | Node graph editor |
| miniz | -- | Compression (save system) |
| tinyobjloader | -- | OBJ file loading |

---

## Codebase Scale

SparkEngine is a substantial codebase with ~371,500 lines of original C++ code across 2,233 source files. For detailed metrics, see [Codebase Statistics](../advanced/Codebase-Statistics.md).

| Section | Lines | Files |
|---------|------:|------:|
| SparkEngine/Source | 203,317 | ~1,100 |
| SparkEditor/Source | 72,945 | ~250 |
| GameModules | 50,630 | ~200 |
| Tests | 41,279 | 160 |
| Tools (Console + ShaderCompiler) | 2,326 | ~20 |

The largest subsystem is Graphics at 83,986 lines (41% of engine source), followed by the Engine subsystem collection at 60,445 lines covering AI, Networking, ECS, Animation, and 20+ other subsystems.

## See Also

- [Entity Component System](../subsystems/Entity-Component-System.md) -- ECS architecture using EnTT
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Render pipelines, PBR materials, post-processing
- [Physics](../subsystems/Physics.md) -- Jolt Physics integration
- [Audio](../subsystems/Audio.md) -- XAudio2 / miniaudio audio engine
- [AI and Navigation](../subsystems/AI-and-Navigation.md) -- Behavior trees, NavMesh, perception
- [Animation](../subsystems/Animation.md) -- Skeletal animation, IK, state machines
- [Networking](../subsystems/Networking.md) -- UDP multiplayer and replication
- [Input System](../subsystems/Input-System.md) -- Keyboard, mouse, and gamepad input
- [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) -- AngelScript VM and hot-reload
- [Event System](../subsystems/Event-System.md) -- Publish/subscribe event bus
- [Scene Management](../subsystems/Scene-Management.md) -- Scene hierarchy and serialization
- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) -- Model and texture loading
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) -- Shader authoring and compilation
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- ImGui-based visual editor
- [SparkConsole](../gameplay-tools/SparkConsole.md) -- Standalone debug console
- [Creating a Game Module](Creating-a-Game-Module.md) -- Building game modules
- [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) -- CMake configuration
- [Codebase Statistics](../advanced/Codebase-Statistics.md) -- Detailed codebase metrics
- [Codebase Health](../advanced/Codebase-Health.md) -- System maturity and known gaps
- [Getting Started](Getting-Started.md) -- Build and run the engine
