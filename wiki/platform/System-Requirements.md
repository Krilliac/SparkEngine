# System Requirements

> **Audience:** Players, Producers, Engineers planning deployment targets
>
> **Thread Context:** N/A (informational)
>
> **Platform/Backend Scope:** Implementation/build-path inventory for Windows,
> Linux, macOS, and the RHI backends; release support is limited below.

> **Release boundary:** This page records build requirements and source
> implementation paths; it is not a support matrix. The only declared profile is
> `stable-v1`; its host/backend boundary is Windows 11 x64, MSVC v143, D3D11, and
> Windows NullRHI, with the full required product set enumerated in readiness. It is
> blocked and uncertified. Windows 10, Linux, macOS, other compiler lines, and
> other graphics backends are development or experimental paths outside it.

## Overview

SparkEngine contains build and runtime paths for Windows, Linux, and macOS, with
per-platform differences in backend and feature wiring. This page distinguishes
source-enforced configuration limits from unverified planning estimates. Only
values with a source reference are treated as current implementation facts;
none of the hardware guidance is `stable-v1` certification.

## Build and Development Matrix

### Windows

| Aspect | Minimum | Recommended |
|---|---|---|
| **Release candidate OS** | Windows 11 x64 (`stable-v1` target; blocked/uncertified) | Windows 11 x64 |
| **Development floor** | Windows 10 x64 (outside the release profile) | Windows 10/11 SDK development path |
| **CPU Architecture** | x86-64 | x86-64 with AVX2 |
| **CPU Baseline** | x64 baseline | AVX2 only when `SPARK_NATIVE_ARCH=ON` (default OFF; distributed builds are not forced to AVX2) |
| **GPU (Primary)** | D3D11 Feature Level 10.0 | D3D11 FL 11.1 |
| **GPU (Optional)** | D3D12 FL 12.0 (Win10+) | D3D12 FL 12.0 with DXR Tier 1.1 |
| **Compiler** | MSVC 19.36+ (VS 2022 17.6+, v143 toolset) | MSVC v143 / v145 |
| **Audio** | XAudio2 (Win32 runtime) | XAudio2 |
| **CMake** | 3.25+ | 3.25+ |
| **C++ Standard** | C++23 | C++23 |

Primary development and CI target. D3D11 is the primary implementation path,
not a shipping-certified backend; D3D12 + DXR are experimental. MinGW + Wine is
a development cross-compile path outside `stable-v1` (see
[Cross-Compilation — Wine Testing](Cross-Compilation-Wine-Testing.md)).

### Linux

| Aspect | Minimum | Recommended |
|---|---|---|
| **OS Version** | Any distro with glibc 2.35+ | Ubuntu 24.04 LTS (CI standard) |
| **CPU Architecture** | x86-64 only (no ARM64 Linux support today) | x86-64 with SSE4.2 |
| **Graphics implementation paths** | Vulkan 1.3; the OpenGL RHI bootstrap requests 4.5, while SDL runtime/editor hosts request 3.3 | Vulkan 1.3 development path |
| **Software graphics route** | Mesa llvmpipe only when explicitly selected and configured; it is not a GPU requirement or automatic fallback | Native GPU for development |
| **Compiler** | GCC 13+ *or* Clang 17+ | GCC 14 |
| **Audio** | OpenAL Soft | OpenAL Soft |
| **Windowing** | SDL2 + X11 | SDL2 + X11/Wayland |
| **CMake** | 3.25+ | 3.25+ |
| **C++ Standard** | C++23 | C++23 |

`SPARK_HEADLESS_SUPPORT` adds a compile definition used by host code; the runtime
`-headless` path currently initializes no graphics/RHI device. `NullRHIDevice` is a separate factory path;
wiring it into packaged headless hosts remains `HEAD-220` work. Linux headless
execution is outside `stable-v1` and uncertified. Vulkan falls back to 1.3 from
1.4 automatically at runtime.

### macOS

| Aspect | Minimum | Recommended |
|---|---|---|
| **OS Version** | macOS 11 Big Sur (`CMAKE_OSX_DEPLOYMENT_TARGET=11.0`) | macOS 12+ Monterey |
| **CPU Architecture** | x86-64 or ARM64 | Apple Silicon (M1+) |
| **CPU Baseline** | ARM NEON *or* x64 SSE4.2 | Apple Silicon M-series |
| **GPU path** | Runtime device capability checks; no certified Metal release profile | Runtime device capability checks; no certified Metal release profile |
| **Alternate path** | OpenGL implementation path (deprecated by Apple) | Metal implementation path |
| **Vulkan compatibility layer (optional)** | MoltenVK translates Vulkan calls to Metal; it is not a GPU | MoltenVK development path |
| **Hardware Ray Tracing** | Not assumed from CPU or OS version | Runtime device must report `supportsRaytracing`; no Metal release certification |
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
Silicon (via OpenGL, for example). They're orthogonal. SparkEngine's source
contains x64 and ARM64 Mac paths; both are experimental and outside
`stable-v1`. Apple Silicon is the preferred development path because the Metal
driver and GPU are co-designed.

## Runtime Hardware Footprint

These numbers describe what the *engine itself* consumes, independent of
game content. Assets, entities, and game logic pile on top.

### CPU

- **Worker threads:** `std::hardware_concurrency() - 1`
  (`Utils/JobSystem.h:75`). So a 4-core box gets 3 workers, an 8-core box
  gets 7.
- **Physics threads:** Jolt uses the same formula, dynamically sized pool
  (`Physics/PhysicsSystem.cpp:385`).
- **No hard cap** on frame work — scales with content.

Core-count guidance below is an unverified planning estimate. `PERF-100` remains
open; no same-commit benchmark artifact establishes a release minimum.

### RAM

Default fixed budgets (all configurable in `EngineSettings::Memory`):

| Pool | Default | Source |
|---|---|---|
| Physics temp allocator (Jolt) | 16 MB | `Physics/PhysicsSystem.cpp:381` |
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

No current same-commit evidence artifact establishes process RSS or game-memory
baselines. The older estimates were removed pending `PERF-100` measurement and
provenance.

### VRAM

Default render targets scale with resolution:

| Target | Default |
|---|---|
| Shadow maps | 2048×2048 (`GraphicsEngineTypes.h:139`) |
| Volumetric fog froxels | 160×90 at 1080p |
| Light-cluster tiles | 16×16 pixels per tile |

Ray-tracing quality-tier settings (`HybridRTTypes.h:100`):

| Tier | Resolution scale | Maximum trace steps |
|---|---|---|
| Low | 0.25× | 32 |
| Medium | 0.5× | 64 |
| High | 1.0× | 96 |
| Ultra | 1.0× | 128 |

No current same-commit evidence artifact establishes total VRAM envelopes.
Content, driver, backend, and resolution make those values workload-specific;
`PERF-100` must provide measured budgets before release guidance is claimed.

The 512 MB texture value is a soft eviction target: `TextureEviction` selects
over-budget textures for eviction and then runs collection. It is not a certified
total-VRAM envelope.

### Editor Overhead

`SparkEditor` runs the full engine plus ImGui, an inventory of 65 `*Panel.h`
classes, the asset database, and collaborative-edit sessions. Registration and
default visibility are separate from that source-file count. No current evidence
artifact establishes a certified editor RAM/VRAM overhead.

## Illustrative Target Hardware

These figures are uncited planning placeholders, not measured requirements or
certified `stable-v1` guidance. Validate them against the actual game and hardware.

For planning hypothetical game targets before release certification:

| Target | CPU | RAM | GPU | VRAM |
|---|---|---|---|---|
| **Minimum** | 2-core x64 (2015+) | 4 GB | GT 1030 / iGPU | 2 GB |
| **Recommended** | 4-core x64 (2018+) | 8 GB | GTX 1660 / RX 580 / Apple M1 | 4 GB |
| **High-End (RT on)** | 6-core x64 | 16 GB | Example discrete RT-class GPU; runtime capability checks still apply | 8 GB |
| **Ultra (4K + RT)** | 8-core x64 | 32 GB | RTX 4070 / RX 7800 / Apple M3 Max | 12 GB+ |

For CI / build machines:

| Role | Spec |
|---|---|
| Linux CI runner | `ubuntu-24.04` (GitHub-hosted) |
| Windows CI runner | GitHub-hosted `windows-2022` with VS 2022; not Windows 11 host certification |
| macOS CI runner | GitHub-hosted `macos-latest`; architecture is not pinned by this repository label |

CI lanes use configured compiler caches for incremental build speed. Historical
wall-clock figures are planning data, not `stable-v1` release evidence.

## Build Toggles That Move the Needle

These CMake options differ in maturity. The table calls out the currently inert
graphics toggle rather than presenting it as a working build reduction:

| Option | Default | Effect when OFF |
|---|---|---|
| `ENABLE_GRAPHICS` | ON | Currently inert: no target/source condition consumes it, so OFF does not strip the RHI (`HEAD-220`) |
| `ENABLE_NETWORKING` | ON | Omits `ENABLE_NETWORKING` and networking libraries from `SparkEngineLib`; standalone service targets are controlled separately by `ENABLE_SERVER_PROCESSES` |
| `ENABLE_VULKAN` | ON | Disables Vulkan discovery and omits `SPARK_VULKAN_SUPPORT`; root CMake does not separately filter Vulkan source files |
| `ENABLE_METAL` | auto-ON on APPLE | Smaller binary on macOS if only OpenGL is wanted |
| `ENABLE_DXR` | ON | Skips DXR shader compilation and, with hybrid RT enabled, omits `SPARK_HARDWARE_RT` |
| `ENABLE_HYBRID_RT` | ON | Omits `SPARK_HYBRID_RT` and its hardware-RT definition setup |
| `ENABLE_RECAST` | ON | Drops the Recast/Detour navigation integration where the option is consumed |
| `SPARK_DOUBLE_PRECISION_PHYSICS` | OFF | Lower precision, less memory for huge worlds |
| `BUILD_TESTS` | ON | Removes SparkTests from the build entirely |
| `BUILD_GAME_MODULES` | ON | Omits in-tree game-module targets; editor, tools, and other enabled products remain |

See [CLAUDE.md](../../CLAUDE.md) Build section for the full list.

## Troubleshooting

- **"Engine won't start on macOS"** — macOS/Metal is experimental and outside
  `stable-v1`. Check the selected backend and device-capability logs; do not infer
  ray-tracing support from CPU architecture alone.
- **"Low-spec Linux box crashes at startup"** — Try
  `-DSPARK_HEADLESS_SUPPORT=ON` at configure time and launch with `-headless`.
  `SPARK_HEADLESS_SUPPORT` adds its compile definition; host code implements the
  entry-point behavior and `-headless` selects the runtime path. Mesa llvmpipe
  is a separate explicitly configured OpenGL development route.
- **"Windows build refuses to run on Win7/8"** — those hosts are below the
  documented Windows 10 development floor and outside `stable-v1`; no compatibility
  claim or diagnosis is made for them.
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
