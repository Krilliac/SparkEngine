# 🚀 Spark Engine — Build Any Game, In Pure C++23

**A production-ready open-source game engine supporting everything from pixel-perfect indie games to massive MMOs.** DirectX, Vulkan, OpenGL, Metal — write once, render anywhere. Jolt Physics, AngelScript scripting with hot-reload, AI/NavMesh, seamless world streaming, multiplayer (UDP client/server + MMO-scale area servers). ImGui editor with 59 panels. No royalties. No fees. No corporate lock-in.

<details open>
<summary><strong>⬇️ One-Click Installation</strong></summary>

Pick your platform and run the installer. It clones the repo, configures CMake, and compiles the engine with your chosen options in **~5 minutes**.

| Platform | Download | What's included |
|---|---|---|
| **Windows x64** | [![Windows x64](https://img.shields.io/badge/↓_Install-Windows_x64-0078D4?style=flat-square&logo=windows)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkInstaller-Windows-x64.exe) | MSVC 2022, D3D11, Editor, Full engine |
| **Linux x64** | [![Linux x64](https://img.shields.io/badge/↓_Install-Linux_x64-E95420?style=flat-square&logo=linux)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkInstaller-Linux-x64) | GCC 13+, Vulkan, Editor, Full engine |
| **macOS arm64** | [![macOS arm64](https://img.shields.io/badge/↓_Install-macOS_arm64-000000?style=flat-square&logo=apple)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkInstaller-macOS-arm64) | Apple Clang, Metal (experimental), Editor |

Each installer is a single ~2 MB binary. Run with `--gui` for visual setup, or `--headless` for servers.  
[→ Read `SparkInstaller/README.md` for flags & update modes](SparkInstaller/README.md)

</details>

---

## ✨ What You Can Build

🎮 **FPS Games** — Like Quake: full weapon system, damage model, HUD, AI enemies  
🗺️ **Open-World RPGs** — Floating-point origin rebasing, seamless area streaming, 100K NPC support  
⚔️ **MMOs** — Area-based server architecture, entity migration, multiplayer physics, millions of entities  
🏎️ **Racing** — Vehicle physics, terrain, particle effects, replays, networking  
🎲 **Anything Else** — 10 pre-built game module templates (RTS, Platformer, Battle Royale, etc.)

---

## 📊 By The Numbers

[![](https://img.shields.io/badge/5962_tests-brightgreen?style=flat-square)](Tests) 
[![](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/loc.json&style=flat-square)](GitHub) 
[![](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/files.json&style=flat-square)](GitHub) 
[![](https://img.shields.io/badge/75+_components-blue?style=flat-square)](SparkEngine/Source/Engine/ECS)
[![](https://img.shields.io/badge/59_editor_panels-blue?style=flat-square)](SparkEditor/Source)
[![](https://img.shields.io/badge/6_render_backends-blue?style=flat-square)](SparkEngine/Source/Graphics/RHI)
[![Build SparkEngine](https://img.shields.io/github/actions/workflow/status/Krilliac/SparkEngine/build.yml?branch=Working&style=flat-square&label=CI)](https://github.com/Krilliac/SparkEngine/actions)
[![Discord](https://img.shields.io/badge/Discord-Join_community-5865F2?style=flat-square&logo=discord)](https://discord.gg/NyX8d9UZM)
[![Lifetime Downloads](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/Krilliac/SparkEngine/Working/.github/badges/downloads.json&style=flat-square)](Releases)
[![License: Spark Open](https://img.shields.io/badge/License-Spark_Open-blue?style=flat-square)](LICENSE)

---

## 🎬 Editor Preview

![SparkEditor — ImGui-based visual editor with 59 dockable panels](docs/screenshots/editor-overview.png)

<details>
<summary>More screenshots & demos</summary>

| Screenshot | What It Does |
|---|---|
| ![Welcome](docs/screenshots/editor-welcome.png) | **Welcome Screen** — Project setup and quick start |
| ![Windows](docs/screenshots/editor-window-menu.png) | **59 Editor Panels** — Scene, Inspector, Asset Browser, AI/Physics debuggers, Shader Graph, Sequencer, more |
| ![GameObject](docs/screenshots/editor-gameobject-menu.png) | **Instant Prototyping** — Drag-drop components, visual editing |
| ![FPS](docs/screenshots/editor-fpstools-menu.png) | **FPS Tools** — Weapon editor, damage model, HUD builder |

</details>

---

## 🎯 Why Spark Engine?

<details>
<summary><strong>✅ Complete Out of the Box</strong></summary>

**Graphics:** 6 rendering backends (D3D11 stable, D3D12/Vulkan/OpenGL/Metal experimental, NullRHI headless). Global illumination (DDGI, Adaptive Probe Volumes), GPU-driven rendering, mesh shaders, DXR ray tracing, virtual texturing, FSR upscaling, Shader Graph visual authoring, RenderGraph declarative rendering.

**Physics:** Jolt Physics with rigid bodies, 15 collision shapes, 12 constraint types, raycasting, vehicles, ragdolls, cloth, destruction, deterministic networking mode.

**Audio:** XAudio2 3D spatial audio with distance attenuation, Doppler effects, mixing, music system. Cross-platform Miniaudio fallback.

**Scripting:** AngelScript with hot-reload (file watcher, state preservation), 60-node visual scripting compiler, Lua fallback. Full engine API bindings.

**Editor:** 59 dockable ImGui panels — Scene hierarchy, Inspector, Asset browser, Gizmos, Node graphs, Material editor, Animation timeline, Terrain editor, Sequencer, AI debugger, Physics debug, Profiler, Shader Graph, Visual script editor, Dialogue editor, and more.

**Networking:** UDP client/server, entity replication, client-side prediction, lag compensation (hitbox rewinding), delta compression, area-based MMO server architecture (AreaServer + WorldServer), cross-area entity migration.

</details>

<details>
<summary><strong>🎮 10 Genre Templates Included</strong></summary>

Jumpstart your project with pre-built game modules for:

| Template | Includes | Use case |
|---|---|---|
| **FPS** | Weapons, damage, HUD, ammo, crosshair | Quake-like games |
| **RPG** | Dialogue trees, quests, inventory, abilities | Story-driven games |
| **MMO** | Area servers, player sessions, entity migration | Multiplayer worlds |
| **Action RPG** | Combat system, loot, progression | Dark Souls-like games |
| **RTS** | Unit selection, pathfinding groups, fog of war | Strategy games |
| **Racing** | Vehicle physics, lap tracking, replays | Racing games |
| **Platformer** | Gravity, jumping, level design tools | 2D/3D platformers |
| **Battle Royale** | Large world, multiple areas, loot spawning | BR games |
| **Open World** | Seamless streaming, origin rebasing, NPC AI | GTA-like games |
| **Visual Script** | No-code gameplay with node editing | Prototyping & mods |

All templates are **extensible game modules** (`.dll`/`.so` libraries) — build independently from the engine using only the installed SDK.

</details>

<details>
<summary><strong>🏗️ Built for Scale</strong></summary>

**Large Worlds:**
- Seamless area streaming (no loading screens)
- Floating-point origin rebasing (build worlds of any size)
- 100K+ entities per area

**Multiplayer at Scale:**
- Area-based MMO server architecture (inspired by HeroEngine)
- Dynamic entity migration across server instances
- Deterministic physics mode for replays and testing

**Performance:**
- GPU-driven rendering (compute frustum culling, indirect dispatch)
- Mesh shaders + amplification shaders (next-gen GPUs)
- Adaptive LOD (meshes, terrain, vegetation)
- GPU particle system (100K+ particles/frame)

</details>

<details>
<summary><strong>🎓 Built for Learning & Modding</strong></summary>

**Clean C++23 Codebase:**
- No inheritance hell — data-driven ECS (75+ component types)
- 30+ toggleable CMake modules (`-DENABLE_VR=ON`, `-DENABLE_RECAST=ON`, etc.)
- Zero compiler warnings (`/W4` MSVC, `-Wall -Wextra` GCC/Clang)
- Zero unsafe code patterns — RAII, smart pointers, const-correct

**Extensive Testing:**
- 5962 unit tests across 484 files
- 5 sanitizer jobs (ASan, UBSan, LSan, TSan, MSan) with downloadable reports
- Code coverage per subsystem

**Mod System:**
- Load game modules as `.dll`/`.so` libraries at startup
- Hot-reload AngelScript without recompiling engine
- Visual script editor — no code required
- Full SDK with headers, CMake config, templates

</details>

<details>
<summary><strong>💰 Truly Free</strong></summary>

- **Spark Open License 1.0** — No royalties, no fees, no corporate lock-in
- **Open source** — Fork, modify, redistribute freely
- **Anti-plagiarism protected** — IP remains legally yours
- **No tiers** — Same engine for indie games and AAA studios

</details>

---

## 📚 Getting Started

<details>
<summary><strong>🚀 Fastest Path (Graphical Installer)</strong></summary>

1. **Download the installer** (links at top of page)
2. **Run it** — Choose graphics backend, editor, networking, physics
3. **Hit play** — Engine + editor ready to use in ~5 minutes

No prerequisites. No terminal. Pure point-and-click.

</details>

<details>
<summary><strong>⚙️ Manual Build from Source</strong></summary>

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/Krilliac/SparkEngine.git
cd SparkEngine

# Configure (Windows)
.\generate.bat -g "Visual Studio 17 2022" release

# Configure (Linux/macOS)
chmod +x generate.sh
./generate.sh release -g Ninja

# Build (Windows)
.\build.ps1 -config Release -editor -angelscript

# Build (Linux)
./build.sh release

# Run
# Windows:  build/bin/SparkEngine.exe
# Linux:    ./build/bin/SparkEngine
```

**Requirements:**
- **Compiler:** MSVC 19.36+ (VS 2022), GCC 13+, or Clang 17+
- **Build:** CMake 3.25+, Ninja (recommended)
- **GPU:** Any DirectX 11+ GPU, or use NullRHI for headless

[→ Full build guide and troubleshooting](TROUBLESHOOTING.md)

</details>

<details>
<summary><strong>🎮 Create Your First Game Module</strong></summary>

```bash
# 1. Install engine to a prefix
cmake --install build --prefix ~/SparkEngine-install

# 2. Copy template
cp -r Templates/EmptyProject MyGame
cd MyGame

# 3. Build with installed SDK (not the build tree!)
cmake -B build -DCMAKE_PREFIX_PATH=~/SparkEngine-install
cmake --build build --config Release

# 4. Run through engine
# Windows:  SparkEngine.exe -game MyGame.dll
# Linux:    ./SparkEngine -game ./libMyGame.so
```

[→ Full game module guide](Templates/README.md) | [→ Template library](https://github.com/Krilliac/SparkTemplates)

</details>

---

## 🎨 Full Feature Deep Dive

<details>
<summary><strong>🖥️ Rendering (6 Backends)</strong></summary>

| Backend | Status | What It Does |
|---|---|---|
| **DirectX 11** | **Stable** | Primary Windows backend. Full feature set. |
| **DirectX 12** | Experimental | Mesh shaders, DXR ray tracing, Variable Rate Shading. Windows 10+ only. |
| **Vulkan 1.4** | Experimental | Cross-platform. Linux/Windows. Full RHI feature parity. |
| **OpenGL 4.6** | Experimental | Fallback on older Windows/Linux systems. |
| **Metal** | Experimental | macOS support. In progress. |
| **NullRHI** | **Stable** | Headless mode — no GPU needed. CPU software rendering via llvmpipe. |

**Render features:**
- PBR metallic/roughness materials with physically-based shading
- Global illumination: DDGI probe grids, Adaptive Probe Volumes (hierarchical brick LOD), hybrid ray tracing + SDF fallback
- Multiple render paths: Forward, Deferred, Forward+, Clustered
- Advanced shadows: Cascaded shadow maps, contact shadows, PCF filtering
- Post-processing: Bloom, HDR tone mapping (Reinhard/ACES/Uncharted 2), TAA, FXAA, MSAA, Depth of Field, Motion Blur, Film Grain, Lens Flares, Light Shafts, Chromatic Aberration, Volumetric Fog
- GPU particle system (100K+ particles/frame)
- Decals, water rendering, sky atmosphere
- Mesh shaders (D3D12/Vulkan) with meshlet clustering
- GPU-driven rendering: Compute frustum culling, hierarchical Z-buffer occlusion, indirect draw generation
- Virtual texturing with feedback-driven page streaming
- DXR 1.1 ray tracing (reflections, soft shadows, ambient occlusion, multi-bounce GI)
- FSR upscaling
- Shader Graph: 35+ nodes, HLSL generation, live preview

</details>

<details>
<summary><strong>⚙️ Physics</strong></summary>

Jolt Physics integration with:
- **Rigid bodies**: Static, kinematic, dynamic
- **Collision shapes**: Box, sphere, capsule, cylinder, cone, mesh, convex hull, heightfield, compound (15 total)
- **Constraints**: Hinge, slider, fixed, generic (12 types)
- **Queries**: Raycasting (single/multi-hit), sphere/box overlap, shape casting
- **Dynamics**: Vehicle physics (wheeled/tracked), ragdolls, cloth, soft bodies, destruction/fracture
- **Networking**: Deterministic mode for replays and multiplayer
- **Performance**: Multi-threaded job dispatch, collision layers, callbacks, debug draw
- **Materials**: Named physics materials with friction/bounce/density

</details>

<details>
<summary><strong>🎵 Audio</strong></summary>

XAudio2 3D spatial audio (Windows) with Miniaudio cross-platform fallback:
- Hardware-accelerated 3D spatialization
- Distance attenuation, Doppler effects
- Pitch and volume control
- Master/SFX/Music volume channels
- Looping support
- Object pool for efficient source management

</details>

<details>
<summary><strong>🤖 AI & Navigation</strong></summary>

- **Behavior trees**: Composite, decorator, and action nodes with blackboard support
- **NavMesh pathfinding**: Recast/Detour A* with dynamic obstacles and off-mesh links
- **Perception**: Vision cones, hearing ranges, memory, threat tracking
- **Steering**: Seek, flee, pursue, evade, flocking, obstacle avoidance
- **EQS (Environmental Query System)**: Tactical point queries for decision-making
- **Cover system**: Dynamic cover detection and usage
- **Formations**: Group movement with formation shapes
- **Group AI**: Coordination between multiple agents
- **AI budget limiter**: Support 100+ agents efficiently
- **AI director**: Scripted event triggers

</details>

<details>
<summary><strong>🎬 Animation</strong></summary>

- Skeletal animation with bone hierarchies
- Keyframe clips with state machines and cross-fading
- Multi-layer blending: Override, additive, layered with per-bone masks
- Inverse kinematics: Two-bone, look-at, FABRIK
- Root motion extraction
- Animation retargeting
- Ragdoll blending with physics
- Cloth and soft body simulation
- Cinematic sequencer with timeline tracks (camera, entity, animation, events)
- Bezier/Catmull-Rom interpolation
- Supports FBX and glTF (Assimp)

</details>

<details>
<summary><strong>🌐 Networking</strong></summary>

UDP client/server with:
- Entity replication with dirty property tracking
- Client-side prediction + server reconciliation
- Lag compensation: Hitbox rewinding with 1-second history
- Message channels: Unreliable, Reliable, ReliableOrdered
- Delta snapshot compression
- Sub-tick input precision
- Network statistics: Ping, jitter, packet loss, bandwidth
- **MMO Scale:**
  - Area-based server architecture (AreaServer + WorldServer)
  - Dynamic entity migration across servers
  - Per-area player session management
  - Load balancing and failover

</details>

<details>
<summary><strong>📝 Scripting</strong></summary>

**AngelScript** (primary):
- Hot-reload with file watcher and state preservation
- Lifecycle callbacks (Start, Update, OnCollision, etc.)
- Full engine API bindings
- Per-file module isolation
- Client/server script context separation (multiplayer)
- Runtime error reporting

**Visual Scripting:**
- 60 node types across 8 categories
- Compiles to AngelScript (no runtime overhead)
- Node-based Shader Graph (35+ nodes for materials)

**Lua** (alternative):
- Full Lua 5.3+ support
- Engine bindings

**Mod System:**
- Load `.dll`/`.so` game modules at startup
- Hot-reload scripts without recompiling engine

</details>

<details>
<summary><strong>🎮 Gameplay Systems</strong></summary>

**Architecture:** ECS (EnTT) with 75+ component types

**Systems:**
- **FPS:** Weapon system (bullet/rocket/grenade), damage model, HUD (crosshairs, health bars, kill feed, minimap, compass), class system
- **Vehicles:** Wheeled/tracked/motorcycle physics, controls, effects
- **Gameplay:** Gravity system, interactive objects, decal system
- **RPG:** Inventory, quest system, achievement system, dialogue system (branching trees)
- **Abilities:** Ability/condition system, cooldowns, triggers, data-driven event response (When/If/Then rules)
- **Procedural:** Noise (Perlin, Simplex, Worley, FBM, domain warping), heightmap terrain with quadtree LOD, mesh LOD
- **World:** Day/night cycle, weather system, game mode management
- **Destruction:** Fracture/destruction system with physics
- **Replay:** Record/playback system
- **Tweens:** Easing functions and animation curves
- **Coroutines:** Async scheduler with cancellation
- **Accessibility:** 5 colorblind modes, subtitles, high-contrast UI, reduced motion, one-handed input, screen reader hooks

</details>

<details>
<summary><strong>🛠️ Editor (59 Panels)</strong></summary>

**Core Panels:**
- Scene hierarchy, Inspector, Asset browser, Game viewport
- Gizmos (ImGuizmo), Node graphs (imnodes), Animation timeline

**Specialized Panels:**
- Material editor (Shader Graph visual authoring)
- Visual script editor (node-based, compiles to AngelScript)
- Terrain editor with splatting
- Weapon editor
- Profiler (frame time, memory, calls)
- AI editor/debugger
- Physics debug overlay
- Cinematic sequencer
- Dialogue editor
- Ability/condition/trigger editors
- Destruction editor
- 2D/sprite/tilemap editors
- FPS tools
- Audio mixer
- Replay panel
- Save system panel
- Dedicated server panel
- Version control integration
- Build/deployment pipeline
- Level streaming
- Search panel
- Command palette (Ctrl+P)
- Prefab system
- Event monitor
- Coroutine debugger
- Collaboration panel (multi-user editing)
- Project management
- Scene statistics
- Accessibility settings
- VR configuration
- Theming

**Features:**
- Collaborative multi-user editing (node locking, edit broadcasting, presence awareness)
- Full undo/redo support
- Play-mode editing
- Plugin system (built-in + DLL)

</details>

<details>
<summary><strong>💾 Save System & Persistence</strong></summary>

- ECS-aware serialization (miniz compression)
- JSON format with metadata
- Multiple save slots
- Quicksave/quickload
- Rotating autosaves
- Per-component serializer registry
- Async database-backed persistence option

</details>

<details>
<summary><strong>🔧 Tools & Utilities</strong></summary>

**Crash Handler:**
- Minidump generation
- All-thread stack traces
- Screenshot capture
- System info collection
- Optional HTTP report upload

**Debug Console (200+ commands):**
- Engine status, graphics, physics, audio, input, networking, profiling
- Real-time variable inspection
- Performance monitoring

**SparkShaderCompiler** — Offline shader compilation:
```bash
SparkShaderCompiler shader.hlsl -stage vertex -backend vulkan -o shader.spv
```

**SparkConsole** — Standalone debug application (named pipes)

</details>

---

## 📦 Downloads & Releases

<details>
<summary><strong>📥 Prebuilt Binaries</strong></summary>

**Nightly** (bleeding-edge, every commit):

| Platform | Release | Debug |
|---|---|---|
| Windows | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/download/nightly/SparkEngine-Windows-Release.zip) | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/download/nightly/SparkEngine-Windows-Debug.zip) |
| Linux | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/download/nightly/SparkEngine-Linux-Release.tar.gz) | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/download/nightly/SparkEngine-Linux-Debug.tar.gz) |

**Stable** (manually tested, v1.0.0):

| Platform | Release | Debug |
|---|---|---|
| Windows | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Release.zip) | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Debug.zip) |
| Linux | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Release.tar.gz) | [↓ Download](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Debug.tar.gz) |

**CI Artifacts** (per-commit builds, sanitizer reports):

[![Windows VS2022](https://img.shields.io/badge/↓-Windows_VS2022-0078D4?style=flat-square)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/SparkEngine-Windows-VS2022-Release.zip)
[![Linux GCC](https://img.shields.io/badge/↓-Linux_GCC-E95420?style=flat-square)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/SparkEngine-Linux-GCC-Release.zip)
[![ASan Report](https://img.shields.io/badge/↓-ASan_Report-2ea44f?style=flat-square)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-asan-ubsan-lsan.zip)
[![TSan Report](https://img.shields.io/badge/↓-TSan_Report-2ea44f?style=flat-square)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-tsan.zip)
[![Coverage](https://img.shields.io/badge/↓-Coverage-blue?style=flat-square)](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/coverage-report.zip)

</details>

<details>
<summary><strong>🖥️ System Requirements</strong></summary>

| Requirement | Minimum | Recommended |
|---|---|---|
| **OS** | Windows 10 / Ubuntu 24.04 / macOS 12+ | Windows 11 / Ubuntu 24.04 / macOS 13+ |
| **Compiler** | MSVC 19.36 (VS 2022 17.6+), GCC 13, Clang 17 | MSVC 19.36+, GCC 13+, Clang 17+ |
| **C++ Standard** | C++23 | C++23 |
| **GPU** | Any DirectX 11 capable GPU | RTX 2080+ (for ray tracing) |
| **RAM** | 4 GB | 16 GB |
| **Storage** | 5 GB (build artifacts) | 10 GB (with all game modules) |
| **Build Tools** | CMake 3.25+, Ninja or Make | CMake 3.25+, Ninja recommended |

**Headless/Server?** Use `NullRHIDevice` — no GPU required. Runs on any machine.

</details>

<details>
<summary><strong>✅ Platform Support Matrix</strong></summary>

| Platform | Compiler | Graphics Backend | Status | Notes |
|---|---|---|---|---|
| **Windows 10+** | MSVC v143 (VS 2022) | DirectX 11 | **Stable** | Primary platform |
| **Windows** | MSVC v144 (VS 2026) | DirectX 11/12 | Experimental | Forward-compatible, CI skipped |
| **Linux** | GCC 13+ | Vulkan/OpenGL | Experimental | CI tested, binaries available |
| **Linux** | Clang 17+ | Vulkan/OpenGL | Experimental | CI tested |
| **macOS** | Apple Clang | Metal | Experimental | No CI yet, self-built |
| **Headless** | Any | NullRHI | **Stable** | Servers, CI, automated testing |

</details>

---

## 🧪 Quality Assurance

<details>
<summary><strong>✅ Testing & Sanitizers</strong></summary>

**Test Suite:** 5962 unit tests across 484 files covering:
- Core utilities (math, strings, files, UUIDs, config)
- ECS (worlds, components, systems)
- Physics (rigid bodies, collisions, vehicles, ragdolls)
- AI (behavior trees, NavMesh, steering, EQS, formations)
- Animation (state machines, IK, retargeting, cloth)
- Networking (replication, prediction, compression, migration)
- Gameplay (weapons, inventory, quests, destruction)
- Graphics (post-effects, LOD, terrain, water)
- Editor (play mode, debugging, collaboration)
- And 50+ more subsystems

```bash
# Run all tests
cd build && ctest --output-on-failure

# Run specific test
ctest -R "Physics" --output-on-failure
```

**Sanitizers (CI-enforced):**

| Sanitizer | Detects | Download |
|---|---|---|
| **ASan + UBSan + LSan** | Buffer overflows, use-after-free, undefined behavior, memory leaks | [Report](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-asan-ubsan-lsan.zip) |
| **TSan** | Data races, deadlocks, thread-safety violations | [Report](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-tsan.zip) |
| **MSan** | Reads of uninitialized memory | [Report](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/sanitizer-report-msan.zip) |

Run locally:
```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
./build-asan/bin/SparkTests
```

**Code Coverage:**
- Per-subsystem thresholds
- Download [lcov HTML report](https://nightly.link/Krilliac/SparkEngine/workflows/build/Working/coverage-report.zip)

**CI Checks:**
- ✅ clang-format (all commits)
- ✅ clang-tidy (static analysis)
- ✅ 5 sanitizer suites
- ✅ Multi-platform builds (Windows/Linux/macOS)
- ✅ 100+ test jobs

</details>

<details>
<summary><strong>🔨 Build Configuration</strong></summary>

**Easy Configuration:**
```bash
# Interactive menu-driven configurator
SparkBuild         # Pick options, build in place
```

**CMake Toggles** (enable/disable features):

| Option | Default | Purpose |
|---|:---:|---|
| `ENABLE_GRAPHICS` | ON | Rendering system |
| `ENABLE_EDITOR` | ON | ImGui editor |
| `ENABLE_NETWORKING` | ON | UDP multiplayer |
| `ENABLE_PROFILING` | ON | Performance tools |
| `ENABLE_VULKAN` | ON | Vulkan backend |
| `ENABLE_OPENGL` | ON | OpenGL backend |
| `ENABLE_METAL` | OFF | Metal (macOS) |
| `ENABLE_DXR` | ON | Ray tracing |
| `ENABLE_VR` | OFF | VR/AR support |
| `ENABLE_MOBILE` | OFF | Mobile features |
| `BUILD_TESTS` | ON | Test suite |
| `BUILD_GAME_MODULES` | ON | 10 game templates |

```bash
# Minimal headless build
cmake -B build -DENABLE_EDITOR=OFF -DENABLE_GRAPHICS=OFF
```

</details>

---

## 📚 Documentation & Resources

| Resource | Purpose |
|---|---|
| [Troubleshooting Guide](TROUBLESHOOTING.md) | Startup issues, debug commands, common fixes |
| [Feature Roadmap](docs/plans/FEATURE_ROADMAP.md) | Planned features across 3 priority tiers |
| [Project Status](docs/status/PROJECT_STATUS.md) | System status and recent changes |
| [API Reference (Wiki)](wiki/reference/API-Reference.md) | Auto-generated symbol indexes, class hierarchy |
| [Packaging Guide](docs/guides/packaging.md) | Install layout, components, versioning |
| [Game Module Guide](Templates/README.md) | Build standalone games with the SDK |
| [SparkTemplates](https://github.com/Krilliac/SparkTemplates) | Physics, AI, networking, and procedural templates |
| [Networking Config](docs/guides/networking.md) | UDP, replication, MMO servers |
| [Documentation Index](docs/README.md) | Master index of all docs |
| [Wiki](wiki/) | 144+ wiki pages on all subsystems |
| [DeepWiki](https://deepwiki.com/Krilliac/SparkEngine) | Community-driven knowledge base |

---

## 🏗️ Architecture

<details>
<summary><strong>System Overview (click to expand)</strong></summary>

```
┌──────────────────────────────────────────────────────────────┐
│  Rendering (6 backends)    Physics (Jolt)    Audio (XAudio2) │
│  RHI abstraction           Vehicles/Ragdoll   3D Spatial      │
│  GI / GPU-Driven           Cloth / Destruction Mixing         │
│  PBR / Shader Graph        Deterministic      Object Pool     │
└──────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────┐
│ Scripting (AngelScript)   Input/UI (ImGui)   Core & ECS       │
│ Hot-reload / Lua         Gamepad / SDL2      EnTT (75+)       │
│ Visual Scripting         ImGui Editor (59)   Scene Manager    │
└──────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────┐
│ Gameplay (Weapons/Vehicles) │ AI & Navigation │ Networking    │
│ Quests/Inventory/Abilities  │ BehaviorTree    │ UDP Client/Srv│
│ Events/Abilities            │ NavMesh A*      │ Area Servers  │
└──────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────┐
│ Procedural (Noise/Erosion)  Animation (IK/Cloth) Large Worlds │
│ Terrain / WFC / Meshes      Skeletal / Sequencer Origin Rebasing
│                             Ragdoll / State Machines Streaming │
└──────────────────────────────────────────────────────────────┘
```

**Execution Order:** Physics → Animation → AI → Audio → Lifecycle → Render

</details>

<details>
<summary><strong>Project File Structure</strong></summary>

```
SparkEngine/
├── SparkEngine/Source/
│   ├── Core/              Platform.h, EngineContext.h
│   ├── Graphics/          RHI (6 backends), RenderGraph, GI
│   ├── Engine/
│   │   ├── ECS/           75+ component types
│   │   ├── AI/            BehaviorTree, NavMesh, EQS
│   │   ├── Animation/     Skeletal, IK, Sequencer
│   │   ├── Networking/    UDP, Area/World Servers
│   │   ├── Scripting/     AngelScript, Visual Scripting
│   │   ├── Physics/       Jolt integration
│   │   ├── Gameplay/      Weapons, Quests, Inventory
│   │   └── 20+ other systems
│   ├── Utils/             Console, Logger, Profiler
│   └── Game/              Player, HUD, Terrain, Vehicles
├── SparkEditor/Source/    59 dockable panels, collaboration
├── SparkConsole/src/      Standalone debug console
├── GameModules/           10 pre-built game templates
├── Tests/                 5962 unit tests, 484 files
├── wiki/                  144+ wiki pages
└── docs/                  API reference, guides, tooling
```

[→ Full project structure](CLAUDE.md)

</details>

---

## 🤝 Community & Contribution

**How to contribute:**
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes
4. Push to your fork
5. Open a Pull Request

**Guidelines:**
- C++23 only — no legacy code
- Zero compiler warnings
- Add tests for new features
- Format with clang-format (enforced in CI)
- One feature per PR

**Get help:**
- 💬 **Discord:** [Join our community](https://discord.gg/NyX8d9UZM)
- 🐛 **Issues:** [Report bugs or request features](https://github.com/Krilliac/SparkEngine/issues)
- 📖 **Wiki:** [Browse documentation](wiki/)
- 📚 **DeepWiki:** [Community knowledge base](https://deepwiki.com/Krilliac/SparkEngine)

**Sponsors & Recognition:**
- Thanks to Khronos for Vulkan and OpenGL specs
- Jolt Physics team for their fantastic engine
- Dear ImGui for the ultimate immediate-mode GUI
- EnTT for the best-in-class ECS
- All contributors and testers worldwide

---

## 📝 License

[Spark Open License 1.0](LICENSE) — Free forever, no royalties, no corporate lock-in.

**In plain English:**
- ✅ Use for any purpose (games, tools, commercial, non-commercial)
- ✅ Modify and redistribute freely
- ✅ No royalties or fees
- ✅ Your IP remains yours
- ✅ Anti-plagiarism protected

---

## 🎉 What's Next?

**v1.0.0 is released** — Production-ready core systems with active feature development.

**Roadmap:**
- Neural rendering (NTC, radiance cache)
- Enhanced visual scripting (more node types)
- Expanded mobile support (Android, iOS)
- macOS full support (currently experimental)
- WebGPU backend (future)
- And [much more](docs/plans/FEATURE_ROADMAP.md)

---

> **AI Disclosure:** This project makes extensive use of AI-assisted development. All AI-generated code is reviewed, tested, and validated to ensure correctness, stability, and functional integrity. If the use of AI in development is a concern for you, this project may not be the right fit — but if you're open to it, contributions of any and all kinds are welcome!

**Ready to build something amazing?** [Install now](#one-click-installation) or [join the Discord](https://discord.gg/NyX8d9UZM). 🚀
