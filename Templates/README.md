# Spark Engine — Game Module Templates

This directory contains project templates for building **game modules** that run on SparkEngine. Each template is a standalone CMake project that compiles into a shared library loaded by the engine at runtime.

## Templates

| Name | Description |
|------|-------------|
| **EmptyProject** | Minimal scaffolding — a blank `IModule` implementation ready for your game logic |
| **FPSStarter** | First-person shooter starter — weapon definitions, player state, HUD hook-up |
| **MultiplayerArena** | Networked arena deathmatch — team assignment, round logic, respawn flow |
| **PlatformerKit** | 2D/2.5D platformer foundation — checkpoints, collectibles, player controller |
| **RPGStarter** | Single-player RPG — quest system, inventory, character progression |

> For additional templates (physics, AI, networking, procedural generation, and more), see the **[SparkTemplates](https://github.com/Krilliac/SparkTemplates)** repository.

## Prerequisites

These templates require a **built and installed** copy of SparkEngine on your system. "Installed" means you have run CMake's install step — pointing at the raw build tree will not work because the exported CMake config files (`SparkEngineConfig.cmake`, `SparkGameModule.cmake`, etc.) are only generated during installation.

### Installing SparkEngine

```bash
# 1. Clone and build SparkEngine (if you haven't already)
git clone --recurse-submodules https://github.com/Krilliac/SparkEngine.git
cd SparkEngine
cmake -B build
cmake --build build --config Release

# 2. Install to a local prefix (e.g. ~/SparkEngine-install)
cmake --install build --prefix ~/SparkEngine-install
```

After this, `~/SparkEngine-install` will contain the engine executable, headers, libraries, and — critically — the CMake package config files that `find_package(SparkEngine)` needs.

> **Common mistake:** Passing the build directory or the `bin/` subdirectory as `CMAKE_PREFIX_PATH`. This will fail because the build tree does not contain the installed config files. Always point at the **install prefix** (the path you passed to `--prefix`).

## Building a Template

Each template is a standalone CMake project:

```bash
cd Templates/EmptyProject
cmake -B build -DCMAKE_PREFIX_PATH=<path-to-SparkEngine-install-prefix>
cmake --build build --config Release
```

Replace `<path-to-SparkEngine-install-prefix>` with the actual install prefix you used (e.g. `~/SparkEngine-install`).

> **Warning:** Do not pass the SparkEngine build directory or its `bin/` subdirectory as the prefix path. Only the install prefix contains the required CMake config files.

## How Templates Relate to the Engine

Each template compiles into a **game module** — a shared library (`.dll` on Windows, `.so` on Linux) — not a standalone executable. The SparkEngine executable loads the game module at startup:

```bash
# Windows
SparkEngine.exe -game MyGame.dll

# Linux
./SparkEngine -game libMyGame.so
```

Because templates are separate shared libraries, they are built independently from the engine. You do not need the engine source tree at build time — only the installed SDK (headers + CMake config). However, a game module cannot be hot-swapped while the engine is running; you must restart the engine to pick up a rebuilt module. (AngelScript scripts *can* be hot-reloaded at runtime, but compiled C++ modules cannot.)

## Template Structure

Each template follows this layout:

```
TemplateName/
├── CMakeLists.txt          # Standalone CMake project using find_package(SparkEngine)
├── Source/
│   ├── GameModule.h        # IModule implementation (your game logic entry point)
│   └── GameModule.cpp      # DLL exports via SPARK_IMPLEMENT_MODULE()
├── spark.project.json      # Project metadata (name, version, default scene)
└── spark.modules.json      # Module loading configuration (DLL path, load order)
```

### Key Files

- **`CMakeLists.txt`** — Calls `find_package(SparkEngine REQUIRED)` and uses the `spark_add_game_module()` helper to create the shared library with correct definitions and linkage.
- **`GameModule.h`** — Your module class inheriting from `Spark::IModule`. Implement the lifecycle methods: `OnLoad()`, `OnUnload()`, `OnUpdate()`, `OnRender()`.
- **`GameModule.cpp`** — Uses `SPARK_IMPLEMENT_MODULE(YourModuleClass)` to generate the `CreateModule()` / `DestroyModule()` DLL exports the engine requires.

## Creating a New Project from a Template

1. Copy the template directory of your choice and rename it:
   ```bash
   cp -r Templates/EmptyProject MyGame   # or FPSStarter, RPGStarter, etc.
   cd MyGame
   ```

2. Rename every occurrence of the template's name (`EmptyProject`, `FPSStarter`, `MultiplayerArena`, `PlatformerKit`, or `RPGStarter`) to your project name. A one-liner from inside the copied directory:
   ```bash
   grep -rl 'EmptyProject' . | xargs sed -i 's/EmptyProject/MyGame/g'
   ```
   The editor's **File → New Project From Template** flow does this rename automatically via `ProjectManager::CreateProjectFromTemplate` — no manual substitution needed if you use the editor UI.

3. Build and run:
   ```bash
   cmake -B build -DCMAKE_PREFIX_PATH=<path-to-SparkEngine-install-prefix>
   cmake --build build --config Release

   # Windows
   SparkEngine.exe -game MyGame.dll

   # Linux
   ./SparkEngine -game libMyGame.so
   ```

## License

MIT License. See [LICENSE](../LICENSE) for details.
