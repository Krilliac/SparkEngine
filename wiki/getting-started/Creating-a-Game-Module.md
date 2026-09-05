# Creating a Game Module

> **Stable-v1 boundary:** Stable-v1 is blocked and uncertified. Its intended product shape is Windows 11 x64 with MSVC v143, D3D11 (or Windows NullRHI), C++ game modules, and one first-party single-player `SparkGameFPS` vertical slice. There is no certified versioned SDK or module ABI release. Other hosts, backends, scripting, templates, addons, and multiplayer remain experimental or unsupported.

SparkEngine uses a dynamic module system. Game logic is compiled as a shared library (a DLL on the declared Windows profile; source build paths also exist elsewhere). The current manager permits **one** `ModuleKind::Game` module per process; `ModuleKind::Addon` modules (library/extension-style modules) may coexist when their own metadata permits it. Development reload and compatibility checks do not establish a stable public ABI or hot-reload support guarantee.

**Source:** `SparkSDK/Include/Spark/SparkSDK.h`, `cmake/SparkGameModule.cmake`

## Quick Start from Template

`Templates/EmptyProject` is a standalone installed-SDK package, not an in-tree `GameModules/` directory. Copy or materialize it outside the engine checkout, install a configured engine build, and configure it against the installed CMake package:

```powershell
cmake --install <engine-build> --prefix <sdk> --config Release
cmake -S <MyGame> -B <MyGame>/build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build <MyGame>/build --config Release
```

The template package layout is:

```
MyGame/
├── CMakeLists.txt          # Build configuration
├── Source/
│   ├── GameModule.h        # Module class (IModule implementation)
│   └── GameModule.cpp      # Module factory exports
├── <ProjectName>.sparkproject # Project/editor metadata
└── spark.modules.json      # Module manifest
```

When materializing a template under a new name, update the literal package name consistently across its directory, CMake target, source, metadata, and scene files. Do not copy the stock package directly below `GameModules/`; use the in-tree setup below for that route.

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

`SPARK_SDK_VERSION` is **4** (`SparkSDK/include/Spark/Version.h`) after `IEngineContext` gained
`GetInvalidStateDetector()` and `GetComponentSerializers()`. `IsSDKCompatible` is exact equality, so
a v3 module is refused by a v4 host and a v4 module by a v3 host — there is no forward or backward
window. `Spark/IEngineContext.h` pins `EngineContextVirtualCount = 90` with a `static_assert` tying
it to the version constant: adding or removing a virtual means updating **both** together, or an old
host will accept a module that calls off the end of its vtable.

### IModule Method Reference

| Method | Required | When Called | Description |
|--------|----------|------------|-------------|
| `GetModuleInfo()` | Yes | After DLL load | Return module name, version, SDK version, load order |
| `OnLoad()` | Yes | After `CreateModule()` | Initialize the module; return `false` to abort loading |
| `OnUnload()` | Yes | Before `DestroyModule()` | Clean up resources, unsubscribe events |
| `OnUpdate()` | Yes | Every frame | Main game logic tick |
| `OnRender()` | No | Every frame after update | Custom rendering (HUD, debug overlays) |
| `OnResize()` | No | Window resize | Handle viewport changes |

### Example Module Implementation

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
#include <Spark/ModuleDllMain.h>  // Emits the canonical Windows DllMain

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

If your game module lives inside the SparkEngine source tree, put its CMake file under `GameModules/<name>/`. The root `CMakeLists.txt` already includes `cmake/SparkGameModule.cmake` and auto-discovers those directories, so the child file should use the helper directly:

```cmake
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")
spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

Configure and launch an in-tree module from the repository root. With a multi-config generator, keep the configuration segment in both paths:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target SparkEngine MyGame
.\build\bin\Release\SparkEngine.exe -game .\build\bin\Release\MyGame.dll
```

### What spark_add_game_module() Does

The `spark_add_game_module(TARGET_NAME ...)` function performs the following:

| Step | Action |
|------|--------|
| 1 | Creates a `SHARED` library target from the provided sources |
| 2 | Defines `SPARK_MODULE_DLL` and `SPARK_GAME_DLL` compile definitions |
| 3 | Links against `Spark::SparkEngineLib` |
| 4 | Sets up SDK include directories (`SPARK_ENGINE_INCLUDE_DIR`) |
| 5 | Enforces C++23 standard (`cxx_std_23`) |
| 6 | On MSVC, sets the runtime library to `MultiThreadedDLL` / `MultiThreadedDebugDLL` |

### As a Standalone Project

For standalone projects outside the engine tree, use the installed CMake package:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGame LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(SparkEngine CONFIG REQUIRED)

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")
spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

### Installed SDK Configuration

`SparkEngineConfig.cmake` imports the package targets and the installed `SparkGameModule.cmake` helper. `SparkEnginePreflight.cmake` is a source-tree utility, not an installed-SDK dependency; do not add a relative source-tree module path to a standalone package.

Use `-DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"` (or an equivalent package-discovery setting) after `cmake --install <engine-build> --prefix <sdk> --config Release`. A raw engine build directory is not the standalone SDK contract.

## Configuration Files

### `<ProjectName>.sparkproject`

Current template packages use a project/editor metadata file. It does not replace explicit runtime module selection:

```json
{
    "projectFileVersion": 1,
    "name": "MyGame",
    "version": "0.1.0",
    "engineVersion": "1.0.0",
    "description": "My first SparkEngine game",
    "modules": ["MyGame"],
    "defaultScene": "Scenes/Default.sparkscene",
    "scenes": ["Scenes/Default.sparkscene"]
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Project display name |
| `version` | Yes | Semantic version of your game |
| `engineVersion` | Yes | Minimum compatible engine version |
| `description` | No | Human-readable project description |
| `modules` | Yes | Project metadata naming this package's module target |
| `defaultScene` | No | Scene to load on startup |

### spark.modules.json

Module manifest whose loader consumes each non-empty `path` entry:

```json
{
    "modules": [
        {
            "path": "MyGame.dll"
        }
    ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `path` | `string` | Non-empty relative or absolute path to the DLL/SO; a relative path is resolved against the manifest directory |
| `name` / `loadOrder` | metadata | Not consumed by the current manifest parser; use `ModuleInfo` for module metadata |

For the engine-directory fallback, place `spark.modules.json` next to the executable: `build/bin/<Config>/` for a multi-config root build and `build/bin/` for a single-config root build. An explicit `-manifest <path>` may name a regular manifest file elsewhere.

### One Game Module Plus Addons

The loader rejects a second `ModuleKind::Game` module because two games would own the same simulation. A manifest can include the selected Game module plus compatible `ModuleKind::Addon` modules (library/extension-style modules); their kinds and lifecycle dependencies come from their `ModuleInfo`, not manifest `loadOrder` metadata:

```json
{
    "modules": [
        { "path": "MyGame.dll" },
        { "path": "MyGameplayAddon.dll" }
    ]
}
```

## Module Discovery

The engine resolves a selected game module using this priority:

1. **Command-line game path**: `SparkEngine.exe -game path/to/MyGame.dll`
2. **Explicit manifest**: `SparkEngine.exe -manifest path/to/spark.modules.json`
3. **Engine-directory manifest**: `spark.modules.json` next to the executable
4. **Bare launch (platform-specific):**
   - **Windows:** discover candidates without bulk-loading them; one candidate loads directly, while several require the windowed selector or headless pick-one guidance
   - **Current non-Windows path:** scan the executable directory through `LoadModulesFromDirectory`; the manager still refuses a second Game-kind module, so prefer `-game` or `-manifest`

```
Module Discovery Flow
    │
    ├── Check command line: -game <path>
    │   └── Found? → Load specified DLL
    │
    ├── Check command line: -manifest <path>
    │   └── Found? → Load each manifest path (one Game module maximum)
    │
    ├── Check for spark.modules.json next to the executable
    │   └── Found? → Load each manifest path (one Game module maximum)
    │
    └── Bare launch (platform-specific)
        ├── Windows: one candidate loads; multiple candidates require an explicit choice
        └── Non-Windows: directory scan attempts matches; one-Game policy still applies
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

The process hosts one Game module. Compatible `ModuleKind::Addon` modules (library/extension-style modules), when present, follow their own `ModuleInfo` lifecycle rules; manifest `loadOrder` is not a runtime control.

## Using Engine Services

### The IEngineContext Interface

Once you have the `IEngineContext*`, access services exposed by the active build. Availability varies by configuration; do not infer that every listed subsystem is part of stable-v1.

Since the 2026-09 sweep the gameplay lifecycle populates `GetLocalization`, `GetAI`, `GetAnimation`,
`GetWeapons`, `GetMusic`, `GetDestruction`, `GetAbilities`, `GetConditions`, `GetInstances`,
`GetTween`, `GetVFS`, and `GetAreaStreaming` with the host's instances. Two accessors are new:
`GetInvalidStateDetector()` and `GetComponentSerializers()` -- modules must use these host instances
rather than `GetInstance()`, which returns a DLL-local copy inside the module. `GetSceneManager()` is
`nullptr` unless a host registers one, and the engine loads no language file on its own.

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
| `GetGraphics()` | `GraphicsEngine*` | Rendering implementation compiled into the active build; stable-v1 is D3D11/Windows NullRHI only |
| `GetInput()` | `InputManager*` | Keyboard, mouse, gamepad input |
| `GetTimer()` | `Timer*` | Frame timing and delta time |
| `GetEventBus()` | `EventBus*` | Publish/subscribe event system |
| `GetAudio()` | `AudioEngine*` | Concrete engine audio service; the host's separate `IAudioBackend` factory is not exposed through this getter |
| `GetPhysics()` | `PhysicsSystem*` | Jolt Physics simulation |
| `GetAnimation()` | `AnimationSystem*` | Skeletal animation pipeline |
| `GetAI()` | `AISystem*` | Behavior trees and NavMesh |
| `GetNetwork()` | `NetworkManager*` | UDP multiplayer (requires `ENABLE_NETWORKING`) |
| `GetSceneManager()` | `SceneManager*` | Scene hierarchy and serialization |
| `GetScriptEngine()` | `AngelScriptEngine*` | Experimental AngelScript surface outside stable-v1 |
| `GetSaveSystem()` | `SaveSystem*` | Game state serialization |
| `GetCoroutineScheduler()` | `CoroutineScheduler*` | Coroutine-based async tasks |
| `IsHeadless()` | `bool` | True if running without graphics (dedicated server) |

### Module-Owned and Addon Services

The public `Spark::IEngineContext` passed to a module does **not** expose the
concrete host's `RegisterSystem<T>()` or `GetSystem<T>()` registry APIs. Keep
game-specific managers in module-owned state, communicate through the public
event bus or named getters, or define an explicit addon interface. Do not
downcast `IEngineContext` to the private engine implementation.

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

See [Event System](../subsystems/Event-System.md) for the full list of built-in event types.

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

See [Entity Component System](../subsystems/Entity-Component-System.md) for the full component and system reference.

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

4. **Keep module metadata coherent** -- `ModuleInfo` owns a module's kind and lifecycle metadata. Do not rely on manifest `loadOrder`; a second Game-kind module is refused.

5. **Avoid global state** -- Keep all state inside your module class. Global variables in a DLL can cause issues with hot-reload and multiple module instances.

6. **Match SDK versions** -- Always build your module against the same SDK version as the engine. The engine logs a warning on version mismatch.

## Next Steps

- [Entity Component System](../subsystems/Entity-Component-System.md) -- Work with entities and components
- [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) -- Add scripted gameplay logic
- [Scene Management](../subsystems/Scene-Management.md) -- Load and manage scenes
- [Event System](../subsystems/Event-System.md) -- Subscribe to engine events
- [Input System](../subsystems/Input-System.md) -- Handle player input

---

## See Also

- [Architecture Overview](Architecture-Overview.md) -- Engine architecture and module system
- [Entity Component System](../subsystems/Entity-Component-System.md) -- Work with entities and components
- [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) -- Add scripted gameplay logic
- [Scene Management](../subsystems/Scene-Management.md) -- Load and manage scenes
- [Event System](../subsystems/Event-System.md) -- Subscribe to engine events
- [Input System](../subsystems/Input-System.md) -- Handle player input
- [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) -- Build configuration
