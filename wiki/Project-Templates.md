# Project Templates

Project templates provide ready-to-use starting points for building games on SparkEngine. Each template is a standalone CMake project that compiles into a game module (shared library) loaded by the engine at runtime. Templates include pre-configured gameplay systems, placeholder content, and the `IModule` lifecycle wiring needed to get a new project running immediately.

**Source:** `Templates/` directory

## Overview

| Template | Genre | Game Module | Description |
|----------|-------|-------------|-------------|
| `EmptyProject` | General | `SparkGame` | Minimal boilerplate with a blank `IModule` implementation |
| `FPSStarter` | FPS | `SparkGameFPS` | First-person shooter with weapons, AI enemies, health, and HUD |
| `RPGStarter` | RPG | `SparkGameRPG` | RPG with inventory, dialogue, quests, abilities, and save system |
| `PlatformerKit` | Platformer | `SparkGamePlatformer` | 2D/3D platformer with character controller, collectibles, and checkpoints |
| `MultiplayerArena` | Multiplayer | `SparkGameFPS` | Multiplayer arena with networking, lobby, scoreboard, and teams |

## Key Types

### template.json

Each template contains a `template.json` file describing its metadata:

```json
{
    "name": "FPSStarter",
    "description": "First-person shooter with weapons, AI enemies, and HUD",
    "genre": "FPS",
    "gameModule": "SparkGameFPS",
    "features": ["weapons", "ai-enemies", "hud", "health-system"]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Template identifier (matches directory name) |
| `description` | string | Human-readable description |
| `genre` | string | Game genre classification |
| `gameModule` | string | Which engine game module DLL to base on |
| `features` | string[] | Feature tags included in this template |

### spark.project.json

Project metadata used by the engine at load time:

```json
{
    "name": "{{PROJECT_NAME}}",
    "version": "0.1.0",
    "engineVersion": "1.0.0",
    "description": "A new SparkEngine game project",
    "modules": ["{{PROJECT_NAME}}"],
    "defaultScene": "Assets/Scenes/Default.scene"
}
```

### spark.modules.json

Module loading configuration:

```json
{
    "modules": [
        {
            "name": "{{PROJECT_NAME}}",
            "path": "{{PROJECT_NAME}}.dll",
            "loadOrder": 1000
        }
    ]
}
```

## Quick Start

### Creating a New Project

1. Copy a template directory and rename it:

```bash
cp -r Templates/FPSStarter MyShooter
cd MyShooter
```

2. Replace all `{{PROJECT_NAME}}` placeholders with your project name:

```bash
# Replace in all files
find . -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.json' -o -name 'CMakeLists.txt' \) \
  -exec sed -i 's/{{PROJECT_NAME}}/MyShooter/g' {} +
```

3. Build the game module:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=~/SparkEngine-install
cmake --build build --config Release
```

4. Run with the engine:

```bash
# Windows
SparkEngine.exe -game MyShooter.dll

# Linux
./SparkEngine -game libMyShooter.so
```

### Template Structure

Every template follows this layout:

```
TemplateName/
├── CMakeLists.txt          # Standalone CMake project using find_package(SparkEngine)
├── template.json           # Template metadata (name, genre, features)
├── spark.project.json      # Project metadata (name, version, default scene)
├── spark.modules.json      # Module loading configuration (DLL path, load order)
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

The minimal starting point. Contains only the `IModule` interface with stub lifecycle methods.

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

An RPG template with inventory, dialogue, quests, abilities, and save system integration.

**Features:** `inventory`, `dialogue`, `quests`, `abilities`, `save-system`

Based on the `SparkGameRPG` module, which provides access to the engine's quest, dialogue, and save system APIs.

### PlatformerKit

A 2D/3D platformer template with character controller, collectibles, checkpoints, and a level timer.

**Features:** `character-controller`, `collectibles`, `checkpoints`, `level-timer`

Based on the `SparkGamePlatformer` module.

### MultiplayerArena

A multiplayer arena template with networking, lobby management, scoreboard, respawn, and team system.

**Features:** `networking`, `lobby`, `scoreboard`, `respawn`, `team-system`

Based on the `SparkGameFPS` module with networking extensions. Connects to the engine's `NetworkManager` and `AreaServer` systems.

## Configuration

### CMakeLists.txt

Templates use `find_package(SparkEngine REQUIRED)` and the `spark_add_game_module()` CMake helper:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(SparkEnginePreflight)

find_package(SparkEngine REQUIRED)

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")

spark_add_game_module(MyGame ${GAME_SOURCES})
target_include_directories(MyGame PRIVATE "Source")
```

### Prerequisites

Templates require a **built and installed** copy of SparkEngine. Point `CMAKE_PREFIX_PATH` at the install prefix (not the build directory):

```bash
cmake --install build --prefix ~/SparkEngine-install
cmake -B build -DCMAKE_PREFIX_PATH=~/SparkEngine-install
```

### Module Load Order

The `loadOrder` field in `spark.modules.json` controls initialization order. Lower values load first. Game modules typically use 1000+.

## Creating Custom Templates

1. Copy `EmptyProject` as a starting point
2. Add your game-specific source files to `Source/`
3. Update `template.json` with your template's metadata:

```json
{
    "name": "MyTemplate",
    "description": "Description of what this template provides",
    "genre": "YourGenre",
    "gameModule": "SparkGame",
    "features": ["feature-a", "feature-b"]
}
```

4. Use `{{PROJECT_NAME}}` placeholders in all files that should be renamed per-project
5. Place the template directory under `Templates/`

## Integration

### With the Engine Runtime

Game modules are shared libraries loaded at startup. The engine calls `CreateModule()` (exported via `SPARK_IMPLEMENT_MODULE`) to instantiate the module, then drives it through the `IModule` lifecycle.

```bash
# The engine loads the game module and calls OnLoad -> OnUpdate/OnRender loop -> OnUnload
SparkEngine.exe -game MyGame.dll
```

### With Game Modules

Templates map to specific `GameModules/` directories that provide genre-specific engine integrations:

| Template | Game Module | Module Path |
|----------|-------------|-------------|
| EmptyProject | SparkGame | `GameModules/SparkGame/` |
| FPSStarter | SparkGameFPS | `GameModules/SparkGameFPS/` |
| RPGStarter | SparkGameRPG | `GameModules/SparkGameRPG/` |
| PlatformerKit | SparkGamePlatformer | `GameModules/SparkGamePlatformer/` |
| MultiplayerArena | SparkGameFPS | `GameModules/SparkGameFPS/` |

### With the SparkSDK

Templates include `<Spark/SparkSDK.h>` which provides access to the `IModule` interface, `IEngineContext`, `ModuleInfo`, and `SPARK_SDK_VERSION`. The SDK headers are installed as part of the engine installation.

### With AngelScript

Game modules provide the C++ framework, but gameplay scripting can be done in AngelScript with hot-reload support. Templates include TODO comments indicating where script system initialization belongs.

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
- [[Scripting]] -- AngelScript hot-reload scripting
- [[Build-System]] -- CMake build system and presets
