# Engine Viability Evaluation

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** All platforms; primary focus Windows/D3D11, with cross-platform notes

## Overview

Can you actually make and ship a game with SparkEngine? This page is an end-to-end
viability assessment — what is real and traced through actual code, what is missing or
incomplete, and which game types are realistic targets today.

**Executive verdict: YES, with caveats.** SparkEngine is a legitimate, functional game
engine — not a collection of stubs or architectural skeletons. Someone with C++
experience could build and ship an indie/AA game with it today, particularly an FPS on
Windows. Real gaps remain that would complicate certain game types and platforms, but
several gaps flagged in the original evaluation have since closed (see Source &
Freshness).

## What's Real (Traced End-to-End)

### 1. Core Engine Loop — Fully Wired

Complete startup-to-shutdown lifecycle, traced through actual code:

```
wWinMain() / main()
  -> InitInstance()            -- Win32 window, timer, settings
  -> InitEngineContext()       -- Service locator, physics, job system, ECS
  -> InitGameplaySubsystems()  -- Weather, UI, dialogue, modding
  -> LoadAndInitModules()      -- DLL game modules loaded & initialized
  -> RunWindowedMainLoop()     -- Real message pump, per-frame:
       Input.Update()
       ModuleManager.UpdateAll(dt) / RenderAll()
       UpdateGameplaySystems(dt)   -- 40+ subsystems ticked
       UpdateDebugSystems(dt)
       ConsoleProcessManager.ProcessCommands()
  -> ShutdownEngine()
```

The main loop is real: delta-smoothed, fixed-timestep accumulator, guarded updates
with fault isolation, and test frame limits for CI.

### 2. Rendering Pipeline — Functional (D3D11 primary)

| Feature | Status | Notes |
|---------|--------|-------|
| D3D11 device/swapchain | Working | Real `CreateDeviceAndSwapChain()` |
| Forward / Deferred / Forward+ | Working | G-Buffer MRTs, tile-based light culling |
| Mesh loading (OBJ, glTF) | Working | tinyobjloader + cgltf, real `DrawIndexed()` |
| Procedural primitives | Working | Cube, sphere, plane, pyramid with normals |
| Shader compilation | Working | `D3DCompile` + offline compiler tool |
| Camera system | Working | View/projection feed the pipeline |
| Post-processing | Working | 14 effects (bloom, FXAA, DOF, tonemapping…) |
| Material system (PBR) | Working | Initialized in GraphicsEngine |
| Lighting + shadows | Working | Directional / point / spot |
| Particle system | Working | GPU-friendly emitters with collision |
| 2D sprites | Working | Batched rendering with blend modes |
| Render graph | Working | Topological sort, resource aliasing, 150+ tests |
| NullRHIDevice | Working | Headless CI without GPU |

**Backend reality (updated 2026-06-08):**
- D3D11 is the primary, fully functional backend.
- D3D12 and Vulkan are **no longer thin stubs** — `D3D12Device.cpp` (~78 KB) and
  `VulkanDevice.cpp` (~104 KB), each with command lists, are substantial backends.
  They are still experimental relative to D3D11.
- OpenGL is real; Metal is implemented (`MetalDevice.mm`, see Mac Compatibility page).
- Some advanced rendering systems still ship the GPU-binding layer ahead of final
  HLSL/GLSL shaders.
- DXR ray tracing is extensive but disabled by default.

### 3. Physics (Jolt) — Fully Functional

All shape types; dynamic/kinematic/static bodies; 16-layer collision filtering;
contact callbacks, raycasts, sweep tests; lag compensation for networking; ECS
integration via RigidBodyComponent/ColliderComponent.

### 4. Audio — Functional

XAudio2 is fully functional on Windows (32-source pool, 3 mixer buses, 3D spatial audio
with Doppler). Cross-platform audio now goes through an `IAudioBackend` abstraction with
XAudio2 / OpenAL / Null backends and a factory (Linux/macOS audio path).

### 5. Input — Fully Functional

Keyboard, mouse (raw input), gamepad with key binding/remapping; thread-safe;
Windows + Linux/macOS (SDL2) branches.

### 6. AI — Professional Grade

Behavior trees, NavMesh + Recast/Detour, perception, formation, cover system,
frame-budget limiter, collision avoidance.

### 7. Animation — Comprehensive

Skeletal animation, blend spaces, state machines, IK solvers (TwoBone, LookAt,
FABRIK), ragdoll integration, compression, retargeting.

### 8. Networking — Enterprise Grade

UDP transport, reliable messaging, entity replication with delta snapshots, client
prediction + server reconciliation, lag compensation, AES-256 encryption, instability
simulator, AreaServer/WorldServer architecture.

### 9. Game Module System — Working

DLL plugin architecture (`IGameModule`), hot-reload via `ModuleHotReloadManager`.
SparkGame and genre modules (FPS, RPG, MMO, ARPG, RTS, Racing, Platformer, OpenWorld,
VisualScript) demonstrate the interface.

### 10. Supporting Systems — Wired via Gameplay Lifecycle

Save system, UI, dialogue, ability system (WoW-style), scripting (conditional on SDK),
weather, destruction, modding (VFS + SparkPak), coroutines, events. Engine-level
**Inventory** and **Quest** systems now exist (`InventorySystem`, `QuestSystem`).

### 11. Editor — Extensive

Many panels, collaborative multi-user editing.

### 12. Tests — Large Suite

~6,000 unit tests across ~480 files, all green on Linux.

## What's Missing or Incomplete

### Gaps (updated)

| Gap | Impact | Severity | Status 2026-06-08 |
|-----|--------|----------|-------------------|
| D3D11 is the most complete backend | Best-supported on Windows | MEDIUM | D3D12/Vulkan now substantial, OpenGL/Metal real |
| Advanced rendering shaders | Some systems have binding but await shaders | MEDIUM | Open |
| AngelScript needs external SDK | Scripting stubs without it | MEDIUM | Open |
| ~~No terrain system~~ | ~~Can't build open-world~~ | ~~HIGH~~ | **Resolved** — `TerrainRenderer`, `ClipmapTerrain`, TerrainSystem/Editor exist |
| ~~Streaming deleted~~ | ~~Open-world blocked~~ | ~~HIGH~~ | **Resolved** — `Engine/Streaming/SeamlessAreaManager`, AreaAssetLoader, DirectStorageLoader |
| ~~Audio is XAudio2 only~~ | ~~No Linux/macOS audio~~ | ~~MEDIUM~~ | **Resolved** — IAudioBackend + OpenAL/Null |
| ~~SparkGame has no AI enemies~~ | — | — | **Resolved** — enemies, types, wave spawner |

### Platform Reality

| Platform | Status |
|----------|--------|
| Windows (MSVC) | **Primary, functional** |
| Linux (GCC/Clang) | Builds, headless works, audio via OpenAL, SDL2 windowing |
| macOS | Buildable end-to-end; Metal backend implemented; CI `continue-on-error` |
| Consoles | Not supported |

## Game Type Viability

| Game Type | Viability | Why |
|-----------|-----------|-----|
| **FPS (arena/corridor)** | HIGH | SparkGameFPS demonstrates it; AI enemies + wave spawner exist |
| **Third-person action** | MEDIUM-HIGH | Physics/animation/AI work; need camera mode |
| **RPG** | MEDIUM-HIGH | Ability/dialogue/save plus engine-level inventory & quests now exist |
| **MMO** | MEDIUM | Enterprise networking; large content pipeline needed |
| **Open-world** | MEDIUM | Terrain + seamless streaming now present; origin rebasing exists; large-scale validation still needed |
| **2D game** | MEDIUM | Sprite batching works; 2D workflow still developing |
| **Mobile** | LOW | Header exists, no real mobile backend |
| **VR** | LOW-MEDIUM | VR system registered, not hardware-validated |

## Bottom Line

SparkEngine is a real engine. The core loop runs, draw calls hit the GPU, physics
simulates, AI pathfinds, networking replicates entities, and ~6,000 tests validate it.
The game modules prove a game can load, run, and process logic through the module
interface.

**To ship a game today:** Windows is the smoothest target; C++ proficiency and
willingness to build against `IGameModule` are required; some missing features still
need custom work. Comparable to early-access indie engines (Flax, Stride) in system
breadth, behind in tooling/platform polish/artist workflows. The networking stack is
notably more sophisticated than most indie engines.

## Source & Freshness

- **Original entry date:** 2026-04-01 (`.claude/knowledge/engine-viability-evaluation.md`)
- **Verified against codebase 2026-06-08.**

Updates / status changes since the original:

- **Terrain** — now exists (`TerrainRenderer`, `ClipmapTerrain`). The original listed
  "No terrain system" as a HIGH gap; resolved.
- **Streaming** — `Engine/Streaming/` exists (SeamlessAreaManager, AreaAssetLoader,
  DirectStorageLoader, SceneManifest). Original said "streaming deleted"; resolved.
- **D3D12 / Vulkan backends** — now substantial (`D3D12Device.cpp` ~78 KB,
  `VulkanDevice.cpp` ~104 KB). Original called them stubs; downgraded the severity.
- **Metal** — implemented (`MetalDevice.mm`). Original listed Metal among stubs.
- **Engine-level inventory/quest** — `InventorySystem` and `QuestSystem` now exist.
  Original listed these as a remaining gap (game-module level only).
- **Cross-platform audio** — IAudioBackend / OpenAL / Null delivered; XAudio2-only
  limitation resolved.
- **Test count** — grown to ~6,000 tests across ~480 files (original cited 2,577 /
  211 files).

## Related Pages

- [Mac Compatibility Analysis](Mac-Compatibility-Analysis.md)
- [Third-Party Library Evaluation](Third-Party-Library-Evaluation.md)
- [Engine Feature Recommendations](Engine-Feature-Recommendations.md)
- [Project Recommendations](Project-Recommendations.md)
