# SparkEngine Project Status

Current status of all major subsystems as of v1.0.0 (April 2026).

## Status Legend

| Status | Meaning |
|--------|---------|
| **Stable** | Production-ready, fully tested in CI, primary code path |
| **Experimental** | Functional with basic features, may have gaps, CI tested |
| **Framework** | Interface designed, partial or stub implementation |
| **Planned** | Not yet implemented |

## Core Systems

| System | Status | Notes |
|--------|--------|-------|
| Engine Framework | **Stable** | `EngineContext` service locator, phase-based execution |
| ECS (EnTT) | **Stable** | 75 component types, 25 systems, 17 component headers |
| Scene Management | **Stable** | Serialization, snapshots, streaming |
| Asset Pipeline | **Stable** | Native FBX importer + glTF/OBJ loaders, async LRU caching, VRAM budget |
| Job System | **Stable** | Multi-threaded task dispatch |
| Event Bus | **Stable** | Typed publish/subscribe with event response system |

## Rendering

| System | Status | Notes |
|--------|--------|-------|
| DirectX 11 | **Stable** | Primary backend, all features |
| DirectX 12 | Experimental | Mesh shaders, DXR, VRS |
| Vulkan 1.4 | Experimental | Dynamic rendering, push descriptors, timeline semaphores |
| OpenGL 4.6 | Experimental | DSA, SPIR-V, Mesa llvmpipe software rendering |
| Metal | Framework | Header designed (`MetalDevice.h`), no `.mm` implementation |
| NullRHIDevice | **Stable** | Headless mode, automatic fallback |
| RenderGraph | **Stable** | Declarative render pass system |
| PBR Materials | **Stable** | Metallic/roughness, 18 texture slots, hot-reload |
| Post-Processing | **Stable** | 14 effects (bloom, DOF, motion blur, TAA, etc.) |
| Global Illumination | **Stable** | DDGI + Adaptive Probe Volumes + Hybrid RT |
| GPU-Driven Rendering | **Stable** | Compute culling, HiZ occlusion, indirect draws |
| Mesh Shaders | Experimental | Meshlet pipeline, requires SM 6.5 / D3D12 |
| Virtual Texturing | **Stable** | Feedback-driven page streaming, LRU cache |
| DXR Ray Tracing | Experimental | Reflections, shadows, AO, GI, denoising |
| Shader Graph | **Stable** | 35+ nodes, HLSL generation, live preview |
| Cluster-Based LOD | **Stable** | DAG hierarchy, screen-space error traversal |
| FSR Upscaling | Experimental | AMD FidelityFX Super Resolution |

## Physics (Jolt)

| System | Status | Notes |
|--------|--------|-------|
| Rigid Bodies | **Stable** | Static/kinematic/dynamic, 15 collision shapes |
| Character Controller | **Stable** | CCD, slopes, stairs, moving platforms |
| Vehicle Physics | **Stable** | Wheeled/tracked/motorcycle, powertrain |
| Ragdoll | **Stable** | Physics-driven ragdoll blending |
| Soft Body / Cloth | **Stable** | Cloth simulation |
| Destruction | **Stable** | Fracture system |
| Deterministic Mode | **Stable** | For replay and networking |

## Audio

| System | Status | Notes |
|--------|--------|-------|
| XAudio2 | **Stable** | 3D spatial, distance/Doppler, Windows |
| Miniaudio | Experimental | Cross-platform fallback |
| Audio Mixer | **Stable** | Master/SFX/Music channels |

## AI & Navigation

| System | Status | Notes |
|--------|--------|-------|
| Behavior Trees | **Stable** | Composites, decorators, actions, blackboard |
| NavMesh (Recast/Detour) | **Stable** | Dynamic obstacles, off-mesh links |
| Perception | **Stable** | Vision, hearing, memory |
| Steering | **Stable** | Seek, flee, pursue, evade, flocking |
| Tactical Points (EQS) | **Stable** | Environmental queries |
| Cover / Formations | **Stable** | Group AI coordination |
| AI Budget / Director | **Stable** | 100+ agents, scripted events, difficulty FSM |

## Animation

| System | Status | Notes |
|--------|--------|-------|
| Skeletal Animation | **Stable** | Bone hierarchies, keyframe clips |
| State Machines | **Stable** | Cross-fade transitions |
| IK | **Stable** | Two-bone, look-at, FABRIK |
| Retargeting | **Stable** | Animation retargeting across skeletons |
| Cinematic Sequencer | **Stable** | Timeline tracks, Bezier/Catmull-Rom |

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
| AngelScript VM | **Stable** | Hot-reload, state preservation, lifecycle callbacks |
| Visual Scripting | **Stable** | 60 node types, compiles to AngelScript |
| Lua | Experimental | Secondary scripting language |
| Mod System | **Stable** | User-created content support |

## Editor (ImGui)

| System | Status | Notes |
|--------|--------|-------|
| Panel System | **Stable** | 59 dockable panels, layout save/load |
| Gizmos | **Stable** | Translate/rotate/scale (ImGuizmo) |
| Node Graphs | **Stable** | Visual scripting, shader graph (imnodes) |
| Command Palette | **Stable** | Ctrl+P quick access |
| Undo/Redo | **Stable** | Full edit history |
| Plugin System | **Stable** | Built-in + DLL plugins |
| Collaborative Editing | Experimental | Multi-user, node locking |

## Gameplay Systems

| System | Status | Notes |
|--------|--------|-------|
| Weapons | **Stable** | Bullet/rocket/grenade, vehicles |
| Inventory / Quests | **Stable** | Item management, quest tracking |
| Achievements | **Stable** | Achievement system |
| Abilities / Conditions | **Stable** | Ability system with conditions |
| Dialogue | **Stable** | Branching dialogue trees |
| Destruction | **Stable** | Destructible objects, fracture |
| Replay | **Stable** | Record/playback system |
| Save System | **Stable** | ECS serialization, compression, slots |
| Day/Night / Weather | **Stable** | Time-of-day, weather system |
| Accessibility | **Stable** | 5 colorblind modes, subtitles, reduced motion |

## Platforms

| Platform | Status | Notes |
|----------|--------|-------|
| Windows 10+ (MSVC v143) | **Stable** | Primary, fully CI tested |
| Windows (MSVC v145 / VS 2026) | Experimental | Native CMake VS 18 generator coverage |
| Linux (GCC 13+) | Experimental | CI tested, pre-built binaries |
| Linux (Clang 17+) | Experimental | CI tested |
| macOS (Apple Clang) | Experimental | Builds, no Metal yet |
| VR (OpenXR) | Framework | Interface designed, not implemented |
| Mobile (iOS/Android) | Framework | Touch input working, needs platform layer |
| Console (PlayStation/Xbox) | Planned | Not started |

## Testing & CI

| System | Status | Notes |
|--------|--------|-------|
| Unit Tests | **Stable** | 4290 tests, 338 files |
| ASan / UBSan / LSan | **Stable** | CI enforced |
| TSan | **Stable** | CI enforced |
| MSan | Experimental | Advisory (uninstru mented libc++) |
| Code Coverage | **Stable** | lcov reports |
| Golden Image Testing | **Stable** | Regression testing |
| clang-format | **Stable** | CI enforced |
| clang-tidy | Experimental | Advisory |

---

*Last updated: 2026-04-06*
