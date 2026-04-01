# Engine Viability Evaluation: Can You Make a Game with SparkEngine?

**Last updated:** 2026-04-01
**Type:** Observation
**Status:** Active (gaps being addressed across 4 phases)
**Severity:** N/A (evaluation, not issue)

## Executive Verdict: YES, with caveats

SparkEngine is a **legitimate, functional game engine** -- not a collection of stubs or architectural skeletons. Someone with C++ experience could build and ship an indie/AA game with it today, particularly an FPS on Windows. However, there are real gaps that would block certain game types and platforms.

---

## What's Real (Traced End-to-End)

### 1. Core Engine Loop -- FULLY WIRED

The engine has a complete startup-to-shutdown lifecycle, traced through actual code:

```
wWinMain() / main()
  -> InitInstance()            -- Win32 window, timer, settings
  -> InitEngineContext()       -- Service locator, physics, job system, ECS
  -> InitGameplaySubsystems() -- Weather, UI, dialogue, modding
  -> LoadAndInitModules()      -- DLL game modules loaded & initialized
  -> RunWindowedMainLoop()     -- Real message pump with per-frame:
       Input.Update()
       ModuleManager.UpdateAll(dt) / RenderAll()
       UpdateGameplaySystems(dt)   -- 40+ subsystems ticked
       UpdateDebugSystems(dt)
       ConsoleProcessManager.ProcessCommands()
  -> ShutdownEngine()
```

**Key evidence:** `SparkEngine.cpp:710-796` -- the main loop is real, delta-smoothed, with fixed-timestep accumulator, guarded updates with fault isolation, and test frame limits for CI.

### 2. Rendering Pipeline -- FUNCTIONAL (D3D11 primary)

| Feature | Status | Evidence |
|---------|--------|----------|
| D3D11 device/swapchain | Working | `GraphicsEngine.cpp` -- real CreateDeviceAndSwapChain() |
| Forward rendering | Working | `GraphicsRenderPipelines.cpp:44` |
| Deferred rendering | Working | G-Buffer with 4 MRTs, lighting pass |
| Forward+ | Working | Tile-based light culling |
| Mesh loading (OBJ, glTF) | Working | `Mesh.cpp` -- tinyobjloader + cgltf, real DrawIndexed() calls |
| Procedural primitives | Working | Cube, sphere, plane, pyramid with proper normals |
| Shader compilation | Working | D3DCompile with VS/PS binding, offline compiler tool |
| Camera system | Working | View/projection matrices feed into render pipeline |
| Post-processing | Working | 14 effects (bloom, FXAA, DOF, tonemapping, etc.) |
| Material system (PBR) | Working | MaterialSystem.cpp -- initialized in GraphicsEngine |
| Lighting | Working | Directional/point/spot with shadows |
| Particle system | Working | GPU-friendly emitters with collision |
| 2D sprites | Working | Batched sprite rendering with blend modes |
| Render graph | Working | Topological sort, resource aliasing, 150+ tests |
| NullRHIDevice | Working | Headless CI without GPU |

**Gaps:**
- D3D12/Vulkan/OpenGL/Metal backends are **stubs** -- only D3D11 actually renders
- 12 advanced rendering systems (sky, water, GI, shadow atlas, etc.) have type definitions but no GPU work
- DXR ray tracing is complete (45K lines) but disabled by default

### 3. Physics (Jolt) -- FULLY FUNCTIONAL

- All shape types, dynamic/kinematic/static bodies, 16-layer collision filtering
- Contact callbacks, raycasts, sweep tests, lag compensation for networking
- ECS integration via RigidBodyComponent/ColliderComponent
- Tested: TestPhysicsComponents.cpp

### 4. Audio (XAudio2) -- FULLY FUNCTIONAL (Windows only)

- 32-source pool, 3 mixer buses, 3D spatial audio with Doppler
- Tested: TestAudioEngine.cpp, TestAudioMixerBus.cpp

### 5. Input -- FULLY FUNCTIONAL

- Keyboard, mouse (raw input), gamepad with key binding/remapping
- Thread-safe, tested across 3 dedicated test files

### 6. AI -- PROFESSIONAL GRADE (5,952 lines)

- Behavior trees, NavMesh + Recast/Detour, perception, formation, cover system
- Frame-budget limiter, collision avoidance, 8 test files

### 7. Animation -- COMPREHENSIVE (4,100+ lines)

- Skeletal animation, blend spaces, state machines, IK solvers (TwoBone, LookAt, FABRIK)
- Ragdoll integration, compression, retargeting, 4 test files

### 8. Networking -- ENTERPRISE GRADE (7,300+ lines)

- UDP transport, reliable messaging, entity replication with delta snapshots
- Client prediction + server reconciliation, lag compensation
- AES-256 encryption, instability simulator, AreaServer/WorldServer architecture

### 9. Game Module System -- WORKING

- DLL plugin architecture (IGameModule interface)
- Hot-reload via ModuleHotReloadManager
- SparkGame example: complete FPS framework (player, weapons, vehicles, HUD, scenes)

### 10. Supporting Systems -- ALL WIRED via InitGameplaySystems()

Save system, UI, dialogue, ability system (WoW-style), scripting (conditional on SDK),
weather, destruction, modding (VFS + SparkPak), coroutines, events

### 11. Editor -- EXTENSIVE (52 panels, collaborative editing)

### 12. Tests -- 211 files, 2,577 tests (all pass on Linux)

---

## What's Missing or Incomplete

### Critical Gaps

| Gap | Impact | Severity |
|-----|--------|----------|
| **Only D3D11 renders** | Windows-only for real graphics | HIGH |
| **No terrain system** | Can't build open-world games | HIGH |
| **12 rendering stubs** (sky, water, GI) | Advanced visuals need custom work | MEDIUM |
| **~~SparkGame has no AI enemies~~** | RESOLVED: 12 enemies, 6 types, wave spawner | ~~MEDIUM~~ |
| **AngelScript needs external SDK** | Scripting stubs without it | MEDIUM |
| **Audio is XAudio2 only** | No Linux/macOS audio | MEDIUM |

### Platform Reality

| Platform | Status |
|----------|--------|
| Windows (MSVC) | **Primary, functional** |
| Linux (GCC/Clang) | Builds, headless works, no audio, SDL2 windowing |
| macOS | Experimental, continue-on-error CI |
| Consoles | Not supported |

---

## Game Type Viability

| Game Type | Viability | Why |
|-----------|-----------|-----|
| **FPS (arena/corridor)** | HIGH | SparkGame demonstrates this. Add AI enemies and it's playable. |
| **Third-person action** | MEDIUM-HIGH | Physics/animation/AI all work. Need camera mode. |
| **RPG** | MEDIUM | Ability system, dialogue, save system work. Need engine-level inventory/quests. |
| **MMO** | MEDIUM | Enterprise-grade networking. Massive content pipeline needed. |
| **Open-world** | LOW | No terrain, streaming deleted, origin rebasing untested at scale. |
| **2D game** | MEDIUM | Sprite batching works, but 2D workflow underdeveloped. |
| **Mobile** | LOW | Header exists, no real mobile backend. |
| **VR** | LOW-MEDIUM | VR system registered but not validated with hardware. |

---

## Bottom Line

SparkEngine is a real engine. The core loop runs, draw calls hit the GPU, physics simulates, AI pathfinds, networking replicates entities, and 2,577 tests validate it. The SparkGame module proves a game can load, run, and process logic through the module interface.

**To ship a game today:** Windows target, C++ proficiency, willingness to build against IGameModule, custom work for missing features. Comparable to early-access indie engines (Flax, Stride) in system breadth, behind in tooling/platform support/artist workflows. Networking stack is notably more sophisticated than most indie engines.

---

## Gaps Addressed (2026-04-01, 4-phase session)

| Phase | What | Lines |
|-------|------|-------|
| 1 | Making Your First Game tutorial, vehicle weapons fixed, IAudioBackend + NullBackend | 940 |
| 2 | GPU binding for 8 rendering systems (sky, water, terrain, shadow, cluster, SSAO, probes, DQS) | 986 |
| 3 | Cross-platform audio (XAudio2/OpenAL/Null backend adapters, factory, engine wiring) | 620 |
| 4 | Artist Workflow Guide, Multiplayer Quick Start, knowledge updates | ~900 |

**Remaining gaps after session:**
- Actual HLSL/GLSL shaders for GPU rendering systems (GPU binding layer exists but no shaders)
- D3D12/Vulkan backends still stubs (OpenGL is real: 1,921 lines, 251 GL calls)
- Engine-level inventory/quest systems (game-module level only)
- Console/mobile platforms
