# Project Templates

> **Stable-v1 boundary:** Stable-v1 is blocked and uncertified. Its intended product shape is Windows 11 x64 with MSVC v143, D3D11 (or Windows NullRHI), C++ game modules, and one first-party single-player `SparkGameFPS` vertical slice. The template catalog is source-development material outside that certified release surface; it is not a versioned stable installer, SDK, or game release. Other platforms, backends, template packages, and multiplayer work remain experimental or unsupported.

Project templates are source-development starting packages for SparkEngine. Each is a standalone CMake project that consumes an **installed** SparkEngine SDK and produces one game-module shared library. They provide authored examples and `IModule` lifecycle wiring; they do not certify a complete, packaged, or production game.

**Source:** `Templates/` directory

## Overview

| Package | Genre | CMake module target | Source example |
|---------|-------|---------------------|----------------|
| `EmptyProject` | General | `EmptyProject` | Minimal scene-editing and runtime-preview starter |
| `FPSStarter` | FPS | `FPSStarter` | Movement, mouselook, weapons, damage, HUD, and a training range |
| `ThirdPersonStarter` | Adventure | `ThirdPersonStarter` | Third-person movement, orbit camera, pickup, and objective example |
| `TopDownStarter` | Action | `TopDownStarter` | Top-down movement, camera, enemy pursuit, and combat-feedback example |
| `Blank3D` | General | `Blank3D` | Primitives, lighting, fly camera, and composition study |
| `MMOStarter` | MMO | `MMOStarter` | Local client/server-shaped gameplay example, not an MMO service |
| `PlatformerKit` | Platformer | `PlatformerKit` | Movement, collectibles, hazards, checkpoints, and finish example |
| `RPGStarter` | RPG | `RPGStarter` | Dialogue, quest, inventory, combat, and bounded save/load example |
| `MultiplayerArena` | Multiplayer Arena | `MultiplayerArena` | Legacy compatibility sample with deterministic local arena rules; outside the built-in registry and not network transport/server evidence |

## Key Types

### template.json

Each template contains a `template.json` file describing its metadata:

```json
{
    "name": "FPSStarter",
    "identity": "first-person",
    "description": "A playable first-person training range with authored arena props, hitscan combat, a live HUD, reloads, and round reset.",
    "genre": "FPS",
    "gameModule": "FPSStarter",
    "defaultScene": "Scenes/Arena.sparkscene",
    "features": ["movement", "mouselook", "weapons", "damage", "hud", "restart"]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Template identifier (matches directory name) |
| `description` | string | Human-readable description |
| `genre` | string | Game genre classification |
| `gameModule` | string | This package's CMake module target; it is not an alias for an in-tree `GameModules/` target |
| `features` | string[] | Feature tags included in this template |

### `<Package>.sparkproject`

Each package has a project file used by project/editor tooling. Module selection remains an explicit runtime choice through `-game` or `-manifest`.

```json
{
    "projectFileVersion": 1,
    "name": "FPSStarter",
    "version": "0.2.0",
    "engineVersion": "1.0.0",
    "template": "first-person",
    "defaultScene": "Scenes/Arena.sparkscene",
    "modules": ["FPSStarter"],
    "scenes": ["Scenes/Arena.sparkscene"]
}
```

The older `spark.project.json` name is a compatibility path, not the package layout used by the current templates.

### spark.modules.json

Module loading configuration:

```json
{
    "modules": [
        {
            "path": "FPSStarter.dll"
        }
    ]
}
```

## Quick Start

### Creating a New Project

1. Install a configured engine build, then configure an unmodified package against that SDK:

```powershell
cmake --install <engine-build> --prefix <sdk> --config Release
cmake -S Templates/FPSStarter -B build/fps-starter -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build/fps-starter --config Release
```

2. On Windows with a multi-config generator, launch the matching configuration and pass the actual module artifact:

```powershell
<sdk>\bin\SparkEngine.exe -game .\build\fps-starter\Release\FPSStarter.dll
```

On a single-config POSIX build, the corresponding target normally has the flat library path:

```bash
<sdk>/bin/SparkEngine -game "$PWD/build/fps-starter/libFPSStarter.so"
```

`-game` requires a non-empty path to an existing regular module file. If you materialize a package under a new name, use SparkEditor or rename the literal package token consistently across its directory, CMake target, source, metadata, and scene files; current templates do not use `{{PROJECT_NAME}}` placeholders.


### Template Structure

Every template follows this layout:

```
TemplateName/
├── CMakeLists.txt          # Standalone CMake project using find_package(SparkEngine)
├── template.json           # Template metadata (name, genre, features)
├── Package.sparkproject    # Project/editor metadata
├── spark.modules.json      # Optional module-path manifest
└── Source/
    ├── GameModule.h        # IModule implementation (game logic entry point)
    └── GameModule.cpp      # DLL exports via SPARK_IMPLEMENT_MODULE()
```

### The IModule Lifecycle

All templates implement `Spark::IModule`. The engine drives modules through these lifecycle methods:

```cpp
class MyGameModule : public Spark::IModule
{
public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name       = "MyGame";
        info.version    = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder  = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        // Initialize game systems
        return true;
    }

    void OnUnload() override
    {
        // Clean up game systems
        m_context = nullptr;
    }

    void OnUpdate(float deltaTime) override
    {
        // Per-frame game logic
    }

    void OnRender() override
    {
        // Per-frame rendering
    }

private:
    Spark::IEngineContext* m_context = nullptr;
};
```

## Template Details

### EmptyProject

The minimal starting point. It ships an empty authoring scene (`Scenes/Default.sparkscene`)
plus an explicit 8-entity runtime preview (`Scenes/RuntimePreview.sparkscene`) and one HUD
sprite, so it exercises the same scene/render lifecycle as the gameplay packages (including in
headless hosts) without shipping gameplay; it is the one package that does not read input.

**Features:** `minimal`

```cpp
// Source/GameModule.h — just the bare IModule implementation
bool OnLoad(Spark::IEngineContext* context) override
{
    m_context = context;
    // TODO: Initialize your game systems here
    return true;
}

void OnUpdate(float deltaTime) override
{
    // TODO: Update your game logic here
    (void)deltaTime;
}
```

Use this when starting from scratch or building a non-standard game type.

### FPSStarter

A first-person shooter template with weapon definitions, player state, health system, and respawn logic.

**Features:** `weapons`, `ai-enemies`, `hud`, `health-system`

Includes pre-defined types:

```cpp
struct PlayerState
{
    float health = 100.0f;
    float maxHealth = 100.0f;
    float armor = 0.0f;
    uint32_t currentWeapon = 0;
    uint32_t ammo = 30;
    uint32_t reserveAmmo = 90;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    bool isAlive = true;
};

struct WeaponDef
{
    std::string name;
    float damage = 25.0f;
    float fireRate = 0.1f;       // Seconds between shots
    float range = 100.0f;
    uint32_t magazineSize = 30;
    float reloadTime = 2.0f;
    bool isAutomatic = true;
};
```

Ships with four weapons pre-configured:

| Weapon | Damage | Fire Rate | Range | Magazine | Auto |
|--------|--------|-----------|-------|----------|------|
| Pistol | 20 | 0.3s | 50 | 12 | No |
| Assault Rifle | 25 | 0.1s | 80 | 30 | Yes |
| Shotgun | 80 | 0.8s | 15 | 8 | No |
| Sniper Rifle | 100 | 1.2s | 200 | 5 | No |

### RPGStarter

An RPG template with movement, proximity dialogue, a relic quest, combat, reward state, and an intentionally bounded in-process save slot.

**Features:** `movement`, `inventory`, `dialogue`, `quests`, `combat`, `demo-save-slot`

The starter keeps its rules deterministic and self-contained. It demonstrates where a project can connect the engine's larger quest, dialogue, and persistent save services without claiming those production integrations are already present in the template.

### PlatformerKit

A 2D/3D platformer template with character controller, collectibles, checkpoints, and a level timer.

**Features:** `character-controller`, `collectibles`, `checkpoints`, `level-timer`

This package builds its own `PlatformerKit` module target; it does not inherit the in-tree `SparkGamePlatformer` target.

### MultiplayerArena

A bounded local arena rules sample with lobby readiness, scoreboard, respawn, and teams. It does not open a network transport or dedicated server.

**Features:** `local-simulation`, `lobby`, `scoreboard`, `respawn`, `team-system`

The stable team and player IDs are suitable for a future transport adapter, but authoritative networking must be added and tested separately before treating this as a networked game.

## Configuration

### CMakeLists.txt

Templates use the installed package's `find_package(SparkEngine CONFIG REQUIRED)` and `spark_add_game_module()` helper:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SparkEngine CONFIG REQUIRED)

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")

spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

### Prerequisites

Templates require a **built and installed** copy of SparkEngine. A raw engine build tree is not the standalone SDK package:

```powershell
cmake --install <engine-build> --prefix <sdk> --config Release
cmake -S <package> -B <package>/build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
```

### Module Load Order

The manifest loader consumes each `modules[*].path` in array order and resolves a relative path against the manifest directory. It does not consume manifest `name` or `loadOrder`; module metadata supplies its own kind and lifecycle ordering. A process accepts one `ModuleKind::Game` module; `ModuleKind::Addon` modules (library/extension-style modules) may coexist when their own metadata allows it.

## Creating Custom Templates

1. Copy `EmptyProject` as a starting point
2. Add your game-specific source files to `Source/`
3. Update `template.json` with your template's metadata:

```json
{
    "name": "MyTemplate",
    "description": "Description of what this template provides",
    "genre": "YourGenre",
    "gameModule": "MyTemplate",
    "features": ["feature-a", "feature-b"]
}
```

4. Keep the package's literal name consistent across its CMake target, source, metadata, and scene files when materializing a new package
5. Place the template directory under `Templates/`

## Integration

### With the Engine Runtime

Game modules are shared libraries loaded at startup. The engine calls `CreateModule()` (exported via `SPARK_IMPLEMENT_MODULE`) to instantiate the module, then drives it through the `IModule` lifecycle.

```powershell
# The engine loads the game module and calls OnLoad -> OnUpdate/OnRender loop -> OnUnload
<sdk>\bin\SparkEngine.exe -game <absolute-path-to-MyGame.dll>
```

### With Game Modules

Template packages and in-tree `GameModules/` directories are independent targets. A template's `gameModule` field names its own package target; it does not select or inherit a same-genre in-tree module. Use an explicit `-game` path or a manifest when choosing a module to load.

### With the SparkSDK

Templates include `<Spark/SparkSDK.h>` which provides access to the `IModule` interface, `IEngineContext`, `ModuleInfo`, and `SPARK_SDK_VERSION`. The SDK headers are installed as part of the engine installation.

**Header-surface contract (decision record for `SDK-240`, option (a)):** the supported installed-SDK
surface for templates and game modules is `SparkSDK/Include/Spark` (ABI-stable) **plus** the engine
headers the installer stages under `include/SparkEngine` (source-compatible per engine version:
`Game/TemplateRuntime.h`, `Core/Reflection.h`, `Engine/ECS/Components.h`, `Graphics/GraphicsEngine.h`,
`Graphics/WorldBasicRenderer.h`, `Input/InputManager.h`, `SceneManager/ReflectedSceneSerializer.h`,
`Utils/LogMacros.h`). Only `SparkSDK/Include/Spark` carries the ABI promise; a module that includes
`include/SparkEngine` headers must be rebuilt against the SDK it ships with. This matches
`Templates/README.md`; any "public SDK surface alone" wording elsewhere means this two-part surface.

**Other contract facts:** `template.json` is the source of truth and the editor registry is gated
against it by `ProjectMaterialization_EditorRegistryMatchesEveryPackageTemplateJson`. A scene
contract requires only the entities a module cannot run without (camera and, where present, the
player); a missing decorative prop logs one warning instead of failing the load. `OnLoad(nullptr)`
is construction-only -- it proves nothing about scene loading -- and
`TemplateRuntimeScene::LastLoadResult()` (`Deterministic` / `Loaded` / `Failed`) distinguishes the
cases for the eight packages that share it (`FPSStarter` carries its own copy).
`Tools/normalize_template_runtime_sheets.py` (needs Pillow) regenerates the 3x3 runtime sheets and
`Tools/validate_template_runtime_sheets.py` is the CI validator (`SparkTemplateRuntimeSheets`).
`spark.modules.json` may keep the `.dll` form: `LoadModulesFromManifest` retries a `.dll` path as
`lib<Name>.so` / `lib<Name>.dylib` on POSIX (an extension-less stem is not accepted). All nine
`Assets/manifest.json` provenance/SHA-256 records are enforced by `tools/site-data/validate.py --assets`.

### With AngelScript

Game modules provide the C++ framework. AngelScript work is experimental and outside stable-v1, so template TODO comments are extension points rather than a scripting or hot-reload support guarantee.

## API Reference

### IModule Lifecycle Methods

| Method | Description |
|--------|-------------|
| `GetModuleInfo() -> ModuleInfo` | Return module name, version, SDK version, and load order |
| `OnLoad(IEngineContext*) -> bool` | Initialize game systems (called once at startup) |
| `OnUnload()` | Clean up game systems (called once at shutdown) |
| `OnUpdate(float deltaTime)` | Per-frame game logic update |
| `OnRender()` | Per-frame rendering |

### ModuleInfo

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Module display name |
| `version` | string | Module version string |
| `sdkVersion` | uint32_t | SDK version for compatibility checking |
| `loadOrder` | int | Initialization priority (lower = earlier) |

## Thread Safety

Game modules run on the **main thread**. `OnUpdate()` and `OnRender()` are called sequentially each frame. If your game logic needs background work (e.g., pathfinding, asset streaming), use the engine's coroutine scheduler or standard `std::async` with appropriate synchronization.

The `IEngineContext` pointer received in `OnLoad()` is valid for the module's entire lifetime and should not be accessed after `OnUnload()` returns.

## See Also

- [[Game-Modules]] -- Engine game module architecture
- [[SparkSDK]] -- Public SDK interface headers
- [[Networking]] -- NetworkManager for multiplayer templates
- [[Scripting]] -- Experimental AngelScript scripting surface
- [[Build-System]] -- CMake build system and presets
