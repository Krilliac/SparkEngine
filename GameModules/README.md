# GameModules

This directory contains game modules that the engine discovers and builds automatically.

## How It Works

CMake scans `GameModules/*/CMakeLists.txt` at configure time. Every subdirectory with a
`CMakeLists.txt` is added to the build as a shared library (DLL on Windows, `.so` on Linux).
At runtime, the engine's `ModuleManager` loads these DLLs and manages their lifecycle.

**Add a module** -- create a folder here with a `CMakeLists.txt` and source files.
**Remove a module** -- delete the folder (or move it out). No CMake editing required.
**Skip all modules** -- configure with `-DBUILD_GAME_MODULES=OFF` for an engine-only build.

## Creating a New Game Module

### 1. Directory Structure

```
GameModules/
  MyGame/
    CMakeLists.txt
    Source/
      Core/
        Main.cpp        # DLL entry point + IModule implementation
        MyGame.h        # Module class declaration
      Game/
        ...             # Your game logic
```

### 2. Implement `Spark::IModule`

Your module class implements the `Spark::IModule` interface from `SparkSDK/Include/Spark/IModule.h`:

```cpp
#include <Spark/IModule.h>
#include <Spark/ModuleRegistry.h>

class MyGameModule : public Spark::IModule
{
public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "My Game";
        info.version = "1.0.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;  // Lower values load first
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        // Initialize your game systems here
        return true;
    }

    void OnUnload() override
    {
        // Clean up in reverse initialization order
    }

    void OnUpdate(float deltaTime) override
    {
        // Called every frame
    }

    // Optional overrides:
    // void OnFixedUpdate(float fixedDt) override;  // Fixed timestep
    // void OnRender() override;                     // After update
    // void OnResize(int w, int h) override;         // Window resize
    // void OnPause() override;                      // Game paused
    // void OnResume() override;                     // Game resumed
    // void OnImGui() override;                      // Debug UI

private:
    Spark::IEngineContext* m_context = nullptr;
};
```

### 3. Export the Module

In one `.cpp` file (typically `Main.cpp`), use the `SPARK_IMPLEMENT_MODULE` macro:

```cpp
#include <Spark/ModuleRegistry.h>
#include <Spark/ModuleDllMain.h>  // Emits the standard DllMain on Windows (no-op elsewhere)

SPARK_IMPLEMENT_MODULE(MyGameModule)
```

This generates the `CreateModule()` and `DestroyModule()` exports that the engine looks for. The `ModuleDllMain.h` header defines a canonical `DllMain` that calls `DisableThreadLibraryCalls` — include it in exactly one TU per DLL.

### 4. CMakeLists.txt

Use SparkGame as a template. The minimum required:

```cmake
cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Collect sources
file(GLOB_RECURSE MY_SOURCES "Source/*.cpp" "Source/*.h")

# Build as shared library
add_library(MyGame SHARED ${MY_SOURCES})

# The target name MUST match the directory name for auto-discovery to
# link it correctly. GameModules/MyGame/ -> target name "MyGame".

target_compile_definitions(MyGame PRIVATE SPARK_MODULE_DLL)

# Link engine (available when built as subdirectory of the engine)
# On Linux, use SparkEngineInterface (headers only) to avoid duplicate
# singletons — symbols resolve from the exe at dlopen time.
if(WIN32)
    target_link_libraries(MyGame PRIVATE SparkEngineLib)
elseif(TARGET SparkEngineInterface)
    target_link_libraries(MyGame PRIVATE SparkEngineInterface)
endif()

# Include paths
target_include_directories(MyGame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_SOURCE_DIR}/SparkEngine/Source
    ${CMAKE_SOURCE_DIR}/SparkSDK/Include
)
```

See `SparkGame/CMakeLists.txt` for a full example including standalone build support,
platform libraries, and optional dependencies (Jolt, Vulkan, OpenGL).

## Module Lifecycle

```
Engine startup
  1. Discover modules in GameModules/
  2. Load each DLL (LoadLibrary / dlopen)
  3. Call CreateModule() to get an IModule*
  4. Sort by loadOrder and dependencies
  5. Call OnLoad(context) on each module

Main loop (each frame)
  6. OnUpdate(deltaTime) on all modules
  7. OnFixedUpdate(fixedDt) at fixed intervals
  8. OnRender() on all modules

Engine shutdown
  9. OnUnload() in reverse load order
 10. DestroyModule() and unload DLLs
```

## Module Dependencies

If your module depends on another module being loaded first:

```cpp
Spark::ModuleInfo GetModuleInfo() const override
{
    Spark::ModuleInfo info{};
    info.name = "CombatModule";
    info.loadOrder = 1001;
    SPARK_MODULE_DEPENDENCIES(info, "CoreGameplay", "WeaponSystem");
    return info;
}
```

The engine resolves dependencies via topological sort and will error on circular dependencies.

## Hot Reload

During development, the engine watches module DLLs for changes. When you recompile:

1. The engine calls `OnUnload()` on the module
2. Unloads the old DLL
3. Loads the new DLL
4. Calls `OnLoad()` with the same engine context

You can also trigger a manual reload via the console: `reload_module MyGame`

## Runtime Loading (Without Build Integration)

Modules don't have to live in `GameModules/`. The engine can load any module DLL at runtime:

- **Command line:** `SparkEngine.exe -game path/to/MyGame.dll`
- **Manifest:** Create a `spark.modules.json` next to the engine executable:
  ```json
  {
    "modules": [
      { "name": "MyGame", "path": "path/to/MyGame.dll" }
    ]
  }
  ```
- **Directory scan:** Place the DLL next to the engine executable (auto-discovered)

## Included Modules

| Module | Description | Load Order |
|--------|-------------|------------|
| **SparkGame** | FPS arena showcase (player, weapons, enemies, projectiles) | 1000 |
| **SparkGameMMO** | MMO networking showcase (chat, guilds, inventory, crafting) | 1001 |
