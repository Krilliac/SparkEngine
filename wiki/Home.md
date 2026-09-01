# Spark Engine

**Spark Engine** is a free, open-source 3D game engine written in C++23. Originally designed for first-person shooters, Spark Engine is evolving into a general-purpose engine with a broad source inventory spanning FPS, RPG, MMO, open-world, and other genres. That inventory includes a multi-backend RHI (DirectX 11/12, Vulkan, OpenGL, Metal, NullRHI), rendering experiments, Jolt Physics, audio paths, scripting tools, an EnTT-based ECS architecture, an ImGui editor, world streaming, server architecture, and collaborative-editing prototypes. Presence in the source tree is not a support or release claim; non-profile breadth remains experimental unless the readiness contract says otherwise.

> **Release hardening in progress** — SparkEngine is currently source-usable,
> but no versioned release has been published. The repository's readiness gates
> distinguish implemented features from verified release claims. The only declared
> profile is the blocked and uncertified `stable-v1` Windows 11 x64/MSVC v143 +
> D3D11/Windows NullRHI + C++ module/SparkGameFPS slice, together with the
> required Windows editor and delivery-tool products enumerated in readiness.

![SparkEditor — ImGui-based visual editor](../docs/screenshots/editor-overview.png)

*SparkEditor default layout with the Spark Professional theme. Open more panels from the Window menu.*

## Platform Support

| Platform | Status | Compiler |
|----------|--------|----------|
| Windows 11 x64 | `stable-v1` target — blocked/uncertified | MSVC v143 (VS 2022) |
| Windows 10 x64 | Documented development path — outside `stable-v1` | MSVC development toolchains |
| Linux x64 | Experimental | GCC 13+, Clang 17+ |
| macOS 11+ | Experimental | Apple Clang with C++23 |

See [System Requirements](platform/System-Requirements.md) for minimum and
recommended hardware per platform (CPU, RAM, GPU, VRAM), runtime resource
footprint, and the build toggles that move the needle.

## Feature Highlights

- **Rendering** — Multi-backend RHI source paths (DirectX 11/12, Vulkan, path-specific OpenGL development contexts, Metal, NullRHI) with forward, deferred, forward+, and clustered pipeline implementations via a declarative RenderGraph. PBR materials, cascaded shadow mapping, SSAO, SSR, volumetric lighting/fog, bloom, HDR tone mapping, TAA/FXAA/MSAA, IBL, GPU particles, decals, water rendering, sky atmosphere, and quality presets are implementation inventory, not blanket support claims. [Global illumination](graphics/Global-Illumination.md), [GPU-driven rendering](graphics/GPU-Driven-Rendering.md), [mesh shaders](graphics/Mesh-Shaders.md), [virtual texturing](graphics/Virtual-Texturing.md), [DXR ray tracing](graphics/DXR-Raytracing.md), [Shader Graph](graphics/Shader-Graph.md), and FSR paths remain subject to their declared experimental boundaries.
- **Physics** — Jolt Physics with rigid bodies, 15 collision shape types, 12 constraint types, raycasting, overlap queries, physics materials, character controller, vehicle physics (wheeled/tracked/motorcycle), ragdoll, soft body/cloth, destruction/fracture, deterministic mode, and debug draw.
- **Audio** — The active factory selects XAudio2 on Windows, OpenAL on non-Windows hosts, then Null audio if neither backend initializes. Spatial-audio, mixer, and pooling code exists, but audio is outside the stable profile.
- **Gameplay** — ECS architecture (EnTT components and systems), FPS player controller, weapons, vehicles, inventory, quests, achievements, abilities/conditions, event response system, dialogue, destruction, replay, day/night cycle, weather, terrain with quadtree LOD, tween/coroutine systems, and [accessibility](platform/Accessibility.md) (colorblind modes, subtitles, reduced motion).
- **AI** — Behavior trees, NavMesh A* pathfinding (Recast/Detour), perception system (vision/hearing/memory), steering behaviors (seek/flee/pursue/evade/flocking), tactical points (EQS-like), cover/formation system, AI budget limiter for 100+ agents, and AI director.
- **Animation** — Skeletal animation, state machines, multi-layer blending, IK (two-bone, look-at, FABRIK), root motion, retargeting, ragdoll blending, cloth simulation, [cinematic sequencer](gameplay-tools/Cinematic-Sequencer.md), FBX/glTF import.
- **Scripting** — Experimental AngelScript integration includes hot reload, lifecycle callbacks, partial engine bindings, and client/server-oriented source paths. [Visual scripting](subsystems/Visual-Scripting.md), Lua-related source paths, and the [mod system](subsystems/Mod-System.md) are implementation inventory, not certified support.
- **Networking** — UDP client/server, entity replication, client-side prediction, lag compensation (hitbox rewinding), delta snapshots. [HeroEngine-inspired MMO architecture](subsystems/Area-Server-Architecture.md) with AreaServers, WorldServer, seamless entity migration, and dynamic load balancing.
- **Editor** — ImGui-powered visual editor with 65 `*Panel.h` classes in the source inventory; registration and default visibility are separate. It includes scene hierarchy, inspector, gizmos, [Shader Graph](graphics/Shader-Graph.md), [visual scripting](subsystems/Visual-Scripting.md), and other experimental tooling. Required command-backed world editing and undo coverage are inside the release profile but remain blocked; collaborative editing stays outside it.

## Get the Source

There are no supported or versioned binary releases yet. Rolling `nightly`
artifacts may be published for development evaluation; they remain unsupported,
unversioned snapshots of a specific commit. Clone the canonical repository and
follow the [Build Guide](Build-Guide.md) for the authoritative development path.

```bash
git clone --recurse-submodules https://github.com/Krilliac/SparkEngine.git
cd SparkEngine
```

## Where to Start

Pick the path that matches your role:

| Role | Recommended reading order |
|------|---------------------------|
| **New to SparkEngine** | [Getting Started](getting-started/Getting-Started.md) → [Quick-Start Tutorial](getting-started/Quick-Start-Tutorial.md) → [FAQ](getting-started/FAQ.md) |
| **Programmer** | [Getting Started](getting-started/Getting-Started.md) → [Architecture Overview](getting-started/Architecture-Overview.md) → [Creating a Game Module](getting-started/Creating-a-Game-Module.md) → [Entity Component System](subsystems/Entity-Component-System.md) |
| **Artist / Designer** | [Getting Started](getting-started/Getting-Started.md) → [Quick-Start Tutorial](getting-started/Quick-Start-Tutorial.md) → [Editor Walkthrough](getting-started/Editor-Walkthrough.md) → [Artist Workflow Guide](getting-started/Artist-Workflow-Guide.md) |
| **Gameplay Designer** | [Making Your First Game](getting-started/Making-Your-First-Game.md) → [Gameplay Systems](gameplay-tools/Gameplay-Systems.md) → [Scripting with AngelScript](subsystems/Scripting-with-AngelScript.md) |
| **Multiplayer Developer** | [Multiplayer Quick Start](subsystems/Multiplayer-Quick-Start.md) → [Networking](subsystems/Networking.md) → [Dedicated Server](subsystems/Dedicated-Server.md) |
| **Optimizer / QA** | [Performance Tips](advanced/Performance-Tips.md) → [Configuration Reference](advanced/Configuration-Reference.md) → [Profiler and Debugging](advanced/Profiler-and-Debugging.md) |
| **Upgrading** | [Migration Guide](getting-started/Migration-Guide.md) → [Asset Migration](gameplay-tools/Asset-Migration.md) |

## Wiki Navigation

`wiki/_Sidebar.md` is the canonical table of contents for all wiki pages and categories.

- [Documentation portal](Documentation.md)
- [Guides](Guides.md), [Tutorials](Tutorials.md), and [Samples](Samples.md)
- [Build Guide](Build-Guide.md) and [Dependencies](Dependencies.md)
- [Browse full wiki index](./_Sidebar.md)
- [API Reference](reference/API-Reference.md)
- [Documentation Index](../docs/README.md)

## Code Quality

SparkEngine enforces code quality automatically:

- **clang-format** — Enforced in CI on every PR (Microsoft-based style, Allman braces, 120-col, 4-space indent)
- **clang-tidy** — Static analysis for bugprone, modernize, performance, and readability checks
- **Comprehensive unit tests** — CTest suite covering all major subsystems (live counts in Project Statistics)
- **5 sanitizers** — ASan, UBSan, LSan, TSan, MSan in CI
- **Code coverage** — lcov reports generated per CI run

See [Contributing](advanced/Contributing.md) for the full pre-commit checklist.

## License

SparkEngine is licensed under the [Spark Open License](https://github.com/Krilliac/SparkEngine/blob/Working/LICENSE) — no royalties, no fees, anti-plagiarism protected.

## Project Statistics

<!-- AUTO:stats -->
| Metric | Count |
|--------|-------|
| Header files | 999 |
| Struct declarations in 17 component headers | 79 |
| Engine System Classes | 75 |
| `*Panel.h` class inventory | 65 |
| Test-bearing `.cpp`/`.mm` files | 576 |
| Source-level test definitions | 6989 |
| Wiki pages | 198 |
<!-- /AUTO:stats -->
