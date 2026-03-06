# Spark Engine

[![Build SparkEngine](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml/badge.svg)](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml)
[![Release](https://github.com/Krilliac/SparkEngine/actions/workflows/release.yml/badge.svg)](https://github.com/Krilliac/SparkEngine/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Last Commit](https://img.shields.io/github/last-commit/Krilliac/SparkEngine)](https://github.com/Krilliac/SparkEngine/commits/master)

A modular, high-performance C++ game engine built for 3D FPS titles and beyond. Features a DirectX 11 rendering pipeline, Bullet Physics integration, XAudio2 spatial audio, AngelScript hot-reload scripting, an ECS architecture powered by EnTT, and an integrated ImGui editor.

> **Early Development** — SparkEngine is under active development. Systems are being built out and stabilized. Expect rough edges.

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

**Rendering** — DirectX 11 with PBR materials, deferred/forward+ pipelines, shadow mapping (PCF/VSM/CSM), SSAO, SSR, volumetric lighting, bloom, tone mapping (Reinhard/ACES/Uncharted 2), FXAA, IBL lighting, and a particle system. Experimental Vulkan and OpenGL backends via an RHI abstraction layer.

**Physics** — Full Bullet Physics integration with rigid bodies, collision shapes (box/sphere/capsule/mesh/convex hull), constraints (hinge/slider/fixed), raycasting, overlap tests, and collision callbacks.

**Audio** — XAudio2-based 3D spatial audio with distance attenuation, Doppler effects, and an object pool for efficient source management. Miniaudio as a cross-platform fallback.

**Gameplay** — ECS architecture (EnTT), FPS player controller, weapon system (bullet/rocket/grenade), class system, vehicle mechanics, gravity system, HUD (crosshairs, health bars, kill feed, minimap, compass), heightmap terrain with LOD, mesh LOD, decal system, and a save/load system.

**Editor** — Optional ImGui-powered visual editor with scene hierarchy, inspector, asset browser, gizmos (ImGuizmo), node graphs (imnodes), docking, theming, and viewport rendering.

**Scripting** — AngelScript with Unity-style hot-reload, full engine API bindings, and debugging support.

**AI** — NavMesh-based pathfinding with binary `.snav` format loading.

**Tooling** — Crash handler with minidumps and stack traces, debug console overlay with 200+ commands, performance profiler, JSON scene serialization, and a prefab system.

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
- **Build System**: CMake 3.16+
- **Graphics**: DirectX 11 capable GPU (Windows), Vulkan SDK (optional), OpenGL 4.5 (optional)
- **Platform**: Windows 10+ (primary), Linux/macOS (experimental)

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
|    Gameplay       |    AI & Nav       |    Utilities      |
|                   |                   |                   |
|  PlayerController |  NavMesh          |  CrashHandler     |
|  WeaponSystem     |  Pathfinding      |  Logger (spdlog)  |
|  VehicleSystem    |  Binary .snav     |  Timer / FileIO   |
+-------------------+-------------------+-------------------+
```

## Project Structure

```
SparkEngine/
|-- Spark Engine/
|   |-- Source/
|       |-- Audio/           # XAudio2 3D audio engine
|       |-- Camera/          # First-person camera controller
|       |-- Console/         # Debug console integration
|       |-- Core/            # Entry point, engine framework
|       |-- Engine/          # Core systems (ECS, AI, animation, save, procedural)
|       |-- Enums/           # Shared enumerations
|       |-- Game/            # Gameplay logic, HUD, weapons, vehicles
|       |-- Graphics/        # DX11 renderer, shaders, post-processing, RHI
|       |-- Input/           # Keyboard, mouse, gamepad input
|       |-- Physics/         # Bullet Physics integration
|       |-- Projectiles/     # Weapon projectile system
|       |-- SceneManager/    # Scene and level management
|       |-- Utils/           # Logging, timers, file I/O, crash handling
|-- SparkEditor/
|   |-- Source/              # ImGui editor (22 subsystems)
|-- SparkConsole/
|   |-- src/                 # Debug console application
|-- ThirdParty/              # Git submodules (see Dependencies)
|-- Shaders/
|   |-- HLSL/               # DirectX shaders
|   |-- GLSL/               # OpenGL shaders
|   |-- SPIRV/              # Pre-compiled SPIR-V
|-- Assets/
|   |-- Models/             # 3D model files
|   |-- Scenes/             # Level/scene JSON files
|   |-- Scripts/            # Game scripts
|-- Tests/                   # Unit tests
|-- docs/                    # Doxygen docs, roadmap, status reports
|-- .github/
|   |-- workflows/          # CI/CD (build + test)
|   |-- prompts/            # AI assistant prompt library
|   |-- dependabot.yml      # Automated dependency updates
|-- CMakeLists.txt           # Cross-platform build configuration
|-- build.ps1 / build.sh    # Build scripts
|-- generate.bat / .sh      # CMake configure scripts
```

## Dependencies

All dependencies are managed as git submodules under `ThirdParty/`. Dependabot is configured to propose weekly update PRs.

| Library | Path | Purpose |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | `UI/imgui` | Immediate-mode GUI |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | `UI/ImGuizmo` | 3D editor gizmos |
| [imnodes](https://github.com/Nelarius/imnodes) | `UI/imnodes` | Node graph editor |
| [EnTT](https://github.com/skypjack/entt) | `ECS/entt` | Entity component system |
| [DirectXTK](https://github.com/Microsoft/DirectXTK) | `Rendering/DirectXTK` | DirectX 11 toolkit |
| [Bullet Physics](https://github.com/bulletphysics/bullet3) | `Physics/bullet3` | Physics engine |
| [Assimp](https://github.com/assimp/assimp) | `FileFormats/assimp` | 3D model import |
| [miniaudio](https://github.com/mackron/miniaudio) | `Audio/miniaudio` | Cross-platform audio |
| [AngelScript](https://github.com/codecat/angelscript-mirror) | `Scripting/angelscript-mirror` | Scripting language |
| [curl](https://github.com/curl/curl) | `Networking/curl` | HTTP client (disabled) |
| [GLM](https://github.com/g-truc/glm) | `Utils/glm` | Math library |
| [miniz](https://github.com/richgel999/miniz) | `Utils/miniz` | Compression |
| [RapidJSON](https://github.com/Tencent/rapidjson) | `Utils/rapidjson` | JSON parsing |
| [spdlog](https://github.com/gabime/spdlog) | `Utils/spdlog` | Structured logging |
| [stb](https://github.com/nothings/stb) | `Utils/stb` | Image loading and more |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | `Utils/tinyobjloader` | OBJ file loader |

## Build Options

All options are passed to CMake via `-D<OPTION>=ON/OFF` or edited directly in `CMakeLists.txt`.

| Option | Default | Description |
|---|:---:|---|
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
| `ENABLE_NETWORKING` | OFF | Networking (CURL, disabled) |
| `ENABLE_DXR` | OFF | DirectX Raytracing (needs D3D12) |
| `ENABLE_SDL2` | OFF | SDL2 cross-platform input |

```bash
# Example: minimal build without editor or scripting
cmake -B build -DENABLE_EDITOR=OFF -DENABLE_LUA=OFF
```

## CI/CD

Two GitHub Actions workflows run automatically:

**`build.yml`** — runs on every push / PR to `main`, `develop`, and `feature/**`:
- Platforms: Windows (MSVC VS 2022 + experimental VS 2026), Linux GCC, Linux Clang
- Configurations: Debug and Release matrix
- Steps: checkout with submodules, CMake configure, build, test (Release only), artifact upload
- Artifacts retained for 7 days

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

## License

MIT License. See [LICENSE](LICENSE) for details.
