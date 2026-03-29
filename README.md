# Spark Engine — Open-Source C++ Game Engine

[![Build SparkEngine](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml/badge.svg?branch=Working)](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml?query=branch%3AWorking)
[![Publish](https://github.com/Krilliac/SparkEngine/actions/workflows/release.yml/badge.svg?branch=Working)](https://github.com/Krilliac/SparkEngine/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Downloads](https://img.shields.io/github/downloads/Krilliac/SparkEngine/total?color=brightgreen)](https://github.com/Krilliac/SparkEngine/releases)
[![Last Commit](https://img.shields.io/github/last-commit/Krilliac/SparkEngine)](https://github.com/Krilliac/SparkEngine/commits/Working)
[![Lines of Code](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/loc.json)](https://github.com/Krilliac/SparkEngine)
[![Source Files](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/files.json)](https://github.com/Krilliac/SparkEngine)

**Platforms & Compilers:**

[![Windows MSVC](https://img.shields.io/badge/Windows-MSVC_v143_(VS_2022)-0078D4?logo=windows&logoColor=white)](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml)
[![Windows MSVC v144](https://img.shields.io/badge/Windows-MSVC_v144_(VS_2026)-0078D4?logo=windows&logoColor=white)](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml)
[![Linux GCC](https://img.shields.io/badge/Linux-GCC_13+-E95420?logo=linux&logoColor=white)](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml)
[![Linux Clang](https://img.shields.io/badge/Linux-Clang_17+-A9A9A9?logo=llvm&logoColor=white)](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml)
[![macOS](https://img.shields.io/badge/macOS-Experimental-999999?logo=apple&logoColor=white)](https://github.com/Krilliac/SparkEngine)

**Quality & Testing:**

[![Tests](https://img.shields.io/badge/tests-2%2C108_cases-brightgreen)](https://github.com/Krilliac/SparkEngine/tree/Working/Tests)
[![clang--format](https://img.shields.io/badge/style-clang--format-blue)](https://github.com/Krilliac/SparkEngine/blob/Working/.clang-format)
[![clang--tidy](https://img.shields.io/badge/analysis-clang--tidy-blue)](https://github.com/Krilliac/SparkEngine/blob/Working/.clang-tidy)

**Sanitizers (CI-enforced) — click to download latest report:**

[![ASan](https://img.shields.io/badge/⬇_ASan-address_errors-2ea44f?logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-asan-ubsan-lsan.zip)
[![UBSan](https://img.shields.io/badge/⬇_UBSan-undefined_behavior-2ea44f?logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-asan-ubsan-lsan.zip)
[![LSan](https://img.shields.io/badge/⬇_LSan-memory_leaks-2ea44f?logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-asan-ubsan-lsan.zip)
[![TSan](https://img.shields.io/badge/⬇_TSan-data_races-2ea44f?logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-tsan.zip)
[![MSan](https://img.shields.io/badge/⬇_MSan-uninitialized_memory_(advisory)-yellow?logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-msan.zip)
[![Coverage](https://img.shields.io/badge/⬇_Coverage-lcov_report-blue?logo=codecov&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/coverage-report.zip)

**Rendering Backends:**

[![DirectX 11](https://img.shields.io/badge/DX11-Stable-brightgreen)](https://github.com/Krilliac/SparkEngine)
[![DirectX 12](https://img.shields.io/badge/DX12-Experimental-yellow)](https://github.com/Krilliac/SparkEngine)
[![Vulkan](https://img.shields.io/badge/Vulkan-Experimental-yellow?logo=vulkan&logoColor=white)](https://github.com/Krilliac/SparkEngine)
[![OpenGL](https://img.shields.io/badge/OpenGL_4.5-Experimental-yellow?logo=opengl&logoColor=white)](https://github.com/Krilliac/SparkEngine)

**Spark Engine** is a free, open-source 3D game engine written in C++23. While originally built for first-person shooters, Spark Engine is evolving into a general-purpose game engine capable of supporting a wide range of genres — from FPS and action games to open-world RPGs, MMOs, battle royales, and more. Built-in support for DirectX 11 rendering, Jolt Physics, XAudio2 spatial audio, AngelScript hot-reload scripting, an ECS architecture (EnTT), and an ImGui-based visual editor. Features inspired by HeroEngine's MMO technology include seamless world streaming, area-based server architecture, floating-point origin rebasing for large worlds, and collaborative multi-user editing. Cross-platform (Windows and Linux), modular, and MIT-licensed.

> **Early Development** — SparkEngine is under active development. Systems are being built out and stabilized. Expect rough edges.

> **AI Disclosure** — This project makes extensive use of AI-assisted development. All AI-generated code is reviewed, tested, and validated to ensure correctness, stability, and functional integrity. If the use of AI in development is a concern for you, this project may not be the right fit — but if you're open to it, contributions of any and all kinds are welcome!

### Why Spark Engine?

- **Complete game engine** — rendering, physics, audio, AI, animation, networking, scripting, and editor all in one package
- **General-purpose** — FPS, RPG, MMO, battle royale, open-world — build any genre with one engine
- **Scalable multiplayer** — from single-player to MMO-scale via area-based server architecture
- **Large world support** — seamless area streaming and floating-point origin rebasing for worlds of any size
- **Truly open-source** — MIT license, no royalties, no strings attached
- **Built for learning and modding** — clean C++23 codebase with 30+ toggleable CMake modules
- **Ready-to-download binaries** — pre-built Windows and Linux binaries on every commit

## Downloads

Latest binaries are published automatically on every commit to `master`.
The commit hash shown on each badge matches the build you are downloading.

[![Commit](https://img.shields.io/github/last-commit/Krilliac/SparkEngine?label=built%20from&style=flat-square)](https://github.com/Krilliac/SparkEngine/commits/Working)

**Windows (VS 2022 · x64)**

[![Windows Release](https://img.shields.io/badge/Windows-Release-0078D4?style=for-the-badge&logo=windows)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Release.zip)
[![Windows Debug](https://img.shields.io/badge/Windows-Debug-555555?style=for-the-badge&logo=windows)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Debug.zip)

**Linux (GCC · ubuntu-24.04 · x64)**

[![Linux Release](https://img.shields.io/badge/Linux-Release-E95420?style=for-the-badge&logo=linux&logoColor=white)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Release.tar.gz)
[![Linux Debug](https://img.shields.io/badge/Linux-Debug-555555?style=for-the-badge&logo=linux&logoColor=white)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Debug.tar.gz)

> All downloads are from the [`latest` release](https://github.com/Krilliac/SparkEngine/releases/tag/latest). Each release body includes the exact commit hash and timestamp of the build.

### CI Build Artifacts (latest Working branch)

Build artifacts from the most recent CI run on the `Working` branch. These are per-commit builds, updated automatically. Provided via [nightly.link](https://nightly.link) (no GitHub login required).

**Build Binaries:**

[![Windows VS2022 Release](https://img.shields.io/badge/⬇_Windows_VS2022-Release-0078D4?style=for-the-badge&logo=windows&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/SparkEngine-Windows-VS2022-Release.zip)
[![Linux GCC Release](https://img.shields.io/badge/⬇_Linux_GCC-Release-E95420?style=for-the-badge&logo=linux&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/SparkEngine-Linux-GCC-Release.zip)
[![Linux Clang Release](https://img.shields.io/badge/⬇_Linux_Clang-Release-A9A9A9?style=for-the-badge&logo=llvm&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/SparkEngine-Linux-Clang-Release.zip)

**Sanitizer Reports:**

[![ASan+UBSan+LSan Report](https://img.shields.io/badge/⬇_ASan+UBSan+LSan-report-2ea44f?style=for-the-badge&logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-asan-ubsan-lsan.zip)
[![TSan Report](https://img.shields.io/badge/⬇_TSan-report-2ea44f?style=for-the-badge&logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-tsan.zip)
[![MSan Report](https://img.shields.io/badge/⬇_MSan-report_(advisory)-yellow?style=for-the-badge&logo=gnuprivacyguard&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-msan.zip)

**Code Coverage:**

[![Coverage Report](https://img.shields.io/badge/⬇_Coverage-lcov_report-blue?style=for-the-badge&logo=codecov&logoColor=white)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/coverage-report.zip)

## Key Features

### Rendering

DirectX 11 with multiple render paths (forward, deferred, forward+, clustered). PBR metallic/roughness materials, cascaded shadow mapping, SSAO, SSR, volumetric lighting, bloom, HDR tone mapping (Reinhard/ACES/Uncharted 2), temporal anti-aliasing, FXAA, MSAA (2x/4x/8x), IBL lighting, GPU particle system, decals, fog, and quality presets (Low/Medium/High/Ultra). Experimental Vulkan and OpenGL backends via an RHI abstraction layer. Optional DirectX Raytracing (DXR) support. Automatic fallback to NullRHIDevice for headless servers, plus full CPU software rendering via OpenGL + Mesa llvmpipe for GPU-less environments.

### Physics

Jolt Physics integration with rigid bodies (static/kinematic/dynamic), 15 collision shapes (box, sphere, capsule, cylinder, cone, mesh, convex hull, heightfield, compound), 12 constraint types (hinge, slider, fixed, generic), raycasting (single/multi-hit), sphere and box overlap queries, named physics materials, collision and trigger callbacks, character controller with CCD, vehicle physics, ragdoll system, soft body/cloth simulation, deterministic mode for replay/networking, multi-threaded job dispatch, and debug draw overlay.

### Audio

XAudio2 hardware-accelerated 3D spatial audio with distance attenuation, Doppler effects, pitch control, looping, master/SFX/music volume channels, and an object pool for efficient source management. Miniaudio as a cross-platform fallback.

### Gameplay

ECS architecture (EnTT) with FPS player controller, weapon system (bullet/rocket/grenade), class system, vehicle mechanics, gravity system, HUD (crosshairs, health bars, kill feed, minimap, compass), interactive objects, inventory system, quest system, day/night cycle, weather system, game mode management, heightmap terrain with quadtree LOD and texture splatting, mesh LOD, and decal system.

### AI & Navigation

Behavior tree framework (composite, decorator, and action nodes), NavMesh A* pathfinding (Recast/Detour) with dynamic obstacles and off-mesh links, perception system (vision cones, hearing ranges, memory), steering behaviors (seek, flee, pursue, evade, flocking), tactical point system (EQS-like environmental queries), cover system, formation system, group AI coordination, AI budget limiter for 100+ agents, AI director for scripted events, and collision avoidance.

### Animation

Skeletal animation with bone hierarchies, keyframe clips, state machines with cross-fading, multi-layer blending (override/additive/layered with per-bone masks), inverse kinematics (two-bone, look-at, FABRIK), and root motion extraction. Supports FBX and glTF via Assimp.

### Networking

UDP client/server architecture with entity replication, client-side prediction with server reconciliation, lag compensation (hitbox rewinding with 1-second history), reliable/unreliable/ordered message channels, delta snapshot compression, sub-tick input precision, connection scope filtering, network instability simulation (for testing), and network statistics (ping, jitter, packet loss, bandwidth). Area-based server architecture (inspired by HeroEngine) with WorldServer coordination, per-area AreaServer instances, cross-area entity migration, dynamic load balancing, and player session management across area transitions — enabling MMO-scale multiplayer worlds.

### Scripting

AngelScript with Unity-style hot-reload (file watcher with debouncing and state preservation), lifecycle callbacks (Start, Update, OnCollision), full engine API bindings (math, components, input, entities), per-file module isolation, client/server script context separation for multiplayer, and runtime error reporting.

### Editor

ImGui-powered visual editor with 52 specialized panels: scene hierarchy, inspector, asset browser, game viewport, gizmos (ImGuizmo), node graphs (imnodes), animation timeline, material editor, terrain editing, weapon editor, profiler, AI editor, physics debug, 2D editors, FPS tools, version control integration, build/deployment system, level streaming, search, prefab system, project management, scene statistics, collaborative multi-user editing (HeroEngine-inspired node locking, edit broadcasting, peer presence awareness), docking, and theming. Full undo/redo support and play-mode editing.

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

- **Compiler**: MSVC v143 (Visual Studio 2022 17.6+), GCC 13+, or Clang 17+ with C++23 support
- **Build System**: CMake 3.25+, Ninja (recommended on Linux)
- **Linux packages**: `build-essential`, `ninja-build`, `cmake` (e.g. `sudo apt install build-essential ninja-build cmake`)
- **Graphics**: DirectX 11 capable GPU (Windows), Vulkan SDK (optional), OpenGL 4.5 (optional)
- **Platform**: Windows 10+ (primary), Linux (experimental)

### Platform Support Matrix

| Platform / Backend | Status | Notes |
|---|:---:|---|
| **Windows 10+ (MSVC v143)** | **Stable** | Primary development platform, fully tested in CI |
| **Windows (MSVC v144 / VS 2026)** | Experimental | CI job included but skipped until runners ship v144 toolset |
| **Linux (GCC 13+)** | Experimental | CI tested on ubuntu-24.04; pre-built binaries available |
| **Linux (Clang 17+)** | Experimental | CI tested; some platform-specific features may be missing |
| **macOS (Apple Clang)** | Experimental | Builds with C++23 support; no CI or pre-built binaries yet |
| **DirectX 11** | **Stable** | Primary rendering backend |
| **Vulkan** | Experimental | Via RHI abstraction layer; requires Vulkan SDK |
| **OpenGL 4.5** | Experimental | Via RHI abstraction layer; GLSL shaders in `Shaders/GLSL/` |
| **DirectX Raytracing (DXR)** | Experimental | Requires D3D12; disabled by default (`ENABLE_DXR=OFF`) |
| **Networking (UDP)** | Experimental | Enabled by default (`ENABLE_NETWORKING=ON`); see [Networking](#networking-configuration) |

> **What does "Experimental" mean?** These platforms and backends compile and have basic functionality, but are not yet fully tested, may have missing features, and are not guaranteed to work in all configurations. Bug reports are welcome!

## Tools

### SparkBuild

[SparkBuild](https://github.com/Krilliac/SparkBuild) is a standalone C++ build tool for SparkEngine. A pre-built binary is included at `tools/SparkBuild.exe` and updated automatically every week via GitHub Actions. You can also update it manually:

```bash
# Windows:  .\tools\update-sparkbuild.ps1
# Linux:    ./tools/update-sparkbuild.sh
```

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
|  ShaderManager    |  Jolt Physics     |  XAudio2 / mini   |
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
|  WeaponSystem     |  NavMesh (A*)     |  AreaServer       |
|  VehicleSystem    |  Perception       |  WorldServer      |
+-------------------+-------------------+-------------------+
|    Procedural     |    Animation      |  Large Worlds     |
|                   |                   |                   |
|  Noise (Perlin+)  |  Skeletal Anim    |  Origin Rebasing  |
|  Erosion / WFC    |  IK / Blending    |  Seamless Areas   |
|  Mesh Generation  |  State Machines   |  Scene Streaming  |
+-------------------+-------------------+-------------------+
|   Collaboration   |                   |    Utilities      |
|                   |                   |                   |
|  Multi-User Edit  |                   |  CrashHandler     |
|  Node Locking     |                   |  Console (200+)   |
|  Edit Broadcast   |                   |  Profiler / Debug |
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
|       |   |-- Networking/  # UDP multiplayer, replication, AreaServer, WorldServer
|       |   |-- Procedural/  # Noise, erosion, mesh generation, WFC
|       |   |-- SaveSystem/  # Serialization, compression, save slots
|       |   |-- Scripting/   # AngelScript VM, hot-reload, API bindings
|       |   |-- Streaming/   # SeamlessAreaManager, SceneTransitionManager
|       |   |-- World/       # WorldOriginSystem (floating-point origin rebasing)
|       |-- Enums/           # Shared enumerations
|       |-- Game/            # Player, weapons, vehicles, HUD, terrain, inventory
|       |-- Graphics/        # DX11 renderer, PBR, post-processing, particles, RHI
|       |-- Input/           # Keyboard, mouse, gamepad input
|       |-- Physics/         # Jolt Physics integration
|       |-- Projectiles/     # Weapon projectile system
|       |-- SceneManager/    # Scene and level management
|       |-- Utils/           # Logging, profiler, crash handler, console, debug tools
|-- SparkEditor/
|   |-- Source/              # ImGui editor (52 panels)
|-- SparkConsole/
|   |-- src/                 # Standalone debug console application
|-- SparkShaderCompiler/
|   |-- src/                 # Offline shader compilation tool
|-- SparkSDK/                # Public SDK/interface headers
|-- GameModules/             # Game module shared libraries (8 modules)
|   |-- SparkGame/           # Base game module
|   |-- SparkGameFPS/        # FPS game module
|   |-- SparkGameMMO/        # MMO game module
|   |-- SparkGameRPG/        # RPG game module
|   |-- SparkGameARPG/       # Action RPG game module
|   |-- SparkGameRTS/        # RTS game module
|   |-- SparkGameRacing/     # Racing game module
|   |-- SparkGamePlatformer/ # Platformer game module
|-- ThirdParty/              # Git submodules (see Dependencies)
|-- Shaders/
|   |-- HLSL/               # DirectX shaders
|   |-- GLSL/               # OpenGL shaders
|   |-- Compiled/           # Pre-compiled DirectX bytecode (.cso)
|-- Assets/
|   |-- Models/             # 3D model files (.obj)
|   |-- Scenes/             # Level/scene JSON files
|   |-- Scripts/            # AngelScript game scripts
|-- Templates/               # Game module project templates
|-- Tests/                   # 2,108 unit tests across 174 files (CTest + 5 sanitizers)
|-- tools/
|   |-- SparkBuild.exe       # Pre-built SparkBuild binary
|   |-- update-sparkbuild.*  # Manual update scripts (ps1/sh)
|-- docs/                    # Doxygen docs, wiki, API reference
|-- wiki/                    # 64 wiki pages covering all subsystems
|-- cmake/                   # CMake utility modules
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
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | `Physics/JoltPhysics` | Physics engine |
| [AngelScript](https://github.com/codecat/angelscript-mirror) | `Scripting/angelscript-mirror` | Scripting language |
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

2,108 unit tests across 174 test files covering all major engine systems, built with a lightweight internal test framework (no external test dependencies). Integrated with CMake's CTest.

```bash
# Build and run tests
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure

# Export test results to a file (useful for CI review)
./build/bin/SparkTests --output-file test-results.txt

# Export only failures/errors
./build/bin/SparkTests --output-file errors.txt --errors-only
```

### Running Sanitizers Locally

```bash
# ASan + UBSan + LSan (GCC) — detects memory errors, undefined behavior, leaks
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel $(nproc)
LSAN_OPTIONS=suppressions=Tests/lsan_suppressions.txt ./build-asan/bin/SparkTests --output-file asan-results.txt

# TSan (GCC) — detects data races and deadlocks
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --parallel $(nproc)
TSAN_OPTIONS=suppressions=Tests/tsan_suppressions.txt ./build-tsan/bin/SparkTests --output-file tsan-results.txt

# MSan (Clang + libc++) — detects uninitialized memory reads
cmake -B build-msan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -stdlib=libc++ -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_C_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++ -lc++abi" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++"
cmake --build build-msan --parallel $(nproc)
./build-msan/bin/SparkTests --output-file msan-results.txt
```

Test coverage spans all major subsystems: core utilities (math, string, file, UUID, config, ring buffer, object pool), ECS (world, components, integration), physics (rigid bodies, collision layers, frustum culling, interpolation), AI (behavior trees, NavMesh, steering, EQS, formations, cover, group AI), animation (state machines, IK, retargeting, cloth, blend spaces), networking (NetBuffer, encryption, prediction, reliable channels, replication, dedicated server, integration), gameplay (weapons, inventory, quests, achievements, destruction, dialogue, cooldowns), graphics (fog, SSAO/SSR, post-processing, temporal effects, mesh LOD, lights, upscaling, render graph, water, terrain), scene management (serialization, snapshots, loading), events (EventBus, coroutines, tweens), editor (play mode, debug tools, collaborative editing), and infrastructure (engine context, modules, profiling, RBAC).

## Build Options

All options are passed to CMake via `-D<OPTION>=ON/OFF`.

| Option | Default | Description |
|---|:---:|---|
| `ENABLE_GRAPHICS` | ON | Graphics rendering system |
| `ENABLE_EDITOR` | ON | ImGui visual editor |
| `ENABLE_PROFILING` | ON | Performance profiling tools |
| `ENABLE_NETWORKING` | ON | UDP networking (client/server, area servers, replication) |
| `ENABLE_VULKAN` | ON | Vulkan graphics backend (experimental) |
| `ENABLE_OPENGL` | ON | OpenGL graphics backend (experimental) |
| `ENABLE_METAL` | OFF | Metal graphics backend (macOS, experimental) |
| `ENABLE_DXR` | OFF | DirectX Raytracing (experimental, needs D3D12) |
| `ENABLE_HYBRID_RT` | ON | Hybrid ray tracing (SDFGI software + optional hardware DXR/Vulkan RT) |
| `ENABLE_RECAST` | ON | Recast/Detour navmesh generation |
| `ENABLE_SDL2` | OFF | SDL2 cross-platform input (auto-enabled on Linux) |
| `SPARK_HEADLESS_SUPPORT` | ON | Headless/dedicated server mode support |
| `BUILD_TESTS` | ON | Build test suite (CTest) |
| `BUILD_GAME_MODULES` | ON | Build in-tree game modules (SparkGameFPS, SparkGameMMO, etc.) |
| `SPARK_STRICT_DEPS` | OFF | FATAL_ERROR on missing critical dependencies (Jolt, ImGui, EnTT) |
| `SPARK_SUPPRESS_THIRDPARTY_WARNINGS` | ON | Suppress compiler warnings from third-party libraries |
| `SPARK_DOUBLE_PRECISION_PHYSICS` | OFF | Double precision physics (JPH_DOUBLE_PRECISION) for large worlds |
| `ENABLE_CONSOLE_IN_SHIPPING` | OFF | Include SparkConsole in Shipping builds |
| `ENABLE_DEVCOMMANDS_IN_SHIPPING` | OFF | Include developer commands in Shipping builds |
| `STRIP_DEBUG_SYMBOLS` | OFF | Strip debug symbols from the final binary |

```bash
# Example: minimal build without editor
cmake -B build -DENABLE_EDITOR=OFF
```

## CI/CD

Two GitHub Actions workflows run automatically:

**`build.yml`** — runs on every push / PR to `main`, `develop`, and `feature/**`:
- Platforms: Windows (MSVC VS 2022 + experimental VS 2026^1), Linux GCC, Linux Clang
- Configurations: Debug and Release matrix
- Sanitizers: 5 sanitizer jobs with downloadable report artifacts (14-day retention)
- Steps: checkout with submodules, CMake configure, build, test, artifact upload

| Sanitizer Job | Compiler | What it detects | Suppression files |
|---|---|---|---|
| **ASan + UBSan + LSan** | GCC | Buffer overflows, use-after-free, undefined behavior, memory leaks | `Tests/lsan_suppressions.txt` |
| **TSan** | GCC | Data races, deadlocks, thread-safety violations | `Tests/tsan_suppressions.txt` |
| **MSan** | Clang + libc++ | Reads of uninitialized memory | `Tests/msan_ignorelist.txt` (`continue-on-error`^2) |

> ^2 **MSan (MemorySanitizer):** MSan requires the entire process — including the C++ standard library — to be compiled with MSan instrumentation. The system libc++ on ubuntu-24.04 is not instrumented, which produces false positives in basic string/IO operations. The job is marked `continue-on-error` and its report artifact is uploaded for manual review of genuine findings. A fully clean MSan run would require building libc++ from source with `-fsanitize=memory`.

> ^1 **VS 2026 (v144 toolset):** The VS 2026 CI job is included for forward compatibility but will be skipped until GitHub Actions runners ship with the v144 platform toolset. It is marked `continue-on-error` and does not gate merges.

**`release.yml`** — runs on every push to `master` / `main`:
- Builds Windows (VS 2022) and Linux (GCC) in Debug + Release
- Packages each configuration into a zip / tar.gz archive
- Creates or updates the rolling [`latest` GitHub Release](https://github.com/Krilliac/SparkEngine/releases/tag/latest) with all four binaries and the exact commit hash

**`update-sparkbuild.yml`** — runs weekly (Monday 06:00 UTC) or on manual dispatch:
- Downloads the latest [SparkBuild](https://github.com/Krilliac/SparkBuild) release binary
- Opens a PR to update `tools/SparkBuild.exe` when a new version is detected

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

## Networking Configuration

The networking system is **enabled by default** (`ENABLE_NETWORKING=ON`). It uses raw UDP sockets — no external networking dependencies are required.

### What you get

| Feature | Details |
|---|---|
| **Protocol** | UDP client/server |
| **Replication** | Entity state sync with dirty property tracking |
| **Prediction** | Client-side prediction with server reconciliation |
| **Lag compensation** | Hitbox rewinding with 1-second history |
| **Message channels** | Unreliable, Reliable, ReliableOrdered |
| **Statistics** | Ping, jitter, packet loss, bandwidth |
| **Area servers** | Per-area server instances coordinated by a WorldServer |
| **Entity migration** | Cross-area entity serialization and transfer |
| **Load balancing** | Dynamic area reassignment across machines |
| **Player sessions** | Session management across area transitions |

### Platform support

| Platform | Status | Dependencies |
|---|:---:|---|
| Windows | Experimental | `ws2_32`, `wsock32` (linked automatically) |
| Linux | Experimental | POSIX sockets (no extra packages) |
| macOS | Untested | Expected to work via POSIX sockets |

### Troubleshooting

- **Connection refused** — Ensure the server is running and firewall rules allow UDP traffic on your chosen port.
- **High packet loss** — Check `NetworkStats` via the console command `net_stats` for diagnostics.

### Console commands

`net_status`, `net_connect <host> <port>`, `net_disconnect`, `net_host <port>`, `net_stats`, `net_clients`

### Source

`SparkEngine/Source/Engine/Networking/NetworkManager.h` — Full API with message types, replication, and lag compensation.

## License

MIT License. See [LICENSE](LICENSE) for details.
