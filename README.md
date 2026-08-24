# Spark Engine

A C++23 open-source 3D game engine with a full RHI abstraction layer, ECS (EnTT), Jolt Physics, AngelScript scripting, and an ImGui-based editor. Originally built around first-person shooters, it now includes genre templates for RPGs, MMOs, RTS, racing, open-world, and platformers.

> 🌐 **Website now live:** [sparkengine.dev](https://sparkengine.dev/)

[![Build SparkEngine](https://img.shields.io/github/actions/workflow/status/Krilliac/SparkEngine/build.yml?branch=Working&style=flat-square&label=CI)](https://github.com/Krilliac/SparkEngine/actions)
[![Tests](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/tests.json&style=flat-square)](Tests)
[![C++ lines of code](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/loc.json&style=flat-square)](https://github.com/Krilliac/SparkEngine)
[![Source files](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/files.json&style=flat-square)](https://github.com/Krilliac/SparkEngine)
[![License: Spark Open](https://img.shields.io/badge/License-Spark_Open-blue?style=flat-square)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-community-5865F2?style=flat-square&logo=discord)](https://discord.gg/NyX8d9UZM)

---

## Getting Started

SparkEngine is currently distributed as source. No versioned installer or
prebuilt release has been published yet; the release-readiness gates remain
authoritative. The in-tree [installer documentation](SparkInstaller/README.md)
describes development and packaging workflows, not an available download.

**Build from source:**

```bash
git clone --recurse-submodules https://github.com/Krilliac/SparkEngine.git
cd SparkEngine

# Windows
.\generate.bat -g "Visual Studio 17 2022" release
.\build.ps1 -config Release -editor -angelscript

# Linux / macOS
./generate.sh release -g Ninja
./build.sh release
```

Visual Studio and Ninja Multi-Config builds keep binaries isolated under
`build/bin/<Config>` (for example, `build/bin/Release/SparkEditor.exe`).

Requirements: MSVC 19.36+ / GCC 13+ / Clang 17+, CMake 3.25+.  
See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) for build issues.

**Create a game module:**

```bash
cmake --install build --prefix ~/SparkEngine-install
cp -r Templates/EmptyProject MyGame && cd MyGame
cmake -B build -DCMAKE_PREFIX_PATH=~/SparkEngine-install
cmake --build build --config Release
./SparkEngine -game ./libMyGame.so
```

See [Templates/README.md](Templates/README.md) and [SparkTemplates](https://github.com/Krilliac/SparkTemplates).

---

## Editor

![SparkEditor — ImGui-based visual editor with 59 dockable panels](docs/screenshots/editor-overview.png)

<details>
<summary>More screenshots</summary>

| Screenshot | Description |
|---|---|
| ![Welcome](docs/screenshots/editor-welcome.png) | Welcome screen and project setup |
| ![Windows](docs/screenshots/editor-window-menu.png) | Panel layout — Scene, Inspector, Asset Browser, Shader Graph, Sequencer |
| ![GameObject](docs/screenshots/editor-gameobject-menu.png) | Component editing and drag-drop |
| ![FPS](docs/screenshots/editor-fpstools-menu.png) | FPS-specific tools — weapon editor, damage model, HUD builder |

</details>

---

## Features

### Rendering

Six backends via a shared RHI abstraction:

| Backend | Status |
|---|---|
| DirectX 11 | Stable — primary Windows backend |
| DirectX 12 | Experimental — mesh shaders, DXR ray tracing, VRS |
| Vulkan 1.4 | Experimental — Linux and Windows |
| OpenGL 4.6 | Experimental — fallback on older hardware |
| Metal | Experimental — macOS, in progress |
| NullRHI | Stable — headless / software rendering via llvmpipe |

Render features include PBR materials, global illumination (DDGI, Adaptive Probe Volumes, hybrid ray tracing), forward/deferred/clustered render paths, cascaded shadow maps, GPU-driven rendering (compute frustum culling, indirect draw), virtual texturing, mesh shaders, DXR 1.1, FSR upscaling, and a 35-node Shader Graph. Post-processing covers bloom, HDR tone mapping (Reinhard/ACES/Uncharted 2), TAA, FXAA, MSAA, depth of field, motion blur, volumetric fog, lens flares, and light shafts.

### Physics

Jolt Physics with rigid bodies (static/kinematic/dynamic), 15 collision shapes, 12 constraint types, raycasting, sphere/box overlap, shape casting, vehicles (wheeled/tracked), ragdolls, cloth, soft bodies, and destruction. Supports deterministic mode for replays and multiplayer.

### Audio

XAudio2 3D spatial audio on Windows with Miniaudio cross-platform fallback. Distance attenuation, Doppler, pitch/volume control, master/SFX/music channels, and an object pool for source management.

### Scripting

- **AngelScript** — hot-reload via file watcher with state preservation, full engine API bindings, per-file module isolation, client/server context separation
- **Visual scripting** — 60 node types across 8 categories, compiles to AngelScript (no runtime overhead)
- **Shader Graph** — 35+ nodes, HLSL generation, live preview
- **Lua** — engine bindings, Lua 5.3+ compatible

### AI and Navigation

Behavior trees with blackboard, Recast/Detour NavMesh with dynamic obstacles, EQS for tactical point queries, steering behaviors (seek/flee/pursue/flock), perception (vision cones, hearing, threat memory), cover detection, formation movement, group coordination, and an AI budget limiter supporting 100+ simultaneous agents.

### Networking

UDP client/server with entity replication, dirty property tracking, client-side prediction, server reconciliation, and lag compensation (hitbox rewinding, 1-second history). Message channels: Unreliable, Reliable, ReliableOrdered. Delta snapshot compression, network statistics (ping, jitter, packet loss, bandwidth). MMO-scale architecture: AreaServer + WorldServer, entity migration across server instances, per-area session management, load balancing.

### ECS and Gameplay

EnTT-backed ECS with 75+ component types. Includes: FPS weapons, damage model, HUD; vehicle physics; inventory, quests, achievements, dialogue trees; ability/cooldown/trigger system; destructible objects; replay record/playback; day/night cycle; weather; 2D/sprite rendering; tween system; async coroutine scheduler; save/load with ECS-aware serialization; async database-backed persistence.

**Large worlds:** seamless area streaming (no load screens), floating-point origin rebasing, 100K+ entities per area.

### Editor (59 panels)

Scene hierarchy, Inspector, Asset browser, Game viewport, Gizmos (ImGuizmo), Node graphs (imnodes), Animation timeline, Material editor, Visual script editor, Terrain editor, Weapon editor, Profiler, AI debugger, Physics debug overlay, Cinematic sequencer, Dialogue editor, Ability/condition editors, Destruction editor, 2D/tilemap editors, Audio mixer, Replay panel, Save system panel, Dedicated server panel, Version control integration, Build/deployment pipeline, Level streaming, Command palette (Ctrl+P), Prefab system, Event monitor, Coroutine debugger, Collaboration panel (multi-user with node locking and presence), and more. Supports collaborative multi-user editing, full undo/redo, play-mode editing, and a plugin system.

### Game Module Templates

10 prebuilt modules loadable as `.dll`/`.so` at runtime, each buildable against the installed SDK independently of the engine:

| Template | Highlights |
|---|---|
| FPS | Weapons, damage, HUD, ammo, crosshair |
| RPG | Dialogue trees, quests, inventory, abilities |
| MMO | Area servers, player sessions, entity migration |
| Action RPG | Combat, loot, progression |
| RTS | Unit selection, pathfinding groups, fog of war |
| Racing | Vehicle physics, lap tracking, replays |
| Platformer | Gravity, jumping, level design tools |
| Battle Royale | Large world, multiple areas, loot spawning |
| Open World | Seamless streaming, origin rebasing, NPC AI |
| Visual Script | Node-based gameplay, no-code prototyping |

---

## Quality Assurance

**Tests:** 6,581 unit tests across 553 files covering core utilities, ECS, physics, AI, animation, networking, gameplay, graphics, editor, and 50+ other subsystems.

```bash
cd build && ctest --output-on-failure
ctest -R "Physics" --output-on-failure  # run a subset
```

**Sanitizer CI jobs:**

| Suite | Detects |
|---|---|
| ASan + UBSan + LSan | Buffer overflows, use-after-free, UB, memory leaks |
| TSan | Data races, deadlocks |
| MSan | Reads of uninitialized memory |

Run locally:
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && ./build-asan/bin/SparkTests
```

When a workflow uploads sanitizer or coverage results, they are available from
that run on the [GitHub Actions page](https://github.com/Krilliac/SparkEngine/actions/workflows/build.yml).

---

## Build Configuration

Key CMake options:

| Option | Default | Description |
|---|:---:|---|
| `ENABLE_GRAPHICS` | ON | Rendering system |
| `ENABLE_EDITOR` | ON | ImGui editor |
| `ENABLE_NETWORKING` | ON | UDP multiplayer |
| `ENABLE_VULKAN` | ON | Vulkan backend |
| `ENABLE_OPENGL` | ON | OpenGL backend |
| `ENABLE_METAL` | OFF | Metal (macOS) |
| `ENABLE_DXR` | ON | Ray tracing |
| `ENABLE_VR` | OFF | VR/AR |
| `ENABLE_MOBILE` | OFF | Mobile features |
| `BUILD_TESTS` | ON | Test suite |
| `BUILD_GAME_MODULES` | ON | 10 game templates |

Headless build (no GPU, no editor):
```bash
cmake -B build -DENABLE_EDITOR=OFF -DENABLE_GRAPHICS=OFF
```

---

## System Requirements

| | Minimum | Recommended |
|---|---|---|
| OS | Windows 10 / Ubuntu 24.04 / macOS 12+ | Windows 11 / Ubuntu 24.04 |
| Compiler | MSVC 19.36, GCC 13, Clang 17 | MSVC 19.36+, GCC 13+, Clang 17+ |
| C++ | C++23 | C++23 |
| GPU | Any DirectX 11 capable | RTX 2080+ for ray tracing |
| RAM | 4 GB | 16 GB |
| Storage | 5 GB | 10 GB with all game modules |
| Build tools | CMake 3.25+ | CMake 3.25+, Ninja |

Headless/server deployments use `NullRHIDevice` — no GPU required.

**Platform support:**

| Platform | Compiler | Backend | Status |
|---|---|---|---|
| Windows 10+ | MSVC v143 (VS 2022) | DirectX 11 | Stable |
| Windows | MSVC v144 (VS 2026) | DirectX 11/12 | Experimental |
| Linux | GCC 13+ / Clang 17+ | Vulkan/OpenGL | Experimental, CI tested |
| macOS | Apple Clang | Metal | Experimental |
| Headless | Any | NullRHI | Stable |

---

## Documentation

| Resource | Description |
|---|---|
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | Startup issues, debug commands, common fixes |
| [Feature Roadmap](docs/plans/FEATURE_ROADMAP.md) | Planned features |
| [Project Status](docs/status/PROJECT_STATUS.md) | System status, recent changes |
| [API Reference](wiki/reference/API-Reference.md) | Auto-generated symbol indexes, class hierarchy |
| [Packaging Guide](docs/guides/packaging.md) | Install layout, components, versioning |
| [Game Module Guide](Templates/README.md) | Building standalone games with the SDK |
| [Networking Config](docs/guides/networking.md) | UDP, replication, MMO server setup |
| [Wiki](wiki/) | 144+ pages covering all subsystems |
| [DeepWiki](https://deepwiki.com/Krilliac/SparkEngine) | Community knowledge base |

---

## Architecture

**Execution order:** Physics → Animation → AI → Audio → Lifecycle → Render

```
SparkEngine/
├── SparkEngine/Source/
│   ├── Core/              Platform.h, EngineContext.h
│   ├── Graphics/          RHI (6 backends), RenderGraph, GI
│   └── Engine/
│       ├── ECS/           75+ component types
│       ├── AI/            BehaviorTree, NavMesh, EQS
│       ├── Animation/     Skeletal, IK, Sequencer
│       ├── Networking/    UDP, Area/World Servers
│       ├── Scripting/     AngelScript, Visual Scripting
│       ├── Gameplay/      Weapons, Quests, Inventory
│       └── 20+ other systems
├── SparkEditor/Source/    59 dockable panels, collaboration
├── SparkConsole/src/      Standalone debug console
├── GameModules/           10 prebuilt game templates
├── Tests/                 6,581 unit tests, 553 files
├── wiki/                  144+ wiki pages
└── docs/                  API reference, guides
```

---

## Contributing

1. Fork the repository and create a feature branch
2. Follow the [coding standards in CLAUDE.md](CLAUDE.md) — C++23, zero warnings, RAII, const-correct
3. Add tests for new functionality
4. Run `clang-format` (enforced in CI) and ensure all tests pass
5. Open a pull request — one feature per PR

Questions and bug reports: [GitHub Issues](https://github.com/Krilliac/SparkEngine/issues)  
Discussion: [Discord](https://discord.gg/NyX8d9UZM)  
Documentation: [Wiki](wiki/)

Thanks to the [Jolt Physics](https://github.com/jrouwe/JoltPhysics), [Dear ImGui](https://github.com/ocornut/imgui), [EnTT](https://github.com/skypjack/entt), Khronos (Vulkan/OpenGL), and AngelScript teams.

---

## License

[Spark Open License 1.0](LICENSE) — no royalties, no fees, use for any purpose (commercial or otherwise), modify and redistribute freely.

> This project makes use of AI-assisted development. All AI-generated code is reviewed and tested before merging.
