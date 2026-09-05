# Architecture Overview

SparkEngine follows a modular, service-oriented architecture where the engine executable loads game logic as dynamically linked modules (DLLs on Windows, shared libraries on Linux).

> **Release boundary:** The only declared profile is the blocked and uncertified
> `stable-v1` Windows 11 x64/MSVC v143 + D3D11/Windows NullRHI + C++ module
> product set. Architecture described beyond that boundary is implementation
> inventory, not certified support.

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
│  Host-only concrete registry/lifecycle APIs are not exposed  │
│  through the public IEngineContext module interface.         │
└─────────────────────────────────────────────────────────────┘
                            │
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
     ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
     │ SparkGame.dll│ │  Addon1.dll  │ │  Addon2.dll  │
     │ (Game kind)  │ │ (Addon kind) │ │ (Addon kind) │
     └──────────────┘ └──────────────┘ └──────────────┘
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

A process may load exactly one `ModuleKind::Game` module plus compatible `ModuleKind::Addon` modules. The manager hard-refuses a second Game-kind module; addons may coexist with the active game and with one another. Loaded modules are initialized in load-order (lower `ModuleInfo::loadOrder` values first, default is 1000), and shutdown happens in reverse order.

**Module discovery** (in order of priority):
1. Command-line argument: `-game <path>`
2. Explicit manifest: `-manifest <path>`
3. `spark.modules.json` manifest next to the executable
4. Platform-specific bare-launch candidate discovery/directory scan

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
    // Non-const on purpose: a `static const char id = 0` is identical read-only
    // data for every T, which MSVC's /OPT:ICF folds into one address in Release —
    // collapsing all type ids. A writable static keeps a distinct address per T.
    static char id;
    return &id;
}
```

#### Host-Only Generic System Registry

The following APIs belong to the concrete engine-owned `EngineContext`; they
are not members of the public `Spark::IEngineContext` passed to game modules.
Engine startup code uses them to assemble named services internally.

```cpp
// Register any subsystem by type (non-owning pointer)
template <typename T> void RegisterSystem(T* system);

// Retrieve a subsystem by type (nullptr if not registered)
template <typename T> T* GetSystem() const;
```

Concrete host bootstrap code can register and retrieve services beyond the
named public getters:

```cpp
engineContext.RegisterSystem<MyCustomManager>(&myManager);
auto* mgr = engineContext.GetSystem<MyCustomManager>();
```

Game modules must use the named `IEngineContext` getters, their own owned state,
or an explicit public addon/event interface; they cannot downcast the public
context and depend on this private host registry.

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
    [&audio]{ return audio.Initialize(); },
    [&audio]{ audio.Shutdown(); });

ctx->RegisterSubsystem<PhysicsSystem>(&physics,
    DependsOn<Timer>{},
    [&physics]{ return physics.Initialize(); },
    [&physics]{ physics.Shutdown(); });

// Initialize all in topological (dependency) order
ctx->InitializeAll();

// Shutdown in reverse order
ctx->ShutdownAll();
```

`InitializeAll()` performs a topological sort of subsystem entries based on declared dependencies. It returns `false` if a dependency cycle is detected or any subsystem fails to initialize.

> **Not the production init path.** `EngineContext::InitializeAll`/`ShutdownAll` (the R1.2 dependency-ordered registry) is kept because `Tests/harden/Test_tests_enginecontext_real.cpp` and six other test files exercise it. The production order is `LifecycleCompositionRoot` (`Core/Lifecycle/`), whose `InitDebug` stage now sorts first (`LifecycleOrder::Diagnostics`); the gameplay lifecycle registers the engine-lifetime services on `EngineContext` and nulls them at shutdown.

#### Concrete EngineContext API Summary

This table describes host-internal construction and teardown, not the public
module-facing `IEngineContext` contract.

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
│ RHI (DX11,      │ Jolt Physics     │ XAudio2/OpenAL/  │
│   Vulkan, GL)   │ Collision        │ 3D Spatial       │
│ PBR Materials   │ Raycasting       │ Null audio       │
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
| **Rendering** | `GraphicsEngine` | DX11 primary implementation; Vulkan and OpenGL have substantive experimental RHI device/resource implementations; PBR materials, shadow maps, post-processing pipeline |
| **Physics** | `PhysicsSystem` | Jolt Physics wrapper; rigid bodies, collision detection, raycasting, trigger volumes |
| **Audio** | `AudioEngine` | Concrete service returned by `IEngineContext::GetAudio()`; the separate `IAudioBackend` factory selects XAudio2/OpenAL/Null implementations for host wiring |
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

Components are EnTT-attached, data-oriented structs. Some own standard-library
containers or expose validation/transform/tag helpers, so the inventory is not
uniformly POD or behavior-free. Systems query and mutate component state each
frame; cross-system interaction commonly flows through shared components and
events.

## Project Structure

```
SparkEngine/
├── SparkEngine/              # Main engine executable + core library
│   └── Source/
│       ├── Audio/            # XAudio2, OpenAL, and Null backend implementations
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
├── SparkEditor/              # Windows 11 x64 stable-v1 target; experimental Linux SDL2/OpenGL path
├── GameModules/              # 11 in-tree module directories
│   ├── SparkGameFPS/         # Blocked stable-v1 candidate
│   ├── SparkGameMMO/         # Outside-profile prototype surface
│   └── ...                   # Nine additional prototype/template/showcase modules
├── SparkConsole/             # Standalone debug console
├── SparkShaderCompiler/      # Offline shader compilation tool
├── SparkBuild/               # Terminal-UI CMake configurator (C++17, in-tree)
├── SparkSDK/                 # Public SDK headers for module development
├── Templates/                # Game module templates (EmptyProject)
├── ThirdParty/               # Audited dependencies: six submodules plus vendored snapshots
├── Assets/                   # Models, Scenes, Scripts
├── Shaders/                  # HLSL, GLSL, Compiled bytecode
├── Tests/                    # Test-bearing sources and internal test framework
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

CMake-based with declared build options, including several compatibility cache
variables that are not source-selection switches. See [Build System and CMake
Modules](../advanced/Build-System-and-CMake-Modules.md) for the source-backed set.

Key build targets:

| Target | Type | Description |
|--------|------|-------------|
| `SparkEngineLib` | Static library | All engine subsystems |
| `SparkEngine` | Executable | Runtime host |
| `SparkGame` | Shared library | Default game module |
| `SparkEditor` | Executable | Windows 11 x64 stable-v1 target; experimental Linux SDL2/OpenGL path |
| `SparkTests` | Executable | Unit test runner |

## Dependencies

`ThirdParty/dependencies.lock` is the authoritative audited dependency
manifest. The selected linked or compiled surfaces below are implementation
inventory, not support certification.

| Library | Purpose |
|---------|---------|
| EnTT | Entity Component System |
| Jolt Physics | Physics simulation |
| Dear ImGui, ImGuizmo, imnodes | Editor and node/gizmo UI |
| AngelScript | Gameplay scripting |
| SDL2 | Experimental non-Windows window/input path |
| Recast/Detour | Navigation backend |
| miniz and zstd | Compression paths |
| stb_image, cgltf, tinyobjloader, tinyexr | Image/model import paths |
| nlohmann/json | JSON parsing when available |
| VulkanMemoryAllocator and glad | Experimental Vulkan/OpenGL backend support |
| miniaudio | Linked implementation surface; not the active audio-factory fallback |

---

## Codebase Scale

The deterministic source inventory produced by
`python docs/codebase-metrics.py --shell` currently reports:

| Inventory metric | Value |
|---|---:|
| Total source lines (`TOTAL_LINES`) | 760,331 |
| Scanned source files (`FILE_COUNT`) | 2,583 |
| Engine lines (`ENGINE_LINES`) | 311,582 |
| Editor lines (`EDITOR_LINES`) | 109,363 |
| Game/template lines (`GAME_LINES`) | 143,040 |
| Test-source lines (`TEST_LINES`) | 169,319 |
| Tool-source lines (`TOOL_LINES`) | 13,450 |

These are checkout source-inventory metrics, not claims of build success, test
execution, platform support, or release readiness. For detailed metrics, see
[Codebase Statistics](../advanced/Codebase-Statistics.md).

## See Also

- [Entity Component System](../subsystems/Entity-Component-System.md) -- ECS architecture using EnTT
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Render pipelines, PBR materials, post-processing
- [Physics](../subsystems/Physics.md) -- Jolt Physics integration
- [Audio](../subsystems/Audio.md) -- concrete `AudioEngine` service and the separate XAudio2/OpenAL/Null backend factory
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
