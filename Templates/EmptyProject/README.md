# EmptyProject — Spark Engine Template

A minimal game module template for SparkEngine. Provides a blank `Spark::IModule` implementation with the correct project structure, CMake configuration, and DLL exports — ready for you to add your game logic.

## Prerequisites

This template requires a **built and installed** SparkEngine SDK. See the [Templates README](../README.md) for full installation instructions.

> **Short version:** You need to run `cmake --install build --prefix <install-path>` on SparkEngine so that `find_package(SparkEngine)` can locate the SDK.

## Setup

1. Copy this directory and rename it to your project name:
   ```bash
   cp -r Templates/EmptyProject MyGame
   cd MyGame
   ```

2. Replace all `{{PROJECT_NAME}}` placeholders with your project name in:
   - `CMakeLists.txt`
   - `Source/GameModule.h`
   - `Source/GameModule.cpp`
   - `spark.project.json`
   - `spark.modules.json`

## Build

```bash
cmake -B build -DCMAKE_PREFIX_PATH=<path-to-SparkEngine-install-prefix>
cmake --build build --config Release
```

> **Warning:** Do not pass the SparkEngine build directory or its `bin/` subdirectory as `CMAKE_PREFIX_PATH`. This will fail because the build tree does not contain the installed config files. Always use the **install prefix** (the path you passed to `--prefix`).

## Run

The template compiles into a shared library (game module), not a standalone executable. Launch it through the SparkEngine executable:

```bash
# Windows
SparkEngine.exe -game MyGame.dll

# Linux
./SparkEngine -game libMyGame.so
```

## What's Included

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Standalone CMake project — calls `find_package(SparkEngine)` and `spark_add_game_module()` |
| `Source/GameModule.h` | Your `IModule` subclass with lifecycle stubs (`OnLoad`, `OnUnload`, `OnUpdate`, `OnRender`) |
| `Source/GameModule.cpp` | DLL exports via `SPARK_IMPLEMENT_MODULE()` |
| `spark.project.json` | Project metadata (name, version, engine version, default scene) |
| `spark.modules.json` | Module loading config (DLL path and load order) |

## Next Steps

- Implement your game logic in `GameModule::OnUpdate()` and `GameModule::OnRender()`
- Use the `Spark::IEngineContext*` provided in `OnLoad()` to access engine subsystems
- Add assets under an `Assets/` directory — they will be copied to the output automatically
- See the **[SparkTemplates](https://github.com/Krilliac/SparkTemplates)** repository for more advanced examples (physics, AI, networking, etc.)
