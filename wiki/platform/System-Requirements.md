# System Requirements

> **Audience:** Players, Producers, Engineers planning deployment targets
>
> **Thread Context:** N/A (informational)
>
> **Platform/Backend Scope:** Windows, Linux, macOS — all RHI backends

## Overview

SparkEngine runs on Windows, Linux, and macOS, with per-platform differences
in primary graphics backend and supported features. This page pins what the
code actually enforces today (CMake checks, compiler minimums, `#ifdef` gates)
and how much hardware the engine itself consumes at runtime — so you can plan
deployment targets, CI hardware, and recommended specs for end users.

All numbers below are pulled from the source tree, not marketing targets.
Where a limit is configurable (streaming budgets, quality tiers), the default
is shown and the relevant `EngineSettings` / setter is called out.

## Platform Support Matrix

### Windows

| Aspect | Minimum | Recommended |
|---|---|---|
| **OS Version** | Windows 10 (x64) | Windows 11 (x64) |
| **CPU Architecture** | x86-64 | x86-64 with AVX2 |
| **CPU Baseline** | x64 baseline | AVX2 (`/arch:AVX2` forced in Release, `CMakeLists.txt:403`) |
| **GPU (Primary)** | D3D11 Feature Level 10.0 | D3D11 FL 11.1 |
| **GPU (Optional)** | D3D12 FL 12.0 (Win10+) | D3D12 FL 12.0 with DXR Tier 1.1 |
| **Compiler** | MSVC 19.36+ (VS 2022 17.6+, v143 toolset) | MSVC v143 / v145 |
| **Audio** | XAudio2 (Win32 runtime) | XAudio2 |
| **CMake** | 3.25+ | 3.25+ |
| **C++ Standard** | C++23 | C++23 |

Primary development and CI target. Everything else is a port. D3D11 is the
shipping backend; D3D12 + DXR are experimental. MinGW + Wine is a supported
cross-compile path for Linux contributors (see
[Cross-Compilation — Wine Testing](Cross-Compilation-Wine-Testing.md)).

### Linux

| Aspect | Minimum | Recommended |
|---|---|---|
| **OS Version** | Any distro with glibc 2.35+ | Ubuntu 24.04 LTS (CI standard) |
| **CPU Architecture** | x86-64 only (no ARM64 Linux support today) | x86-64 with SSE4.2 |
| **GPU (Primary)** | OpenGL 4.6 Core *or* Vulkan 1.3 | Vulkan 1.3 |
| **GPU (Fallback)** | Mesa llvmpipe (software OpenGL) | Native discrete GPU |
| **Compiler** | GCC 13+ *or* Clang 17+ | GCC 14 |
| **Audio** | OpenAL Soft | OpenAL Soft |
| **Windowing** | SDL2 + X11 | SDL2 + X11/Wayland |
| **CMake** | 3.25+ | 3.25+ |
| **C++ Standard** | C++23 | C++23 |

`SPARK_HEADLESS_SUPPORT` builds allow running without a display at all
(NullRHIDevice). Vulkan falls back to 1.3 from 1.4 automatically at runtime.

### macOS

| Aspect | Minimum | Recommended |
|---|---|---|
| **OS Version** | macOS 11 Big Sur (`CMAKE_OSX_DEPLOYMENT_TARGET=11.0`) | macOS 12+ Monterey |
| **CPU Architecture** | x86-64 or ARM64 | Apple Silicon (M1+) |
| **CPU Baseline** | ARM NEON *or* x64 SSE4.2 | Apple Silicon M-series |
| **GPU (Primary)** | Metal 2.3 (macOS 11) | Metal 3 (macOS 12+) |
| **GPU (Fallback)** | OpenGL 4.1 (deprecated on macOS 10.15+) | Metal (via the MetalDevice backend) |
| **GPU (Optional)** | — | MoltenVK for Vulkan compatibility |
| **Hardware Ray Tracing** | *Not supported* on M1 or macOS 11 | Apple Silicon M2+ with `supportsRaytracing`, macOS 12+ |
| **Compiler** | Apple Clang (Xcode 15+) | Apple Clang (Xcode 16) |
| **Audio** | OpenAL Soft (Homebrew) | OpenAL Soft |
| **Windowing** | SDL2 | SDL2 |
| **CMake** | 3.25+ | 3.25+ |

Hardware RT is gated with `[device supportsRaytracing]` *and*
`@available(macOS 12.0, *)` in `MetalRayTracing.mm` — older targets fall
back to the SDFGI software path automatically.

## Apple Silicon vs Metal

These are two different things that often get conflated:

- **Apple Silicon** is the CPU/SoC *architecture* — the M-series chips
  (M1/M2/M3/M4) introduced in 2020. ARM64-based with unified memory
  shared between CPU, GPU, and Neural Engine. Replaced Intel x86 Macs.
- **Metal** is Apple's *graphics API* — the analogue of DirectX or
  Vulkan. Shipped in 2014 (iOS 8, OS X 10.11), long before Apple Silicon.
  Runs on both Intel Macs and Apple Silicon Macs.

You can run Metal on an Intel Mac. You can run non-Metal code on Apple
Silicon (via OpenGL, for example). They're orthogonal. SparkEngine
supports both x64 and ARM64 Macs; Apple Silicon is the recommended path
because the Metal driver and GPU are co-designed.

## Runtime Hardware Footprint

These numbers describe what the *engine itself* consumes, independent of
game content. Assets, entities, and game logic pile on top.

### CPU

- **Worker threads:** `std::hardware_concurrency() - 1`
  (`Utils/JobSystem.h:75`). So a 4-core box gets 3 workers, an 8-core box
  gets 7.
- **Physics threads:** Jolt uses the same formula, dynamically sized pool
  (`Physics/PhysicsSystem.cpp:362`).
- **Main-thread frame cost** with 25+ subsystems live and no game load:
  avg 8.7 µs, p99 62.5 µs (load-test baseline, 3000 frames).
- **No hard cap** on frame work — scales with content.

Minimum: 2 cores. Recommended: 4–8. High-end: 8+ helps the physics and
job systems spread.

### RAM

Default fixed budgets (all configurable in `EngineSettings::Memory`):

| Pool | Default | Source |
|---|---|---|
| Physics temp allocator (Jolt) | 16 MB | `Physics/PhysicsSystem.cpp:358` |
| Texture streaming | 512 MB | `Graphics/TextureSystem.h:379` |
| Mesh streaming | 256 MB | `Core/EngineSettings.h` |
| Audio streaming | 128 MB | `Core/EngineSettings.h` |
| Shader cache | 64 MB | `Core/EngineSettings.h` |
| Constant-buffer ring (per frame) | 2 MB | `Graphics/ConstantBufferRing.h:15` |
| Transient vertex / index (per frame) | 4 / 2 MB | `Graphics/RHI/TransientBufferAllocator.h:67` |

Content-side caps:

| Category | Cap |
|---|---|
| SDF primitives (software RT) | 2,048 (`SDFSceneManager.h:42`) |
| Particles (default) | 10,000 |
| Decals (default) | 256 |
| Loaded streaming areas | 4 |
| ECS entities | No hard cap — EnTT sparse set |

Measured baselines:

| Scenario | RAM |
|---|---|
| Empty engine with 25 subsystems | ~35 MB RSS |
| 10,000 entity stress test | ~37 MB RSS (no growth over 3000 frames) |
| Shipping game, small scope | 50–100 MB |
| Shipping game, 1080p assets | 256–512 MB |
| Shipping game, 1440p+ rich content | 1–2 GB |

### VRAM

Default render targets scale with resolution:

| Target | Default |
|---|---|
| Shadow maps | 2048×2048 (`GraphicsEngineTypes.h:139`) |
| Volumetric fog froxels | 160×90 at 1080p |
| Light-cluster tiles | 16×16 pixels per tile |

Ray-tracing quality tiers (`HybridRTTypes.h:100`):

| Tier | Resolution scale | Samples/pixel | Approx VRAM at 1080p |
|---|---|---|---|
| Low | 0.25× | 32 | ~12 MB |
| Medium | 0.5× | 64 | ~48 MB |
| High | 1.0× | 96 | ~200 MB |

Total VRAM envelope (engine + render targets, excluding game assets):

| Scenario | VRAM |
|---|---|
| 1080p, Low RT quality | ~100 MB |
| 1080p, Medium RT | 300–500 MB |
| 1440p, High RT | 600–900 MB |
| 4K, Ultra RT | 1.5–2.5 GB |

On top of that, textures and meshes pile on as they stream in — the
512 MB texture budget is advisory, not enforced.

### Editor Overhead

`SparkEditor` runs the full engine plus ImGui, 59 editor panels, asset
database, and collaborative-edit sessions. Expect **+300–500 MB RAM**
over the equivalent shipping build, same VRAM footprint plus the editor
viewport render targets.

## Recommended Target Hardware

For end users who will run a shipping game built on SparkEngine:

| Target | CPU | RAM | GPU | VRAM |
|---|---|---|---|---|
| **Minimum** | 2-core x64 (2015+) | 4 GB | GT 1030 / iGPU | 2 GB |
| **Recommended** | 4-core x64 (2018+) | 8 GB | GTX 1660 / RX 580 / Apple M1 | 4 GB |
| **High-End (RT on)** | 6-core x64 | 16 GB | RTX 3060 / RX 6700 / Apple M2+ | 8 GB |
| **Ultra (4K + RT)** | 8-core x64 | 32 GB | RTX 4070 / RX 7800 / Apple M3 Max | 12 GB+ |

For CI / build machines:

| Role | Spec |
|---|---|
| Linux CI runner | `ubuntu-24.04` (GitHub-hosted) |
| Windows CI runner | `windows-latest` with VS 2022 |
| macOS CI runner | `macos-latest` (Apple Silicon M-series) |

All three CI jobs use ccache for incremental build speed. Full
build + test wall-clock on a GitHub-hosted runner: ~8–12 minutes.

## Build Toggles That Move the Needle

These CMake toggles materially change the runtime footprint:

| Option | Default | Effect when OFF |
|---|---|---|
| `ENABLE_GRAPHICS` | ON | Strips the RHI entirely — headless server builds |
| `ENABLE_NETWORKING` | ON | Drops AreaServer/WorldServer — single-player builds |
| `ENABLE_VULKAN` | OFF (Win) / ON (Linux) | Smaller binary; loses cross-platform RHI |
| `ENABLE_METAL` | auto-ON on APPLE | Smaller binary on macOS if only OpenGL is wanted |
| `ENABLE_DXR` | OFF | Drops the DirectX ray-tracing pipeline |
| `ENABLE_HYBRID_RT` | ON | Drops SDFGI + hardware-RT fallback coordinator |
| `ENABLE_RECAST` | OFF | Smaller AI footprint if no nav-mesh is needed |
| `SPARK_DOUBLE_PRECISION_PHYSICS` | OFF | Lower precision, less memory for huge worlds |
| `BUILD_TESTS` | ON | Removes SparkTests from the build entirely |
| `BUILD_GAME_MODULES` | ON | Build just the engine exe, no game DLLs |

See [CLAUDE.md](../../CLAUDE.md) Build section for the full list.

## Troubleshooting

- **"Engine won't start on my Intel Mac"** — Metal backend requires a
  Metal-capable GPU; all Macs from ~2012+ qualify. Check logs for
  `[device supportsRaytracing]` — RT is M2+ only, but the non-RT path
  runs on everything.
- **"Low-spec Linux box crashes at startup"** — Try
  `SPARK_HEADLESS_SUPPORT=ON` (or omit graphics entirely) to bypass GPU
  init. Mesa llvmpipe gives a software fallback if a GPU is present.
- **"Windows build refuses to run on Win7/8"** — Minimum is Win10.
  Earlier versions lack the required D3D11 feature level and C++23
  runtime dependencies.
- **"Out-of-memory under heavy RT"** — Drop
  `HybridRTManager::SetQuality(RayTracingQuality::Low)` or reduce the
  streaming budgets in `EngineSettings::Memory`.

## Related Pages

- [Platform Input](Platform-Input.md) — per-platform input backends
- [Cross-Compilation — Wine Testing](Cross-Compilation-Wine-Testing.md) —
  MinGW + Wine path
- [Mobile Platform](Mobile-Platform.md) — mobile port notes
- [VR Support](VR-Support.md) — VR-specific hardware
- [RHI Abstraction Layer](../graphics/RHI-Abstraction-Layer.md) — backend internals
- [Codebase Statistics](../advanced/Codebase-Statistics.md) — size metrics
