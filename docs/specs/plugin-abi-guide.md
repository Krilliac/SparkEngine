# Plugin / Game Module ABI Stability Guide

This document describes the binary interface contract between the SparkEngine
runtime and dynamically loaded game modules (DLLs on Windows, `.so` on Linux,
`.dylib` on macOS). Following these rules ensures that modules load correctly,
remain compatible across engine updates, and avoid the subtle memory corruption
bugs that plague cross-DLL C++ code.

This guide covers the compiler-specific C++ game-module boundary. Importers,
processors, editor extensions, runtime extensions, and external tools that need
a compiler-independent C boundary should use the
[Stable C Plugin ABI](../guides/plugin-abi.md).

**Audience:** Game module authors, engine contributors adding SDK surface area.

---

## Table of Contents

1. [Game Module Interface](#1-game-module-interface)
2. [Module Lifecycle](#2-module-lifecycle)
3. [DLL Boundary Rules](#3-dll-boundary-rules)
4. [Version Compatibility](#4-version-compatibility)
5. [Required Headers](#5-required-headers)
6. [Thread Safety Requirements](#6-thread-safety-requirements)
7. [Memory Ownership Rules](#7-memory-ownership-rules)
8. [Console Command Registration](#8-console-command-registration)
9. [Example Minimal Module](#9-example-minimal-module)

---

## 1. Game Module Interface

### Required Exports

Every module DLL must export three `extern "C"` functions:

```cpp
extern "C" SPARK_MODULE_API Spark::IModule* CreateModule();
extern "C" SPARK_MODULE_API void DestroyModule(Spark::IModule* mod);
extern "C" SPARK_MODULE_API const SparkModuleCompatibilityDescriptor*
    SparkGetModuleCompatibility();
```

- `CreateModule` allocates and returns a heap-allocated `Spark::IModule`
  implementation. The engine takes non-owning custody of this pointer.
- `DestroyModule` frees the module instance. The engine calls this before
  unloading the DLL. The module DLL **must** delete the object because
  the module's allocator created it (see [Memory Ownership](#7-memory-ownership-rules)).
- `SparkGetModuleCompatibility` exposes the runtime ABI descriptor checked
  after the mandatory pre-load `.sparkabi` sidecar passes validation.

Use the `SPARK_IMPLEMENT_MODULE` macro (from `Spark/ModuleRegistry.h`) to
generate all three exports, and include `Spark/ModuleDllMain.h` to emit the
canonical Windows DllMain (it is a no-op on other platforms):

```cpp
#include <Spark/ModuleRegistry.h>
#include <Spark/ModuleDllMain.h>

SPARK_IMPLEMENT_MODULE(MyGameModule)
```

### The IModule Interface

Defined in `SparkSDK/Include/Spark/IModule.h`. Pure virtual methods that every
module **must** implement:

| Method | Purpose |
|--------|---------|
| `GetModuleInfo()` | Return module metadata (name, version, SDK version, load order) |
| `OnLoad(IEngineContext*)` | Initialize the module; return `false` to abort loading |
| `OnUnload()` | Release all resources before DLL unload |
| `OnUpdate(float deltaTime)` | Called every frame (variable timestep) |

Optional virtual methods with default no-op implementations:

| Method | Purpose |
|--------|---------|
| `OnFixedUpdate(float fixedDt)` | Fixed timestep for deterministic simulation (typically 1/60s) |
| `OnRender()` | Called after `OnUpdate` for rendering work |
| `OnResize(int w, int h)` | Window resize notification |
| `OnPause()` / `OnResume()` | Game pause/resume events |
| `OnImGui()` | Draw debug UI when the editor overlay is active |
| `CanUnload()` | Non-destructive durability/readiness gate; return `false` to postpone unload safely |
| `SupportsHotReload()` | Return `false` when a replacement image cannot initialize beside the live module |

### ModuleInfo

Returned by `GetModuleInfo()`. All `const char*` fields must point to
**module-owned** memory (string literals are the simplest approach). The engine
copies these strings during registration.

```cpp
struct ModuleInfo
{
    const char* name       = "Unnamed";
    const char* version    = "1.0.0";
    uint32_t sdkVersion    = SPARK_SDK_VERSION;  // Must match engine's SDK version
    int loadOrder          = 1000;               // Lower values load first

    const char* const* dependencies = nullptr;   // Module names this depends on
    int dependencyCount             = 0;
};
```

### Legacy Interface (IGameModule)

The older `IGameModule` interface (`SparkEngine/Source/Core/IGameModule.h`)
with `CreateGameModule`/`DestroyGameModule` exports is still supported through
a compatibility adapter in ModuleManager. New modules should use `Spark::IModule`.

---

## 2. Module Lifecycle

```
Engine startup
  1. Discover module DLLs (GameModules/ directory, manifest, or command line)
  2. Validate the sibling .sparkabi descriptor before loading executable code
  3. LoadLibrary / dlopen the DLL
  4. Resolve SparkGetModuleCompatibility(), CreateModule(), and DestroyModule()
  5. Validate the in-image compatibility descriptor before calling the factory
  6. Call CreateModule() to instantiate IModule
  7. Call GetModuleInfo() to copy module metadata
  8. Sort modules by loadOrder + topological dependency sort
  9. Call OnLoad(context) on each module in sorted order

Main loop (each frame)
 10. OnUpdate(deltaTime) on all modules in load order
 11. OnFixedUpdate(fixedDt) at fixed intervals
 12. OnRender() on all modules in load order

Engine shutdown
 13. CanUnload() on all modules in REVERSE load order (abort non-destructively on veto)
 14. OnUnload() on all modules in REVERSE load order after the complete preflight succeeds
 15. DestroyModule(instance) for each module
 16. FreeLibrary / dlclose each DLL
```

### Key Rules

- **OnLoad is your constructor.** Do all initialization here, not in
  `CreateModule`. The `IEngineContext*` is not available at construction time.
- **OnUnload is your destructor.** Release all engine resources (subscriptions,
  registered commands, ECS entities) here. Your C++ destructor should be a
  safety net, not the primary cleanup path.
- **Reverse shutdown order.** If module A depends on module B, then B is loaded
  first and unloaded last. Design teardown accordingly.
- **Shutdown is two-phase.** `CanUnload` may checkpoint durable state, but it
  must not dismantle the live module. A `false` result leaves every module
  initialized so the engine can keep running and retry. Once every module
  passes, shutdown is committed and `OnUnload` cannot veto.
- **Hot reload is opt-out.** The default `SupportsHotReload()` is `true` for
  compatibility. Stateful modules and modules holding exclusive OS/service
  resources should override it to return `false`. Reload asks the working
  image's `CanUnload` gate before staging a replacement, then loads and
  initializes the replacement beside it. A successful replacement commits
  directly; a staging/load/init failure leaves the working image active.

---

## 3. DLL Boundary Rules

Passing C++ objects across DLL boundaries is the most common source of
ABI-related crashes. The fundamental problem: the engine executable and the
module DLL may use different allocators, different STL implementations, or
different class layouts.

### Safe to Pass Across Boundaries

| Type | Why Safe |
|------|----------|
| Primitive types (`int`, `float`, `bool`, `uint32_t`) | No allocator, fixed layout |
| `const char*` (C strings) | No allocator ownership ambiguity (see ownership rules) |
| Plain-old-data structs (`Spark::Vec3`, `Spark::Quat`, `Spark::Color`, `Spark::ModuleInfo`) | No vtable, no allocator, fixed layout |
| Pointers to engine-owned abstract interfaces (`IEngineContext*`, `GraphicsEngine*`) | Virtual dispatch through engine vtable; module never allocates/frees these |
| `enum class` with explicit underlying type | Fixed-size integer |
| C-style arrays of POD types | Contiguous, no allocator |

### NEVER Pass Across Boundaries

| Type | Why Dangerous |
|------|---------------|
| `std::string` | Different allocators between DLLs; different layouts between STL implementations |
| `std::vector`, `std::map`, other STL containers | Same allocator/layout issues as `std::string` |
| `std::unique_ptr`, `std::shared_ptr` | Destructor runs in the wrong allocator context |
| `std::function` | Contains heap-allocated callable; allocator mismatch |
| Non-POD structs with `std::string` members | The string member triggers the same issues |
| Exceptions (`throw` across DLL boundary) | Undefined behavior across DLL boundaries on most platforms |

### The Boundary Rule of Thumb

**The DLL boundary is a C boundary.** Think of the `CreateModule`/`DestroyModule`
exports as a C API. Everything that crosses the boundary should be expressible
in C: integers, floats, pointers, C strings, POD structs.

### Virtual Interfaces Are the Escape Hatch

The engine's entire module API is built on abstract virtual interfaces
(`IModule`, `IEngineContext`, `ILogger`). Virtual dispatch works correctly
across DLL boundaries because:

1. The vtable lives in the DLL that defines the concrete class.
2. Callers only hold a pointer and invoke through the vtable.
3. No object layout knowledge is needed by the caller.

This is why `IEngineContext` returns raw pointers to subsystems, not
`std::shared_ptr`. Modules call methods on engine-owned objects through
virtual dispatch; neither side needs to know the other's allocator or STL.

### Windows DllMain Restrictions

If you provide a `DllMain` (Windows only), keep it minimal:

```cpp
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
```

Do **not** call engine APIs, allocate complex objects, or create threads in
`DllMain`. The OS loader lock is held during these calls.

---

## 4. Version Compatibility

### SDK Version

The SDK ABI version (`SPARK_SDK_VERSION`, defined in `SparkSDK/Include/Spark/Version.h`)
is the primary compatibility gate. It is a single integer that is incremented whenever
any SDK interface changes in a binary-incompatible way, including:

- Adding, removing, or reordering virtual methods in `IModule` or `IEngineContext`
- Changing the layout of `ModuleInfo` or any SDK POD struct
- Changing function signatures of `CreateModule`/`DestroyModule`

The engine checks `ModuleInfo::sdkVersion` against its own `SPARK_SDK_VERSION`
at load time. If they do not match, the module is rejected with an error log
and is not initialized.

```cpp
// From Version.h
inline constexpr bool IsSDKCompatible(uint32_t moduleSDKVersion)
{
    return moduleSDKVersion == SPARK_SDK_VERSION;
}
```

**This is an exact-match check**, not a range check. A module compiled against
SDK version 2 will not load in an engine built with SDK version 3, even if the
changes were "additive." This is intentional -- vtable layout changes silently
corrupt virtual dispatch if versions mismatch.

### Engine Version

The packed engine version (`SPARK_ENGINE_VERSION_PACKED`, format `0xMMmmpp`)
tracks the human-readable release version (major.minor.patch). Modules can
query it at runtime via `IEngineContext::GetEngineVersion()`. This version is
informational and is **not** used for ABI compatibility checks. Use it for
feature detection or logging.

### When the SDK Version Increments

As a module author, when `SPARK_SDK_VERSION` changes between engine releases:

1. **Recompile your module** against the new SDK headers. This is mandatory.
2. Review the changelog for interface changes that affect your code.
3. There is no binary backward compatibility across SDK version bumps.

As an engine contributor adding SDK surface area:

1. Adding a new virtual method to `IEngineContext` with a default
   implementation (`{ return nullptr; }`) still changes the vtable layout.
   **Increment `SPARK_SDK_VERSION`.**
2. Adding a new field to `ModuleInfo` changes its layout.
   **Increment `SPARK_SDK_VERSION`.**
3. Adding a new header to the SDK that does not change existing interfaces
   does **not** require a version bump.

---

## 5. Required Headers

### The Single-Include Option

```cpp
#include <Spark/SparkSDK.h>
```

This pulls in everything a module needs from the SDK:

| Header | Contents |
|--------|----------|
| `Spark/IModule.h` | `IModule` interface, `ModuleInfo` struct |
| `Spark/IEngineContext.h` | Service locator with subsystem getters |
| `Spark/ILogger.h` | Abstract logging interface |
| `Spark/ModuleRegistry.h` | `SPARK_IMPLEMENT_MODULE`, `SPARK_MODULE_DEPENDENCIES` macros |
| `Spark/Version.h` | `SPARK_SDK_VERSION`, `SPARK_ENGINE_VERSION_*`, compatibility checks |
| `Spark/SparkExport.h` | `SPARK_MODULE_API`, `SPARK_EXPORT`, `SPARK_IMPORT` |
| `Spark/MathTypes.h` | `Vec2`, `Vec3`, `Vec4`, `Quat`, `Color`, `Mat4x4`, `AABB`, `Ray` |
| `Spark/InputTypes.h` | `MouseButton`, `GamepadButton`, `GamepadAxis`, `InputAction` |
| `Spark/EventTypes.h` | Event system usage guide, forward declarations |

### Engine-Internal Headers

Modules that need deeper engine integration can include headers from
`SparkEngine/Source/`:

```cpp
#include "Utils/SparkConsole.h"     // SimpleConsole for command registration
#include "Utils/Logger.h"           // SPARK_LOG_INFO, LogCategory, LogLevel
#include "Utils/LogMacros.h"        // Convenience logging macros
#include "Engine/Events/EventSystem.h"  // Built-in event types
#include "Utils/EventBus.h"         // EventBus, SubscriptionHandle
```

These are **not** part of the stable SDK. They are engine-internal headers whose
layout may change without an SDK version bump. When including them, you accept
a tighter coupling to the engine build -- your module must be recompiled
whenever those headers change, even if `SPARK_SDK_VERSION` stays the same.

### Include Path Setup (CMake)

```cmake
target_include_directories(MyGame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_SOURCE_DIR}/SparkEngine/Source   # Engine-internal headers
    ${CMAKE_SOURCE_DIR}/SparkSDK/Include     # Stable SDK headers
)
```

### Compile Definitions

Every module DLL must define `SPARK_MODULE_DLL` so that `SPARK_MODULE_API`
resolves to `dllexport` (Windows) or `visibility("default")` (Linux/macOS):

```cmake
target_compile_definitions(MyGame PRIVATE SPARK_MODULE_DLL)
```

---

## 6. Thread Safety Requirements

### Which Thread Calls Your Module?

All `IModule` lifecycle methods are called on the **main thread**:

- `OnLoad`, `CanUnload`, `SupportsHotReload`, `OnUnload` -- main thread, during engine startup/shutdown or reload
- `OnUpdate`, `OnFixedUpdate`, `OnRender`, `OnImGui` -- main thread, during the frame loop
- `OnResize`, `OnPause`, `OnResume` -- main thread, from the window message pump

You do **not** need synchronization for these methods unless your module
creates its own worker threads.

### Engine Subsystem Thread Safety

When calling engine subsystems through `IEngineContext`, be aware of their
thread safety characteristics:

| Subsystem | Thread Safety | Notes |
|-----------|--------------|-------|
| `SimpleConsole` | Thread-safe | Mutex-protected; safe to log from any thread |
| `PhysicsSystem` | Multithreaded dispatch | Jolt physics runs jobs on worker threads; do not call physics APIs outside `OnFixedUpdate` unless you hold the appropriate lock |
| `GraphicsEngine` | Main thread only | Render state uses `std::atomic` for frame synchronization, but all rendering calls must originate from the main thread |
| `NetworkManager` | Queue mutex | Message I/O and handler registration are mutex-protected; safe to enqueue from worker threads |
| `EventBus` | See docs | Check `Utils/EventBus.h` for current guarantees; subscribe/unsubscribe on the main thread |

### If Your Module Creates Threads

- Protect shared state with mutexes. Prefer `std::mutex` + `std::lock_guard`.
- Never call `IModule` interface methods from worker threads -- the engine
  does not expect reentrant calls.
- Join or detach all threads in `OnUnload`. The DLL will be unloaded after
  `DestroyModule` returns; any threads still referencing module code will crash.

---

## 7. Memory Ownership Rules

### The Golden Rule

**The allocator that created an object must be the one to destroy it.**

Because the engine executable and each module DLL may have separate CRT heaps
(especially on Windows with static CRT linking), memory allocated in one
module cannot be freed in another.

### Ownership Patterns

| Scenario | Who Allocates | Who Frees | How |
|----------|--------------|-----------|-----|
| Module instance | Module (`CreateModule`) | Module (`DestroyModule`) | Engine calls `DestroyModule` before unloading |
| Engine subsystems (`GraphicsEngine*`, etc.) | Engine | Engine | Module holds non-owning raw pointer; never `delete` |
| Module-internal objects | Module | Module | Use `std::unique_ptr` internally; release in `OnUnload` |
| `const char*` in `ModuleInfo` | Module (string literal) | Nobody (static storage) | Engine copies the string during registration |
| ECS entities created by module | Engine (ECS allocator) | Engine (ECS) | Module creates via `World*` API; engine owns the storage |

### Practical Consequences

- **Never `delete` a pointer you received from `IEngineContext`.** Those are
  engine-owned. Store them as raw, non-owning pointers.
- **Never return `std::unique_ptr` or `std::shared_ptr` across the DLL
  boundary.** The destructor would run the wrong allocator.
- **Use `std::unique_ptr` for objects owned by your module internally.** Reset
  them in `OnUnload` so cleanup happens before the DLL is unloaded.
- **String literals for `ModuleInfo` fields.** If you must build a string
  dynamically, store it as a `std::string` member variable on your module class
  and return `.c_str()`. The engine copies it immediately.

### RAII and Cleanup Order

In `OnUnload`, release resources in reverse initialization order:

```cpp
void OnUnload() override
{
    // 1. Unsubscribe from events
    // 2. Unregister console commands (if the console supports it)
    // 3. Destroy module-owned systems (unique_ptr::reset)
    // 4. Null out engine pointers
    m_context = nullptr;
}
```

Your C++ destructor is a safety net. If `OnUnload` was called properly, the
destructor should have nothing to do.

---

## 8. Console Command Registration

Modules register console commands through `Spark::SimpleConsole`, which is
accessed as a global singleton:

```cpp
#include "Utils/SparkConsole.h"

void MyModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "my_command",                                         // Command name
        [this](const std::vector<std::string>& args) -> std::string
        {
            // Command implementation
            return "Command executed";
        },
        "Description of what this command does",              // Help text
        "MyModule",                                           // Category
        "my_command [arg1] [arg2]"                            // Usage string
    );
}
```

### Rules

- **Register in `OnLoad`, not in the constructor.** The console may not be
  initialized when `CreateModule` is called.
- **All commands for one module go in one registration function.** This keeps
  them discoverable and easy to maintain (see Anti-Bloat Guidelines).
- **Prefix commands with your module name** to avoid collisions with engine
  commands or other modules (e.g., `rpg_quest_status`, `fps_weapon_list`).
- **Command callbacks capture `this`.** Ensure the module outlives the
  registration. Since `OnUnload` is called before `DestroyModule`, commands
  referencing `this` are valid for the module's entire loaded lifetime.
- **Note on the `std::vector<std::string>` parameter.** This crosses the DLL
  boundary. It works because the module links against the same engine library
  (`SparkEngineLib` on Windows, or resolves symbols from the executable on
  Linux). If you ever build a module with a different CRT or STL, this will
  break. The SDK-stable alternative is to use `const char*` and `int argc`
  style callbacks if this becomes an issue.

---

## 9. Example Minimal Module

### Directory Structure

```
GameModules/
  MyGame/
    CMakeLists.txt
    Source/
      Core/
        MyGame.h
        Main.cpp
```

### MyGame.h

```cpp
#pragma once

#include <Spark/SparkSDK.h>

class MyGameModule : public Spark::IModule
{
public:
    MyGameModule() = default;
    ~MyGameModule() override = default;

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name       = "My Game";
        info.version    = "1.0.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder  = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        // Initialize game systems here
        return true;
    }

    void OnUnload() override
    {
        // Clean up in reverse initialization order
        m_context = nullptr;
    }

    void OnUpdate(float deltaTime) override
    {
        if (!m_context)
            return;
        // Game logic here
        (void)deltaTime;
    }

private:
    Spark::IEngineContext* m_context = nullptr;
};
```

### Main.cpp

```cpp
#include "MyGame.h"
#include <Spark/ModuleRegistry.h>
#include <Spark/ModuleDllMain.h>  // Canonical Windows DllMain; no-op elsewhere

// Generates CreateModule() and DestroyModule() exports
SPARK_IMPLEMENT_MODULE(MyGameModule)
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGame LANGUAGES CXX)

find_package(SparkEngine CONFIG REQUIRED)

file(GLOB_RECURSE MY_SOURCES "Source/*.cpp" "Source/*.h")
spark_add_game_module(MyGame ${MY_SOURCES})
target_include_directories(MyGame PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
```

`spark_add_game_module` supplies the SDK includes, link target, compile
definitions, C++ level, and the mandatory post-build `.sparkabi` sidecar.
When building in the engine tree, the root already loads this helper and
auto-discovers directories under `GameModules/`; omit the standalone
`find_package` line there.

---

## Appendix: Quick Reference Checklist

Before shipping a module, verify:

- [ ] Module exports compatibility, create, and destroy entry points (use `SPARK_IMPLEMENT_MODULE`)
- [ ] Build uses `spark_add_game_module` or explicitly calls `spark_configure_module_abi`
- [ ] A sibling `.sparkabi` sidecar is emitted beside every built module
- [ ] `GetModuleInfo().sdkVersion` is set to `SPARK_SDK_VERSION`
- [ ] `SPARK_MODULE_DLL` is defined in CMake compile definitions
- [ ] All `const char*` returns in `ModuleInfo` are string literals or module-owned storage
- [ ] No `std::string`, `std::vector`, or smart pointers cross the DLL boundary in your API
- [ ] All resources are released in `OnUnload`, not just in the destructor
- [ ] `CanUnload` is non-destructive and vetoes when durable state cannot be checkpointed
- [ ] Stateful or exclusive-resource modules override `SupportsHotReload()` to return `false`
- [ ] Console commands are registered in `OnLoad`, in a single registration function
- [ ] Worker threads (if any) are joined before `OnUnload` returns
- [ ] Engine pointers are treated as non-owning (never `delete` them)
- [ ] Module compiles with the same C++ standard and compiler as the engine
