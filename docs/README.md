# SparkEngine Documentation Index

Master index for every SparkEngine documentation artifact, grouped by type. Each category lives in its own subfolder of `docs/`. The full user-facing guide set lives in [`wiki/`](../wiki/) and is cross-linked below.

The repository is the canonical documentation source. [GitHub Wiki](https://github.com/Krilliac/SparkEngine/wiki) is a flattened publication of `wiki/` for GitHub and website links, while [sparkengine.dev](https://sparkengine.dev/) consumes the deterministic repository site-data bundle. `tools/publish-wiki.py` rewrites folder-relative links and assets for GitHub Wiki without creating a second authored documentation tree; `.github/workflows/publish-wiki.yml` republishes the exact `Working` revision when canonical documentation changes.

> **Regenerate auto-produced artifacts:** `docs/update-all-docs.sh`.
> **Generate full API reference:** `docs/generate-api-docs.sh generate` (writes to `docs/api/`, gitignored).

---

## Repository-driven website and readiness handoff

The website does not maintain a second copy of engine status, counts, learning paths, legal framing, or documentation. The controlling inputs are:

- [`site/content.json`](site/content.json) — repository-owned public wording and navigation data.
- [`site/readiness.json`](site/readiness.json) — capability dimensions, release gates, promotion rules, execution waves, and the declared release profiles. The only declared profile is `stable-v1` (Windows 11 x64, MSVC v143, D3D11, NullRHI, C++ gameplay modules, installed single-player vertical slice); every capability outside it is explicitly classified experimental or unsupported, and every public surface the profile owns must name it.
- [`site/docs-catalog.json`](site/docs-catalog.json) — recursive documentation inclusion, classification, and routes.
- [`readiness/work-items/`](readiness/work-items/) — dependency-ordered, code-session-ready implementation briefs.
- [`readiness/ENGINE_READINESS_HANDOFF.md`](readiness/ENGINE_READINESS_HANDOFF.md) — generated complete handoff; do not edit it directly.

Validate and regenerate locally:

```bash
python3 tools/site-data/validate.py
python3 tools/site-data/render_handoff.py --check
python3 tools/site-data/generate.py --output .site-data
python3 tools/site-data/validate.py --published .site-data
```

The site-data generator rebuilds the ignored `docs/api/` reference corpus from
the checked-out headers and sources before collecting documentation. This keeps
clean CI runners and local generation on the same exact-commit inputs.

`.github/workflows/site-data.yml` proves deterministic generation on pull requests and pushes. After the exact `Working` commit's `Build SparkEngine` run completes, `.github/workflows/site-data-publish.yml` publishes a hash-verified snapshot to the `site-data` branch. A failed/cancelled build publishes the exact commit with a blocked publication state; an invalid contract publishes nothing. Application/layout changes still require the website's own deployment workflow.

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
| [`../wiki/`](../wiki/) | Primary user and developer wiki (136 pages) | [jump ↓](#wiki-user--developer-docs) |

---

## Architecture

Architectural decisions and cross-subsystem policies.

- [Dependency Matrix](architecture/dependency-matrix.md) — Subsystem-to-subsystem dependency rules and cycle checks.
- [Gameplay Extension Policy](architecture/gameplay-extension-policy.md) — How to extend gameplay systems without breaking engine boundaries.

## Specifications

Wire formats, binary layouts, and stable ABI contracts.

- [Asset Format](specs/asset-format.md) — SparkEngine native asset binary layout.
- [Networking Wire Format](specs/networking-wire-format.md) — UDP packet structure and serialization rules.
- [C++ Game Module ABI Guide](specs/plugin-abi-guide.md) — Game-module DLL boundary, lifecycle, and version compatibility.

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
- [External Services and Orchestration](guides/External-Services-and-Orchestration.md) — Dedicated hosting, gateway handoff, daemon supervision, orchestration, and collaboration isolation.
- [Offline Cooking and Runtime Automation](guides/Offline-Cooking-and-Automation.md) — Deterministic cooking, bounded workers, runtime smoke tests, and read-only package inspection.
- [Stable C Plugin ABI](guides/plugin-abi.md) — Versioned C extension boundary, sidecar integrity, task quiescence, and hot reload.

## Tooling

Documentation automation: every script that keeps docs, wikis, badges, and AI context in sync.

- [Tooling Index](tooling/README.md) — Master script, individual generators, validation scripts, legacy Doxygen setup.

## API Reference

Auto-generated from `.h`/`.hpp`/`.cpp` by pure-shell scripts — no Doxygen required.

**Committed entry points (in the wiki):**

| Page | Contents |
|------|----------|
| [API Reference](../wiki/reference/API-Reference.md) | Top-level hub that links to every index below. |
| [Symbol Index](../wiki/reference/Symbol-Index.md) | Every class, struct, enum, function, method, macro, and type alias. |
| [Function Index](../wiki/reference/Function-Index.md) | Every free function and out-of-line method definition. |
| [Class & Struct Index](../wiki/reference/Class-Index.md) | Every declared class and struct. |
| [Enum Index](../wiki/reference/Enum-Index.md) | Every enum with its source location. |
| [Macro & Alias Index](../wiki/reference/Macro-Index.md) | Every `#define` (excluding guards) plus `typedef`/`using` aliases. |
| [File Tree](../wiki/reference/File-Tree.md) | Hierarchical file listing with LOC, `@brief`, and Mermaid module graph. |
| [Class Hierarchy](../wiki/reference/Class-Hierarchy.md) | Mermaid `classDiagram` per top-level module. |

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

The primary user-facing docs are in [`wiki/`](../wiki/) — 136 pages. The authoritative navigation is [`wiki/_Sidebar.md`](../wiki/_Sidebar.md); [`wiki/Home.md`](../wiki/Home.md) is the landing page.

Categories (mirrored from the sidebar):

### Getting Started
- [Home](../wiki/Home.md)
- [FAQ](../wiki/getting-started/FAQ.md)
- [Getting Started](../wiki/getting-started/Getting-Started.md)
- [Quick-Start Tutorial](../wiki/getting-started/Quick-Start-Tutorial.md)
- [Making Your First Game](../wiki/getting-started/Making-Your-First-Game.md)
- [Making Your First Multiplayer Game](../wiki/getting-started/Making-Your-First-Multiplayer-Game.md)
- [Artist Workflow Guide](../wiki/getting-started/Artist-Workflow-Guide.md)
- [Editor Walkthrough](../wiki/getting-started/Editor-Walkthrough.md)
- [Migration Guide](../wiki/getting-started/Migration-Guide.md)
- [How SparkEngine Works](../wiki/getting-started/How-SparkEngine-Works.md)
- [Architecture Overview](../wiki/getting-started/Architecture-Overview.md)
- [Engine Architecture Flowchart](../wiki/getting-started/Engine-Architecture-Flowchart.md)
- [Creating a Game Module](../wiki/getting-started/Creating-a-Game-Module.md)
- [Game Modules (catalog)](../wiki/getting-started/Game-Modules.md)

### Engine Subsystems
- [Entity Component System](../wiki/subsystems/Entity-Component-System.md)
- [Rendering and Graphics](../wiki/subsystems/Rendering-and-Graphics.md)
- [Physics](../wiki/subsystems/Physics.md)
- [Cloth Simulation](../wiki/subsystems/Cloth-Simulation.md)
- [Audio](../wiki/subsystems/Audio.md)
- [Input System](../wiki/subsystems/Input-System.md)
- [Camera System](../wiki/subsystems/Camera-System.md)
- [Scripting with AngelScript](../wiki/subsystems/Scripting-with-AngelScript.md)
- [Visual Scripting](../wiki/subsystems/Visual-Scripting.md)
- [AI and Navigation](../wiki/subsystems/AI-and-Navigation.md)
- [Animation](../wiki/subsystems/Animation.md)
- [2D Systems](../wiki/subsystems/2D-Systems.md)
- [Networking](../wiki/subsystems/Networking.md)
- [Dedicated Server](../wiki/subsystems/Dedicated-Server.md)
- [Multiplayer Quick Start](../wiki/subsystems/Multiplayer-Quick-Start.md)
- [Area Server Architecture](../wiki/subsystems/Area-Server-Architecture.md)
- [Scene Management](../wiki/subsystems/Scene-Management.md)
- [Large World Support](../wiki/subsystems/Large-World-Support.md)
- [Collaborative Editing](../wiki/subsystems/Collaborative-Editing.md)
- [Coroutine System](../wiki/subsystems/Coroutine-System.md)
- [Event System](../wiki/subsystems/Event-System.md)
- [Event Response System](../wiki/subsystems/Event-Response-System.md)
- [Job System](../wiki/subsystems/Job-System.md)
- [UI System](../wiki/subsystems/UI-System.md)
- [UI Layout Extensions](../wiki/subsystems/UI-Layout-Extensions.md)
- [Localization](../wiki/subsystems/Localization.md)
- [Dialogue System](../wiki/subsystems/Dialogue-System.md)
- [Destruction System](../wiki/subsystems/Destruction-System.md)
- [Replay System](../wiki/subsystems/Replay-System.md)
- [Achievement System](../wiki/subsystems/Achievement-System.md)
- [Loading System](../wiki/subsystems/Loading-System.md)
- [Mod System](../wiki/subsystems/Mod-System.md)
- [Content Delivery](../wiki/subsystems/Content-Delivery.md)
- [Tween System](../wiki/subsystems/Tween-System.md)
- [Memory Integrity](../wiki/subsystems/Memory-Integrity.md)

### Gameplay & Tools
- [Gameplay Systems](../wiki/gameplay-tools/Gameplay-Systems.md)
- [Terrain and Procedural Generation](../wiki/gameplay-tools/Terrain-and-Procedural-Generation.md)
- [Save System](../wiki/gameplay-tools/Save-System.md)
- [Persistence System](../wiki/gameplay-tools/Persistence-System.md)
- [Day Night Cycle and Weather](../wiki/gameplay-tools/Day-Night-Cycle-and-Weather.md)
- [Cinematic Sequencer](../wiki/gameplay-tools/Cinematic-Sequencer.md)
- [Runtime Prefabs](../wiki/gameplay-tools/Runtime-Prefabs.md)
- [SparkEditor](../wiki/gameplay-tools/SparkEditor.md)
- [Editor Tutorials](../wiki/gameplay-tools/Editor-Tutorials.md)
- [SparkConsole](../wiki/gameplay-tools/SparkConsole.md)
- [SparkDaemon](../wiki/gameplay-tools/SparkDaemon.md)
- [Shader Pipeline](../wiki/gameplay-tools/Shader-Pipeline.md)
- [Asset Pipeline](../wiki/gameplay-tools/Asset-Pipeline.md)
- [Asset Validation](../wiki/gameplay-tools/Asset-Validation.md)
- [Asset Migration](../wiki/gameplay-tools/Asset-Migration.md)
- [Game Packaging](../wiki/gameplay-tools/Game-Packaging.md)
- [Online Services](../wiki/gameplay-tools/Online-Services.md)
- [DataTable System](../wiki/gameplay-tools/DataTable-System.md)
- [Loot and Crafting System](../wiki/gameplay-tools/Loot-And-Crafting-System.md)
- [CSG System](../wiki/gameplay-tools/CSG-System.md)
- [Font System](../wiki/gameplay-tools/Font-System.md)
- [Timer Manager](../wiki/gameplay-tools/Timer-Manager.md)
- [Movie Render Pipeline](../wiki/gameplay-tools/Movie-Render-Pipeline.md)
- [HLOD and World Partition](../wiki/gameplay-tools/HLOD-And-World-Partition.md)
- [Remote Debug System](../wiki/gameplay-tools/Remote-Debug-System.md)
- [Selection Manager](../wiki/gameplay-tools/Selection-Manager.md)
- [Asset Dependency Graph](../wiki/gameplay-tools/Asset-Dependency-Graph.md)
- [Editor Automation](../wiki/gameplay-tools/Editor-Automation.md)
- [File Watcher](../wiki/gameplay-tools/File-Watcher.md)
- [Project Templates](../wiki/gameplay-tools/Project-Templates.md)

### Platform Support
- [VR Support](../wiki/platform/VR-Support.md)
- [Mobile Platform](../wiki/platform/Mobile-Platform.md)
- [Accessibility](../wiki/platform/Accessibility.md)
- [Platform Input](../wiki/platform/Platform-Input.md)
- [Cross-Compilation: Wine Testing](../wiki/platform/Cross-Compilation-Wine-Testing.md)

### Graphics
- [RHI Abstraction Layer](../wiki/graphics/RHI-Abstraction-Layer.md)
- [D3D11 Backend](../wiki/graphics/D3D11-Backend.md)
- [D3D12 Backend](../wiki/graphics/D3D12-Backend.md)
- [Vulkan Backend](../wiki/graphics/Vulkan-Backend.md)
- [OpenGL Backend](../wiki/graphics/OpenGL-Backend.md)
- [Metal Backend](../wiki/graphics/Metal-Backend.md)
- [DXR Raytracing](../wiki/graphics/DXR-Raytracing.md)
- [Hybrid Ray Tracing](../wiki/graphics/Hybrid-Ray-Tracing.md)
- [Upscaling (DLSS/FSR)](../wiki/graphics/Upscaling-System.md)
- [Render Graph](../wiki/graphics/Render-Graph.md)
- [Shader Graph](../wiki/graphics/Shader-Graph.md)
- [GPU Particles](../wiki/graphics/GPU-Particles.md)
- [GPU-Driven Rendering](../wiki/graphics/GPU-Driven-Rendering.md)
- [Volumetric Fog](../wiki/graphics/Volumetric-Fog.md)
- [Global Illumination](../wiki/graphics/Global-Illumination.md)
- [Virtual Texturing](../wiki/graphics/Virtual-Texturing.md)
- [Water Rendering](../wiki/graphics/Water-Rendering.md)
- [Clustered Lighting](../wiki/graphics/Clustered-Lighting.md)
- [Material System](../wiki/graphics/Material-System.md)
- [Post-Processing](../wiki/graphics/Post-Processing.md)
- [Shadow System](../wiki/graphics/Shadow-System.md)
- [Particle System](../wiki/graphics/Particle-System.md)
- [Decal System](../wiki/graphics/Decal-System.md)
- [Sky and Atmosphere](../wiki/graphics/Sky-and-Atmosphere.md)
- [Foliage System](../wiki/graphics/Foliage-System.md)
- [Mesh Shaders](../wiki/graphics/Mesh-Shaders.md)
- [Neural Rendering](../wiki/graphics/Neural-Rendering.md)

### Advanced
- [Configuration Reference](../wiki/advanced/Configuration-Reference.md)
- [Performance Tips](../wiki/advanced/Performance-Tips.md)
- [Benchmark Framework](../wiki/advanced/Benchmark-Framework.md)
- [Threading Model](../wiki/advanced/Threading-Model.md)
- [Memory Safety](../wiki/advanced/Memory-Safety.md)
- [Memory Management Patterns](../wiki/advanced/Memory-Management-Patterns.md)
- [Build System and CMake Modules](../wiki/advanced/Build-System-and-CMake-Modules.md)
- [Profiler and Debugging](../wiki/advanced/Profiler-and-Debugging.md)
- [Performance Profiling Guide](../wiki/advanced/Performance-Profiling-Guide.md)
- [Telemetry System](../wiki/advanced/Telemetry-System.md)
- [Golden Image Testing](../wiki/advanced/Golden-Image-Testing.md)
- [Utilities](../wiki/advanced/Utilities.md)
- [Testing](../wiki/advanced/Testing.md)
- [Codebase Statistics](../wiki/advanced/Codebase-Statistics.md)
- [Codebase Health](../wiki/advanced/Codebase-Health.md)
- [Error Handling Patterns](../wiki/advanced/Error-Handling-Patterns.md)
- [Hot Reload Overview](../wiki/advanced/Hot-Reload-Overview.md)
- [Troubleshooting](../wiki/advanced/Troubleshooting.md)
- [Contributing](../wiki/advanced/Contributing.md)

### Reference
- [API Reference](../wiki/reference/API-Reference.md)
- [Symbol Index](../wiki/reference/Symbol-Index.md)
- [Function Index](../wiki/reference/Function-Index.md)
- [Class Index](../wiki/reference/Class-Index.md)
- [Enum Index](../wiki/reference/Enum-Index.md)
- [Macro Index](../wiki/reference/Macro-Index.md)
- [File Tree](../wiki/reference/File-Tree.md)
- [Class Hierarchy](../wiki/reference/Class-Hierarchy.md)

---

## License

Part of the SparkEngine project. Spark Open License 1.0.
