# Codebase Health

Historical implementation-inventory snapshot from 2026-03-26. Its status labels
record what that audit believed was present in source; they are not current release,
support, interoperability, or deployment certification. The authoritative profile is
the blocked and uncertified `stable-v1` Windows 11 x64/MSVC v143 + D3D11/Windows
NullRHI + C++ module slice in `docs/site/readiness.json`.

Last updated: 2026-03-26

## System Maturity Overview

Historical legend: **DONE** = the 2026-03-26 audit marked an implementation present/wired, not release-ready | **PARTIAL** = that audit found missing breadth | **STUB** = interface/minimal implementation | **EXPERIMENTAL** = implementation without release evidence

### Core Systems

| System | Status | Notes |
|--------|:------:|-------|
| Engine initialization (EngineContext) | **DONE** | Service locator with dependency-aware init/shutdown |
| Module system (IModule, DLL loading) | **DONE** | Dynamic loading, discovery, load ordering |
| Error handling (Result, CrashHandler) | **DONE** | Minidump generation, stack traces, HTTP upload |
| Logging (project logger + SimpleConsole) | **DONE** | Historical audit found the custom logging path present; no spdlog dependency is tracked |
| Event system (EventBus) | **DONE** | Type-safe pub/sub, queued dispatch |
| Job system (ThreadPool) | **DONE** | Built but underutilized by core systems |
| Coroutine scheduler | **DONE** | C++20 coroutines, yield/resume |

### Rendering & Graphics

| System | Status | Notes |
|--------|:------:|-------|
| D3D11 renderer | **DONE** | Primary backend, PBR, shadows, post-processing |
| D3D12 backend | **EXPERIMENTAL** | Via RHI abstraction |
| Vulkan backend | **EXPERIMENTAL** | Via RHI abstraction |
| OpenGL backend | **EXPERIMENTAL** | Via RHI abstraction |
| RHI abstraction layer | **DONE** | Factory pattern, backend selection |
| Post-processing pipeline | **DONE** | Bloom, tone mapping, SSAO, SSR, FXAA, TAA |
| PBR materials | **DONE** | Metallic/roughness workflow |
| Mesh LOD | **DONE** | Distance-based switching |
| Decal system | **DONE** | Lifetime management |
| GPU particles | **DONE** | Compute-based particle system |
| DXR raytracing | **EXPERIMENTAL** | Requires D3D12 (Windows); enabled by default with SDFGI fallback |
| DLSS/FSR upscaling | **EXPERIMENTAL** | Requires NVIDIA/AMD SDK |
| Render graph | **DONE** | Pass dependency management |
| NullRHIDevice (headless) | **DONE** | `RHIBridge` retries available GPU candidates after device/swap-chain failure, then creates NullRHI if none succeeds; the historical “5 integration tests” count is not a current verdict |
| Software rendering (llvmpipe) | **DONE** | Historical development-route note: OpenGL can be exercised with Mesa llvmpipe and required host/display setup; not a universal or certified fallback |

### Physics

| System | Status | Notes |
|--------|:------:|-------|
| Jolt Physics integration | **DONE** | Rigid bodies, collision, raycasting |
| Collision shapes (15 types) | **DONE** | Box, sphere, capsule, cylinder, mesh, etc. |
| Constraints (12 types) | **DONE** | Hinge, slider, fixed, generic, etc. |
| Character controller | **DONE** | With CCD |
| Vehicle physics | **DONE** | Wheel simulation |
| Ragdoll | **DONE** | Joint-based ragdoll |
| Cloth simulation | **DONE** | Position-based dynamics |

### Audio

| System | Status | Notes |
|--------|:------:|-------|
| XAudio2 3D spatial | **DONE** | Distance attenuation, Doppler |
| Audio backend factory | **DONE** | Active factory: XAudio2 on Windows, OpenAL on non-Windows, then Null audio; miniaudio is a linked compatibility/implementation surface, not the active fallback |
| Audio mixer | **DONE** | Master/SFX/music channels |

### ECS

| System | Status | Notes |
|--------|:------:|-------|
| EnTT registry | **DONE** | Current source inventory finds 79 component structs across 17 component headers |
| System execution order | **DONE** | Physics → Animation → AI → Audio → Lifecycle → Render |
| Reactive systems | **DONE** | Component change detection via EnTT signals |

### AI & Navigation

| System | Status | Notes |
|--------|:------:|-------|
| Behavior trees | **DONE** | Composite, decorator, action nodes |
| NavMesh pathfinding (A*) | **DONE** | Recast/Detour integration |
| Perception system | **DONE** | Vision cones, hearing, memory |
| Steering behaviors | **DONE** | Seek, flee, pursue, evade, flock |
| Tactical point system (EQS) | **DONE** | Environmental queries |
| Cover system | **DONE** | Cover point evaluation |
| Formation system | **DONE** | Group formations |
| AI director | **DONE** | Scripted event orchestration |

### Animation

| System | Status | Notes |
|--------|:------:|-------|
| Skeletal animation | **DONE** | Bone hierarchies, keyframe clips |
| State machines | **DONE** | Cross-fading transitions |
| Multi-layer blending | **DONE** | Override/additive/layered |
| IK solvers | **DONE** | Two-bone, look-at, FABRIK |
| Root motion | **DONE** | Extraction and application |

### Networking

| System | Status | Notes |
|--------|:------:|-------|
| UDP client/server | **DONE** | WinSock2 + POSIX sockets |
| Entity replication | **DONE** | Dirty-flag delta sync |
| Lag compensation | **DONE** | Snapshots, RewindToTime(), ValidateHit() with ray-AABB intersection |
| Reliable channels | **DONE** | ACK bitmask, retransmission with exponential backoff, duplicate detection, ordered delivery, RTT estimation |
| Client-side prediction | **DONE** | Input recording, local simulation, server reconciliation with smooth correction |
| Connection timeout | **DONE** | Heartbeat-based detection |
| Area server architecture | **DONE** | WorldServer coordination, per-area instances |
| Entity migration | **DONE** | Cross-area serialization and transfer |
| Network authenticated encryption | **BLOCKED** | NET-100 open; XOR/FNV helpers are isolated prototypes, not cryptography |

### Scripting

| System | Status | Notes |
|--------|:------:|-------|
| AngelScript VM | **DONE** | Module isolation, lifecycle callbacks |
| Hot-reload | **DONE** | File watcher with debouncing |
| Visual scripting | **DONE** | Node-based graph editor |
| Engine API bindings | **DONE** | Math, components, input, entities |

### Editor

| System | Status | Notes |
|--------|:------:|-------|
| ImGui editor core | **DONE** | 59 panel classes, docking, theming |
| Scene hierarchy | **DONE** | Tree view with drag-drop |
| Inspector | **DONE** | Component renderers |
| Material editor | **DONE** | PBR material editing |
| Animation timeline | **DONE** | Keyframe editing |
| Collaborative editing | **DONE** | Multi-user sessions, node locking |
| Performance profiler | **DONE** | Frame timing, GPU timing |

### Gameplay Systems

| System | Status | Notes |
|--------|:------:|-------|
| Player controller | **DONE** | FPS movement and interaction |
| Weapon system | **DONE** | Fire modes, reload, recoil, ADS |
| Vehicle system | **DONE** | Vehicle mechanics |
| Inventory | **DONE** | Item management, stacking, weight |
| Quest system | **DONE** | Stages, objectives, completion |
| Achievement system | **DONE** | Tracking and unlocking |
| Day/night cycle | **DONE** | Time progression |
| Weather system | **DONE** | Dynamic weather transitions |
| Terrain (quadtree LOD) | **DONE** | Heightmap with texture splatting |
| Dialogue system | **DONE** | Branching dialogue trees |
| Destruction system | **DONE** | Runtime mesh destruction |
| Replay system | **DONE** | Record/playback |
| Save system | **DONE** | ECS-aware serialization with compression |

### Platform Support

| System | Status | Notes |
|--------|:------:|-------|
| Windows (MSVC) | **DONE** | Historical primary-platform CI note; `stable-v1` remains blocked and uncertified |
| Linux (GCC/Clang) | **EXPERIMENTAL** | Historical CI/build-artifact note; no Linux product certification |
| macOS | **EXPERIMENTAL** | Historical build note; no macOS product certification |
| VR (OpenXR) | **STUB** | Framework exists, requires OpenXR SDK |
| Mobile (iOS/Android) | **STUB** | Platform abstraction layer |

## Architectural Observations

### Strengths

- **Modular service locator** — EngineContext provides clean subsystem access with dependency-aware initialization
- **ECS source breadth** — Current source inventory finds 79 component structs across 17 component headers
- **Test source inventory** — 6,952 test definitions across 575 test-bearing files; these counts do not establish execution or pass results
- **Consistent code style** — clang-format enforced in CI, Allman braces, 120-col limit
- **RHI abstraction** — Clean backend selection via factory pattern

### Known Architectural Considerations

These are areas identified during code audits where future improvement may be beneficial:

1. **GraphicsEngine complexity** — The GraphicsEngine class (~1,326 lines) handles multiple responsibilities. Future refactoring could decompose it into focused subsystems (material management, render submission, state management).

2. **ECS parallelism** — Systems currently execute serially on the main thread. The Job System exists and could be integrated for parallel system execution in the future.

3. **RHI integration depth** — D3D11 is the primary implementation path, but no backend is certified by the blocked `stable-v1` profile. D3D12/Vulkan/OpenGL remain experimental implementations outside that profile.

4. **Networking** — Source implementations exist for reliable channels, lag compensation, and client-side prediction, but this historical inventory does not establish interoperability or test status. Networking is outside `stable-v1`, and `NET-100` remains open.

## Priority Improvements

### High Priority

- Investigate parallel ECS system execution via Job System
- Expand RHI backend test coverage (D3D12, Vulkan, OpenGL)

### Medium Priority

- Add performance regression benchmarks to CI

### Lower Priority

- Evaluate GraphicsEngine decomposition
- Add audio subsystem unit tests
- Consider formal plugin architecture for editor extensions

---

## See Also

- [Codebase Statistics](Codebase-Statistics.md) — Detailed code metrics and file counts
- [Architecture Overview](../getting-started/Architecture-Overview.md) — Engine design and structure
- [Testing](Testing.md) — Test suite details
- [Contributing](Contributing.md) — How to contribute
