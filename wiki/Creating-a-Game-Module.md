# Creating a Game Module

SparkEngine uses a dynamic module system similar to Unreal Engine and Unity. Your game logic is compiled as a shared library (DLL on Windows, .so on Linux) that the engine loads at runtime. This architecture enables hot-reload during development, clean separation of engine and game code, and the ability to load multiple modules simultaneously.

**Source:** `SparkSDK/Include/Spark/SparkSDK.h`, `cmake/SparkGameModule.cmake`, `cmake/SparkEnginePreflight.cmake`

## Quick Start from Template

The fastest way to start is using the `EmptyProject` template:

```bash
cp -r Templates/EmptyProject MyGame
```

This gives you:

```
MyGame/
├── CMakeLists.txt          # Build configuration
├── Source/
│   ├── GameModule.h        # Module class (IModule implementation)
│   └── GameModule.cpp      # Module factory exports
├── spark.project.json      # Project metadata
└── spark.modules.json      # Module manifest
```

## The IModule Interface

Every game module implements `Spark::IModule` from `<Spark/SparkSDK.h>`. This is the contract between the engine and your game code.

### Interface Definition

```cpp
class IModule
{
public:
    virtual ~IModule() = default;

    // Return metadata about your module
    virtual ModuleInfo GetModuleInfo() const = 0;

    // Called once when the DLL is loaded
    virtual bool OnLoad(IEngineContext* context) = 0;

    // Called before the DLL is unloaded
    virtual void OnUnload() = 0;

    // Called every frame
    virtual void OnUpdate(float deltaTime) = 0;

    // Called every frame after OnUpdate (optional)
    virtual void OnRender() {}

    // Called when the window is resized (optional)
    virtual void OnResize(int width, int height) {}
};
```

### ModuleInfo Struct

The `ModuleInfo` struct provides metadata about your module:

| Field | Type | Description |
|-------|------|-------------|
| `name` | `const char*` | Human-readable module name |
| `version` | `const char*` | Semantic version string (e.g. `"1.0.0"`) |
| `sdkVersion` | `uint32_t` | SDK version this module was built against (`SPARK_SDK_VERSION`) |
| `loadOrder` | `int` | Initialization priority (lower = earlier, default 1000) |

### IModule Method Reference

| Method | Required | When Called | Description |
|--------|----------|------------|-------------|
| `GetModuleInfo()` | Yes | After DLL load | Return module name, version, SDK version, load order |
| `OnLoad()` | Yes | After `CreateModule()` | Initialize the module; return `false` to abort loading |
| `OnUnload()` | Yes | Before `DestroyModule()` | Clean up resources, unsubscribe events |
| `OnUpdate()` | Yes | Every frame | Main game logic tick |
| `OnRender()` | No | Every frame after update | Custom rendering (HUD, debug overlays) |
| `OnResize()` | No | Window resize | Handle viewport changes |

### Complete Module Implementation

```cpp
#include <Spark/SparkSDK.h>

class MyGameModule : public Spark::IModule
{
public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name       = "MyGame";
        info.version    = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder  = 1000;   // lower values load first
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;

        // Access engine systems through the context
        m_graphics = context->GetGraphics();
        m_input    = context->GetInput();
        m_timer    = context->GetTimer();
        m_events   = context->GetEventBus();

        // Subscribe to engine events
        m_collisionSub = m_events->Subscribe<Spark::CollisionEvent>(
            [this](const Spark::CollisionEvent& e) {
                HandleCollision(e);
            });

        m_sceneSub = m_events->Subscribe<Spark::SceneLoadedEvent>(
            [this](const Spark::SceneLoadedEvent& e) {
                OnSceneLoaded(e.sceneName);
            });

        return true;  // return false to abort loading
    }

    void OnUnload() override
    {
        // Clean up event subscriptions
        m_events->Unsubscribe<Spark::CollisionEvent>(m_collisionSub);
        m_events->Unsubscribe<Spark::SceneLoadedEvent>(m_sceneSub);

        m_context = nullptr;
    }

    void OnUpdate(float deltaTime) override
    {
        // Game logic here
        UpdatePlayer(deltaTime);
        UpdateEnemies(deltaTime);
    }

    void OnRender() override
    {
        // Custom rendering (HUD, debug overlays)
        DrawHUD();
    }

    void OnResize(int width, int height) override
    {
        m_viewportWidth  = width;
        m_viewportHeight = height;
    }

private:
    void HandleCollision(const Spark::CollisionEvent& e) { /* ... */ }
    void OnSceneLoaded(const std::string& name) { /* ... */ }
    void UpdatePlayer(float dt) { /* ... */ }
    void UpdateEnemies(float dt) { /* ... */ }
    void DrawHUD() { /* ... */ }

    Spark::IEngineContext* m_context  = nullptr;
    GraphicsEngine*        m_graphics = nullptr;
    InputManager*          m_input    = nullptr;
    Timer*                 m_timer    = nullptr;
    Spark::EventBus*       m_events   = nullptr;

    Spark::SubscriptionID  m_collisionSub = 0;
    Spark::SubscriptionID  m_sceneSub     = 0;

    int m_viewportWidth  = 1280;
    int m_viewportHeight = 720;
};
```

## Exporting the Module

In exactly one `.cpp` file, use the `SPARK_IMPLEMENT_MODULE` macro to generate the required DLL exports:

```cpp
// GameModule.cpp
#include "GameModule.h"

SPARK_IMPLEMENT_MODULE(MyGameModule)
```

This generates the following exported functions:

```cpp
extern "C" SPARK_MODULE_API Spark::IModule* CreateModule();
extern "C" SPARK_MODULE_API void DestroyModule(Spark::IModule*);
```

The engine calls `CreateModule()` to instantiate your module and `DestroyModule()` to clean it up. You should never call these functions directly.

### How the Macro Works

`SPARK_IMPLEMENT_MODULE` expands to:

```cpp
extern "C" __declspec(dllexport) Spark::IModule* CreateModule()
{
    return new MyGameModule();
}

extern "C" __declspec(dllexport) void DestroyModule(Spark::IModule* mod)
{
    delete mod;
}
```

On Linux, `__declspec(dllexport)` is replaced with `__attribute__((visibility("default")))`.

## CMake Setup

### As Part of the SparkEngine Tree

If your game module lives inside the SparkEngine source tree, use the `spark_add_game_module()` helper from `cmake/SparkGameModule.cmake`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyGame LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

include(${CMAKE_SOURCE_DIR}/cmake/SparkGameModule.cmake)

file(GLOB_RECURSE GAME_SOURCES "Source/*.cpp" "Source/*.h")
spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

### What spark_add_game_module() Does

The `spark_add_game_module(TARGET_NAME ...)` function performs the following:

| Step | Action |
|------|--------|
| 1 | Creates a `SHARED` library target from the provided sources |
| 2 | Defines `SPARK_MODULE_DLL` and `SPARK_GAME_DLL` compile definitions |
| 3 | Links against `Spark::SparkEngineLib` |
| 4 | Sets up SDK include directories (`SPARK_ENGINE_INCLUDE_DIR`) |
| 5 | Enforces C++20 standard (`cxx_std_20`) |
| 6 | On MSVC, sets the runtime library to `MultiThreadedDLL` / `MultiThreadedDebugDLL` |

### As a Standalone Project

For standalone projects outside the engine tree, use `find_package`. Including the preflight check first gives clear diagnostics if the SDK installation is incomplete:

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyGame LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

# Pre-flight validation (optional but recommended)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(SparkEnginePreflight)

find_package(SparkEngine REQUIRED)

file(GLOB_RECURSE GAME_SOURCES "Source/*.cpp" "Source/*.h")
spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

### Preflight Validation

The `SparkEnginePreflight.cmake` module validates that the SparkEngine SDK installation is complete before `find_package` runs. It checks for:

| Required File | Purpose |
|---------------|---------|
| `SparkEngineConfig.cmake` | CMake package configuration |
| `SparkEngineTargets.cmake` | Imported target definitions |
| `SparkGameModule.cmake` | Game module helper function |

If any file is missing, the preflight check emits a `FATAL_ERROR` with the exact missing file and remediation steps, rather than letting CMake produce a cryptic error message.

## Configuration Files

### spark.project.json

Project metadata describing your game:

```json
{
    "name": "MyGame",
    "version": "0.1.0",
    "engineVersion": "1.0.0",
    "description": "My first SparkEngine game",
    "modules": ["MyGame"],
    "defaultScene": "Assets/Scenes/Default.scene"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Project display name |
| `version` | Yes | Semantic version of your game |
| `engineVersion` | Yes | Minimum compatible engine version |
| `description` | No | Human-readable project description |
| `modules` | Yes | Array of module names to load |
| `defaultScene` | No | Scene to load on startup |

### spark.modules.json

Module manifest that tells the engine which DLLs to load:

```json
{
    "modules": [
        {
            "name": "MyGame",
            "path": "MyGame.dll",
            "loadOrder": 1000
        }
    ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | `string` | Must match the module's `GetModuleInfo().name` |
| `path` | `string` | Relative or absolute path to the DLL/SO |
| `loadOrder` | `int` | Initialization priority (lower = earlier) |

Place `spark.modules.json` next to the engine executable (`build/bin/`).

### Loading Multiple Modules

You can load multiple modules simultaneously. They are initialized in `loadOrder` and shut down in reverse order:

```json
{
    "modules": [
        { "name": "CoreGameplay", "path": "CoreGameplay.dll", "loadOrder": 100 },
        { "name": "UIModule",     "path": "UIModule.dll",     "loadOrder": 200 },
        { "name": "DebugTools",   "path": "DebugTools.dll",   "loadOrder": 9000 }
    ]
}
```

## Module Discovery

The engine finds modules using three fallback mechanisms (in order):

1. **Command-line argument**: `SparkEngine.exe -game path/to/MyGame.dll`
2. **Manifest file**: `spark.modules.json` next to the executable
3. **Directory scan**: Scans for `*Game*.dll` or `*Module*.dll` in the executable directory

```
Module Discovery Flow
    │
    ├── Check command line: -game <path>
    │   └── Found? → Load specified DLL
    │
    ├── Check for spark.modules.json
    │   └── Found? → Load all listed modules in loadOrder
    │
    └── Scan executable directory
        └── Match *Game*.dll or *Module*.dll → Load matched DLLs
```

## Module Lifecycle

```
Engine Startup
    │
    ├── Load DLL/SO (platform LoadLibrary / dlopen)
    ├── Resolve CreateModule() symbol
    ├── Call CreateModule()          ← allocate module instance
    ├── Call GetModuleInfo()         ← metadata, SDK version check
    ├── Validate SDK version
    │   └── Mismatch? → Log warning, skip module
    ├── Call OnLoad(context)         ← initialization
    │   └── Returns false? → Call DestroyModule(), skip
    │
    ├── Main Loop ──────────────────┐
    │   ├── OnUpdate(deltaTime)     │ ← every frame
    │   └── OnRender()              │ ← every frame (if overridden)
    │   └────────────────────────────┘
    │
    ├── Call OnUnload()             ← cleanup
    ├── Call DestroyModule()        ← deallocate module instance
    └── Unload DLL/SO (FreeLibrary / dlclose)
```

Multiple modules are loaded in `loadOrder` and shut down in reverse order.

## Using Engine Services

### The IEngineContext Interface

Once you have the `IEngineContext*`, access any engine subsystem. The context uses a service locator pattern backed by a generic registry.

```cpp
class IEngineContext
{
public:
    virtual GraphicsEngine*                GetGraphics()          = 0;
    virtual InputManager*                  GetInput()             = 0;
    virtual Timer*                         GetTimer()             = 0;
    virtual Spark::EventBus*               GetEventBus()         = 0;
    virtual AudioEngine*                   GetAudio()             = 0;
    virtual PhysicsSystem*                 GetPhysics()           = 0;
    virtual Spark::Animation::AnimationSystem* GetAnimation()     = 0;
    virtual Spark::AI::AISystem*           GetAI()                = 0;
    virtual Spark::NetworkManager*         GetNetwork()           = 0;
    virtual SceneManager*                  GetSceneManager()      = 0;
    virtual AngelScriptEngine*             GetScriptEngine()      = 0;
    virtual Spark::SaveSystem*             GetSaveSystem()        = 0;
    virtual Spark::CoroutineScheduler*     GetCoroutineScheduler()= 0;
    virtual uint32_t                       GetEngineVersion() const = 0;
    virtual uint32_t                       GetSDKVersion() const   = 0;
    virtual bool                           IsHeadless() const      = 0;
};
```

### Available Subsystems

| Getter | Returns | Description |
|--------|---------|-------------|
| `GetGraphics()` | `GraphicsEngine*` | DX11/Vulkan/GL rendering engine |
| `GetInput()` | `InputManager*` | Keyboard, mouse, gamepad input |
| `GetTimer()` | `Timer*` | Frame timing and delta time |
| `GetEventBus()` | `EventBus*` | Publish/subscribe event system |
| `GetAudio()` | `AudioEngine*` | XAudio2 / miniaudio spatial audio |
| `GetPhysics()` | `PhysicsSystem*` | Bullet Physics simulation |
| `GetAnimation()` | `AnimationSystem*` | Skeletal animation pipeline |
| `GetAI()` | `AISystem*` | Behavior trees and NavMesh |
| `GetNetwork()` | `NetworkManager*` | UDP multiplayer (requires `ENABLE_NETWORKING`) |
| `GetSceneManager()` | `SceneManager*` | Scene hierarchy and serialization |
| `GetScriptEngine()` | `AngelScriptEngine*` | AngelScript VM and hot-reload |
| `GetSaveSystem()` | `SaveSystem*` | Game state serialization |
| `GetCoroutineScheduler()` | `CoroutineScheduler*` | Coroutine-based async tasks |
| `IsHeadless()` | `bool` | True if running without graphics (dedicated server) |

### Custom Subsystems via Generic Registry

The `EngineContext` also supports registering custom subsystems via a generic type-based registry:

```cpp
// Register a custom subsystem
context->RegisterSystem<MyCustomManager>(&myManager);

// Retrieve it later
MyCustomManager* mgr = context->GetSystem<MyCustomManager>();
```

This uses a compile-time `TypeId` system that works with forward-declared types. No RTTI is required.

## Subscribing to Events

Game modules typically subscribe to events during `OnLoad()` and unsubscribe during `OnUnload()`:

```cpp
bool OnLoad(Spark::IEngineContext* context) override
{
    auto* bus = context->GetEventBus();

    m_damageSub = bus->Subscribe<Spark::EntityDamagedEvent>(
        [this](const Spark::EntityDamagedEvent& e) {
            OnEntityDamaged(e);
        });

    m_killSub = bus->Subscribe<Spark::EntityKilledEvent>(
        [this](const Spark::EntityKilledEvent& e) {
            OnEntityKilled(e);
        });

    m_frameSub = bus->Subscribe<Spark::FrameBeginEvent>(
        [this](const Spark::FrameBeginEvent& e) {
            OnFrameBegin(e.deltaTime);
        });

    return true;
}

void OnUnload() override
{
    auto* bus = m_context->GetEventBus();
    bus->Unsubscribe<Spark::EntityDamagedEvent>(m_damageSub);
    bus->Unsubscribe<Spark::EntityKilledEvent>(m_killSub);
    bus->Unsubscribe<Spark::FrameBeginEvent>(m_frameSub);
}
```

See [Event System](Event-System) for the full list of built-in event types.

## Working with ECS from a Module

Game modules can create entities and attach components using the World:

```cpp
bool OnLoad(Spark::IEngineContext* context) override
{
    // Get the ECS world from the scene manager
    auto* sceneMgr = context->GetSceneManager();

    // Create a player entity
    EntityID player = world.CreateEntity("Player");

    auto& tf = world.AddComponent<Transform>(player);
    tf.position = {0.0f, 1.0f, 0.0f};

    auto& hp = world.AddComponent<HealthComponent>(player);
    hp.maxHealth = 100.0f;
    hp.health    = 100.0f;

    auto& mesh = world.AddComponent<MeshRenderer>(player);
    mesh.meshIndex     = 0;
    mesh.materialIndex = 0;
    mesh.visible       = true;

    return true;
}
```

See [Entity Component System](Entity-Component-System) for the full component and system reference.

## Debugging Modules

### Module Load Failures

Common causes of module load failures:

| Symptom | Cause | Fix |
|---------|-------|-----|
| DLL not found | Wrong path in manifest | Check `spark.modules.json` path |
| `CreateModule` symbol not found | Missing `SPARK_IMPLEMENT_MODULE` macro | Add macro to exactly one .cpp file |
| SDK version mismatch | Module built with different SDK version | Rebuild module against current SDK |
| `OnLoad()` returns false | Initialization error in module code | Check module logs for details |
| Missing DLL dependency | Module links against absent library | Use `dumpbin /dependents` (Windows) or `ldd` (Linux) |

### Console Commands

The engine provides console commands for module inspection:

```
module_list         # List all loaded modules with version and load order
module_info <name>  # Show detailed info for a specific module
module_reload <name># Hot-reload a module (development only)
```

## Best Practices

1. **Store the context pointer** -- Save `IEngineContext*` in `OnLoad()` and use it throughout your module's lifetime. The pointer remains valid until `OnUnload()`.

2. **Clean up subscriptions** -- Always unsubscribe from events in `OnUnload()`. Dangling subscriptions cause crashes after the module DLL is unloaded.

3. **Check for null subsystems** -- Some subsystems may be null in headless or minimal configurations. Always check before using:
   ```cpp
   if (auto* audio = context->GetAudio())
       audio->PlaySound("startup");
   ```

4. **Use `loadOrder` wisely** -- Modules with lower `loadOrder` values are initialized first. If your module depends on another module's initialization, give it a higher `loadOrder`.

5. **Avoid global state** -- Keep all state inside your module class. Global variables in a DLL can cause issues with hot-reload and multiple module instances.

6. **Match SDK versions** -- Always build your module against the same SDK version as the engine. The engine logs a warning on version mismatch.

## Next Steps

- [Entity Component System](Entity-Component-System) -- Work with entities and components
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Add scripted gameplay logic
- [Scene Management](Scene-Management) -- Load and manage scenes
- [Event System](Event-System) -- Subscribe to engine events
- [Input System](Input-System) -- Handle player input

---

## See Also

- [Architecture Overview](Architecture-Overview) -- Engine architecture and module system
- [Entity Component System](Entity-Component-System) -- Work with entities and components
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Add scripted gameplay logic
- [Scene Management](Scene-Management) -- Load and manage scenes
- [Event System](Event-System) -- Subscribe to engine events
- [Input System](Input-System) -- Handle player input
- [Build System and CMake Modules](Build-System-and-CMake-Modules) -- Build configuration
