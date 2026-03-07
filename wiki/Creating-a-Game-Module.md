# Creating a Game Module

SparkEngine uses a dynamic module system similar to Unreal Engine and Unity. Your game logic is compiled as a shared library (DLL on Windows, .so on Linux) that the engine loads at runtime.

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

Every game module implements `Spark::IModule` from `<Spark/SparkSDK.h>`:

```cpp
#include <Spark/SparkSDK.h>

class MyGameModule : public Spark::IModule
{
public:
    // Return metadata about your module
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name       = "MyGame";
        info.version    = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder  = 1000;   // lower values load first
        return info;
    }

    // Called once when the DLL is loaded
    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        // Access engine systems:
        // context->GetGraphics()  — rendering
        // context->GetInput()     — input
        // context->GetTimer()     — frame timing
        // context->GetEventBus()  — events
        return true;  // return false to abort loading
    }

    // Called before the DLL is unloaded
    void OnUnload() override
    {
        m_context = nullptr;
    }

    // Called every frame
    void OnUpdate(float deltaTime) override
    {
        // Game logic here
    }

    // Called every frame after OnUpdate (optional)
    void OnRender() override
    {
        // Custom rendering here
    }

    // Called when the window is resized (optional)
    void OnResize(int width, int height) override
    {
        // Handle resize
    }

private:
    Spark::IEngineContext* m_context = nullptr;
};
```

## Exporting the Module

In exactly one `.cpp` file, use the `SPARK_IMPLEMENT_MODULE` macro to generate the required DLL exports:

```cpp
// GameModule.cpp
#include "GameModule.h"

SPARK_IMPLEMENT_MODULE(MyGameModule)
```

This generates:
```cpp
extern "C" SPARK_MODULE_API Spark::IModule* CreateModule();
extern "C" SPARK_MODULE_API void DestroyModule(Spark::IModule*);
```

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

### As a Standalone Project

For standalone projects, use `find_package`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyGame LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(SparkEngine REQUIRED)

file(GLOB_RECURSE GAME_SOURCES "Source/*.cpp" "Source/*.h")
spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

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

Place `spark.modules.json` next to the engine executable (`build/bin/`).

## Module Discovery

The engine finds modules using three fallback mechanisms (in order):

1. **Command-line argument**: `SparkEngine.exe -game path/to/MyGame.dll`
2. **Manifest file**: `spark.modules.json` next to the executable
3. **Directory scan**: Scans for `*Game*.dll` or `*Module*.dll` in the executable directory

## Using Engine Services

Once you have the `IEngineContext*`, access any engine subsystem:

```cpp
bool OnLoad(Spark::IEngineContext* context) override
{
    m_context = context;

    // Get the graphics engine for rendering
    GraphicsEngine* graphics = context->GetGraphics();

    // Get the input manager for player controls
    InputManager* input = context->GetInput();

    // Get the timer for frame timing
    Timer* timer = context->GetTimer();

    // Subscribe to engine events
    Spark::EventBus* events = context->GetEventBus();
    events->Subscribe<Spark::CollisionEvent>([](const Spark::CollisionEvent& e) {
        // Handle collision
    });

    return true;
}
```

## Module Lifecycle

```
Engine Startup
    │
    ├── Load DLL/SO
    ├── Call CreateModule()
    ├── Call GetModuleInfo()      ← metadata, version check
    ├── Call OnLoad(context)      ← initialization
    │
    ├── Main Loop ──────────────┐
    │   ├── OnUpdate(deltaTime) │ ← every frame
    │   └── OnRender()          │ ← every frame
    │   └───────────────────────┘
    │
    ├── Call OnUnload()          ← cleanup
    ├── Call DestroyModule()
    └── Unload DLL/SO
```

Multiple modules are loaded in `loadOrder` and shut down in reverse order.

## Next Steps

- [Entity Component System](Entity-Component-System) — Work with entities and components
- [Scripting with AngelScript](Scripting-with-AngelScript) — Add scripted gameplay logic
- [Scene Management](Scene-Management) — Load and manage scenes

---

## See Also

- [Architecture Overview](Architecture-Overview) — Engine architecture and module system
- [Entity Component System](Entity-Component-System) — Work with entities and components
- [Scripting with AngelScript](Scripting-with-AngelScript) — Add scripted gameplay logic
- [Scene Management](Scene-Management) — Load and manage scenes
- [Event System](Event-System) — Subscribe to engine events
- [Input System](Input-System) — Handle player input
