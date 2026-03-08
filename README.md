# Spark Engine — Open-Source C++ Game Engine

[![Build SparkEngine](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml/badge.svg)](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml)
[![Release](https://github.com/Krilliac/SparkEngine/actions/workflows/release.yml/badge.svg)](https://github.com/Krilliac/SparkEngine/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Last Commit](https://img.shields.io/github/last-commit/Krilliac/SparkEngine)](https://github.com/Krilliac/SparkEngine/commits/master)

**Spark Engine** is a free, open-source 3D game engine written in C++20. It is designed for first-person shooters and other 3D games, with built-in support for DirectX 11 rendering, Bullet Physics, XAudio2 spatial audio, AngelScript hot-reload scripting, an ECS architecture (EnTT), and an ImGui-based visual editor. Cross-platform (Windows and Linux), modular, and MIT-licensed.

> **Early Development** — SparkEngine is under active development. Systems are being built out and stabilized. Expect rough edges.

> **AI Disclosure** — This project makes extensive use of AI-assisted development. All AI-generated code is reviewed, tested, and validated to ensure correctness, stability, and functional integrity. If the use of AI in development is a concern for you, this project may not be the right fit — but if you're open to it, contributions of any and all kinds are welcome!

### Why Spark Engine?

- **Complete game engine** — rendering, physics, audio, AI, animation, networking, scripting, and editor all in one package
- **Truly open-source** — MIT license, no royalties, no strings attached
- **Built for learning and modding** — clean C++20 codebase with 30+ toggleable CMake modules
- **Ready-to-download binaries** — pre-built Windows and Linux binaries on every commit

## Downloads

Latest binaries are published automatically on every commit to `master`.
The commit hash shown on each badge matches the build you are downloading.

[![Commit](https://img.shields.io/github/last-commit/Krilliac/SparkEngine?label=built%20from&style=flat-square)](https://github.com/Krilliac/SparkEngine/commits/master)

**Windows (VS 2022 · x64)**

[![Windows Release](https://img.shields.io/badge/Windows-Release-0078D4?style=for-the-badge&logo=windows)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Release.zip)
[![Windows Debug](https://img.shields.io/badge/Windows-Debug-555555?style=for-the-badge&logo=windows)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Debug.zip)

**Linux (GCC · ubuntu-24.04 · x64)**

[![Linux Release](https://img.shields.io/badge/Linux-Release-E95420?style=for-the-badge&logo=linux&logoColor=white)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Release.tar.gz)
[![Linux Debug](https://img.shields.io/badge/Linux-Debug-555555?style=for-the-badge&logo=linux&logoColor=white)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Debug.tar.gz)

> All downloads are from the [`latest` release](https://github.com/Krilliac/SparkEngine/releases/tag/latest). Each release body includes the exact commit hash and timestamp of the build.

## Key Features

### Rendering

DirectX 11 with multiple render paths (forward, deferred, forward+, clustered). PBR metallic/roughness materials, cascaded shadow mapping, SSAO, SSR, volumetric lighting, bloom, HDR tone mapping (Reinhard/ACES/Uncharted 2), temporal anti-aliasing, FXAA, MSAA (2x/4x/8x), IBL lighting, GPU particle system, decals, fog, and quality presets (Low/Medium/High/Ultra). Experimental Vulkan and OpenGL backends via an RHI abstraction layer. Optional DirectX Raytracing (DXR) support.

### Physics

Bullet Physics 3 integration with rigid bodies (static/kinematic/dynamic), collision shapes (box, sphere, capsule, cylinder, cone, mesh, convex hull, heightfield, compound), constraints (hinge, slider, fixed, generic), raycasting (single/multi-hit), sphere and box overlap queries, named physics materials, collision and trigger callbacks, and debug draw overlay.

### Audio

XAudio2 hardware-accelerated 3D spatial audio with distance attenuation, Doppler effects, pitch control, looping, master/SFX/music volume channels, and an object pool for efficient source management. Miniaudio as a cross-platform fallback.

### Gameplay

ECS architecture (EnTT) with FPS player controller, weapon system (bullet/rocket/grenade), class system, vehicle mechanics, gravity system, HUD (crosshairs, health bars, kill feed, minimap, compass), interactive objects, inventory system, quest system, day/night cycle, weather system, game mode management, heightmap terrain with quadtree LOD and texture splatting, mesh LOD, and decal system.

### AI & Navigation

Behavior tree framework (composite, decorator, and action nodes), NavMesh A* pathfinding with binary `.snav` format loading, perception system (vision cones, hearing ranges, memory), and steering behaviors (seek, flee, pursue, evade, flocking).

### Animation

Skeletal animation with bone hierarchies, keyframe clips, state machines with cross-fading, multi-layer blending (override/additive/layered with per-bone masks), inverse kinematics (two-bone, look-at, FABRIK), and root motion extraction. Supports FBX and glTF via Assimp.

### Networking

UDP client/server architecture with entity replication, client-side prediction with server reconciliation, lag compensation (hitbox rewinding with 1-second history), reliable/unreliable/ordered message channels, and network statistics (ping, jitter, packet loss, bandwidth).

### Scripting

AngelScript with Unity-style hot-reload, lifecycle callbacks (Start, Update, OnCollision), full engine API bindings (math, components, input, entities), per-file module isolation, and runtime error reporting.

### Editor

ImGui-powered visual editor with scene hierarchy, inspector, asset browser, game viewport, gizmos (ImGuizmo), node graphs (imnodes), animation timeline, material editor, visual scripting, terrain editing, weapon editor, profiler, version control integration, build/deployment system, level streaming, docking, and theming.

### Procedural Generation

Noise functions (Perlin, Simplex, Worley, FBM, ridged multifractal, domain warping), heightmap generation with thermal and hydraulic erosion, procedural meshes (plane, box, sphere, cylinder, cone, torus, terrain, rock, tree), rule-based object placement, and Wave Function Collapse room/dungeon layout generation.

### Save System

ECS-aware serialization with miniz compression, JSON format, multiple save slots, quicksave/quickload, rotating autosaves, per-component serializer registry, and metadata tracking (scene, player class, playtime).

### Tooling

Crash handler with minidump generation, all-thread stack traces, screenshot capture, system info collection, and optional HTTP report upload. Debug console overlay with 200+ commands spanning engine, graphics, physics, audio, input, networking, scripting, and profiling. Performance profiler, memory debugger, frame inspector, and debug draw overlay.

## Quick Start

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/Krilliac/SparkEngine.git
cd SparkEngine

# Configure (Windows — Visual Studio 2022)
.\generate.bat -g "Visual Studio 17 2022" release

# Configure (Linux/macOS)
chmod +x generate.sh
./generate.sh release -g Ninja

# Build (Windows)
.\build.ps1 -config Release -editor -angelscript

# Build (Linux/macOS)
./build.sh release

# Run
# Windows:  build/bin/SparkEngine.exe
# Linux:    build/bin/SparkEngine
```

### Requirements

- **Compiler**: MSVC v143 (Visual Studio 2022), GCC 11+, or Clang 14+ with C++20 support
- **Build System**: CMake 3.16+, Ninja (recommended on Linux)
- **Linux packages**: `build-essential`, `ninja-build`, `cmake` (e.g. `sudo apt install build-essential ninja-build cmake`)
- **Graphics**: DirectX 11 capable GPU (Windows), Vulkan SDK (optional), OpenGL 4.5 (optional)
- **Platform**: Windows 10+ (primary), Linux (experimental)

## Tools

### SparkShaderCompiler

Standalone offline shader compilation tool using the RHI cross-compilation pipeline. Compiles HLSL/GLSL shaders for D3D11, Vulkan, and OpenGL backends.

```bash
# Compile a single shader
SparkShaderCompiler BasicVS.hlsl -stage vertex -backend d3d11 -o BasicVS.cso

# Cross-compile HLSL to SPIR-V
SparkShaderCompiler PBR.hlsl -stage pixel -backend vulkan -o PBR.spv

# Validate without writing output
SparkShaderCompiler Water.glsl -stage vertex -backend opengl -validate

# Print shader reflection data
SparkShaderCompiler Phong.hlsl -stage pixel -backend vulkan -reflect
```

Options: `-stage` (vertex/pixel/geometry/hull/domain/compute), `-backend` (d3d11/vulkan/opengl/auto), `-entry` (entry point), `-D` (defines), `-I` (include paths), `-O`/`-Od` (optimization), `-Zi` (debug info), `-v` (verbose).

### SparkConsole

Standalone debug console application that communicates with SparkEngine via named pipes. Provides real-time engine inspection with 200+ commands covering engine status, graphics, physics, audio, networking, and performance monitoring.

## Controls

| Input | Action |
|---|---|
| W / A / S / D | Move |
| Mouse | Look |
| Space | Jump |
| Ctrl | Crouch |
| Left Click | Fire / Capture Mouse |
| Esc | Release Mouse / Menu |
| ` (Backtick) | Toggle Debug Console |

## Architecture

```
+-------------------+-------------------+-------------------+
|    Rendering      |     Physics       |      Audio        |
|                   |                   |                   |
|  GraphicsEngine   |  PhysicsSystem    |  AudioEngine      |
|  ShaderManager    |  Bullet3 World    |  XAudio2 / mini   |
|  PostProcessing   |  Collision        |  3D Spatial       |
|  PBR Materials    |  Raycasting       |  Object Pool      |
+-------------------+-------------------+-------------------+
|    Scripting      |    Input & UI     |    Core & ECS     |
|                   |                   |                   |
|  AngelScript VM   |  InputManager     |  EnTT ECS         |
|  Hot Reload       |  Gamepad Support  |  SceneManager     |
|  Engine Bindings  |  ImGui Editor     |  AssetPipeline    |
+-------------------+-------------------+-------------------+
|    Gameplay       |    AI & Nav       |    Networking     |
|                   |                   |                   |
|  PlayerController |  BehaviorTree     |  UDP Client/Srv   |
|  WeaponSystem     |  NavMesh (A*)     |  Prediction       |
|  VehicleSystem    |  Perception       |  Lag Compensation |
+-------------------+-------------------+-------------------+
|    Procedural     |    Animation      |    Utilities      |
|                   |                   |                   |
|  Noise (Perlin+)  |  Skeletal Anim    |  CrashHandler     |
|  Erosion / WFC    |  IK / Blending    |  Console (200+)   |
|  Mesh Generation  |  State Machines   |  Profiler / Debug |
+-------------------+-------------------+-------------------+
```

## Project Structure

```
SparkEngine/
|-- SparkEngine/
|   |-- Source/
|       |-- Audio/           # XAudio2 3D audio engine
|       |-- Camera/          # First-person camera controller
|       |-- Console/         # Debug console integration
|       |-- Core/            # Entry point, engine framework
|       |-- Engine/
|       |   |-- AI/          # Behavior trees, NavMesh, perception, steering
|       |   |-- Animation/   # Skeletal animation, IK, state machines
|       |   |-- ECS/         # Entity component system (EnTT)
|       |   |-- Networking/  # UDP multiplayer, replication, lag compensation
|       |   |-- Procedural/  # Noise, erosion, mesh generation, WFC
|       |   |-- SaveSystem/  # Serialization, compression, save slots
|       |   |-- Scripting/   # AngelScript VM, hot-reload, API bindings
|       |-- Enums/           # Shared enumerations
|       |-- Game/            # Player, weapons, vehicles, HUD, terrain, inventory
|       |-- Graphics/        # DX11 renderer, PBR, post-processing, particles, RHI
|       |-- Input/           # Keyboard, mouse, gamepad input
|       |-- Physics/         # Bullet Physics integration
|       |-- Projectiles/     # Weapon projectile system
|       |-- SceneManager/    # Scene and level management
|       |-- Utils/           # Logging, profiler, crash handler, console, debug tools
|-- SparkEditor/
|   |-- Source/              # ImGui editor (22 subsystems)
|-- SparkConsole/
|   |-- src/                 # Standalone debug console application
|-- SparkShaderCompiler/
|   |-- src/                 # Offline shader compilation tool
|-- ThirdParty/              # Git submodules (see Dependencies)
|-- Shaders/
|   |-- HLSL/               # DirectX shaders
|   |-- GLSL/               # OpenGL shaders
|   |-- Compiled/           # Pre-compiled DirectX bytecode (.cso)
|-- Assets/
|   |-- Models/             # 3D model files (.obj)
|   |-- Scenes/             # Level/scene JSON files
|   |-- Scripts/            # AngelScript game scripts
|-- Tests/                   # 35 unit tests (CTest integration)
|-- docs/                    # Doxygen docs, roadmap, status reports
|-- .github/
|   |-- workflows/          # CI/CD (build + release)
|   |-- prompts/            # AI assistant prompt library
|   |-- dependabot.yml      # Automated dependency updates
|-- CMakeLists.txt           # Cross-platform build configuration
|-- build.ps1 / build.sh    # Build scripts
|-- generate.bat / .sh      # CMake configure scripts
```

## Dependencies

All external dependencies are managed as git submodules under `ThirdParty/`. Dependabot is configured to propose weekly update PRs.

| Library | Submodule Path | Purpose |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | `UI/imgui` | Immediate-mode GUI (docking branch) |
| [EnTT](https://github.com/skypjack/entt) | `ECS/entt` | Entity component system |
| [Bullet Physics](https://github.com/bulletphysics/bullet3) | `Physics/bullet3` | Physics engine |
| [AngelScript](https://github.com/codecat/angelscript-mirror) | `Scripting/angelscript-mirror` | Scripting language |
| [curl](https://github.com/curl/curl) | `Networking/curl` | HTTP client (disabled by default) |
| [miniz](https://github.com/richgel999/miniz) | `Utils/miniz` | Compression (zlib-compatible) |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | `Utils/tinyobjloader` | OBJ file loader |

The following libraries are included directly in the source tree:

| Library | Purpose |
|---|---|
| [DirectXTK](https://github.com/Microsoft/DirectXTK) | DirectX 11 toolkit |
| [Assimp](https://github.com/assimp/assimp) | 3D model import (FBX, glTF, etc.) |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | 3D editor gizmos |
| [imnodes](https://github.com/Nelarius/imnodes) | Node graph editor |
| [GLM](https://github.com/g-truc/glm) | Math library |
| [RapidJSON](https://github.com/Tencent/rapidjson) | JSON parsing |
| [spdlog](https://github.com/gabime/spdlog) | Structured logging |
| [stb](https://github.com/nothings/stb) | Image loading |
| [miniaudio](https://github.com/mackron/miniaudio) | Cross-platform audio fallback |

## Tests

35 unit tests covering all major engine systems, built with a lightweight internal test framework (no external test dependencies). Integrated with CMake's CTest.

```bash
# Build and run tests
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Test coverage includes: math utilities, object pool, ECS world, game modes, AI behavior trees, animation, NavMesh, physics, input, save system, event system, weather, inventory, quests, day/night cycle, performance stats, light manager, fog, screen-space effects, post-processing pipeline, sequencer, mesh LOD, console command history, debug tools, noise generator, string utilities, ring buffer, UUID, file utilities, config parser, frustum culling, coroutine scheduler, tweening, and temporal effects.

## Build Options

All options are passed to CMake via `-D<OPTION>=ON/OFF`.

| Option | Default | Description |
|---|:---:|---|
| `BUILD_TESTS` | OFF | Unit tests (CTest) |
| `ENABLE_EDITOR` | ON | ImGui visual editor |
| `ENABLE_GRAPHICS` | ON | Graphics rendering system |
| `ENABLE_PHYSX` | ON | Physics engine (Bullet) |
| `ENABLE_LUA` | ON | Lua scripting support |
| `ENABLE_PROFILING` | ON | Performance profiling |
| `ENABLE_VULKAN` | ON | Vulkan graphics backend |
| `ENABLE_OPENGL` | ON | OpenGL graphics backend |
| `ENABLE_AI` | ON | AI and navigation systems |
| `ENABLE_ANIMATION` | ON | Skeletal animation |
| `ENABLE_SAVE_SYSTEM` | ON | Save/load system |
| `ENABLE_TERRAIN_SYSTEM` | ON | Heightmap terrain |
| `ENABLE_POST_PROCESSING` | ON | Bloom, tone mapping, FXAA |
| `ENABLE_LIGHTING_SYSTEM` | ON | Advanced lighting / IBL |
| `ENABLE_ADVANCED_INPUT` | ON | Extended input features |
| `ENABLE_ASSET_STREAMING` | ON | Runtime asset streaming |
| `ENABLE_HOT_RELOAD` | ON | Script hot-reload |
| `ENABLE_COLLABORATIVE` | ON | Collaborative features |
| `ENABLE_PROCEDURAL` | ON | Procedural generation |
| `ENABLE_CINEMATIC` | ON | Cinematic sequencer |
| `ENABLE_DECALS` | ON | Decal system |
| `ENABLE_MESH_LOD` | ON | Mesh level-of-detail |
| `ENABLE_WEATHER` | ON | Dynamic weather system |
| `ENABLE_INVENTORY` | ON | Inventory system |
| `ENABLE_QUEST_SYSTEM` | ON | Quest tracking |
| `ENABLE_EVENT_SYSTEM` | ON | Event system |
| `ENABLE_DAY_NIGHT` | ON | Day/night cycle |
| `ENABLE_SCREEN_SPACE` | ON | Screen-space effects |
| `ENABLE_FOG_SYSTEM` | ON | Volumetric fog |
| `ENABLE_NETWORKING` | OFF | Networking (curl, disabled) |
| `ENABLE_DXR` | OFF | DirectX Raytracing (needs D3D12) |
| `ENABLE_SDL2` | OFF | SDL2 cross-platform input |

```bash
# Example: minimal build without editor or scripting
cmake -B build -DENABLE_EDITOR=OFF -DENABLE_LUA=OFF
```

## CI/CD

Two GitHub Actions workflows run automatically:

**`build.yml`** — runs on every push / PR to `main`, `develop`, and `feature/**`:
- Platforms: Windows (MSVC VS 2022 + experimental VS 2026^1), Linux GCC, Linux Clang
- Configurations: Debug and Release matrix
- Steps: checkout with submodules, CMake configure, build, test (Release only), artifact upload
- Artifacts retained for 7 days

> ^1 **VS 2026 (v144 toolset):** The VS 2026 CI job is included for forward compatibility but will be skipped until GitHub Actions runners ship with the v144 platform toolset. It is marked `continue-on-error` and does not gate merges.

**`release.yml`** — runs on every push to `master` / `main`:
- Builds Windows (VS 2022) and Linux (GCC) in Debug + Release
- Packages each configuration into a zip / tar.gz archive
- Creates or updates the rolling [`latest` GitHub Release](https://github.com/Krilliac/SparkEngine/releases/tag/latest) with all four binaries and the exact commit hash

## Documentation

- **[Troubleshooting Guide](TROUBLESHOOTING.md)** — Startup issues, debug commands, common fixes
- **[Feature Roadmap](docs/FEATURE_ROADMAP.md)** — Planned features across 3 priority tiers
- **[Project Status](docs/PROJECT_STATUS.md)** — Current system status and recent changes
- **[API Documentation](docs/README.md)** — Doxygen-based auto-generated API docs
- **[AI Prompt Library](.github/AI_README.md)** — Prompts for Copilot, GPT, Claude, and others

### Generating API Docs

```bash
cd docs
./generate-docs.sh        # One-time generation
./auto-update.sh monitor  # Continuous monitoring
```

Requires `doxygen` and `graphviz`.

## Templates & Game Modules

SparkEngine supports **game modules** — shared libraries (`.dll` on Windows, `.so` on Linux) that the engine loads at startup. This lets you build your game independently from the engine using only the installed SDK.

### Quick Start (Game Module)

```bash
# 1. Install SparkEngine to a local prefix
cmake --install build --prefix ~/SparkEngine-install

# 2. Copy the empty template and build it
cp -r Templates/EmptyProject MyGame && cd MyGame
# Replace {{PROJECT_NAME}} placeholders with your project name
cmake -B build -DCMAKE_PREFIX_PATH=~/SparkEngine-install
cmake --build build --config Release

# 3. Run your module through the engine
# Windows:  SparkEngine.exe -game MyGame.dll
# Linux:    ./SparkEngine -game libMyGame.so
```

> **Important:** You must use the **install prefix** (the path passed to `--prefix`) as `CMAKE_PREFIX_PATH`, not the build directory. The build tree does not contain the CMake config files that `find_package(SparkEngine)` needs.

See **[Templates/README.md](Templates/README.md)** for full documentation on prerequisites, project structure, and creating new projects.

For additional templates covering physics, AI, networking, procedural generation, and more, check out **[SparkTemplates](https://github.com/Krilliac/SparkTemplates)**.

## License

MIT License. See [LICENSE](LICENSE) for details.
