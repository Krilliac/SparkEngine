# GameModules

This directory contains in-tree game-module targets. The root build enumerates
them when `BUILD_GAME_MODULES` is enabled; runtime selection is a separate step.

Only the SparkGameFPS single-player slice is declared in `stable-v1`, and it is
blocked and uncertified. Every other module is experimental or outside the
profile. SparkGameFPS still links the private engine and includes source-tree
headers, so it does not yet prove the required installed-public-SDK boundary.

## How It Works

Root CMake enumerates the known `GameModules/*/CMakeLists.txt` targets at configure
time and builds enabled modules as shared libraries. An explicit `-game <path>`
loads one requested module. On Windows, a bare launch discovers candidates but
loads only one directly or through the project selector; it does not bulk-load
them. Manifests and the Linux executable-directory fallback may iterate multiple
modules for an addon pack, but `ModuleManager` allows only one `ModuleKind::Game`;
library/extension modules must be declared as add-ons and can coexist with it.

**Add a module** -- create a folder here with a `CMakeLists.txt` and source files.
**Remove a module** -- delete the folder (or move it out). No CMake editing required.
**Skip all modules** -- configure with `-DBUILD_GAME_MODULES=OFF` for an engine-only build.

## Creating a New Game Module

### 1. Directory Structure

```
GameModules/
  MyGame/
    CMakeLists.txt
    README.md           # Required: what runs, release classification, known limits
    module.json         # Required: per-module facts checked by site-data validation
    Source/
      Core/
        Main.cpp        # DLL entry point + IModule implementation
        MyGame.h        # Module class declaration
      Game/
        ...             # Your game logic
```

`module.json` names the module's CMake target, source directory, asset roots
(or `"state": "none"` with a reason), the registered test sources and the
`TEST` name prefixes that cover it (each with the exact number of tests it
selects), its README, and any parity dimensions it declares N/A. Release-profile policy stays in `tools/module-evidence/manifest.json`.
`python3 tools/site-data/validate.py --modules` and the `ModuleManifest_Contract`
CTest fail when a discovered module has no manifest or any referenced path,
test prefix, test count, README, or N/A declaration does not hold.
`Tests/CMakeLists.txt` also generates one `ModuleManifest_<Module>_<Prefix>`
CTest (label `module-kit`) per declared prefix; it runs exactly that many
tests or fails. A prefix with `"requires": ["angelscript"]` is not registered
when the build disables AngelScript, because its tests are compiled out. The field rules are in
[Creating a Game Module](../wiki/getting-started/Creating-a-Game-Module.md#gamemodulesnamemodulejson-in-tree-modules).

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

# In-tree-only link example; this cannot prove installed-SDK compatibility.
# On Linux, use SparkEngineInterface (headers only) to avoid duplicate
# singletons — symbols resolve from the exe at dlopen time.
if(WIN32)
    target_link_libraries(MyGame PRIVATE SparkEngineLib)
elseif(TARGET SparkEngineInterface)
    target_link_libraries(MyGame PRIVATE SparkEngineInterface)
endif()

# In-tree-only include paths; the engine-source include violates the stable-v1
# standalone installed-SDK contract and must not be copied into an SDK consumer.
target_include_directories(MyGame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_SOURCE_DIR}/SparkEngine/Source
    ${CMAKE_SOURCE_DIR}/SparkSDK/Include
)
```

See `SparkGame/CMakeLists.txt` for an in-tree example with platform libraries and
optional dependencies (Jolt, Vulkan, OpenGL). It does not establish standalone
installed-SDK support.

Engine-private includes in prototype modules are ratcheted, not endorsed. For
every module outside all release profiles, `module-content-inventory.json`
publishes `privateEngineHeaders` (each `SparkEngine/Source` header the module
includes), `privateEngineHeaderCount`, and `copiedInfrastructureFiles` (its
copied `*EngineSystems.cpp` setup). `python3 tools/site-data/module_content.py
--check` and the `PrototypeModuleKit_PrivateDependencyRatchet` CTest fail when a
module gains an engine-private header its committed entry does not list, or when
a removed include is not dropped from the list. Regenerate the inventory with
`python3 tools/site-data/module_content.py` only when the new dependency is
reviewed and intended.

## Module Lifecycle

```
Engine startup (one Game-kind module maximum)
  1. `-game` loads one explicit module; the Windows selector also chooses one
  2. A manifest or Linux directory fallback may add compatible addon modules
  3. Reject a second Game-kind module; load selected Game module plus add-ons
  4. Call CreateModule() and validate each loaded module's metadata/compatibility
  5. Call OnLoad(context) in load order

Main loop (each frame)
  6. OnUpdate(deltaTime) on loaded modules in load order
  7. OnFixedUpdate(fixedDt) at fixed intervals
  8. OnRender() on loaded modules

Engine shutdown
  9. OnUnload() in reverse load order
 10. DestroyModule() and unload each loaded library
```

### Experimental module lifecycle evidence (RDY-015)

Each experimental module (every `tools/module-evidence/manifest.json` entry
tracked under RDY-015) has an `ExperimentalModuleLifecycle_<Module>` CTest on
Linux. The test runs `cmake/RunSparkExperimentalModuleLifecycle.cmake`, which
launches the real `SparkEngine` host on the SDL path with NullRHI and
`-require-game`. The run passes only if the host exits 0, prints one
`SPARK_MODULE_READY count=1` line, and then prints one post-teardown
`SPARK_MODULE_LIFECYCLE module=<Module> ...` record in which every phase
(create, load, update, fixed, render, unload, destroy) is at least 1 and
`faults=0`. The runner sets 120000 frames because fixed steps follow a 60 Hz
wall clock, and a shorter NullRHI run can finish before one fixed step elapses.

```bash
ctest --test-dir build/linux-gcc-release -L experimental-modules --output-on-failure --no-tests=error
ctest --test-dir build/linux-gcc-release -R ExperimentalModuleLifecycleParserContract
```

These tests are prototype evidence and never certify a module. They carry the
`experimental-modules;prototype` labels and never `stable-v1`
or `module-profile`. The required full-ctest Linux lanes exclude them with
`--label-exclude '^experimental-modules$'`. Only the advisory
`experimental-module-lifecycle` job in `build.yml` runs them. That job is
`continue-on-error`, is not a `required-ci-gate` dependency, and uploads its JUnit
even when a module fails. A failing module stays visible there without changing
stable-v1 support. `test-workflow-failure-propagation.py` enforces this separation.

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

- **Command line:** `SparkEngine.exe -game path/to/MyGame.dll` loads one explicit module.
- **Manifest:** Create a `spark.modules.json` next to the engine executable. It may list one Game-kind module and compatible add-on/library entries:
  ```json
  {
    "modules": [
      { "name": "MyGame", "path": "path/to/MyGame.dll" }
    ]
  }
  ```
- **Windows staged candidates:** a bare Windows launch loads one candidate directly or presents the project selector when several are found; it does not bulk-load game modules.
- **Linux directory fallback:** absent `-game` and a manifest, the Linux host iterates executable-directory candidates. This can load an addon pack, but a second Game-kind module is rejected.

## Selected Module Examples

The two rows below are selected examples, not a complete target inventory. The
root build currently enumerates 11 in-tree module directories when
`BUILD_GAME_MODULES` is enabled; see the [full game-module catalog](../wiki/getting-started/Game-Modules.md).

| Module | Description | Load Order |
|--------|-------------|------------|
| **SparkGame** | FPS arena showcase (player, weapons, enemies, projectiles) | 1000 |
| **SparkGameMMO** | MMO networking showcase (chat, guilds, inventory, crafting) | 1001 |
