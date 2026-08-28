# Frequently Asked Questions

Common questions about SparkEngine — what it is, who it's for, and how to get started.

> **Release boundary:** The only declared release profile is `stable-v1`
> (Windows 11 x64, MSVC v143, D3D11, Windows NullRHI, C++ modules, and the
> SparkGameFPS single-player slice, plus the required Windows product set listed
> in `docs/site/readiness.json`, including SparkEditor and SparkConsole). It is
> blocked and uncertified. Platform,
> backend, scripting, networking, collaboration, and module descriptions below
> are implementation guidance unless that contract explicitly places them in profile.

---

## General

### What is SparkEngine?

SparkEngine is a free, open-source 3D game engine written in C++23. It started as an FPS engine and is evolving into a general-purpose engine with source implementations and prototypes for FPS, RPG, MMO, open-world, racing, platformer, and RTS genres. The repository includes DirectX 11 rendering, Jolt Physics, XAudio2 spatial audio, an EnTT ECS, an ImGui-based editor, and experimental AngelScript tooling outside `stable-v1`.

### Is SparkEngine free?

Yes. SparkEngine is licensed under the [Spark Open License](https://github.com/Krilliac/SparkEngine/blob/Working/LICENSE) — no royalties, no fees, fully free for commercial use. The license includes anti-plagiarism protection to prevent wholesale copying without attribution.

### What platforms does SparkEngine support?

| Platform | Status |
|----------|--------|
| Windows 11 x64 | `stable-v1` target — blocked and uncertified |
| Windows 10 x64 | Documented development floor — outside `stable-v1` |
| Linux x64 | Experimental (GCC 13+, Clang 17+) — outside `stable-v1` |
| macOS | Experimental (Apple Clang) — outside `stable-v1` |

Windows is the primary development platform. Build or CI availability is not release certification; Linux, macOS, and Windows 10 remain outside `stable-v1`.

### What genres can I make with SparkEngine?

The repository contains example and prototype game modules for FPS, platformer, racing, RPG, RTS, and MMO work. They are source implementations, not released products; only the blocked and uncertified SparkGameFPS single-player slice is inside `stable-v1`. The ECS and gameplay systems can be extended for other genres without implying release support.

### How does SparkEngine compare to Unity / Unreal / Godot?

SparkEngine is smaller and earlier in development than those engines. Key differences:

- **Open-source C++23** — You have full source access and can modify anything. No black-box runtime.
- **No editor lock-in** — Game logic lives in C++ modules or AngelScript scripts, not in a proprietary project format.
- **MMO-oriented experimental architecture** — Area-server, world-streaming, and collaborative-editing implementations exist, but they are outside `stable-v1` and are not authenticated-transport or deployment evidence.
- **Source-oriented** — The repository builds locally from source; current release evidence does not establish a portable download-size or build-time budget.
- **Trade-off** — Fewer ready-made assets, smaller community, and less polish than mature engines.

### Do I need to know C++ to use SparkEngine?

C++ is the primary language, but you have options:

- **AngelScript (experimental)** — A C-like scripting implementation with hot-reload; outside `stable-v1`.
- **Visual Scripting (experimental)** — A node-based editor that targets AngelScript; outside `stable-v1`.
- **C++ Modules** — For full engine access, write a game module in C++23.

Artists and level designers can use the editor without writing code at all — see the [Artist Workflow Guide](Artist-Workflow-Guide.md).

---

## Building & Running

### What do I need to build SparkEngine?

- A C++23 compiler: MSVC v143+ (VS 2022), GCC 13+, or Clang 17+
- CMake 3.25+
- Git (for submodules)

See [Getting Started](Getting-Started.md) for full platform-specific instructions.

### How long does it take to build?

Build time depends on the selected targets, toolchain, cache state, and host. No current stable-v1 evidence artifact establishes a certified clean or incremental build-time range.

### Can I build without a GPU?

Yes, through several distinct GPU-less development routes. They are not one
automatic fallback contract:

| Backend | GPU-less route |
|---------|-------------------|
| D3D11 | WARP adapter route where explicitly selected/available |
| D3D12 | WARP adapter route where explicitly selected/available |
| Vulkan | Mesa Lavapipe CPU ICD when installed and selected by the host/runtime |
| OpenGL | Mesa llvmpipe when the host display/context is explicitly configured |
| None | `NullRHIDevice`, a distinct no-render device that rasterizes nothing |

`-headless` selects the platform's no-graphics host entry for server or automation
development. The current host path does not instantiate `NullRHIDevice`; explicit
host wiring and evidence remain `HEAD-220`.

### How do I run the engine on Linux without a display?

Use Xvfb (virtual framebuffer) with Mesa software rendering:

```bash
sudo apt-get install -y xvfb libgl1-mesa-dri
Xvfb :99 -screen 0 1024x768x24 &
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./SparkEngine
```

### Can I build just the engine without the editor?

You can disable the editor explicitly. The current `minimal` preset is not a general core-only profile: it disables networking and DXR, while several requested feature variables in that preset are currently inert and tracked by `HEAD-220`.

```bash
cmake --preset minimal                    # Reduced development preset; not a core-only contract
cmake -B build -DENABLE_EDITOR=OFF        # Everything except editor
```

### What are common implemented command-line arguments?

| Argument | Description |
|----------|-------------|
| `-headless` / `-dedicated` | Run the host without a graphics window |
| `-game <path>` | Load a specific game module DLL/SO |
| `-window-size WxH` | Override window resolution (e.g., `1920x1080`) |
| `-test-frames N` | Run N frames then exit (for benchmarking) |
| `-scene <path>` | Load a specific scene on startup |
| `--help` / `-h` | Print the platform host's authoritative option list |

---

## Editor

### How do I open the editor?

Build with `ENABLE_EDITOR=ON` and launch the separate `SparkEditor` executable.
The source does not define an F1 engine-overlay toggle. The repository inventory
currently contains 65 `*Panel.h` classes; registration and default visibility are
separate metrics.

### What panels does the editor have?

The source tree includes 65 `*Panel.h` classes covering scene editing, asset
management, physics, gameplay, audio, scripting, profiling, and more. This
source-file inventory is not `stable-v1` editor certification and does not mean
all classes are registered or shown by default. The 6 core panels shown by
default are:

- **Scene View** — 3D viewport with gizmos
- **Hierarchy** — Scene graph tree
- **Inspector** — Component property editor
- **Asset Browser** — File browser with thumbnails
- **Console** — Command and log panel
- **Game View** — In-game preview
Additional registered panels are available from the **Window** menu. The source
inventory is not itself a registration guarantee. See
[Editor Walkthrough](Editor-Walkthrough.md) for a practical guide.

### Can multiple people edit a scene at the same time?

An experimental collaborative-editing implementation exists outside `stable-v1`. One user hosts and others join; the path is not release or hostile-network evidence. See [Collaborative Editing](../subsystems/Collaborative-Editing.md).

---

## Gameplay & Scripting

### How do I create a new game?

Create a dynamic **game module** that implements `IModule`, then select one module with `-game`, a manifest, or the staged executable-directory candidate rules. The runtime does not bulk-discover the source `GameModules/` tree. See [Creating a Game Module](Creating-a-Game-Module.md) for a step-by-step guide, or [Making Your First Game](Making-Your-First-Game.md) for a tutorial.

### What scripting language does SparkEngine use?

[AngelScript](../subsystems/Scripting-with-AngelScript.md) is the experimental C-like scripting implementation. Its hot-reload path exists, but scripting is outside `stable-v1` and is not release-certified.

### Does SparkEngine support visual scripting?

An experimental [Visual Scripting](../subsystems/Visual-Scripting.md) panel compiles node graphs to AngelScript. Both surfaces are outside `stable-v1`.

### How does the ECS work?

SparkEngine uses [EnTT](https://github.com/skypjack/entt) for its Entity Component System. Entities are lightweight IDs, components are plain data structs, and systems operate on component groups. A reproducible source inventory currently finds 79 component structs across 17 component headers; no canonical source-backed total is claimed for systems here. See [Entity Component System](../subsystems/Entity-Component-System.md).

### How do I add multiplayer to my game?

The repository includes an experimental UDP client/server stack with replication, prediction, and lag compensation. It is unauthenticated and unencrypted, and multiplayer is outside the single-player `stable-v1` profile. Use the [Multiplayer Quick Start](../subsystems/Multiplayer-Quick-Start.md) only for isolated development, then see [Networking](../subsystems/Networking.md).

---

## Graphics

### What rendering backends are available?

| Backend | Status | Platform |
|---------|--------|----------|
| DirectX 11 | `stable-v1` target — blocked and uncertified | Windows 11 x64 |
| DirectX 12 | Experimental — outside `stable-v1` | Windows |
| Vulkan | Experimental — outside `stable-v1` | Windows, Linux |
| OpenGL | Experimental — outside `stable-v1` | Windows/Linux implementation paths; Linux requests 4.5, while macOS uses its separate 4.1 system-context path |
| Metal | Experimental — outside `stable-v1` | macOS |
| NullRHI | No-render path; in-profile only on Windows 11 x64, blocked and uncertified | Host-dependent |

Where their platform gates and dependencies permit compilation, these backends
provide `IRHIDevice` implementations behind the [RHI Abstraction
Layer](../graphics/RHI-Abstraction-Layer.md). That common interface does not
establish complete renderer feature or behavioral parity between backends.

### What rendering implementation inventory exists?

The source inventory includes forward, deferred, forward+, and clustered
pipelines; PBR materials; cascaded shadow maps; SSAO; SSR; volumetric fog;
bloom; HDR tone mapping; TAA/FXAA/MSAA; particles; decals; image-based
lighting; mesh-shader and GPU-driven paths; and dynamic quality scaling.
Availability, completeness, and parity vary by backend. Breadth beyond the
blocked D3D11 profile path is experimental and outside `stable-v1`.

### Can I use ray tracing?

Experimental DXR 1.1 and hybrid ray-tracing implementations are available on the D3D12 path (Windows, SM6.5+ GPU required). D3D12 and DXR are outside `stable-v1`. See [DXR Raytracing](../graphics/DXR-Raytracing.md) and [Hybrid Ray Tracing](../graphics/Hybrid-Ray-Tracing.md).

---

## Physics

### What physics engine does SparkEngine use?

[Jolt Physics](https://github.com/jrouwe/JoltPhysics) — a modern, high-performance physics engine. It supports rigid bodies, 15 collision shapes, 12 constraint types, character controllers, vehicles, ragdolls, cloth simulation, and multithreaded job dispatch.

### Can I tune physics settings at runtime?

Yes. Use console commands:

```
physics_gravity 0 -9.81 0     # Set gravity
physics_timestep 0.01667      # Set fixed timestep
physics_debug on              # Toggle debug visualization
physics_metrics               # Show performance stats
```

Or edit `[Physics]` in `settings.ini`. See [Configuration Reference](../advanced/Configuration-Reference.md).

---

## Audio

### What audio backend does SparkEngine use?

The active backend factory selects XAudio2 on Windows, OpenAL on non-Windows
development paths, and then Null when neither backend is available. Miniaudio may
be linked elsewhere but is not the factory's cross-platform fallback.

---

## Modding & Content

### Does SparkEngine support mods?

An experimental [Mod System](../subsystems/Mod-System.md) handles discovery, load order, dependency checks, and enable/disable toggling. The editor includes a Modding Panel, but mod delivery is outside `stable-v1`.

### What asset extensions does the pipeline recognize?

| Type | Formats |
|------|---------|
| 3D Models | FBX, glTF/GLB, OBJ |
| Textures | PNG, JPG, TGA, DDS, HDR |
| Audio ingestion | WAV, OGG, MP3, FLAC are accepted/copied by `AudioProcessor`; it does not transcode them |
| Runtime audio playback | WAV is the implemented XAudio2/OpenAL loader format |
| Scenes | `.scene` (JSON-based) |

See [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) and [Asset Format Specifications](../specifications/Asset-Format-Specifications.md).

---

## Troubleshooting

### The engine crashes on startup

1. Check that the selected development backend and host context are available (D3D11 on Windows; the Linux OpenGL path requests 4.5)
2. Update GPU drivers
3. Check `spark.log` for error messages
4. Try a Debug build for better error output
5. Try `-headless` to rule out graphics issues

See [Troubleshooting](../advanced/Troubleshooting.md) for a comprehensive list of known issues and fixes.

### CI is failing on my PR

Check which job failed:

| Job | What it checks |
|-----|----------------|
| `check-format` | clang-format compliance |
| `build-linux-gcc` | GCC compilation + tests |
| `build-linux-clang` | Clang compilation + tests |
| `build-linux-asan` | Memory safety (ASan + UBSan) |
| `build-windows-vs2022` | MSVC compilation + tests |

The VS2026 and macOS jobs are job-level advisory. MinGW is a manual
`workflow_dispatch` development lane rather than a required push gate. The
`clang-tidy` job is a dependency of `required-ci-gate` (even though individual
diagnostics may be advisory), so its job/configuration outcome is blocking. See
[Contributing](../advanced/Contributing.md) for the full CI overview.

### Where do I get help?

- [Troubleshooting](../advanced/Troubleshooting.md) — Common issues and fixes
- [GitHub Issues](https://github.com/Krilliac/SparkEngine/issues) — Bug reports and feature requests
- Engine console: type `help` to list all commands, or `help <command>` for details

---

## See Also

- [Getting Started](Getting-Started.md) — Build and run the engine
- [Making Your First Game](Making-Your-First-Game.md) — Step-by-step game tutorial
- [Quick-Start Tutorial](Quick-Start-Tutorial.md) — Your first 10 minutes
- [Configuration Reference](../advanced/Configuration-Reference.md) — All settings and commands
- [Editor Walkthrough](Editor-Walkthrough.md) — Practical editor guide
- [Performance Tips](../advanced/Performance-Tips.md) — Optimization guide
