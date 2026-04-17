# Spark Engine

**Spark Engine** is a free, open-source 3D game engine written in C++23. Originally designed for first-person shooters, Spark Engine is evolving into a general-purpose engine supporting FPS, RPG, MMO, open-world, and other genres. It ships with a multi-backend RHI (DirectX 11/12, Vulkan, OpenGL, Metal, NullRHI), global illumination, GPU-driven rendering, mesh shaders, DXR ray tracing, Jolt Physics, XAudio2 spatial audio, AngelScript hot-reload scripting with visual scripting and Shader Graph, an EnTT-based ECS architecture, an ImGui visual editor with dockable panels, and HeroEngine-inspired features including seamless world streaming, area-based server architecture, and collaborative multi-user editing.

> **v1.0.0 Released** — SparkEngine's first official release. Production-ready core systems with active feature development.

![SparkEditor — ImGui-based visual editor](../docs/screenshots/editor-overview.png)

*SparkEditor default layout with the Spark Professional theme. Open more panels from the Window menu.*

## Platform Support

| Platform | Status | Compiler |
|----------|--------|----------|
| Windows 10+ | Primary | MSVC v143 (VS 2022), v144 (VS 2026) |
| Linux x64 | Experimental | GCC 13+, Clang 17+ |
| macOS | Experimental | Apple Clang with C++23 |

## Feature Highlights

- **Rendering** — Multi-backend RHI (DirectX 11/12, Vulkan 1.4, OpenGL 4.6, Metal, NullRHI) with forward, deferred, forward+, and clustered pipelines via a declarative RenderGraph. PBR materials, cascaded shadow mapping, SSAO, SSR, volumetric lighting/fog, bloom, HDR tone mapping, TAA/FXAA/MSAA, IBL, GPU particles, decals, water rendering, sky atmosphere, and quality presets. [Global illumination](Global-Illumination) (DDGI + Adaptive Probe Volumes), [GPU-driven rendering](GPU-Driven-Rendering) (compute culling, HiZ occlusion), [mesh shaders](Mesh-Shaders) (meshlet pipeline), [virtual texturing](Virtual-Texturing), [DXR 1.1 ray tracing](DXR-Raytracing) (reflections, shadows, AO, GI), [Shader Graph](Shader-Graph) (35+ nodes, HLSL generation), and FSR upscaling.
- **Physics** — Jolt Physics with rigid bodies, 15 collision shape types, 12 constraint types, raycasting, overlap queries, physics materials, character controller, vehicle physics (wheeled/tracked/motorcycle), ragdoll, soft body/cloth, destruction/fracture, deterministic mode, and debug draw.
- **Audio** — XAudio2 3D spatial audio with Doppler effects, distance attenuation, volume channels, audio mixer, and object pooling. Miniaudio as cross-platform fallback.
- **Gameplay** — ECS architecture (EnTT components and systems), FPS player controller, weapons, vehicles, inventory, quests, achievements, abilities/conditions, event response system, dialogue, destruction, replay, day/night cycle, weather, terrain with quadtree LOD, tween/coroutine systems, and [accessibility](Accessibility) (colorblind modes, subtitles, reduced motion).
- **AI** — Behavior trees, NavMesh A* pathfinding (Recast/Detour), perception system (vision/hearing/memory), steering behaviors (seek/flee/pursue/evade/flocking), tactical points (EQS-like), cover/formation system, AI budget limiter for 100+ agents, and AI director.
- **Animation** — Skeletal animation, state machines, multi-layer blending, IK (two-bone, look-at, FABRIK), root motion, retargeting, ragdoll blending, cloth simulation, [cinematic sequencer](Cinematic-Sequencer), FBX/glTF import.
- **Scripting** — AngelScript with hot-reload, lifecycle callbacks, full engine API bindings, client/server separation. [Visual scripting](Visual-Scripting) with 60 node types that compiles to AngelScript. Lua also supported. [Mod system](Mod-System).
- **Networking** — UDP client/server, entity replication, client-side prediction, lag compensation (hitbox rewinding), delta snapshots. [HeroEngine-inspired MMO architecture](Area-Server-Architecture) with AreaServers, WorldServer, seamless entity migration, and dynamic load balancing.
- **Editor** — ImGui-powered visual editor with 59 dockable panels: scene hierarchy, inspector, gizmos, [Shader Graph](Shader-Graph) material editor, [visual script editor](Visual-Scripting), cinematic sequencer, dialogue editor, AI debugger, command palette (Ctrl+P), collaborative multi-user editing, and 200+ debug console commands.

## Downloads

Pre-built binaries are published on every commit to `master`:

- [Windows Release (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Release.zip)
- [Windows Debug (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Debug.zip)
- [Linux Release (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Release.tar.gz)
- [Linux Debug (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Debug.tar.gz)

## Where to Start

Pick the path that matches your role:

| Role | Recommended reading order |
|------|---------------------------|
| **New to SparkEngine** | [Getting Started](Getting-Started) → [Quick-Start Tutorial](Quick-Start-Tutorial) → [FAQ](FAQ) |
| **Programmer** | [Getting Started](Getting-Started) → [Architecture Overview](Architecture-Overview) → [Creating a Game Module](Creating-a-Game-Module) → [Entity Component System](Entity-Component-System) |
| **Artist / Designer** | [Getting Started](Getting-Started) → [Quick-Start Tutorial](Quick-Start-Tutorial) → [Editor Walkthrough](Editor-Walkthrough) → [Artist Workflow Guide](Artist-Workflow-Guide) |
| **Gameplay Designer** | [Making Your First Game](Making-Your-First-Game) → [Gameplay Systems](Gameplay-Systems) → [Scripting with AngelScript](Scripting-with-AngelScript) |
| **Multiplayer Developer** | [Multiplayer Quick Start](Multiplayer-Quick-Start) → [Networking](Networking) → [Dedicated Server](Dedicated-Server) |
| **Optimizer / QA** | [Performance Tips](Performance-Tips) → [Configuration Reference](Configuration-Reference) → [Profiler and Debugging](Profiler-and-Debugging) |
| **Upgrading** | [Migration Guide](Migration-Guide) → [Asset Migration](Asset-Migration) |

## Wiki Navigation

`wiki/_Sidebar.md` is the canonical table of contents for all wiki pages and categories.

- [Browse full wiki index](./_Sidebar.md)
- [API Documentation](../docs/README.md)

## Code Quality

SparkEngine enforces code quality automatically:

- **clang-format** — Enforced in CI on every PR (Microsoft-based style, Allman braces, 120-col, 4-space indent)
- **clang-tidy** — Static analysis for bugprone, modernize, performance, and readability checks
- **Comprehensive unit tests** — CTest suite covering all major subsystems (live counts in Project Statistics)
- **5 sanitizers** — ASan, UBSan, LSan, TSan, MSan in CI
- **Code coverage** — lcov reports generated per CI run

See [Contributing](Contributing) for the full pre-commit checklist.

## License

SparkEngine is licensed under the [Spark Open License](https://github.com/Krilliac/SparkEngine/blob/master/LICENSE) — no royalties, no fees, anti-plagiarism protected.

## Project Statistics

<!-- AUTO:stats -->
| Metric | Count |
|--------|-------|
| Header files | 761 |
| ECS Components | 79 |
| Engine System Classes | 75 |
| Editor Panels | 59 |
| Test files | 473 |
| Test cases | 5871+ |
| Wiki pages | 136 |
| *Last synced* | *2026-04-17 01:46* |
<!-- /AUTO:stats -->
