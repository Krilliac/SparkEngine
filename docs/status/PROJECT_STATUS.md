# SparkEngine Project Status

Current implementation inventory for SparkEngine. The authoritative `stable-v1` release profile is blocked and uncertified; see [`docs/site/readiness.json`](../site/readiness.json) and the [readiness handoff](../readiness/ENGINE_READINESS_HANDOFF.md).

This page records source presence and implementation maturity only. An **Implemented** row is not a release-support, CI-certification, packaging, or production-deployment claim.

## Status Legend

| Status | Meaning |
|--------|---------|
| **Implemented** | Present in source as a primary or substantial implementation; no release-readiness or `stable-v1` certification claim |
| **Experimental** | Functional or partially exercised, with known gaps; outside `stable-v1` unless the contract says otherwise |
| **Framework** | Interface designed, partial or stub implementation |
| **Planned** | Not yet implemented |

## Core Systems

| System | Status | Notes |
|--------|--------|-------|
| Engine Framework | **Implemented** | `EngineContext` service locator, phase-based execution |
| ECS (EnTT) | **Implemented** | Source inventory: 79 component structs across 17 component headers; no canonical system-total claim |
| Scene Management | **Implemented** | Serialization, snapshots, streaming |
| Asset Pipeline | **Implemented** | Native FBX importer + glTF/OBJ loaders, async LRU caching, VRAM budget |
| Job System | **Implemented** | Multi-threaded task dispatch |
| Event Bus | **Implemented** | Typed publish/subscribe with event response system |

## Rendering

| System | Status | Notes |
|--------|--------|-------|
| DirectX 11 | **Implemented** | Primary implementation path; `stable-v1` remains blocked and uncertified |
| DirectX 12 | Experimental | Mesh shaders, DXR, VRS |
| Vulkan 1.4 | Experimental | Dynamic rendering, push descriptors, timeline semaphores |
| OpenGL | Experimental | EGL/GLX device path requests 4.5; SDL hosts request 3.3; DSA/SPIR-V source paths and explicit llvmpipe development routing are uncertified |
| Metal | Experimental | Objective-C++ device, ray-tracing, interop, and readback implementations exist; outside `stable-v1` and uncertified |
| NullRHIDevice | **Implemented** | No-render device; in `stable-v1` only on Windows 11 x64, still blocked and uncertified |
| RenderGraph | **Implemented** | Declarative render pass system |
| PBR Materials | **Implemented** | Metallic/roughness, 18 texture slots, hot-reload |
| Post-Processing | **Implemented** | 14 effects (bloom, DOF, motion blur, TAA, etc.) |
| Global Illumination | **Implemented** | DDGI + Adaptive Probe Volumes + Hybrid RT |
| GPU-Driven Rendering | **Implemented** | Compute culling, HiZ occlusion, indirect draws |
| Mesh Shaders | Experimental | Meshlet pipeline, requires SM 6.5 / D3D12 |
| Virtual Texturing | **Implemented** | Feedback-driven page streaming, LRU cache |
| DXR Ray Tracing | Experimental | Reflections, shadows, AO, GI, denoising |
| Shader Graph | **Implemented** | 35+ nodes, HLSL generation, live preview |
| Cluster-Based LOD | **Implemented** | DAG hierarchy, screen-space error traversal |
| FSR Upscaling | Experimental | AMD FidelityFX Super Resolution |

## Physics (Jolt)

| System | Status | Notes |
|--------|--------|-------|
| Rigid Bodies | **Implemented** | Static/kinematic/dynamic, 15 collision shapes |
| Character Controller | **Implemented** | CCD, slopes, stairs, moving platforms |
| Vehicle Physics | **Implemented** | Wheeled/tracked/motorcycle, powertrain |
| Ragdoll | **Implemented** | Physics-driven ragdoll blending |
| Soft Body / Cloth | **Implemented** | Cloth simulation |
| Destruction | **Implemented** | Fracture system |
| Deterministic Mode | **Implemented** | For replay and networking |

## Audio

| System | Status | Notes |
|--------|--------|-------|
| XAudio2 | **Implemented** | 3D spatial, distance/Doppler, Windows |
| OpenAL | Experimental | Non-Windows backend selected by `AudioBackendFactory`; outside `stable-v1` |
| Miniaudio | Experimental | Linked implementation surface, not the active backend-factory fallback |
| Audio Mixer | **Implemented** | Master/SFX/Music channels |

## AI & Navigation

| System | Status | Notes |
|--------|--------|-------|
| Behavior Trees | **Implemented** | Composites, decorators, actions, blackboard |
| NavMesh (Recast/Detour) | **Implemented** | Dynamic obstacles, off-mesh links |
| Perception | **Implemented** | Vision, hearing, memory |
| Steering | **Implemented** | Seek, flee, pursue, evade, flocking |
| Tactical Points (EQS) | **Implemented** | Environmental queries |
| Cover / Formations | **Implemented** | Group AI coordination |
| AI Budget / Director | **Implemented** | 100+ agents, scripted events, difficulty FSM |

## Animation

| System | Status | Notes |
|--------|--------|-------|
| Skeletal Animation | **Implemented** | Bone hierarchies, keyframe clips |
| State Machines | **Implemented** | Cross-fade transitions |
| IK | **Implemented** | Two-bone, look-at, FABRIK |
| Retargeting | **Implemented** | Animation retargeting across skeletons |
| Cinematic Sequencer | **Implemented** | Timeline tracks, Bezier/Catmull-Rom |

## Networking

| System | Status | Notes |
|--------|--------|-------|
| UDP Client/Server | Experimental | Reliable/unreliable/ordered channels |
| Entity Replication | Experimental | Dirty property tracking, delta snapshots |
| Prediction / Reconciliation | Experimental | Client prediction with server authority |
| Lag Compensation | Experimental | Hitbox rewinding, 1s history |
| AreaServer Architecture | Experimental | HeroEngine-inspired, per-region simulation |
| WorldServer | Experimental | Cross-area coordination, load balancing |
| Collaborative Editing | Experimental | Multi-user, node locking, presence |

## Scripting

| System | Status | Notes |
|--------|--------|-------|
| AngelScript VM | Experimental | Hot-reload implementation exists; scripting is outside `stable-v1` |
| Visual Scripting | Experimental | Node/editor implementation exists; scripting is outside `stable-v1` |
| Mod System | Experimental | Discovery and load-order implementation; delivery is outside `stable-v1` |

## Editor (ImGui)

| System | Status | Notes |
|--------|--------|-------|
| Panel System | **Implemented** | 65 `*Panel.h` classes by source inventory; registration and default visibility are separate metrics |
| Gizmos | Framework | Translation path exists; rotate and scale are explicit no-ops (`EDT-210`) |
| Node Graphs | **Implemented** | Visual scripting, shader graph (imnodes) |
| Command Palette | **Implemented** | Ctrl+P quick access |
| Undo/Redo | Experimental | Partial command coverage; full edit-history certification is absent (`EDT-210`) |
| Plugin System | **Implemented** | Built-in + DLL plugins |
| Collaborative Editing | Experimental | Multi-user, node locking |

## Gameplay Systems

| System | Status | Notes |
|--------|--------|-------|
| Weapons | **Implemented** | Bullet/rocket/grenade, vehicles |
| Inventory / Quests | **Implemented** | Item management, quest tracking |
| Achievements | **Implemented** | Achievement system |
| Abilities / Conditions | **Implemented** | Ability system with conditions |
| Dialogue | **Implemented** | Branching dialogue trees |
| Destruction | **Implemented** | Destructible objects, fracture |
| Replay | **Implemented** | Record/playback system |
| Save System | **Implemented** | ECS serialization, compression, slots |
| Day/Night / Weather | **Implemented** | Time-of-day, weather system |
| Accessibility | **Implemented** | 5 colorblind modes, subtitles, reduced motion |

## Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| Windows 10+ (MSVC v143) | Development build floor | Outside `stable-v1`; uncertified |
| Windows (MSVC v145 / VS 2026) | Experimental | Native CMake VS 18 generator coverage |
| Linux (GCC 13+) | Experimental | Development CI/build path; outside `stable-v1` |
| Linux (Clang 17+) | Experimental | CI tested |
| macOS (Apple Clang) | Experimental | Build and Metal implementation paths exist; outside `stable-v1`, with no release evidence |
| VR (OpenXR) | Framework | Interface designed, not implemented |
| Mobile (iOS/Android) | Framework | Touch input working, needs platform layer |
| Console (PlayStation/Xbox) | Planned | Not started |

## Testing & CI

| System | Status | Notes |
|--------|--------|-------|
| Test definitions | **Implemented** | Generated inventory exists; passing definitions alone do not certify `stable-v1` |
| ASan / UBSan / LSan | **Implemented** | Required workflow lane exists; exact-SHA release evidence remains gated |
| TSan | **Implemented** | Required workflow lane exists; exact-SHA release evidence remains gated |
| MSan | Experimental | Advisory (uninstrumented libc++) |
| Code Coverage | **Implemented** | lcov reporting exists; coverage is not release certification |
| Golden Image Testing | Framework | Harness exists; the required D3D11 visual-certification gate remains blocked |
| clang-format | **Implemented** | Policy/workflow exists; exact-SHA release evidence remains gated |
| clang-tidy | Experimental | Job/configuration is required by `required-ci-gate`; individual diagnostics may be advisory |

---

*Last updated: 2026-08-28*
