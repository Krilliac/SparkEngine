# SparkEngine — Comprehensive Gap Analysis Template

> Use this template to audit any SparkEngine system. Copy this file, rename it
> to `<SYSTEM>_GAP_ANALYSIS.md`, and fill in each section. Mark items as
> **DONE**, **PARTIAL**, **STUB**, or **MISSING** with a brief note.

---

## Status Legend

| Tag         | Meaning                                                    |
|-------------|------------------------------------------------------------|
| **DONE**    | Fully implemented, tested, and production-ready            |
| **PARTIAL** | Core path works but edge cases / features are incomplete   |
| **STUB**    | Header or signature exists; body is empty or returns dummy |
| **MISSING** | Not yet created; no header, no source                      |

---

## 1. Core Platform & Infrastructure

### 1.1 Engine Initialization (`Core/`)
- [ ] `SparkEngine` — startup, main loop, shutdown sequence
- [ ] `EngineContext` — service locator registration/retrieval for all subsystems
- [ ] `EngineSettings` — configuration loading, saving, runtime modification
- [ ] `Platform.h` — cross-platform type abstractions (Win32, Linux, macOS)
- [ ] DirectXMath stubs for non-Windows platforms

### 1.2 Module System (`Core/`)
- [ ] `ModuleManager` — dynamic module discovery, loading, unloading
- [ ] `GameModuleLoader` — DLL/shared-library loading for game modules
- [ ] `IGameModule` — module interface contract
- [ ] `SparkExport.h` — DLL export/import macros
- [ ] Hot-reload support for game modules

### 1.3 Error Handling & Crash Recovery (`Utils/`)
- [ ] `CrashHandler` — minidump generation, structured exception handling
- [ ] `CrashHandlerHelpers` — stack walking, symbol resolution
- [ ] `CrashHandlerStub` — platform fallbacks (Linux/macOS)
- [ ] `SparkError` — error code definitions and categories
- [ ] `Result.h` — monadic Result/Error type
- [ ] `Assert.h` — debug/release assertion macros
- [ ] `StackTrace` — programmatic stack trace capture

### 1.4 Logging & Diagnostics (`Utils/`)
- [ ] `Logger` — severity levels, categories, filtering
- [ ] `LogMacros` — convenience macros (LOG_INFO, LOG_WARN, LOG_ERROR)
- [ ] `FileLogger` — file output backend with rotation
- [ ] Thread-safe logging from any thread
- [ ] Console output integration

### 1.5 Configuration & Serialization
- [ ] `ConfigParser` — INI/JSON config file reading and writing
- [ ] Command-line argument parsing
- [ ] Runtime config overrides
- [ ] Settings persistence between sessions

### 1.6 Enum Utilities (`Enums/`)
- [ ] `EnumUtils` — string-to-enum, enum-to-string conversion
- [ ] `GraphicsEnums` — graphics system enumerations
- [ ] `InputEnums` — input enumerations
- [ ] `GameSystemEnums` — gameplay enumerations
- [ ] `EnumTests` — enum validation

---

## 2. Memory & Data Structures

### 2.1 Memory Management (`Utils/`)
- [ ] `FrameAllocator` — per-frame linear allocator with reset
- [ ] `ObjectPool` — typed object pool with reuse
- [ ] `MemoryDebugger` — allocation tracking, leak detection
- [ ] Custom allocator integration with STL containers
- [ ] Memory budget tracking and warnings

### 2.2 Data Structures (`Utils/`)
- [ ] `RingBuffer` — lock-free ring buffer
- [ ] `Octree` — spatial partitioning for broad-phase queries
- [ ] `OpaqueHandle` — type-safe handle system
- [ ] `UUID` — unique identifier generation
- [ ] Spatial hashing / grid structures

---

## 3. Threading & Job System

### 3.1 Job System (`Utils/`)
- [ ] `JobSystem` — worker thread pool, job submission
- [ ] Job dependencies and continuations
- [ ] Priority queues for job scheduling
- [ ] Main-thread task dispatch
- [ ] Job stealing between worker threads

### 3.2 Coroutines (`Engine/Coroutine/`)
- [ ] `CoroutineScheduler` — C++20 coroutine scheduling
- [ ] WaitForSeconds, WaitForFrames, WaitUntil primitives
- [ ] Coroutine cancellation and cleanup
- [ ] Integration with ECS systems

### 3.3 Thread Safety
- [ ] Mutex-protected console (`SimpleConsole`)
- [ ] Atomic frame state in `GraphicsEngine`
- [ ] Queue mutex in `NetworkManager`
- [ ] Thread-safe event dispatch
- [ ] Data race analysis / sanitizer support

---

## 4. Profiling & Performance

### 4.1 CPU Profiling (`Utils/`)
- [ ] `Profiler` — scoped CPU profiler with hierarchical timing
- [ ] `ChromeTracing` — chrome://tracing JSON export
- [ ] `FrameInspector` — per-frame timing breakdown
- [ ] `PerformanceStats` — FPS, frame time, draw call stats

### 4.2 Debug Visualization (`Utils/`)
- [ ] `DebugDraw` — 3D debug lines, boxes, spheres, text
- [ ] `DebugOverlay` — on-screen stats overlay (FPS, memory, draw calls)
- [ ] Physics debug rendering (collision shapes, contacts)
- [ ] NavMesh debug visualization
- [ ] Render graph debug view

---

## 5. Graphics & Rendering

### 5.1 Rendering Hardware Interface (`Graphics/RHI/`)
- [ ] `RHI.h` — abstract graphics API interface
- [ ] `RHIDevice` — device abstraction (create resources, submit commands)
- [ ] `RHIResources` — buffer, texture, sampler, pipeline state abstractions
- [ ] `RHITypes` — common types and enumerations
- [ ] `RHIFactory` — backend selection and device creation
- [ ] `RHIBridge` — legacy-to-RHI bridge layer

### 5.2 RHI Backends
- [ ] `D3D11Device` — DirectX 11 backend (primary)
- [ ] `D3D12Device` — DirectX 12 backend
- [ ] `OpenGLDevice` — OpenGL backend (Linux/macOS)
- [ ] `VulkanDevice` — Vulkan backend
- [ ] `MetalDevice` — Metal backend (macOS)
- [ ] `DXRSupport` — DirectX Raytracing integration

### 5.3 Core Rendering Pipeline (`Graphics/`)
- [ ] `GraphicsEngine` — device init, swap chain, render loop
- [ ] `Shader` — HLSL/GLSL compilation, reflection, binding
- [ ] `RenderTarget` — color/depth buffer management
- [ ] `RenderGraph` — frame graph with pass dependencies and resource transitions
- [ ] `Mesh` — vertex/index buffer management, submesh support
- [ ] `MeshLOD` — automatic LOD selection by distance
- [ ] `D3DUtils` — DirectX helper functions

### 5.4 Render Paths
- [ ] `DeferredLightingPass` — G-buffer generation and lighting resolve
- [ ] `ForwardPlusLightCulling` — tiled forward light culling (compute)
- [ ] Forward rendering fallback path
- [ ] Transparent object rendering path

### 5.5 Lighting & Shadows
- [ ] `LightManager` — point, spot, directional light management and culling
- [ ] `LightingSystem` — light evaluation and integration
- [ ] `ShadowAtlas` — cascaded shadow maps, shadow atlas packing
- [ ] `GlobalIllumination` — GI approximation (light probes, irradiance volumes)
- [ ] `IBLGenerator` — HDR cubemap convolution, BRDF LUT generation
- [ ] `FogSystem` — distance fog, height fog, volumetric fog

### 5.6 Materials & Textures
- [ ] `MaterialSystem` — PBR metallic-roughness pipeline
- [ ] `AdvancedBRDF` — Cook-Torrance, GGX, multi-scattering
- [ ] `TextureSystem` — streaming, mip-chain management, format conversion
- [ ] `TessellationSystem` — displacement mapping, adaptive tessellation
- [ ] Texture compression (BC1-BC7)

### 5.7 Post-Processing
- [ ] `PostProcessingPipeline` — ordered effect chain management
- [ ] `PostProcessingSystem` — individual effect implementations
- [ ] `ScreenSpaceEffects` — SSAO, SSR, GTAO (CPU path)
- [ ] `ScreenSpaceEffectsGPU` — GPU compute variants
- [ ] `TemporalEffects` — TAA, motion blur, temporal upscaling
- [ ] `UpscalingSystem` — spatial/temporal upscaling (DLSS/FSR-like)
- [ ] `DynamicQualityScaler` — dynamic resolution scaling
- [ ] Bloom, tone mapping, color grading, vignette, chromatic aberration

### 5.8 Visual Effects
- [ ] `ParticleSystem` — CPU particle emitters and simulation
- [ ] `GPUParticleSystem` — compute shader particle simulation
- [ ] `DecalSystem` — deferred decal projection
- [ ] `WaterSystem` — water surface rendering (reflection, refraction, caustics)
- [ ] `WeatherSystem` — rain, snow, cloud rendering
- [ ] `SkyAtmosphere` — Rayleigh/Mie scattering
- [ ] `SkyboxRenderer` — cubemap skybox rendering

### 5.9 Rendering Optimization
- [ ] `FrustumCulling` — view frustum culling
- [ ] `OcclusionCulling` — hardware/software occlusion queries
- [ ] `InstanceRenderer` — GPU instancing for repeated meshes
- [ ] `TransparencySorting` — OIT or sorted alpha blending
- [ ] `VRAMBudgetMonitor` — GPU memory tracking and alerts
- [ ] `ResourceResidencyManager` — texture streaming priority
- [ ] `ShaderCacheWarming` — background shader precompilation

### 5.10 Asset Pipeline (`Graphics/`)
- [ ] `AssetPipeline` — model import (OBJ, FBX), texture import
- [ ] `TinyObjImpl` — OBJ file loading
- [ ] Mesh optimization (vertex dedup, index optimization)
- [ ] Texture atlas generation
- [ ] Async asset loading

---

## 6. Physics

### 6.1 Physics System (`Physics/`)
- [ ] `PhysicsSystem` — Bullet Physics 3 integration, world stepping
- [ ] `CollisionSystem` — collision detection, callbacks, layers/masks
- [ ] `PhysicsTypes` — shape types, collision layers, physics materials
- [ ] Rigid body dynamics (static, dynamic, kinematic)
- [ ] Raycasting and shape queries
- [ ] Character controller (capsule-based)
- [ ] Trigger volumes
- [ ] Joint/constraint support (hinge, slider, spring)
- [ ] Continuous collision detection (CCD)
- [ ] Physics debug rendering

---

## 7. Input

### 7.1 Input System (`Input/`)
- [ ] `InputManager` — unified input abstraction, action mapping
- [ ] `PlatformInput` — keyboard and mouse (Win32 raw input)
- [ ] `GamepadInput` — XInput controller support
- [ ] Key binding and remapping
- [ ] Input action mapping (name → keys)
- [ ] Axis smoothing and dead zones
- [ ] Input recording and playback
- [ ] Multi-controller support
- [ ] Touch input (future)

---

## 8. Audio

### 8.1 Audio Engine (`Audio/`)
- [ ] `AudioEngine` — XAudio2 backend, 3D spatial audio, HRTF
- [ ] `SoundEffect` — one-shot and looping sound playback
- [ ] `MusicManager` — background music playback, crossfading
- [ ] Audio source pooling and voice management
- [ ] Reverb zones and environmental audio
- [ ] Audio occlusion and obstruction
- [ ] Volume groups / mix buses
- [ ] Real-time DSP effects
- [ ] Audio streaming for large files
- [ ] Doppler effect

---

## 9. ECS Framework

### 9.1 Components (`Engine/ECS/Components/`)
- [ ] `CoreComponents` — Transform, MeshRenderer, RigidBody, Camera, Tag
- [ ] `PhysicsComponents` — colliders, physics materials, triggers
- [ ] `AIComponents` — agent, behavior tree reference, perception
- [ ] `AnimationComponents` — animator, skeleton, animation state
- [ ] `AudioComponents` — audio source, listener, reverb zone
- [ ] `GameplayComponents` — health, damage, inventory, interaction
- [ ] `LightComponents` — point, spot, directional, area lights

### 9.2 Systems (`Engine/ECS/Systems/`)
- [ ] `ECSystems` — system base class and execution manager
- [ ] System execution order: Physics → Animation → AI → Audio → Lifecycle → Render
- [ ] System enable/disable at runtime
- [ ] System dependency declaration
- [ ] Parallel system execution where safe

### 9.3 Entity Management
- [ ] Entity creation and destruction (EnTT registry)
- [ ] Component addition/removal
- [ ] Entity prefabs / archetypes
- [ ] Entity hierarchy (parent-child)
- [ ] Entity queries and views

---

## 10. AI & Navigation

### 10.1 AI System (`Engine/AI/`)
- [ ] `AISystem` — agent update loop, decision making
- [ ] `BehaviorTree` — composites (sequence, selector, parallel)
- [ ] BehaviorTree decorators (inverter, repeat, cooldown)
- [ ] BehaviorTree leaf nodes (actions, conditions)
- [ ] `PerceptionSystem` — sight, hearing, awareness
- [ ] `SteeringBehaviors` — seek, flee, pursue, evade, wander, flocking
- [ ] AI state machines (patrol, chase, attack, flee)
- [ ] Group/squad coordination

### 10.2 Navigation (`Engine/AI/`)
- [ ] `NavMesh` — navigation mesh generation
- [ ] A* pathfinding on navmesh
- [ ] Dynamic obstacle avoidance
- [ ] NavMesh regions and area costs
- [ ] Off-mesh links (jump, ladder, teleport)
- [ ] Crowd simulation / local avoidance
- [ ] Path smoothing and string-pulling

---

## 11. Animation

### 11.1 Animation System (`Engine/Animation/`)
- [ ] `AnimationSystem` — skeletal animation playback
- [ ] Bone hierarchy and skeleton management
- [ ] Keyframe interpolation (linear, cubic, slerp)
- [ ] Animation state machines with transitions
- [ ] Animation blending (crossfade, layered, additive)
- [ ] Animation events / notifies
- [ ] Root motion support
- [ ] Inverse kinematics (IK) — two-bone, FABRIK, CCD
- [ ] Animation retargeting
- [ ] Morph targets / blend shapes
- [ ] Procedural animation (look-at, foot placement)
- [ ] Animation compression

---

## 12. Cinematic & Sequencer

### 12.1 Sequencer (`Engine/Cinematic/`)
- [ ] `Sequencer` — timeline-based cutscene playback
- [ ] Camera track with keyframes and spline interpolation
- [ ] Actor animation tracks
- [ ] Audio tracks synchronized to timeline
- [ ] Event/trigger tracks
- [ ] Cinematic camera transitions (cuts, fades, blends)
- [ ] Letterbox and aspect ratio control
- [ ] Sequencer scrubbing and preview

---

## 13. Event System

### 13.1 Event Bus (`Engine/Events/`)
- [ ] `EventSystem` — publish/subscribe event bus
- [ ] Typed event dispatching
- [ ] Event priority and ordering
- [ ] Deferred event queuing
- [ ] Event filtering by category
- [ ] Listener lifetime management (weak references)
- [ ] Cross-system event communication

---

## 14. Scripting

### 14.1 AngelScript (`Engine/Scripting/`)
- [ ] `AngelScriptEngine` — script context management
- [ ] Engine API bindings (ECS, input, audio, physics, math)
- [ ] Script hot-reload without engine restart
- [ ] Script debugging (breakpoints, watch variables)
- [ ] Script error handling and sandboxing
- [ ] Script-defined components
- [ ] Script coroutines / yield support
- [ ] Script serialization for save/load

---

## 15. Networking

### 15.1 Network System (`Engine/Networking/`)
- [ ] `NetworkManager` — client/server architecture
- [ ] UDP socket transport
- [ ] Connection management (connect, disconnect, timeout)
- [ ] Entity replication (state synchronization)
- [ ] RPC (remote procedure calls)
- [ ] Input prediction and reconciliation
- [ ] Lag compensation (server rewinding)
- [ ] Packet reliability (reliable/unreliable channels)
- [ ] Bandwidth throttling and prioritization
- [ ] Network profiling and stats
- [ ] Lobby / matchmaking interface

---

## 16. Save System

### 16.1 Serialization (`Engine/SaveSystem/`)
- [ ] `SaveSystem` — ECS-aware save/load
- [ ] JSON serialization backend
- [ ] Binary serialization backend
- [ ] Compression (miniz) for save files
- [ ] Save versioning and migration
- [ ] Async save to prevent hitching
- [ ] Auto-save with configurable interval
- [ ] Save slot management
- [ ] Checkpointing for level transitions

---

## 17. Procedural Generation

### 17.1 Procgen (`Engine/Procedural/`)
- [ ] `ProceduralGeneration` — noise functions (Perlin, Simplex, Worley)
- [ ] Heightmap terrain generation
- [ ] Procedural mesh generation (primitives, terrain, caves)
- [ ] Dungeon / level layout generation
- [ ] Vegetation and prop scattering
- [ ] Procedural textures
- [ ] Seed-based deterministic generation

---

## 18. World Systems

### 18.1 Day/Night Cycle (`Engine/World/`)
- [ ] `DayNightCycle` — sun position, sky color, light intensity over time
- [ ] Moon and stars
- [ ] Dynamic shadow direction updates
- [ ] Time-of-day events (dawn, dusk)
- [ ] Integration with weather system

### 18.2 Scene Management (`SceneManager/`)
- [ ] `SceneManager` — scene loading, unloading, transitions
- [ ] Async scene loading
- [ ] Additive scene loading (multiple scenes at once)
- [ ] Scene transition effects (fade, loading screen)

---

## 19. Camera System

### 19.1 Camera (`Camera/`)
- [ ] `SparkEngineCamera` — perspective/orthographic projection
- [ ] Camera frustum computation
- [ ] First-person camera controller
- [ ] Third-person camera (orbit, follow)
- [ ] Camera shake and screen effects
- [ ] Camera collision avoidance
- [ ] Cinematic camera (dolly, crane, tracking)
- [ ] Multi-viewport support

---

## 20. Game Object System (Legacy)

### 20.1 Game Objects (`Game/`)
- [ ] `GameObject` — base object class
- [ ] `Model` — model rendering component
- [ ] `ModelVertex` — vertex format definitions
- [ ] `Primitives` — procedural shape generation (cube, sphere, plane, etc.)
- [ ] `PlaceholderMesh` — placeholder mesh for missing assets
- [ ] Object lifecycle (create, update, destroy)

---

## 21. Editor — Core

### 21.1 Editor Application (`SparkEditor/Source/Core/`)
- [ ] `EditorApplication` — editor startup, main loop, shutdown
- [ ] `EditorUI` — ImGui integration and custom widgets
- [ ] `EditorTheme` — theming, dark/light mode, custom colors
- [ ] `EditorFonts` — font loading and scaling
- [ ] `EditorIcons` — icon atlas and icon rendering
- [ ] `EditorLayoutManager` — dockable panel layout, save/restore
- [ ] `EditorPanel` — base panel class (show/hide, focus, resize)
- [ ] `EditorCrashHandler` — editor-specific crash recovery
- [ ] `EditorLogger` — log routing to console panel
- [ ] `ProjectManager` — project creation, open, recent projects

### 21.2 Editor–Engine Communication
- [ ] `SparkEngineIntegration` — engine instance management inside editor
- [ ] `EngineInterface` — command/query interface between editor and engine
- [ ] `ExternalConsoleIntegration` — external console window
- [ ] `SparkFutureIntegration` — future integration hooks
- [ ] Play/Pause/Stop workflow (PIE — Play In Editor)

---

## 22. Editor — Panels

### 22.1 Viewport & Scene
- [ ] `SceneViewPanel` — 3D scene viewport with camera controls
- [ ] `GameViewPanel` — runtime game viewport
- [ ] Viewport gizmo rendering
- [ ] Grid and axis visualization
- [ ] Viewport shading modes (wireframe, unlit, lit, debug)

### 22.2 Hierarchy & Inspector
- [ ] `HierarchyPanel` — entity tree with drag-and-drop reparenting
- [ ] `SimpleHierarchyPanel` — simplified hierarchy variant
- [ ] `InspectorPanel` — component property editor with undo/redo
- [ ] `ComponentReflection` — reflection metadata for automatic UI generation
- [ ] Multi-entity selection and bulk editing

### 22.3 Asset Management
- [ ] `AssetBrowserPanel` — directory tree, thumbnails, search
- [ ] `AssetDatabase` — asset metadata, caching, dependency tracking
- [ ] `AdvancedAssetPipeline` — batch import, format conversion, optimization
- [ ] `ProjectBrowserPanel` — project file management
- [ ] Asset drag-and-drop into scene
- [ ] Asset preview (mesh, texture, audio, material)

### 22.4 Console & Logging
- [ ] `ConsolePanel` — filterable log output
- [ ] `SimpleConsolePanel` — simple console variant
- [ ] `SparkConsole` — command input and execution
- [ ] Command auto-completion
- [ ] Console history

---

## 23. Editor — Specialized Tools

### 23.1 Gizmos (`SparkEditor/Source/Gizmos/`)
- [ ] `GizmoSystem` — translate, rotate, scale gizmos
- [ ] Snap to grid / angle snap
- [ ] Local vs world space gizmo modes
- [ ] Multi-object transform

### 23.2 Material Editor (`SparkEditor/Source/MaterialEditor/`)
- [ ] `MaterialEditor` — visual material property editing
- [ ] Material preview sphere
- [ ] Texture slot assignment
- [ ] Shader parameter tweaking
- [ ] Node-based material graph (visual shader)

### 23.3 Terrain Editor (`SparkEditor/Source/Terrain/`)
- [ ] `TerrainEditor` — heightmap sculpting tools
- [ ] Raise, lower, smooth, flatten brushes
- [ ] Texture painting / splatmap editing
- [ ] Terrain LOD preview
- [ ] Vegetation and foliage placement

### 23.4 Animation Timeline (`SparkEditor/Source/Animation/`)
- [ ] `AnimationTimeline` — keyframe editing
- [ ] Curve editor for animation easing
- [ ] Animation preview and scrubbing
- [ ] Animation event placement
- [ ] Blend tree visualization

### 23.5 Lighting Tools (`SparkEditor/Source/Lighting/`)
- [ ] `LightingTools` — light placement and configuration
- [ ] Lightmap baking interface
- [ ] Light probe placement
- [ ] Shadow configuration per light
- [ ] Lighting scenario comparison (A/B)

### 23.6 Visual Scripting (`SparkEditor/Source/VisualScripting/`)
- [ ] `VisualScriptingSystem` — node graph editor (Imnodes)
- [ ] Variable nodes (get/set)
- [ ] Flow control nodes (branch, loop, sequence)
- [ ] Event nodes (OnBeginPlay, OnTick, OnCollision)
- [ ] Blueprint-to-AngelScript compilation
- [ ] Debugging (breakpoints, value inspection)

### 23.7 Level Streaming (`SparkEditor/Source/LevelStreaming/`)
- [ ] `LevelStreamingSystem` — streaming volume definition
- [ ] Level loading/unloading by proximity
- [ ] Persistent level designation
- [ ] Streaming debug visualization

### 23.8 FPS Tools (`SparkEditor/Source/Panels/`)
- [ ] `FPSToolsPanel` — FPS-specific editing tools
- [ ] `WeaponEditorPanel` — weapon stats editing and preview
- [ ] Weapon attachment configuration
- [ ] Recoil pattern editor

---

## 24. Editor — Build & Deployment

### 24.1 Build System (`SparkEditor/Source/BuildSystem/`)
- [ ] `SimpleBuildSystem` — project compilation
- [ ] `BuildDeploymentSystem` — packaging and deployment
- [ ] Platform-specific build profiles
- [ ] Asset cooking and packaging
- [ ] Build log and error reporting

### 24.2 Version Control (`SparkEditor/Source/VersionControl/`)
- [ ] `VersionControlSystem` — Git integration
- [ ] File status display (modified, added, conflicted)
- [ ] Commit, push, pull from editor
- [ ] Diff viewer
- [ ] Merge conflict resolution

### 24.3 Profiler (`SparkEditor/Source/Profiler/`)
- [ ] `PerformanceProfiler` — profiling UI panel
- [ ] CPU timeline view
- [ ] GPU timeline view
- [ ] Memory allocation view
- [ ] Frame comparison

---

## 25. Editor — Scene Serialization

### 25.1 Scene System (`SparkEditor/Source/SceneSystem/`)
- [ ] `SceneManager` (Editor) — editor scene lifecycle
- [ ] `SceneFile` — scene file format (.spark)
- [ ] `SceneSerializer` — entity/component serialization
- [ ] Undo/redo for scene modifications
- [ ] Scene diffing and merging

---

## 26. Game Module (SparkGame)

### 26.1 Core Game (`SparkGame/Source/Core/`)
- [ ] `SparkGame` — game module implementing `IGameModule`
- [ ] `Main.cpp` — entry point and module registration

### 26.2 Gameplay Systems (`SparkGame/Source/Game/`)
- [ ] `Game` — main game class, game loop
- [ ] `GameMode` — game mode management (deathmatch, CTF, etc.)
- [ ] `Player` — FPS player controller (movement, look, interact)
- [ ] `ClassSystem` — player class/loadout system
- [ ] `HUDSystem` — crosshair, health, ammo, minimap
- [ ] `InventorySystem` — item management, equip slots
- [ ] `QuestSystem` — quest tracking, objectives, rewards
- [ ] `GravitySystem` — gravity and falling mechanics
- [ ] `VehicleSystem` — vehicle driving and physics
- [ ] `GameMechanics` — core mechanics (damage, scoring, respawn)
- [ ] `InteractiveObject` — world interactables (doors, buttons, pickups)
- [ ] `Terrain` — runtime terrain rendering with LOD
- [ ] `Console` — in-game developer console

### 26.3 Game Objects (`SparkGame/Source/Game/`)
- [ ] `CubeObject`, `SphereObject`, `PlaneObject` — primitive objects
- [ ] `PyramidObject`, `RampObject`, `WallObject` — structural objects
- [ ] `ModelObject` — model-based game objects

### 26.4 Weapons & Projectiles (`SparkGame/Source/Projectiles/`)
- [ ] `Projectile` — base projectile class
- [ ] `Bullet` — hitscan/ballistic bullet
- [ ] `Grenade` — grenade with fuse and explosion
- [ ] `Rocket` — rocket projectile
- [ ] `ProjectilePool` — object pool for projectiles
- [ ] `WeaponStats` — damage, fire rate, spread, recoil

### 26.5 Console Commands (`SparkGame/Source/Console/`)
- [ ] `AdvancedConsoleCommands` — cheat/debug commands

---

## 27. SDK & Tools

### 27.1 SparkSDK (`SparkSDK/`)
- [ ] `SparkSDK.h` — public SDK header
- [ ] `IEngineContext` — engine context interface for plugins
- [ ] `IModule` — module plugin interface
- [ ] `ModuleRegistry` — module discovery and registration
- [ ] `Version.h` — engine version info
- [ ] SDK documentation and samples

### 27.2 SparkConsole (`SparkConsole/`)
- [ ] `ConsoleApp` — standalone console application
- [ ] `CommandParser` — command-line parsing
- [ ] `CommandRegistry` — command registration and dispatch
- [ ] Remote engine connection

### 27.3 SparkShaderCompiler (`SparkShaderCompiler/`)
- [ ] Offline shader compilation
- [ ] Shader permutation generation
- [ ] Shader reflection and metadata output
- [ ] Cross-platform shader transpilation (HLSL → GLSL/SPIR-V)

---

## 28. Testing

### 28.1 Unit Tests (`Tests/`)
- [ ] Animation system tests
- [ ] AI / behavior tree / NavMesh tests
- [ ] ECS component and system tests
- [ ] Physics component tests
- [ ] Graphics tests (frustum culling, LOD, post-processing, lighting)
- [ ] Input system tests
- [ ] Audio engine tests
- [ ] Save system tests
- [ ] Quest and inventory system tests
- [ ] Weather and day/night cycle tests
- [ ] Profiling and performance tests
- [ ] String, math, and file utility tests
- [ ] Configuration parsing tests
- [ ] Coroutine scheduling tests
- [ ] Event system tests
- [ ] Networking tests

### 28.2 Test Infrastructure
- [ ] CTest integration
- [ ] CI/CD pipeline
- [ ] Code coverage reporting
- [ ] Automated regression testing
- [ ] Performance benchmark suite
- [ ] Stress / fuzz testing

---

## 29. Cross-Platform & Portability

- [ ] Windows (MSVC) — primary platform
- [ ] Linux (GCC/Clang) — experimental
- [ ] macOS (Clang) — experimental
- [ ] CMake preset for each platform
- [ ] Platform-specific abstractions in `Platform.h`
- [ ] Graphics backend selection per platform
- [ ] Input backend selection per platform
- [ ] Audio backend selection per platform (XAudio2 vs OpenAL vs PulseAudio)
- [ ] Console/mobile (future)

---

## 30. Build System & Project Structure

- [ ] CMake 3.16+ with presets (windows-release, linux-release)
- [ ] C++20 requirement enforced
- [ ] Feature toggles: `ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_PHYSX`, `ENABLE_AI`, `ENABLE_ANIMATION`, `ENABLE_NETWORKING`
- [ ] Third-party dependency management (`ThirdParty/`)
- [ ] Project templates (`Templates/EmptyProject/`)
- [ ] `.clang-format` enforcing code style
- [ ] Zero-warning builds (`/W4`, `-Wall -Wextra`)

---

## How to Use This Template

1. **Copy** this file to `<SYSTEM>_GAP_ANALYSIS.md`.
2. **Delete** sections not relevant to the system you are auditing.
3. **Fill in** each checkbox with a status tag and brief note, e.g.:
   ```
   - [x] `PhysicsSystem` — **DONE** — Bullet 3 integrated, stepping, debug draw
   - [ ] Joint/constraint support — **STUB** — header exists, no implementation
   - [ ] Continuous collision detection — **MISSING**
   ```
4. **Summarize** findings at the top: total DONE / PARTIAL / STUB / MISSING counts.
5. **Prioritize** items to address in a "Next Steps" section at the bottom.
