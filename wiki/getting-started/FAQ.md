# Frequently Asked Questions

Common questions about SparkEngine — what it is, who it's for, and how to get started.

---

## General

### What is SparkEngine?

SparkEngine is a free, open-source 3D game engine written in C++23. It started as an FPS engine and is evolving into a general-purpose engine supporting FPS, RPG, MMO, open-world, racing, platformer, and RTS genres. It ships with DirectX 11 rendering, Jolt Physics, XAudio2 spatial audio, AngelScript scripting, an EnTT ECS, and an ImGui-based visual editor.

### Is SparkEngine free?

Yes. SparkEngine is licensed under the [Spark Open License](https://github.com/Krilliac/SparkEngine/blob/master/LICENSE) — no royalties, no fees, fully free for commercial use. The license includes anti-plagiarism protection to prevent wholesale copying without attribution.

### What platforms does SparkEngine support?

| Platform | Status |
|----------|--------|
| Windows 10+ | Primary (fully supported) |
| Linux x64 | Experimental (GCC 13+, Clang 17+) |
| macOS | Experimental (Apple Clang) |

Windows is the primary development platform. Linux and macOS builds run in CI and are usable but may have rough edges.

### What genres can I make with SparkEngine?

SparkEngine ships with example game modules for FPS, platformer, racing, RPG, RTS, and MMO. The ECS architecture is genre-agnostic — you can build any type of game. The engine includes systems for weapons, vehicles, inventory, quests, dialogue, AI, networking, and more.

### How does SparkEngine compare to Unity / Unreal / Godot?

SparkEngine is smaller and earlier in development than those engines. Key differences:

- **Open-source C++23** — You have full source access and can modify anything. No black-box runtime.
- **No editor lock-in** — Game logic lives in C++ modules or AngelScript scripts, not in a proprietary project format.
- **MMO-ready architecture** — Built-in area server architecture, seamless world streaming, and collaborative editing (inspired by HeroEngine).
- **Lightweight** — The full engine compiles in 2–5 minutes. No gigabyte downloads.
- **Trade-off** — Fewer ready-made assets, smaller community, and less polish than mature engines.

### Do I need to know C++ to use SparkEngine?

C++ is the primary language, but you have options:

- **AngelScript** — A C-like scripting language with hot-reload. Write gameplay logic without recompiling the engine.
- **Visual Scripting** — A node-based visual scripting system that compiles to AngelScript. No code required.
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

A clean Release build takes 2–5 minutes on a modern machine with 8+ cores. Incremental builds after small changes take seconds.

### Can I build without a GPU?

Yes. The engine has multiple fallback paths:

| Backend | GPU-less fallback |
|---------|-------------------|
| D3D11 | WARP (Windows software rasterizer) |
| D3D12 | WARP |
| Vulkan | Lavapipe (Mesa) |
| OpenGL | llvmpipe (Mesa) |
| None | NullRHIDevice (headless, no rendering) |

Use `-headless` to run with no graphics at all (for dedicated servers or automated testing).

### How do I run the engine on Linux without a display?

Use Xvfb (virtual framebuffer) with Mesa software rendering:

```bash
sudo apt-get install -y xvfb libgl1-mesa-dri
Xvfb :99 -screen 0 1024x768x24 &
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 ./SparkEngine
```

### Can I build just the engine without the editor?

Yes. Use the `minimal` CMake preset or disable individual features:

```bash
cmake --preset minimal                    # Core engine only
cmake -B build -DENABLE_EDITOR=OFF        # Everything except editor
```

### What are the command-line arguments?

| Argument | Description |
|----------|-------------|
| `-headless` | No graphics or audio (dedicated server mode) |
| `-game <path>` | Load a specific game module DLL/SO |
| `-window-size WxH` | Override window resolution (e.g., `1920x1080`) |
| `-test-frames N` | Run N frames then exit (for benchmarking) |
| `-fullscreen` | Start in fullscreen mode |
| `-console` / `-noconsole` | Force-enable or disable the debug console |
| `-scene <path>` | Load a specific scene on startup |

---

## Editor

### How do I open the editor?

Press **F1** while the engine is running, or build with `ENABLE_EDITOR=ON` (default on Windows). The editor is an ImGui-based overlay with 56 panels.

### What panels does the editor have?

The editor ships with 56 panels covering scene editing, asset management, physics, gameplay, audio, scripting, profiling, and more. The 7 core panels shown by default are:

- **Scene View** — 3D viewport with gizmos
- **Hierarchy** — Scene graph tree
- **Inspector** — Component property editor
- **Asset Browser** — File browser with thumbnails
- **Console** — Command and log panel
- **Game View** — In-game preview
- **Profiler** — Performance metrics

All other panels are available from the **Window** menu. See [Editor Walkthrough](Editor-Walkthrough.md) for a practical guide.

### Can multiple people edit a scene at the same time?

Yes. SparkEngine supports collaborative multi-user editing sessions (inspired by HeroEngine). One user hosts, others join. Entity locks prevent conflicts. See [Collaborative Editing](../subsystems/Collaborative-Editing.md).

---

## Gameplay & Scripting

### How do I create a new game?

Create a **game module** — a DLL/SO that implements the `IModule` interface. The engine auto-discovers modules in the `GameModules/` directory. See [Creating a Game Module](Creating-a-Game-Module.md) for a step-by-step guide, or [Making Your First Game](Making-Your-First-Game.md) for a tutorial.

### What scripting language does SparkEngine use?

[AngelScript](../subsystems/Scripting-with-AngelScript.md) — a statically-typed scripting language with C-like syntax. Scripts hot-reload automatically when you save changes to disk (no recompile needed).

### Does SparkEngine support visual scripting?

Yes. The [Visual Scripting](../subsystems/Visual-Scripting.md) panel provides a node-based editor that compiles to AngelScript under the hood. You can mix visual scripts with hand-written AngelScript.

### How does the ECS work?

SparkEngine uses [EnTT](https://github.com/skypjack/entt) for its Entity Component System. Entities are lightweight IDs, components are plain data structs, and systems operate on component groups. The engine provides 79 built-in component types and 67 systems. See [Entity Component System](../subsystems/Entity-Component-System.md).

### How do I add multiplayer to my game?

The engine includes a UDP client/server networking stack with entity replication, client-side prediction, and lag compensation. Start with the [Multiplayer Quick Start](../subsystems/Multiplayer-Quick-Start.md) guide, then see [Networking](../subsystems/Networking.md) for details.

---

## Graphics

### What rendering backends are available?

| Backend | Status | Platform |
|---------|--------|----------|
| DirectX 11 | Primary | Windows |
| DirectX 12 | Experimental | Windows |
| Vulkan | Experimental | Windows, Linux |
| OpenGL 4.5 | Experimental | Windows, Linux, macOS |
| Metal | Experimental | macOS |
| NullRHI | Headless fallback | All |

All backends implement the same [RHI Abstraction Layer](../graphics/RHI-Abstraction-Layer.md).

### What rendering features are supported?

Forward, deferred, forward+, and clustered rendering pipelines. PBR materials, cascaded shadow maps, SSAO, SSR, volumetric fog, bloom, HDR tonemapping (ACES, Filmic, Reinhard, AgX), TAA/FXAA/MSAA, GPU particles, decals, image-based lighting, mesh shaders, GPU-driven rendering, and dynamic quality scaling.

### Can I use ray tracing?

DXR 1.1 ray tracing is available on the D3D12 backend (Windows, SM6.5+ GPU required). A hybrid ray tracing mode blends rasterization with selective ray-traced effects. See [DXR Raytracing](../graphics/DXR-Raytracing.md) and [Hybrid Ray Tracing](../graphics/Hybrid-Ray-Tracing.md).

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

XAudio2 on Windows with miniaudio as a cross-platform fallback. The audio system supports 3D spatial audio, Doppler effects, distance attenuation, mix buses, DSP effects (reverb, EQ, compressor), and volume channels (master, SFX, music, voice).

---

## Modding & Content

### Does SparkEngine support mods?

Yes. The [Mod System](../subsystems/Mod-System.md) handles mod discovery, load order, dependency checking, and enable/disable toggling. The editor includes a Modding Panel for managing mods.

### What asset formats are supported?

| Type | Formats |
|------|---------|
| 3D Models | FBX, glTF/GLB, OBJ |
| Textures | PNG, JPG, TGA, DDS, HDR |
| Audio | WAV, OGG, MP3, FLAC |
| Scenes | `.scene` (JSON-based) |

See [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) and [Asset Format Specifications](../specifications/Asset-Format-Specifications.md).

---

## Troubleshooting

### The engine crashes on startup

1. Check that your GPU supports DirectX 11 (Windows) or OpenGL 4.5 (Linux)
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

Jobs marked `continue-on-error` (VS2026, MinGW, macOS, clang-tidy) are warnings, not blockers. See [Contributing](../advanced/Contributing.md) for the full CI overview.

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
