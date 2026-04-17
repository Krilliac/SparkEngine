# SparkEngine Documentation Index

Master index for every SparkEngine documentation artifact, grouped by type. Each category lives in its own subfolder of `docs/`. The full user-facing guide set lives in [`wiki/`](../wiki/) and is cross-linked below.

> **Regenerate auto-produced artifacts:** `docs/update-all-docs.sh`.
> **Generate full API reference:** `docs/generate-api-docs.sh generate` (writes to `docs/api/`, gitignored).

---

## Categories

| Folder | Contents | Index |
|--------|----------|-------|
| [`architecture/`](architecture/) | Architectural decisions, dependency matrices, extension policies | [jump ↓](#architecture) |
| [`specs/`](specs/) | Wire formats, asset formats, plugin ABI contracts | [jump ↓](#specifications) |
| [`plans/`](plans/) | Roadmaps and milestone plans | [jump ↓](#plans--roadmaps) |
| [`status/`](status/) | Current subsystem status snapshots | [jump ↓](#status-reports) |
| [`guides/`](guides/) | User-facing guides (packaging, etc.) | [jump ↓](#guides) |
| [`tooling/`](tooling/) | Documentation automation, Doxygen setup | [jump ↓](#tooling) |
| [`api/`](api/) *(generated)* | Per-header API reference pages (regenerated locally) | [jump ↓](#api-reference) |
| [`screenshots/`](screenshots/) | Editor and engine screenshots (images) | [jump ↓](#screenshots) |
| [`wine-upstream/`](wine-upstream/) | Upstream Wine patches for gVisor/UMH compatibility | [jump ↓](#wine-upstream-patches) |
| [`../wiki/`](../wiki/) | Primary user and developer wiki (137 pages) | [jump ↓](#wiki-user--developer-docs) |

---

## Architecture

Architectural decisions and cross-subsystem policies.

- [Dependency Matrix](architecture/dependency-matrix.md) — Subsystem-to-subsystem dependency rules and cycle checks.
- [Gameplay Extension Policy](architecture/gameplay-extension-policy.md) — How to extend gameplay systems without breaking engine boundaries.

## Specifications

Wire formats, binary layouts, and stable ABI contracts.

- [Asset Format](specs/asset-format.md) — SparkEngine native asset binary layout.
- [Networking Wire Format](specs/networking-wire-format.md) — UDP packet structure and serialization rules.
- [Plugin ABI Guide](specs/plugin-abi-guide.md) — Game-module DLL boundary, lifecycle, and version compatibility.

## Plans & Roadmaps

Forward-looking work plans — what's next, why, and when.

- [Feature Roadmap](plans/FEATURE_ROADMAP.md) — Tier 1/2/3 feature priorities.
- [Hardware Acceleration Plan](plans/hardware-acceleration-plan.md) — GPU/CPU acceleration strategy.
- [Tier 3 Polish & Maturity Milestone](plans/tier3-polish-maturity-milestone.md) — Milestone M1 epics, acceptance tests, perf budgets.

## Status Reports

Snapshots of current subsystem maturity.

- [Project Status](status/PROJECT_STATUS.md) — Per-subsystem status (stable / experimental / framework / planned).

## Guides

User-facing how-to guides that live outside the wiki.

- [Packaging Guide](guides/packaging.md) — Package formats, install layout, components, and versioning policy.

## Tooling

Documentation automation: every script that keeps docs, wikis, badges, and AI context in sync.

- [Tooling Index](tooling/README.md) — Master script, individual generators, validation scripts, legacy Doxygen setup.

## API Reference

Auto-generated from `.h`/`.hpp`/`.cpp` by pure-shell scripts — no Doxygen required.

**Committed entry points (in the wiki):**

| Page | Contents |
|------|----------|
| [API Reference](../wiki/API-Reference.md) | Top-level hub that links to every index below. |
| [Symbol Index](../wiki/Symbol-Index.md) | Every class, struct, enum, function, method, macro, and type alias. |
| [Function Index](../wiki/Function-Index.md) | Every free function and out-of-line method definition. |
| [Class & Struct Index](../wiki/Class-Index.md) | Every declared class and struct. |
| [Enum Index](../wiki/Enum-Index.md) | Every enum with its source location. |
| [Macro & Alias Index](../wiki/Macro-Index.md) | Every `#define` (excluding guards) plus `typedef`/`using` aliases. |
| [File Tree](../wiki/File-Tree.md) | Hierarchical file listing with LOC, `@brief`, and Mermaid module graph. |
| [Class Hierarchy](../wiki/Class-Hierarchy.md) | Mermaid `classDiagram` per top-level module. |

**Generated (locally, gitignored):**

Running `docs/generate-api-docs.sh generate` produces:

- `docs/api/README.md` — Per-module index of every generated page.
- `docs/api/<module>/<subpath>/<header>.md` — One page per `.h`/`.hpp` with classes, enums, free functions, macros, and type aliases.
- `docs/api/.symbols.tsv` — TSV with every symbol (kind, name, path, line, brief) for programmatic use.
- `docs/api/ComponentIndex.md` — ECS component quick reference.
- `docs/api/SystemIndex.md` — ECS system quick reference.

See [Tooling Index](tooling/README.md) for the full generator pipeline.

## Screenshots

- [`screenshots/`](screenshots/) — Editor panels and engine captures referenced from the wiki.

## Wine Upstream Patches

- [`wine-upstream/`](wine-upstream/) — Patches against Wine fixing gVisor/UMH incompatibilities; see the folder [README](wine-upstream/README.md).

---

## Wiki (User & Developer Docs)

The primary user-facing docs are in [`wiki/`](../wiki/) — 137 pages. The authoritative navigation is [`wiki/_Sidebar.md`](../wiki/_Sidebar.md); [`wiki/Home.md`](../wiki/Home.md) is the landing page.

Categories (mirrored from the sidebar):

### Getting Started
- [Home](../wiki/Home.md)
- [FAQ](../wiki/FAQ.md)
- [Getting Started](../wiki/Getting-Started.md)
- [Quick-Start Tutorial](../wiki/Quick-Start-Tutorial.md)
- [Making Your First Game](../wiki/Making-Your-First-Game.md)
- [Making Your First Multiplayer Game](../wiki/Making-Your-First-Multiplayer-Game.md)
- [Artist Workflow Guide](../wiki/Artist-Workflow-Guide.md)
- [Editor Walkthrough](../wiki/Editor-Walkthrough.md)
- [Migration Guide](../wiki/Migration-Guide.md)
- [How SparkEngine Works](../wiki/How-SparkEngine-Works.md)
- [Architecture Overview](../wiki/Architecture-Overview.md)
- [Engine Architecture Flowchart](../wiki/Engine-Architecture-Flowchart.md)
- [Creating a Game Module](../wiki/Creating-a-Game-Module.md)

### Engine Subsystems
- [Entity Component System](../wiki/Entity-Component-System.md)
- [Rendering and Graphics](../wiki/Rendering-and-Graphics.md)
- [Physics](../wiki/Physics.md)
- [Cloth Simulation](../wiki/Cloth-Simulation.md)
- [Audio](../wiki/Audio.md)
- [Input System](../wiki/Input-System.md)
- [Camera System](../wiki/Camera-System.md)
- [Scripting with AngelScript](../wiki/Scripting-with-AngelScript.md)
- [Visual Scripting](../wiki/Visual-Scripting.md)
- [AI and Navigation](../wiki/AI-and-Navigation.md)
- [Animation](../wiki/Animation.md)
- [2D Systems](../wiki/2D-Systems.md)
- [Networking](../wiki/Networking.md)
- [Dedicated Server](../wiki/Dedicated-Server.md)
- [Multiplayer Quick Start](../wiki/Multiplayer-Quick-Start.md)
- [Area Server Architecture](../wiki/Area-Server-Architecture.md)
- [Scene Management](../wiki/Scene-Management.md)
- [Large World Support](../wiki/Large-World-Support.md)
- [Collaborative Editing](../wiki/Collaborative-Editing.md)
- [Coroutine System](../wiki/Coroutine-System.md)
- [Event System](../wiki/Event-System.md)
- [Job System](../wiki/Job-System.md)
- [UI System](../wiki/UI-System.md)
- [UI Layout Extensions](../wiki/UI-Layout-Extensions.md)
- [Localization](../wiki/Localization.md)
- [Dialogue System](../wiki/Dialogue-System.md)
- [Destruction System](../wiki/Destruction-System.md)
- [Replay System](../wiki/Replay-System.md)
- [Achievement System](../wiki/Achievement-System.md)
- [Loading System](../wiki/Loading-System.md)
- [Mod System](../wiki/Mod-System.md)
- [Content Delivery](../wiki/Content-Delivery.md)
- [Tween System](../wiki/Tween-System.md)
- [Memory Integrity](../wiki/Memory-Integrity.md)

### Gameplay & Tools
- [Gameplay Systems](../wiki/Gameplay-Systems.md)
- [Terrain and Procedural Generation](../wiki/Terrain-and-Procedural-Generation.md)
- [Save System](../wiki/Save-System.md)
- [Persistence System](../wiki/Persistence-System.md)
- [Day Night Cycle and Weather](../wiki/Day-Night-Cycle-and-Weather.md)
- [Cinematic Sequencer](../wiki/Cinematic-Sequencer.md)
- [Runtime Prefabs](../wiki/Runtime-Prefabs.md)
- [SparkEditor](../wiki/SparkEditor.md)
- [Editor Tutorials](../wiki/Editor-Tutorials.md)
- [SparkConsole](../wiki/SparkConsole.md)
- [SparkDaemon](../wiki/SparkDaemon.md)
- [Shader Pipeline](../wiki/Shader-Pipeline.md)
- [Asset Pipeline](../wiki/Asset-Pipeline.md)
- [Asset Validation](../wiki/Asset-Validation.md)
- [Asset Migration](../wiki/Asset-Migration.md)
- [Game Packaging](../wiki/Game-Packaging.md)
- [Online Services](../wiki/Online-Services.md)
- [DataTable System](../wiki/DataTable-System.md)
- [Loot and Crafting System](../wiki/Loot-And-Crafting-System.md)
- [CSG System](../wiki/CSG-System.md)
- [Font System](../wiki/Font-System.md)
- [Timer Manager](../wiki/Timer-Manager.md)
- [Movie Render Pipeline](../wiki/Movie-Render-Pipeline.md)
- [HLOD and World Partition](../wiki/HLOD-And-World-Partition.md)
- [Remote Debug System](../wiki/Remote-Debug-System.md)
- [Selection Manager](../wiki/Selection-Manager.md)
- [Asset Dependency Graph](../wiki/Asset-Dependency-Graph.md)
- [Editor Automation](../wiki/Editor-Automation.md)
- [File Watcher](../wiki/File-Watcher.md)
- [Project Templates](../wiki/Project-Templates.md)

### Platform Support
- [VR Support](../wiki/VR-Support.md)
- [Mobile Platform](../wiki/Mobile-Platform.md)
- [Accessibility](../wiki/Accessibility.md)
- [Platform Input](../wiki/Platform-Input.md)
- [Cross-Compilation: Wine Testing](../wiki/Cross-Compilation-Wine-Testing.md)

### Graphics
- [RHI Abstraction Layer](../wiki/RHI-Abstraction-Layer.md)
- [D3D12 Backend](../wiki/D3D12-Backend.md)
- [DXR Raytracing](../wiki/DXR-Raytracing.md)
- [Hybrid Ray Tracing](../wiki/Hybrid-Ray-Tracing.md)
- [Upscaling (DLSS/FSR)](../wiki/Upscaling-System.md)
- [Render Graph](../wiki/Render-Graph.md)
- [Shader Graph](../wiki/Shader-Graph.md)
- [GPU Particles](../wiki/GPU-Particles.md)
- [GPU-Driven Rendering](../wiki/GPU-Driven-Rendering.md)
- [Volumetric Fog](../wiki/Volumetric-Fog.md)
- [Global Illumination](../wiki/Global-Illumination.md)
- [Virtual Texturing](../wiki/Virtual-Texturing.md)
- [Water Rendering](../wiki/Water-Rendering.md)
- [Clustered Lighting](../wiki/Clustered-Lighting.md)
- [Material System](../wiki/Material-System.md)
- [Post-Processing](../wiki/Post-Processing.md)
- [Shadow System](../wiki/Shadow-System.md)
- [Particle System](../wiki/Particle-System.md)
- [Decal System](../wiki/Decal-System.md)
- [Sky and Atmosphere](../wiki/Sky-and-Atmosphere.md)
- [Foliage System](../wiki/Foliage-System.md)
- [Mesh Shaders](../wiki/Mesh-Shaders.md)
- [Neural Rendering](../wiki/Neural-Rendering.md)

### Advanced
- [Configuration Reference](../wiki/Configuration-Reference.md)
- [Performance Tips](../wiki/Performance-Tips.md)
- [Benchmark Framework](../wiki/Benchmark-Framework.md)
- [Threading Model](../wiki/Threading-Model.md)
- [Memory Safety](../wiki/Memory-Safety.md)
- [Memory Management Patterns](../wiki/Memory-Management-Patterns.md)
- [Build System and CMake Modules](../wiki/Build-System-and-CMake-Modules.md)
- [Profiler and Debugging](../wiki/Profiler-and-Debugging.md)
- [Performance Profiling Guide](../wiki/Performance-Profiling-Guide.md)
- [Telemetry System](../wiki/Telemetry-System.md)
- [Golden Image Testing](../wiki/Golden-Image-Testing.md)
- [Utilities](../wiki/Utilities.md)
- [Testing](../wiki/Testing.md)
- [Codebase Statistics](../wiki/Codebase-Statistics.md)
- [Codebase Health](../wiki/Codebase-Health.md)
- [Error Handling Patterns](../wiki/Error-Handling-Patterns.md)
- [Hot Reload Overview](../wiki/Hot-Reload-Overview.md)
- [Troubleshooting](../wiki/Troubleshooting.md)
- [Contributing](../wiki/Contributing.md)

### Reference
- [API Reference](../wiki/API-Reference.md)
- [Symbol Index](../wiki/Symbol-Index.md)
- [Function Index](../wiki/Function-Index.md)
- [Class Index](../wiki/Class-Index.md)
- [Enum Index](../wiki/Enum-Index.md)
- [Macro Index](../wiki/Macro-Index.md)
- [File Tree](../wiki/File-Tree.md)
- [Class Hierarchy](../wiki/Class-Hierarchy.md)

---

## License

Part of the SparkEngine project. Spark Open License 1.0.
