# Spark Engine

**Spark Engine** is a free, open-source 3D game engine written in C++20. Designed for first-person shooters and other 3D games, it ships with DirectX 11 rendering, Bullet Physics, XAudio2 spatial audio, AngelScript hot-reload scripting, an EnTT-based ECS architecture, and an ImGui visual editor.

> **Early Development** — SparkEngine is under active development. Expect rough edges.

## Platform Support

| Platform | Status | Compiler |
|----------|--------|----------|
| Windows 10+ | Primary | MSVC v143 (VS 2022), v144 (VS 2026) |
| Linux x64 | Experimental | GCC 11+, Clang 14+ |
| macOS | Experimental | Apple Clang with C++20 |

## Feature Highlights

- **Rendering** — DirectX 11 with forward, deferred, forward+, and clustered pipelines. PBR materials, cascaded shadow mapping, SSAO, SSR, volumetric lighting, bloom, HDR tone mapping, TAA/FXAA/MSAA, IBL, GPU particles, decals, fog, and quality presets. Experimental Vulkan and OpenGL backends via RHI abstraction.
- **Physics** — Bullet Physics 3 with rigid bodies, 9 collision shape types, constraints, raycasting, overlap queries, physics materials, and debug draw.
- **Audio** — XAudio2 3D spatial audio with Doppler effects, distance attenuation, volume channels, and object pooling. Miniaudio as cross-platform fallback.
- **Gameplay** — ECS architecture (EnTT), FPS player controller, weapons, vehicles, inventory, quests, day/night cycle, weather, terrain with quadtree LOD.
- **AI** — Behavior trees, NavMesh A* pathfinding, perception system, steering behaviors.
- **Animation** — Skeletal animation, state machines, multi-layer blending, IK (two-bone, look-at, FABRIK), root motion, FBX/glTF import.
- **Scripting** — AngelScript with hot-reload, lifecycle callbacks, and full engine API bindings.
- **Networking** — UDP client/server, entity replication, client-side prediction, lag compensation.
- **Editor** — ImGui-powered visual editor with scene hierarchy, inspector, gizmos, material editor, animation timeline, and 200+ debug console commands.

## Downloads

Pre-built binaries are published on every commit to `master`:

- [Windows Release (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Release.zip)
- [Windows Debug (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Windows-Debug.zip)
- [Linux Release (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Release.tar.gz)
- [Linux Debug (x64)](https://github.com/Krilliac/SparkEngine/releases/latest/download/SparkEngine-Linux-Debug.tar.gz)

## Wiki Navigation

### Getting Started
- [Getting Started](Getting-Started) — Prerequisites, building, and running
- [Architecture Overview](Architecture-Overview) — Engine design and project structure
- [Creating a Game Module](Creating-a-Game-Module) — Build your first game module

### Engine Subsystems
- [Entity Component System](Entity-Component-System) — EnTT ECS, components, and systems
- [Rendering and Graphics](Rendering-and-Graphics) — Graphics pipeline, materials, and post-processing
- [Physics](Physics) — Bullet Physics integration
- [Cloth Simulation](Cloth-Simulation) — Position-based dynamics cloth simulation
- [Audio](Audio) — Spatial audio system
- [Input System](Input-System) — Keyboard, mouse, and gamepad
- [Scripting with AngelScript](Scripting-with-AngelScript) — Gameplay scripting
- [Visual Scripting](Visual-Scripting) — Node-based visual scripting
- [AI and Navigation](AI-and-Navigation) — Behavior trees and pathfinding
- [Animation](Animation) — Skeletal animation and IK
- [2D Systems](2D-Systems) — 2D physics, sprite batching, and rendering
- [Networking](Networking) — Multiplayer networking
- [Dedicated Server](Dedicated-Server) — Headless dedicated server
- [Scene Management](Scene-Management) — Scenes, hierarchy, and prefabs
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

### Gameplay & Tools
- [Gameplay Systems](Gameplay-Systems) — Player, weapons, inventory, quests
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Terrain and procedural content
- [Save System](Save-System) — Save/load functionality
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) — Time-of-day and weather
- [Cinematic Sequencer](Cinematic-Sequencer) — Timeline-based cinematics
- [SparkEditor](SparkEditor) — Visual editor guide
- [SparkConsole](SparkConsole) — Debug console
- [Shader Pipeline](Shader-Pipeline) — Shader authoring and compilation
- [Asset Pipeline](Asset-Pipeline) — Asset loading and formats

### Platform Support
- [VR Support](VR-Support) — OpenXR virtual reality framework
- [Mobile Platform](Mobile-Platform) — iOS and Android platform abstraction

### Graphics Backends
- [RHI Abstraction Layer](RHI-Abstraction-Layer) — Backend-agnostic graphics interface
- [D3D12 Backend](D3D12-Backend) — Direct3D 12 implementation
- [DXR Raytracing](DXR-Raytracing) — DXR 1.1 ray tracing
- [Upscaling (DLSS/FSR)](Upscaling-System) — Temporal upscaling techniques

### Advanced
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — CMake configuration and CI/CD
- [Profiler and Debugging](Profiler-and-Debugging) — Frame profiling, GPU timing, and debug tools
- [Testing](Testing) — Unit tests and test framework
- [Troubleshooting](Troubleshooting) — Common issues and solutions
- [Contributing](Contributing) — How to contribute (includes pre-commit checks)

### Reference
- [API Documentation](../docs/README.md) — Doxygen-generated API reference (class diagrams, call graphs, source browser)

## Code Quality

SparkEngine enforces code quality automatically:

- **clang-format** — Enforced in CI on every PR (Microsoft-based style, Allman braces, 120-col, 4-space indent)
- **clang-tidy** — Static analysis for bugprone, modernize, performance, and readability checks
- **35+ unit tests** — CTest suite covering all major subsystems
- **AddressSanitizer / UBSanitizer** — Memory safety checks in CI
- **CodeQL** — GitHub security scanning

See [Contributing](Contributing) for the full pre-commit checklist.

## License

SparkEngine is licensed under the [MIT License](https://github.com/Krilliac/SparkEngine/blob/master/LICENSE) — no royalties, no strings attached.

## Project Statistics

<!-- AUTO:stats -->
| Metric | Count |
|--------|-------|
| Header files | 343 |
| ECS Components | 37 |
| ECS Systems | 47 |
| Editor Panels | 32 |
| Test files | 71 |
| Test cases | 864+ |
| Wiki pages | 50 |
| *Last synced* | *2026-03-12 15:56* |
<!-- /AUTO:stats -->
