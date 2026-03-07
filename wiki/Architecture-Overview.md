# Architecture Overview

SparkEngine follows a modular, service-oriented architecture where the engine executable loads game logic as dynamically linked modules (DLLs on Windows, shared libraries on Linux).

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│              SparkEngine (Main Executable)                   │
│  • Initializes all subsystems                               │
│  • Loads game modules dynamically (DLL/SO)                  │
│  • Runs the main loop                                       │
│  • Manages lifecycle (Initialize → MainLoop → Shutdown)     │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│              IEngineContext (Service Locator)                │
│  • GetGraphics()   → GraphicsEngine*                        │
│  • GetInput()      → InputManager*                          │
│  • GetTimer()      → Timer*                                 │
│  • GetEventBus()   → EventBus*                              │
│  • GetEngineVersion() / GetSDKVersion()                     │
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
class IModule {
public:
    virtual ModuleInfo GetModuleInfo() const = 0;
    virtual bool OnLoad(IEngineContext* context) = 0;
    virtual void OnUnload() = 0;
    virtual void OnUpdate(float deltaTime) = 0;
    virtual void OnRender() {}           // optional
    virtual void OnResize(int w, int h) {} // optional
};
```

**Lifecycle order:**
1. `OnLoad()` — Module receives the engine context
2. `OnUpdate()` — Called every frame
3. `OnRender()` — Called every frame after update
4. `OnUnload()` — Called before the DLL is unloaded

Multiple modules can be loaded simultaneously and are initialized in load-order (lower `ModuleInfo::loadOrder` values first, default is 1000).

**Module discovery** (in order of priority):
1. Command-line argument: `-game <path>`
2. `spark.modules.json` manifest next to the executable
3. Directory scan for `*Game*.dll` or `*Module*.dll`

See [Creating a Game Module](Creating-a-Game-Module) for a step-by-step guide.

## Service Locator Pattern

The `IEngineContext` interface provides centralized access to engine subsystems:

```cpp
class IEngineContext {
public:
    virtual GraphicsEngine* GetGraphics() = 0;
    virtual InputManager*   GetInput() = 0;
    virtual Timer*          GetTimer() = 0;
    virtual EventBus*       GetEventBus() = 0;
    virtual uint32_t        GetEngineVersion() const = 0;
    virtual uint32_t        GetSDKVersion() const = 0;
};
```

Game modules store the context pointer during `OnLoad()` and use it throughout their lifetime to access engine services.

## Subsystem Architecture

```
┌──────────────────┬──────────────────┬──────────────────┐
│   RENDERING      │    PHYSICS       │      AUDIO       │
│                  │                  │                  │
│ GraphicsEngine   │ PhysicsSystem    │ AudioEngine      │
│ RHI (DX11,      │ Bullet3 World    │ XAudio2 / mini   │
│   Vulkan, GL)   │ Collision        │ 3D Spatial       │
│ PBR Materials   │ Raycasting       │ Object Pool      │
│ Post-Processing │                  │                  │
├──────────────────┼──────────────────┼──────────────────┤
│   SCRIPTING      │    INPUT & UI    │    CORE & ECS    │
│                  │                  │                  │
│ AngelScript VM   │ InputManager     │ EnTT ECS         │
│ Hot Reload       │ Gamepad Support  │ SceneManager     │
│ Engine Bindings  │ ImGui Editor     │ AssetPipeline    │
├──────────────────┼──────────────────┼──────────────────┤
│   GAMEPLAY       │    AI & NAV      │   NETWORKING     │
│                  │                  │                  │
│ PlayerController │ BehaviorTree     │ UDP Client/Srv   │
│ WeaponSystem     │ NavMesh (A*)     │ Prediction       │
│ VehicleSystem    │ Perception       │ Lag Compensation │
├──────────────────┼──────────────────┼──────────────────┤
│   PROCEDURAL     │    ANIMATION     │    UTILITIES     │
│                  │                  │                  │
│ Noise (Perlin+)  │ Skeletal Anim    │ CrashHandler     │
│ Erosion / WFC    │ IK / Blending    │ Console (200+)   │
│ Mesh Generation  │ State Machines   │ Profiler / Debug │
└──────────────────┴──────────────────┴──────────────────┘
```

## ECS Data Flow

SparkEngine uses the **EnTT** library for its [Entity Component System](Entity-Component-System). The data flow each frame:

1. **Input** — `InputManager::Update()` captures keyboard/mouse/gamepad state (see [Input System](Input-System))
2. **Scripts** — `AngelScriptEngine` calls `Update()` on entity scripts (see [Scripting with AngelScript](Scripting-with-AngelScript))
3. **AI** — `AISystem` ticks behavior trees and updates steering (see [AI and Navigation](AI-and-Navigation))
4. **Physics** — `PhysicsSystem::Update()` steps the Bullet simulation (see [Physics](Physics))
5. **Animation** — `AnimationSystem` evaluates state machines and blends poses (see [Animation](Animation))
6. **Audio** — `AudioEngine` updates 3D listener and source positions (see [Audio](Audio))
7. **Rendering** — `GraphicsEngine` renders the scene with all post-processing (see [Rendering and Graphics](Rendering-and-Graphics))

Components are pure POD structs (state only, no behavior). Systems query and mutate components each frame.

## Project Structure

```
SparkEngine/
├── SparkEngine/              # Main engine executable + core library
│   └── Source/
│       ├── Audio/            # XAudio2 3D audio engine ([Audio](Audio))
│       ├── Camera/           # First-person camera controller
│       ├── Console/          # Debug console integration
│       ├── Core/             # Entry point, platform, framework
│       ├── Engine/
│       │   ├── AI/           # Behavior trees, NavMesh, perception, steering ([AI and Navigation](AI-and-Navigation))
│       │   ├── Animation/    # Skeletal animation, IK, state machines ([Animation](Animation))
│       │   ├── Cinematic/    # Cinematic sequencer ([Cinematic Sequencer](Cinematic-Sequencer))
│       │   ├── Coroutine/    # Coroutine scheduler
│       │   ├── ECS/          # EnTT entity-component system ([Entity Component System](Entity-Component-System))
│       │   ├── Events/       # Publish/subscribe event bus ([Event System](Event-System))
│       │   ├── Networking/   # UDP multiplayer, replication ([Networking](Networking))
│       │   ├── Procedural/   # Noise, erosion, mesh generation, WFC
│       │   ├── SaveSystem/   # Serialization, compression
│       │   ├── Scripting/    # AngelScript VM, hot-reload ([Scripting with AngelScript](Scripting-with-AngelScript))
│       │   └── World/        # World management, day/night
│       ├── Game/             # Player, weapons, vehicles, HUD, terrain
│       ├── Graphics/         # DX11 renderer, RHI, PBR, post-processing ([Rendering and Graphics](Rendering-and-Graphics))
│       ├── Input/            # Keyboard, mouse, gamepad ([Input System](Input-System))
│       ├── Physics/          # Bullet Physics integration ([Physics](Physics))
│       ├── SceneManager/     # Scene hierarchy, serialization ([Scene Management](Scene-Management))
│       └── Utils/            # Logging, profiler, crash handler, debug
├── SparkEditor/              # ImGui-based visual editor (Windows only) ([SparkEditor](SparkEditor))
├── SparkGame/                # Default game module (DLL)
├── SparkConsole/             # Standalone debug console ([SparkConsole](SparkConsole))
├── SparkShaderCompiler/      # Offline shader compilation tool ([Shader Pipeline](Shader-Pipeline))
├── SparkSDK/                 # Public SDK headers for module development
├── Templates/                # Game module templates (EmptyProject)
├── ThirdParty/               # Git submodules (15 libraries)
├── Assets/                   # Models, Scenes, Scripts
├── Shaders/                  # HLSL, GLSL, Compiled bytecode ([Shader Pipeline](Shader-Pipeline))
├── Tests/                    # 35 unit tests
├── Tools/                    # CLI tools (spark-cli)
├── docs/                     # Doxygen API docs, roadmap, status
├── cmake/                    # CMake helper modules
└── CMakeLists.txt            # Main build configuration (1000+ lines)
```

## Key Architectural Patterns

| Pattern | Usage |
|---------|-------|
| **Service Locator** | `IEngineContext` provides centralized access to subsystems |
| **ECS (Entity-Component-System)** | EnTT registry with POD components and system functions |
| **Dynamic Module Loading** | Game logic in DLLs/SOs loaded at runtime |
| **Event Bus** | Type-safe pub/sub for decoupled system communication |
| **RHI Abstraction** | `RHIFactory` selects between D3D11, Vulkan, OpenGL backends |
| **Object Pooling** | Audio sources and particles reuse pooled instances |
| **Factory Pattern** | `RHIFactory` for graphics backend instantiation |
| **Singleton** | `Profiler`, `AnimationManager`, `AudioEngine` via `GetInstance()` |

## Build System

CMake-based with 30+ toggleable feature flags. See [Build System and CMake Modules](Build-System-and-CMake-Modules) for details.

## Dependencies

| Library | Purpose |
|---------|---------|
| EnTT | Entity Component System |
| Bullet Physics 3 | Physics simulation |
| Dear ImGui | Editor UI (docking branch) |
| AngelScript | Gameplay scripting |
| Assimp | 3D model import (FBX, glTF) |
| GLM | Math library |
| RapidJSON | JSON parsing |
| spdlog | Structured logging |
| stb | Image loading |
| miniaudio | Cross-platform audio fallback |
| DirectXTK | DirectX 11 toolkit |
| ImGuizmo | 3D editor gizmos |
| imnodes | Node graph editor |
| miniz | Compression |
| tinyobjloader | OBJ file loading |

---

## See Also

- [Entity Component System](Entity-Component-System) — ECS architecture using EnTT
- [Rendering and Graphics](Rendering-and-Graphics) — Render pipelines, PBR materials, post-processing
- [Physics](Physics) — Bullet Physics integration
- [Audio](Audio) — XAudio2 / miniaudio audio engine
- [AI and Navigation](AI-and-Navigation) — Behavior trees, NavMesh, perception
- [Animation](Animation) — Skeletal animation, IK, state machines
- [Networking](Networking) — UDP multiplayer and replication
- [Input System](Input-System) — Keyboard, mouse, and gamepad input
- [Scripting with AngelScript](Scripting-with-AngelScript) — AngelScript VM and hot-reload
- [Event System](Event-System) — Publish/subscribe event bus
- [Scene Management](Scene-Management) — Scene hierarchy and serialization
- [Asset Pipeline](Asset-Pipeline) — Model and texture loading
- [Shader Pipeline](Shader-Pipeline) — Shader authoring and compilation
- [SparkEditor](SparkEditor) — ImGui-based visual editor
- [SparkConsole](SparkConsole) — Standalone debug console
- [Creating a Game Module](Creating-a-Game-Module) — Building game modules
- [Getting Started](Getting-Started) — Build and run the engine
