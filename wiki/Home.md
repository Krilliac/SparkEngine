# Spark Engine

**Spark Engine** is a free, open-source 3D game engine written in C++23. Originally designed for first-person shooters, Spark Engine is evolving into a general-purpose engine supporting FPS, RPG, MMO, open-world, and other genres. It ships with a multi-backend RHI (DirectX 11/12, Vulkan, OpenGL, Metal, NullRHI), global illumination, GPU-driven rendering, mesh shaders, DXR ray tracing, Jolt Physics, XAudio2 spatial audio, AngelScript hot-reload scripting with visual scripting and Shader Graph, an EnTT-based ECS architecture (75 component types), an ImGui visual editor with 59 dockable panels, and HeroEngine-inspired features including seamless world streaming, area-based server architecture, and collaborative multi-user editing.

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
- **Gameplay** — ECS architecture (EnTT, 75 component types, 25 systems), FPS player controller, weapons, vehicles, inventory, quests, achievements, abilities/conditions, event response system, dialogue, destruction, replay, day/night cycle, weather, terrain with quadtree LOD, tween/coroutine systems, and [accessibility](Accessibility) (colorblind modes, subtitles, reduced motion).
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

### Getting Started
- [Home](Home) — You are here
- [FAQ](FAQ) — Common questions and answers
- [Getting Started](Getting-Started) — Prerequisites, building, and running
- [Quick-Start Tutorial](Quick-Start-Tutorial) — Your first 10 minutes with the engine
- [Architecture Overview](Architecture-Overview) — Engine design and project structure
- [Creating a Game Module](Creating-a-Game-Module) — Build your first game module

### Engine Subsystems
- [Entity Component System](Entity-Component-System) — EnTT ECS, components, and systems
- [Rendering and Graphics](Rendering-and-Graphics) — Graphics pipeline, materials, and post-processing
- [Physics](Physics) — Jolt Physics integration
- [Cloth Simulation](Cloth-Simulation) — Position-based dynamics cloth simulation
- [Audio](Audio) — Spatial audio system
- [Input System](Input-System) — Keyboard, mouse, and gamepad
- [Camera System](Camera-System) — Camera controls and management
- [Scripting with AngelScript](Scripting-with-AngelScript) — Gameplay scripting
- [Visual Scripting](Visual-Scripting) — Node-based visual scripting
- [AI and Navigation](AI-and-Navigation) — Behavior trees and pathfinding
- [Animation](Animation) — Skeletal animation and IK
- [2D Systems](2D-Systems) — 2D physics, sprite batching, and rendering
- [Networking](Networking) — Multiplayer networking
- [Dedicated Server](Dedicated-Server) — Headless dedicated server
- [Multiplayer Quick Start](Multiplayer-Quick-Start) — Get multiplayer running fast
- [Area Server Architecture](Area-Server-Architecture) — Scalable MMO-style area servers
- [Scene Management](Scene-Management) — Scenes, hierarchy, and prefabs
- [Large World Support](Large-World-Support) — Origin rebasing and seamless area streaming
- [Collaborative Editing](Collaborative-Editing) — Multi-user editor sessions
- [Coroutine System](Coroutine-System) — C++20 coroutines and scheduling
- [Event System](Event-System) — Publish/subscribe event bus
- [Job System](Job-System) — Thread pool and parallel task execution
- [UI System](UI-System) — Runtime UI framework
- [Localization](Localization) — Multi-language string tables
- [Dialogue System](Dialogue-System) — Branching dialogue trees
- [Destruction System](Destruction-System) — Runtime mesh destruction
- [Replay System](Replay-System) — Match replay recording and playback
- [Achievement System](Achievement-System) — Achievement and statistics tracking
- [Loading System](Loading-System) — Loading screen framework
- [Mod System](Mod-System) — Mod loading and management
- [Content Delivery](Content-Delivery) — CDN and patching system
- [Tween System](Tween-System) — Value interpolation and easing

### Gameplay & Tools
- [Gameplay Systems](Gameplay-Systems) — Player, weapons, inventory, quests
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Terrain and procedural content
- [Save System](Save-System) — Save/load functionality
- [Persistence System](Persistence-System) — Database-backed persistence
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) — Time-of-day and weather
- [Cinematic Sequencer](Cinematic-Sequencer) — Timeline-based cinematics
- [SparkEditor](SparkEditor) — Visual editor reference
- [Editor Walkthrough](Editor-Walkthrough) — Practical editor guide
- [SparkConsole](SparkConsole) — Debug console
- [Shader Pipeline](Shader-Pipeline) — Shader authoring and compilation
- [Asset Pipeline](Asset-Pipeline) — Asset loading and formats

### Platform Support
- [Accessibility](Accessibility) — Colorblind modes, subtitles, reduced motion, one-handed input
- [VR Support](VR-Support) — OpenXR virtual reality framework
- [Mobile Platform](Mobile-Platform) — iOS and Android platform abstraction
- [Platform Input](Platform-Input) — Cross-platform input abstraction
- [Cross-Compilation: Wine Testing](Cross-Compilation-Wine-Testing) — MinGW cross-compile and Wine testing

### Graphics
- [RHI Abstraction Layer](RHI-Abstraction-Layer) — Backend-agnostic graphics interface
- [D3D12 Backend](D3D12-Backend) — Direct3D 12 implementation
- [DXR Raytracing](DXR-Raytracing) — DXR 1.1 ray tracing
- [Hybrid Ray Tracing](Hybrid-Ray-Tracing) — Software + hardware hybrid RT
- [Upscaling (DLSS/FSR)](Upscaling-System) — Temporal upscaling techniques
- [Render Graph](Render-Graph) — Frame graph rendering system
- [Shader Graph](Shader-Graph) — Node-based shader authoring
- [GPU Particles](GPU-Particles) — Compute-based particle system
- [GPU-Driven Rendering](GPU-Driven-Rendering) — Indirect draw and mesh clustering
- [Volumetric Fog](Volumetric-Fog) — Froxel-based volumetric fog
- [Global Illumination](Global-Illumination) — DDGI and probe-based GI
- [Virtual Texturing](Virtual-Texturing) — Streaming virtual textures
- [Water Rendering](Water-Rendering) — FFT ocean and water surfaces
- [Clustered Lighting](Clustered-Lighting) — Clustered forward+ lighting
- [Mesh Shaders](Mesh-Shaders) — Mesh and amplification shaders

### Advanced
- [Configuration Reference](Configuration-Reference) — All settings, console commands, and CVars
- [Performance Tips](Performance-Tips) — Optimization guide for game developers
- [Threading Model](Threading-Model) — Thread architecture and safety rules
- [Memory Management Patterns](Memory-Management-Patterns) — Allocation strategies and RAII
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — CMake configuration and CI/CD
- [Profiler and Debugging](Profiler-and-Debugging) — Frame profiling, GPU timing, and debug tools
- [Performance Profiling Guide](Performance-Profiling-Guide) — Detailed profiling workflows
- [Utilities](Utilities) — Logger, console, crash handler, debug tools
- [Testing](Testing) — Unit tests and test framework
- [Codebase Statistics](Codebase-Statistics) — Code volume, file counts, and metrics
- [Codebase Health](Codebase-Health) — System maturity and known gaps
- [Error Handling Patterns](Error-Handling-Patterns) — Error handling conventions
- [Hot Reload Overview](Hot-Reload-Overview) — Script and asset hot-reloading
- [Troubleshooting](Troubleshooting) — Common issues and solutions
- [Contributing](Contributing) — How to contribute (includes pre-commit checks)

### Specifications
- [Networking Wire Format](Networking-Wire-Format) — Network packet format spec
- [Asset Format Specifications](Asset-Format-Specifications) — Asset file format spec
- [Editor Plugin Development](Editor-Plugin-Development) — Editor plugin ABI guide

### Reference
- [API Documentation](../docs/README.md) — Generated API reference

## Code Quality

SparkEngine enforces code quality automatically:

- **clang-format** — Enforced in CI on every PR (Microsoft-based style, Allman braces, 120-col, 4-space indent)
- **clang-tidy** — Static analysis for bugprone, modernize, performance, and readability checks
- **3,119 unit tests** — CTest suite across 244 test files covering all major subsystems
- **5 sanitizers** — ASan, UBSan, LSan, TSan, MSan in CI
- **Code coverage** — lcov reports generated per CI run

See [Contributing](Contributing) for the full pre-commit checklist.

## License

SparkEngine is licensed under the [Spark Open License](https://github.com/Krilliac/SparkEngine/blob/master/LICENSE) — no royalties, no fees, anti-plagiarism protected.

## Project Statistics

<!-- AUTO:stats -->
| Metric | Count |
|--------|-------|
| Header files | 633 |
| ECS Components | 79 |
| ECS Systems | 75 |
| Editor Panels | 59 |
| Test files | 340 |
| Test cases | 4416+ |
| Wiki pages | 125 |
| *Last synced* | *2026-04-09 04:51* |
<!-- /AUTO:stats -->
