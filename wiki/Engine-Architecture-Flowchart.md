# Engine Architecture Flowchart

> Complete visual guide to how SparkEngine works, from boot to shutdown.

<!-- AUTO:flowchart_stats -->
_Generated 2026-04-04 from 553 headers, 375 source files, 79 ECS components, 11 ECS systems, 57 editor panels, 6 RHI backends._
<!-- /AUTO:flowchart_stats -->

---

## Table of Contents

1. [Top-Level Overview](#1-top-level-overview)
2. [Startup & Initialization](#2-startup--initialization)
3. [Main Game Loop (Per-Frame)](#3-main-game-loop-per-frame)
4. [ECS Pipeline](#4-ecs-pipeline)
5. [Rendering Pipeline](#5-rendering-pipeline)
6. [RHI Abstraction Layer](#6-rhi-abstraction-layer)
7. [Physics Pipeline (Jolt)](#7-physics-pipeline-jolt)
8. [Audio Pipeline (XAudio2)](#8-audio-pipeline-xaudio2)
9. [AI & Navigation](#9-ai--navigation)
10. [Animation System](#10-animation-system)
11. [Networking Architecture](#11-networking-architecture)
12. [Scripting (AngelScript)](#12-scripting-angelscript)
13. [World Streaming & Origin Rebasing](#13-world-streaming--origin-rebasing)
14. [Editor Architecture (SparkEditor)](#14-editor-architecture-sparkeditor)
15. [Game Module Lifecycle](#15-game-module-lifecycle)
16. [Supporting Subsystems](#16-supporting-subsystems)
17. [Shutdown Sequence](#17-shutdown-sequence)
18. [SparkConsole (External Process)](#18-sparkconsole-external-process)

---


## 1. Top-Level Overview

Three executables and dynamically loaded game modules, all connected through `EngineContext` as the central service locator.

```
 ┌──────────────────────────────────────────────────────────────────┐
 │                    SparkEngine Ecosystem                         │
 └──────────────────────────────────────────────────────────────────┘

 ┌────────────────────┐  ┌────────────────────┐  ┌────────────────┐
 │   SparkEngine.exe  │  │  SparkEditor.exe   │  │ SparkConsole   │
 │   (Runtime Host)   │  │  (Editor + Engine) │  │ (External UI)  │
 │                    │  │                    │  │                │
 │ - Win32/SDL2 host  │  │ - Dear ImGui UI    │  │ - Pipe I/O     │
 │ - Windowed or      │  │ - 57 editor panels │  │ - Command input │
 │   headless mode    │  │ - Engine embedded  │  │ - Log display  │
 │ - Game loop owner  │  │ - Live viewport    │  │                │
 └────────┬───────────┘  └────────┬───────────┘  └───────┬────────┘
          │                       │                       │
          │    ┌──────────────────┘          stdin/stdout  │
          │    │                              pipes        │
          ▼    ▼                                           │
 ┌──────────────────────────────────────────┐              │
 │         EngineContext (Service Locator)   │◄─────────────┘
 │                                          │   (via ConsoleProcessManager)
 │  GetGraphics()    → GraphicsEngine*      │
 │  GetInput()       → InputManager*        │
 │  GetPhysics()     → PhysicsSystem*       │
 │  GetAudio()       → AudioEngine*         │
 │  GetAI()          → AISystem*            │
 │  GetAnimation()   → AnimationSystem*     │
 │  GetNetwork()     → NetworkManager*      │
 │  GetScriptEngine()→ AngelScriptEngine*   │
 │  GetSaveSystem()  → SaveSystem*          │
 │  GetTimer()       → Timer*               │
 │  GetEventBus()    → EventBus*            │
 │  GetSceneManager()→ SceneManager*        │
 │  GetCoroutineScheduler()→ Scheduler*     │
 │                                          │
 │  RegisterSystem<T>() / GetSystem<T>()    │
 └────────────────────┬─────────────────────┘
                      │
        ┌─────────────┼──────────────┐
        ▼             ▼              ▼
 ┌────────────┐ ┌───────────┐ ┌────────────┐
 │ SparkGame  │ │ Module 2  │ │  Module N  │
 │  (IModule) │ │ (IModule) │ │  (IModule) │
 │  .dll/.so  │ │ .dll/.so  │ │  .dll/.so  │
 └────────────┘ └───────────┘ └────────────┘
```

**Key files:**
- `SparkEngine/Source/Core/SparkEngine.cpp` — Runtime host entry point
- `SparkEngine/Source/Core/EngineContext.h` — Service locator singleton
- `SparkEditor/Source/main.cpp` — Editor entry point
- `SparkConsole/src/main.cpp` — Console entry point
- `SparkEngine/Source/Core/ModuleManager.cpp` — Dynamic module loading


## 2. Startup & Initialization

The engine boots through a strict initialization order. Dependencies flow top-down — each step requires the ones above it.

```
                        ┌──────────┐
                        │  main()  │
                        └────┬─────┘
                             │
                    ┌────────▼────────┐
                    │ SetupCrashHandler│
                    └────────┬────────┘
                             │
                    ┌────────▼─────────────┐
                    │  ParseCommandLine    │
                    │  -headless, -test,   │
                    │  -window-size, etc.  │
                    └────────┬─────────────┘
                             │
                ┌────────────┴────────────┐
                ▼                         ▼
    ┌───────────────────┐    ┌────────────────────────┐
    │   WINDOWED PATH   │    │     HEADLESS PATH      │
    └────────┬──────────┘    └───────────┬────────────┘
             │                           │
    ┌────────▼──────────┐    ┌───────────▼────────────┐
    │  CreateWindow      │    │  AllocConsole           │
    │  (Win32/SDL2)      │    └───────────┬────────────┘
    └────────┬──────────┘                 │
             │                  ┌─────────▼──────────────┐
    ┌────────▼──────────┐       │ InitHeadlessContext     │
    │ InitGraphics       │       │ (no GPU, no input)      │
    │ (RHI backend)      │       └─────────┬──────────────┘
    └────────┬──────────┘                  │
             │                  ┌──────────▼──────────────┐
    ┌────────▼──────────┐       │ InitSaveSystem           │
    │ InitInput          │       └──────────┬──────────────┘
    │ (InputManager)     │                  │
    └────────┬──────────┘       ┌──────────▼──────────────┐
             │                  │ InitConsole               │
    ┌────────▼──────────┐       └──────────┬──────────────┘
    │ InitTimer          │                  │
    └────────┬──────────┘       ┌──────────▼──────────────┐
             │                  │ LoadHeadlessModules       │
    ┌────────▼──────────────┐   └──────────┬──────────────┘
    │ InitEngineContext      │              │
    │ (create singleton)     │   ┌──────────▼──────────────┐
    └────────┬──────────────┘   │ Fixed 60Hz Tick Loop     │
             │                  │ (sleep-regulated)         │
    ┌────────▼──────────────┐   └─────────────────────────┘
    │ RegisterCoreSubsystems│
    │ (via EngineSetup)     │
    └────────┬──────────────┘
             │
    ┌────────▼──────────────┐
    │ InitJobSystem          │
    │ (thread pool)          │
    └────────┬──────────────┘
             │
    ┌────────▼──────────────────────────┐
    │ InitGameplaySubsystems             │
    │ WeatherSystem, TimeOfDaySystem,    │
    │ UISystem, DialogueSystem, ModSystem│
    └────────┬──────────────────────────┘
             │
    ┌────────▼──────────────┐
    │ InitSaveSystem         │
    │ + CoroutineScheduler   │
    └────────┬──────────────┘
             │
    ┌────────▼──────────────┐
    │ InitAudioEngine        │
    │ (XAudio2)              │
    └────────┬──────────────┘
             │
    ┌────────▼──────────────┐
    │ LoadAndInitModules     │
    │ (dlopen / LoadLibrary) │
    │ Call OnLoad() each     │
    └────────┬──────────────┘
             │
    ┌────────▼───────────────────────────┐
    │ InitConsole                         │
    │ ├─ SimpleConsole::Initialize()      │
    │ ├─ ConsoleProcessManager (spawn     │
    │ │   SparkConsole.exe subprocess)    │
    │ ├─ InitDebugSystems()               │
    │ └─ InitGameplaySystems()            │
    └────────┬───────────────────────────┘
             │
    ┌────────▼──────────────────┐
    │ Publish EngineStartEvent   │
    └────────┬──────────────────┘
             │
    ┌────────▼──────────────────┐
    │    MAIN LOOP READY        │
    └───────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Core/SparkEngine.cpp` — `wWinMain()`, `main()`, `InitInstance()`, `InitEngineContext()`
- `SparkEngine/Source/Core/EngineBootstrap.h` — `EngineSetup::RegisterCoreSubsystems()`
- `SparkEngine/Source/Core/GameplaySystemLifecycle.cpp` — `InitGameplaySystems()`
- `SparkEngine/Source/Utils/ConsoleProcessManager.h` — subprocess management


## 3. Main Game Loop (Per-Frame)

Every frame follows this exact sequence. Each subsystem update is wrapped in `SPARK_GUARDED_UPDATE()` for exception isolation — a crash in one system won't take down the engine.

```
 ┌──────────────────────────────────────────────────────────┐
 │                    FRAME START                            │
 └────────────────────────┬─────────────────────────────────┘
                          │
              ┌───────────▼───────────┐
              │ PeekMessage() /       │  ◄── Win32 or SDL2
              │ SDL_PollEvent()       │      non-blocking
              │ (process OS events)   │
              └───────────┬───────────┘
                          │
              ┌───────────▼───────────┐
              │ Timer::GetDeltaTime() │
              └───────────┬───────────┘
                          │
              ┌───────────▼───────────┐
              │ DeltaSmoother         │  ◄── Prevents physics
              │ (smooth frame spikes) │      jitter from spikes
              └───────────┬───────────┘
                          │
              ┌───────────▼────────────────┐
              │ FixedTimestepAccumulator   │  ◄── Deterministic
              │ .Advance(smoothedDt)       │      fixed-rate updates
              └───────────┬────────────────┘
                          │
              ┌───────────▼───────────┐
              │ InputManager::Update() │
              │ (keyboard, mouse,     │
              │  gamepad polling)      │
              └───────────┬───────────┘
                          │
              ┌───────────▼───────────────────┐
              │ ModuleManager::UpdateAll(dt)   │  ◄── Game modules
              │ (calls OnUpdate + OnFixedUpdate│      get first crack
              │  on each loaded IModule)       │
              └───────────┬───────────────────┘
                          │
              ┌───────────▼───────────────────┐
              │ ModuleManager::RenderAll()     │
              │ (calls OnRender on each module)│
              └───────────┬───────────────────┘
                          │
              ┌───────────▼────────────────────┐
              │ PollModuleHotReload()           │  ◄── Check for
              │ (detect changed DLLs, reload)   │      recompiled modules
              └───────────┬────────────────────┘
                          │
              ┌───────────▼────────────────────┐
              │ UpdateGameplaySystems(dt)       │
              │ ┌────────────────────────────┐  │
              │ │ Physics → Animation → AI   │  │
              │ │ → Audio → Lifecycle        │  │
              │ │ → Particles → Projectiles  │  │
              │ │ → Scripting → Coroutines   │  │
              │ │ → Weather → TimeOfDay      │  │
              │ │ → Streaming → Networking   │  │
              │ └────────────────────────────┘  │
              └───────────┬────────────────────┘
                          │
              ┌───────────▼────────────────────┐
              │ UpdateDebugSystems(dt)          │
              │ (profiler, debug draw,          │
              │  performance stats)             │
              └───────────┬────────────────────┘
                          │
              ┌───────────▼────────────────────┐
              │ ConsoleProcessManager           │
              │ ::ProcessCommands()             │
              │ (read pipe from SparkConsole)   │
              └───────────┬────────────────────┘
                          │
              ┌───────────▼───────────┐
              │ Console::Update()     │
              │ (flush log messages   │
              │  to SparkConsole)     │
              └───────────┬───────────┘
                          │
              ┌───────────▼───────────┐
              │ Increment frame count │
              └───────────┬───────────┘
                          │
              ┌───────────▼───────────┐
              │      FRAME END        │
              │   (loop back to top)  │
              └───────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Core/SparkEngine.cpp` — `RunWindowedMainLoop()`, `RunSDL2MainLoop()`, `TickFrame()`


## 4. ECS Pipeline

SparkEngine uses [EnTT](https://github.com/skypjack/entt) as its entity-component-system backbone. The `SystemManager` owns all systems and calls them in a strict order each frame.

### System Execution Order

```
 ┌─────────────────────────────────────────────────────────────┐
 │                  EnTT Registry (World)                       │
 │  Entities are just IDs. Components are data. Systems run.    │
 └──────────────────────────┬──────────────────────────────────┘
                            │
         SystemManager::UpdateAll(dt) called each frame
                            │
     ┌──────────────────────▼──────────────────────────┐
     │              CORE SYSTEMS (ordered)              │
     │                                                  │
     │  ┌─────────────────────────────────────────┐     │
     │  │ 1. PhysicsUpdateSystem                  │     │
     │  │    Step Jolt simulation                  │     │
     │  │    Write → Transform, RigidBody          │     │
     │  └──────────────────┬──────────────────────┘     │
     │                     ▼                            │
     │  ┌─────────────────────────────────────────┐     │
     │  │ 2. AnimationUpdateSystem                │     │
     │  │    Evaluate skeleton + state machines    │     │
     │  │    Write → bone matrices                 │     │
     │  └──────────────────┬──────────────────────┘     │
     │                     ▼                            │
     │  ┌─────────────────────────────────────────┐     │
     │  │ 3. AIUpdateSystem                       │     │
     │  │    Tick behavior trees                   │     │
     │  │    Read → Transform; Write → Velocity    │     │
     │  └──────────────────┬──────────────────────┘     │
     │                     ▼                            │
     │  ┌─────────────────────────────────────────┐     │
     │  │ 4. AudioUpdateSystem                    │     │
     │  │    Update 3D source positions            │     │
     │  │    Read → Transform, AudioSource         │     │
     │  └──────────────────┬──────────────────────┘     │
     │                     ▼                            │
     │  ┌─────────────────────────────────────────┐     │
     │  │ 5. LifecycleSystem                      │     │
     │  │    Process health, death, active states  │     │
     │  │    Read/Write → Health, Active           │     │
     │  └──────────────────┬──────────────────────┘     │
     │                     ▼                            │
     │  ┌─────────────────────────────────────────┐     │
     │  │ 6. RenderSystem                         │     │
     │  │    Submit draw calls to GPU              │     │
     │  │    Read → Transform, MeshRenderer        │     │
     │  └─────────────────────────────────────────┘     │
     │                                                  │
     │           AUXILIARY SYSTEMS (also ordered)        │
     │  ┌─────────────────────────────────────────┐     │
     │  │ ParticleUpdateSystem (emitter advance)   │     │
     │  │ DecalSystem (lifetime / fade-out)        │     │
     │  │ ProjectileSystem (movement + gravity)    │     │
     │  │ SplineFollowerSystem (path advancement)  │     │
     │  │ AbilityUpdateSystem (cooldown ticks)     │     │
     │  └─────────────────────────────────────────┘     │
     └──────────────────────────────────────────────────┘
```

### Component Categories

```
 ┌─ CORE ───────────────────────────────────────────────┐
 │ Transform, NameComponent, ActiveComponent, TagComponent│
 │ MeshRenderer, Camera, Camera2D, Script               │
 └──────────────────────────────────────────────────────┘
 ┌─ PHYSICS ────────────────────────────────────────────┐
 │ RigidBodyComponent, ColliderComponent,               │
 │ CharacterControllerComponent, PhysicsJointComponent, │
 │ RagdollComponent, SoftBodyComponent, VehicleComponent│
 └──────────────────────────────────────────────────────┘
 ┌─ GAMEPLAY ───────────────────────────────────────────┐
 │ HealthComponent, AbilityComponent, ProjectileComponent│
 │ DestructibleComponent, InteractionComponent,         │
 │ InventoryTag, QuestTrackerTag, WeatherComponent      │
 └──────────────────────────────────────────────────────┘
 ┌─ AI & NAV ───────────────────────────────────────────┐
 │ AIComponent, NavRegionComponent, NavLinkComponent,   │
 │ NavObstacleComponent, SpawnPointComponent,           │
 │ CoverPointComponent, TacticalPointComponent          │
 └──────────────────────────────────────────────────────┘
 ┌─ AUDIO ──────────────────────────────────────────────┐
 │ AudioSourceComponent, AudioListenerComponent,        │
 │ AudioReverbZoneComponent                             │
 └──────────────────────────────────────────────────────┘
 ┌─ ANIMATION ──────────────────────────────────────────┐
 │ AnimationController, ParticleEmitterComponent,       │
 │ SpriteAnimator, SpriteAnimationClip                  │
 └──────────────────────────────────────────────────────┘
 ┌─ LIGHTING & FX ──────────────────────────────────────┐
 │ LightComponent, LightProbeComponent,                 │
 │ ReflectionProbeComponent, PostProcessVolumeComponent,│
 │ DecalComponent, FogVolumeComponent, WindZoneComponent│
 └──────────────────────────────────────────────────────┘
 ┌─ TERRAIN & WORLD ────────────────────────────────────┐
 │ TerrainComponent, AreaBoundaryComponent,             │
 │ FoliageVolumeComponent, WaterPlaneComponent,         │
 │ BuoyancyVolumeComponent, ForceRegionComponent        │
 └──────────────────────────────────────────────────────┘
 ┌─ 2D ─────────────────────────────────────────────────┐
 │ SpriteRenderer, RigidBody2D, Collider2D,             │
 │ TilemapComponent, NineSliceSprite, ParallaxLayer     │
 └──────────────────────────────────────────────────────┘
 ┌─ SPECIAL ────────────────────────────────────────────┐
 │ NetworkIdentity, LODGroupComponent, OccluderComponent│
 │ SpringArmComponent, CinematicTriggerComponent,       │
 │ DialogueTriggerComponent, TriggerVolumeComponent     │
 └──────────────────────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` — System definitions and execution order
- `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` — Transform, MeshRenderer, Camera, Script
- `SparkEngine/Source/Engine/ECS/Components/` — All 12+ domain component headers


## 5. Rendering Pipeline

The deferred rendering pipeline uses a render graph for automatic resource management and pass ordering.

```
 ┌─────────────────────────────────────────────────────────────┐
 │                 ECS: RenderSystem::Update()                  │
 │          Read Transform + MeshRenderer for each entity       │
 └─────────────────────────┬───────────────────────────────────┘
                           │
               ┌───────────▼───────────────┐
               │ SceneRenderer::Submit()    │
               │ (accumulate draw commands) │
               └───────────┬───────────────┘
                           │
               ┌───────────▼───────────────────────────┐
               │ GraphicsEngine::BeginFrame()           │
               │ ├─ ConstantBufferRing.BeginFrame()     │
               │ ├─ GPUTimestampQuery.BeginFrame()      │
               │ ├─ ShadowAtlas.BeginFrame()            │
               │ └─ UpdateFrameConstants(view,proj,cam) │
               └───────────┬───────────────────────────┘
                           │
               ┌───────────▼───────────────────────────┐
               │ SceneRenderer::CullAndSort()           │
               │ ├─ BVH frustum culling                 │
               │ ├─ Distance-based LOD assignment        │
               │ └─ Material-based draw call sorting     │
               └───────────┬───────────────────────────┘
                           │
          ┌────────────────▼────────────────┐
          │   Build & Execute RenderGraph    │
          │   (compile → topological sort    │
          │    → dead-code elimination       │
          │    → resource aliasing           │
          │    → execute in dependency order) │
          └────────────────┬────────────────┘
                           │
     ┌─────────────────────▼──────────────────────────┐
     │               RENDER PASSES                     │
     │                                                 │
     │  ┌───────────────────────────────────────┐      │
     │  │ 1. SHADOW PASS                        │      │
     │  │    For each shadow-casting light:      │      │
     │  │    ├─ Directional → Cascaded SM (CSM)  │      │
     │  │    ├─ Point → Cubemap (6-face)         │      │
     │  │    └─ Spot → Single depth map          │      │
     │  │    Techniques: PCF, VSM, PCSS          │      │
     │  │    → Write to ShadowAtlas tiles        │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 2. GEOMETRY PASS (G-Buffer Fill)      │      │
     │  │    Render all visible meshes into:     │      │
     │  │    ├─ RT0: Albedo (RGB) + AO (A)       │      │
     │  │    ├─ RT1: Normal (RGB) + Roughness(A) │      │
     │  │    ├─ RT2: Material (Metallic, etc.)   │      │
     │  │    └─ RT3: Motion Vectors              │      │
     │  │    Bind PBR materials per draw call     │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 3. LIGHTING PASS (Full-Screen)        │      │
     │  │    ├─ Read G-buffers as SRVs           │      │
     │  │    ├─ ClusteredLightCulling::Update()  │      │
     │  │    │   ├─ Subdivide frustum (16x8x24)  │      │
     │  │    │   ├─ Assign lights to clusters     │      │
     │  │    │   └─ Upload cluster data to GPU    │      │
     │  │    ├─ Resolve accumulated lighting      │      │
     │  │    └─ Apply shadow atlas lookups        │      │
     │  │    → Output: HDR color texture          │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 4. TRANSPARENT PASS                   │      │
     │  │    Alpha-blended geometry (sorted)     │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 5. POST-PROCESSING CHAIN              │      │
     │  │                                       │      │
     │  │  SSAO (hemisphere sampling)            │      │
     │  │           │                            │      │
     │  │           ▼                            │      │
     │  │  SSR (ray march + HiZ refinement)      │      │
     │  │           │                            │      │
     │  │           ▼                            │      │
     │  │  VolumetricFog (froxel 160x90x64)      │      │
     │  │  ├─ InjectMedia (density/scatter)      │      │
     │  │  ├─ EvaluateLight (Henyey-Greenstein)  │      │
     │  │  └─ TemporalFilter (motion vectors)    │      │
     │  │           │                            │      │
     │  │           ▼                            │      │
     │  │  Bloom (downsample → upsample chain)   │      │
     │  │           │                            │      │
     │  │           ▼                            │      │
     │  │  Tonemapping + Color Grading            │      │
     │  │  (ACES/Filmic/Reinhard/Neutral)        │      │
     │  │  + Auto-exposure (histogram)            │      │
     │  │           │                            │      │
     │  │           ▼                            │      │
     │  │  TAA (temporal anti-aliasing)           │      │
     │  │  + FXAA, DoF, Vignette, Film Grain     │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 6. TERRAIN PASS (ClipmapTerrain)      │      │
     │  │    ├─ Concentric LOD rings around cam   │      │
     │  │    ├─ Cell snapping for coherence        │      │
     │  │    ├─ GenerateMesh() per level           │      │
     │  │    └─ Heightmap sampling → GPU upload    │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 7. GPU OCCLUSION CULLING (optional)   │      │
     │  │    ├─ Build HiZ mip chain from depth   │      │
     │  │    ├─ Compute shader: cull AABBs        │      │
     │  │    └─ DrawIndexedInstancedIndirect      │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 8. UI PASS (ImGui / HUD)              │      │
     │  └──────────────────┬────────────────────┘      │
     │                     ▼                           │
     │  ┌───────────────────────────────────────┐      │
     │  │ 9. DEBUG PASS (wireframe, normals,    │      │
     │  │    light counts, depth viz)            │      │
     │  └─────────────────────────────────────────┘     │
     └─────────────────────┬──────────────────────────┘
                           │
               ┌───────────▼───────────────────────────┐
               │ GraphicsEngine::EndFrame()             │
               │ ├─ ConstantBufferRing.EndFrame()       │
               │ ├─ GPUTimestampQuery.EndFrame()        │
               │ ├─ ShadowAtlas.EndFrame() (evict stale)│
               │ └─ RHIBridge::Present(vsync)           │
               └───────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Graphics/GraphicsEngine.h` — Frame orchestration
- `SparkEngine/Source/Graphics/RenderPipeline.h` — Render graph execution
- `SparkEngine/Source/Graphics/RenderGraph.h` — DAG engine
- `SparkEngine/Source/Graphics/ShadowAtlas.h` — Shadow atlas management
- `SparkEngine/Source/Graphics/ClusteredLightCulling.h` — Light assignment
- `SparkEngine/Source/Graphics/PostProcessingPipeline.h` — Effect chain
- `SparkEngine/Source/Graphics/ClipmapTerrain.h` — Terrain LOD
- `SparkEngine/Source/Graphics/GPUOcclusionCulling.h` — HiZ culling
- `SparkEngine/Source/Graphics/GPUDrivenRenderer.h` — GPU-driven pipeline


## 6. RHI Abstraction Layer

The Rendering Hardware Interface abstracts all GPU APIs behind a unified interface. Backend selection happens once at startup.

```
 ┌───────────────────────────────────────────────┐
 │     RHIFactory::DetectAvailableBackends()     │
 │     (probe system for supported APIs)          │
 └────────────────────┬──────────────────────────┘
                      │
 ┌────────────────────▼──────────────────────────┐
 │     RHIFactory::GetRecommendedBackend()       │
 │     (platform-specific preference)             │
 └────────────────────┬──────────────────────────┘
                      │
 ┌────────────────────▼──────────────────────────┐
 │     RHIFactory::CreateDevice(backend)          │
 │     Returns IRHIDevice*                        │
 └────────────────────┬──────────────────────────┘
                      │
     ┌────────────────┼────────────────────────┐
     │                │                        │
     ▼                ▼                        ▼
 ┌─────────┐  ┌────────────┐  ┌────────────────────┐
 │ D3D11   │  │   D3D12    │  │     Vulkan         │
 │ Device  │  │   Device   │  │     Device         │
 │(primary)│  │(+DXR rays) │  │  (cross-platform)  │
 └─────────┘  └────────────┘  └────────────────────┘
     ▼                ▼                        ▼
 ┌─────────┐  ┌────────────┐  ┌────────────────────┐
 │ OpenGL  │  │   Metal    │  │    NullRHI         │
 │ Device  │  │   Device   │  │    Device          │
 │ (Linux) │  │  (macOS)   │  │  (headless/test)   │
 └─────────┘  └────────────┘  └────────────────────┘

 GPU-less Fallbacks:
 ┌───────────────────────────────────────────────┐
 │ D3D11/D3D12 → WARP (software rasterizer)      │
 │ Vulkan      → Lavapipe (Mesa)                  │
 │ OpenGL      → llvmpipe (Mesa)                  │
 │ No GPU      → NullRHIDevice (headless)         │
 └───────────────────────────────────────────────┘
```

### Abstract Interfaces

```
 ┌─────────────────────────────────────────────────┐
 │              IRHIDevice                          │
 │  CreateBuffer(), CreateTexture(),                │
 │  CreateShader(), CreateSwapChain(),              │
 │  CreateCommandList()                             │
 └──────────────────────┬──────────────────────────┘
                        │ creates
     ┌──────────┬───────┼───────┬──────────┐
     ▼          ▼       ▼       ▼          ▼
 ┌────────┐┌────────┐┌──────┐┌────────┐┌──────────┐
 │IRHIBuf ││IRHITex ││IRHIS ││IRHICmd ││IRHISwap  │
 │ffer   ││ture   ││hader ││List    ││Chain     │
 └────────┘└────────┘└──────┘└────────┘└──────────┘
```

**Key files:**
- `SparkEngine/Source/Graphics/RHI/RHIFactory.h` — Backend detection and creation
- `SparkEngine/Source/Graphics/RHI/RHIDevice.h` — Abstract device interface
- `SparkEngine/Source/Graphics/RHI/RHIBridge.h` — Integration bridge
- `SparkEngine/Source/Graphics/RHI/D3D11/D3D11Device.h` — Primary backend


## 7. Physics Pipeline (Jolt)

Jolt Physics runs in the `PhysicsUpdateSystem` at a fixed timestep. Results are written back to ECS `Transform` components.

```
 ┌──────────────────────────────────────────────────────┐
 │          PhysicsUpdateSystem::Update(dt)              │
 └───────────────────────┬──────────────────────────────┘
                         │
             ┌───────────▼───────────────┐
             │ JoltPhysicsSystem          │
             │ ::StepSimulation(fixedDt)  │
             └───────────┬───────────────┘
                         │
     ┌───────────────────▼───────────────────────┐
     │              SIMULATION STEP               │
     │                                            │
     │  ┌──────────────────────────────────┐      │
     │  │ 1. BroadPhase                    │      │
     │  │    AABB overlap detection         │      │
     │  │    (spatial partitioning)          │      │
     │  └──────────────┬───────────────────┘      │
     │                 ▼                          │
     │  ┌──────────────────────────────────┐      │
     │  │ 2. NarrowPhase                   │      │
     │  │    Exact collision detection      │      │
     │  │    GJK/EPA algorithms             │      │
     │  │    Generate contact manifolds     │      │
     │  └──────────────┬───────────────────┘      │
     │                 ▼                          │
     │  ┌──────────────────────────────────┐      │
     │  │ 3. Contact Solving               │      │
     │  │    Resolve penetrations           │      │
     │  │    Apply friction/restitution     │      │
     │  └──────────────┬───────────────────┘      │
     │                 ▼                          │
     │  ┌──────────────────────────────────┐      │
     │  │ 4. Constraint Solving            │      │
     │  │    Joints, ragdolls, vehicles     │      │
     │  │    Iterative solver               │      │
     │  └──────────────┬───────────────────┘      │
     │                 ▼                          │
     │  ┌──────────────────────────────────┐      │
     │  │ 5. Integrate Positions           │      │
     │  │    Apply velocities to positions  │      │
     │  │    Apply angular velocities       │      │
     │  └──────────────────────────────────┘      │
     └───────────────────┬───────────────────────┘
                         │
             ┌───────────▼───────────────────────┐
             │ Write Back to ECS                  │
             │ ├─ Transform.position ← body pos   │
             │ ├─ Transform.rotation ← body rot   │
             │ └─ RigidBody.velocity ← body vel   │
             └───────────────────────────────────┘

 Physics Components:
 ┌──────────────────────────────────────────────────┐
 │ RigidBodyComponent   │ ColliderComponent          │
 │ (mass, velocity,     │ (box, sphere, capsule,     │
 │  damping, gravity)   │  mesh, heightfield)        │
 ├──────────────────────┼───────────────────────────┤
 │ CharacterController  │ PhysicsJointComponent      │
 │ (ground detection,   │ (hinge, slider, ball,      │
 │  step height, slope) │  fixed, distance)          │
 ├──────────────────────┼───────────────────────────┤
 │ RagdollComponent     │ VehicleComponent           │
 │ (bone→body mapping)  │ (wheels, suspension)       │
 ├──────────────────────┼───────────────────────────┤
 │ SoftBodyComponent    │ ConstantForceComponent     │
 │ (deformable mesh)    │ (continuous force/torque)  │
 └──────────────────────┴───────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` — `PhysicsUpdateSystem`
- `SparkEngine/Source/Engine/ECS/Components/PhysicsComponents.h` — All physics components


## 8. Audio Pipeline (XAudio2)

Audio runs after physics and animation so that 3D sound positions are up to date.

```
 ┌───────────────────────────────────────────────┐
 │       AudioUpdateSystem::Update(dt)            │
 │    Read: AudioSourceComponent + Transform      │
 └──────────────────────┬────────────────────────┘
                        │
            ┌───────────▼───────────────┐
            │ Update 3D Positions       │
            │ (source pos from Transform│
            │  listener from Camera)    │
            └───────────┬───────────────┘
                        │
            ┌───────────▼───────────────┐
            │ AudioEngine::Update()     │
            │ ├─ Apply 3D spatialization│
            │ ├─ Apply distance atten.  │
            │ ├─ Apply reverb zones     │
            │ └─ Mix voices             │
            └───────────┬───────────────┘
                        │
            ┌───────────▼───────────────┐
            │ XAudio2::SubmitSource     │
            │ Buffer()                  │
            └───────────┬───────────────┘
                        │
            ┌───────────▼───────────────┐
            │ Mastering Voice → Output  │
            └───────────────────────────┘

 Audio Components:
 ┌────────────────────────────────────────────┐
 │ AudioSourceComponent                       │
 │ (clip, volume, pitch, loop, 3D/2D,        │
 │  min/max distance, rolloff)                │
 ├────────────────────────────────────────────┤
 │ AudioListenerComponent                     │
 │ (position, forward, up — usually on Camera)│
 ├────────────────────────────────────────────┤
 │ AudioReverbZoneComponent                   │
 │ (bounds, reverb preset, mix level)         │
 └────────────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` — `AudioUpdateSystem`
- `SparkEngine/Source/Engine/ECS/Components/AudioComponents.h`


## 9. AI & Navigation

The AI system uses behavior trees for decision making and Recast/Detour-style navmesh for pathfinding.

```
 ┌──────────────────────────────────────────────────────────┐
 │               AIUpdateSystem::Update(dt)                  │
 │           Read: AIComponent + Transform                   │
 └─────────────────────────┬────────────────────────────────┘
                           │
         ┌─────────────────▼──────────────────┐
         │      For each AI entity:           │
         │                                    │
         │  ┌──────────────────────────────┐  │
         │  │ PerceptionSystem::Update()   │  │
         │  │ ├─ Sight sense (ray/cone)    │  │
         │  │ ├─ Sound sense (radius)      │  │
         │  │ └─ Custom senses             │  │
         │  │                              │  │
         │  │ StimulusSource → AIComponent │  │
         │  │ (perceived enemies, sounds)  │  │
         │  └──────────────┬───────────────┘  │
         │                 ▼                  │
         │  ┌──────────────────────────────┐  │
         │  │ BehaviorTree::Tick()         │  │
         │  │                              │  │
         │  │     [Selector]               │  │
         │  │     ├─ [Sequence: Attack]    │  │
         │  │     │  ├─ HasTarget?         │  │
         │  │     │  ├─ InRange?           │  │
         │  │     │  └─ FireWeapon         │  │
         │  │     ├─ [Sequence: Chase]     │  │
         │  │     │  ├─ HasTarget?         │  │
         │  │     │  └─ MoveTo(target)     │  │
         │  │     └─ [Action: Patrol]      │  │
         │  │        └─ FollowPath         │  │
         │  └──────────────┬───────────────┘  │
         │                 ▼                  │
         │  ┌──────────────────────────────┐  │
         │  │ NavMesh Pathfinding          │  │
         │  │ ├─ NavMeshQuery::FindPath()  │  │
         │  │ ├─ Path smoothing            │  │
         │  │ └─ Steering → Velocity       │  │
         │  └──────────────┬───────────────┘  │
         │                 ▼                  │
         │  Write: Velocity, Target, State    │
         └────────────────────────────────────┘

 NavMesh Pipeline:
 ┌──────────────────────────────────────────────┐
 │ Static geometry → Recast voxelization        │
 │ → Contour tracing → Polygon mesh generation  │
 │ → NavMesh baked (offline or runtime)         │
 │                                              │
 │ NavRegionComponent  — walkable area override │
 │ NavLinkComponent    — off-mesh connections   │
 │ NavObstacleComponent— dynamic obstacles      │
 │ CoverPointComponent — tactical cover spots   │
 │ TacticalPointComponent — strategic positions │
 │ SpawnPointComponent — entity spawn locations  │
 └──────────────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/AI/AISystem.h` — AI coordination
- `SparkEngine/Source/Engine/AI/BehaviorTree.h` — BT nodes and execution
- `SparkEngine/Source/Engine/AI/NavMesh.h` — Navigation mesh
- `SparkEngine/Source/Engine/AI/PerceptionSystem.h` — Sensory awareness


## 10. Animation System

Skeletal animation with state machines, blend trees, and IK solving. Runs after physics so ragdoll results are available.

```
 ┌──────────────────────────────────────────────────────────┐
 │          AnimationUpdateSystem::Update(dt)                │
 └─────────────────────────┬────────────────────────────────┘
                           │
               ┌───────────▼──────────────────────┐
               │ For each AnimationController:     │
               │                                   │
               │  ┌────────────────────────────┐   │
               │  │ StateMachine::Evaluate()   │   │
               │  │ ├─ Check transitions       │   │
               │  │ │  (conditions, triggers)  │   │
               │  │ ├─ Advance current state   │   │
               │  │ └─ Cross-fade if changing  │   │
               │  └────────────┬───────────────┘   │
               │               ▼                   │
               │  ┌────────────────────────────┐   │
               │  │ BlendTree::Evaluate()      │   │
               │  │ ├─ Sample animation clips  │   │
               │  │ ├─ Blend weights (1D/2D)   │   │
               │  │ └─ Additive layers         │   │
               │  └────────────┬───────────────┘   │
               │               ▼                   │
               │  ┌────────────────────────────┐   │
               │  │ Skeletal Evaluation         │   │
               │  │ ├─ Compute local bone poses│   │
               │  │ ├─ Multiply parent chain   │   │
               │  │ └─ Produce world-space     │   │
               │  │    bone matrices           │   │
               │  └────────────┬───────────────┘   │
               │               ▼                   │
               │  ┌────────────────────────────┐   │
               │  │ IK Solving                  │   │
               │  │ ├─ Foot placement IK        │   │
               │  │ ├─ Hand IK (weapon grip)    │   │
               │  │ └─ Look-at IK (head/eyes)   │   │
               │  └────────────┬───────────────┘   │
               │               ▼                   │
               │  ┌────────────────────────────┐   │
               │  │ Final Pose → GPU Upload     │   │
               │  │ (bone matrices → skinning   │   │
               │  │  constant buffer)           │   │
               │  └────────────────────────────┘   │
               └───────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/Animation/` — All animation subsystem files
- `SparkEngine/Source/Engine/ECS/Components/AnimationComponents.h`


## 11. Networking Architecture

UDP client/server with entity replication, client-side prediction, and lag compensation. Inspired by HeroEngine's AreaServer/WorldServer split.

```
 ┌──────────────────── CLIENT SIDE ────────────────────────┐
 │                                                         │
 │  ┌─────────────┐   ┌──────────────────┐                │
 │  │ Input       │──▶│ ClientPrediction │                │
 │  │ Capture     │   │ (apply locally)  │                │
 │  └─────────────┘   └────────┬─────────┘                │
 │                             │                          │
 │                    ┌────────▼─────────┐                │
 │                    │ PackMessage      │                │
 │                    │ (ClientInput)    │                │
 │                    └────────┬─────────┘                │
 │                             │                          │
 │                    ┌────────▼─────────┐                │
 │                    │ UDPTransport     │                │
 │                    │ ::Send()         │──────────┐     │
 │                    └──────────────────┘          │     │
 └─────────────────────────────────────────────────│─────┘
                                                   │
                                            ───────│───────
                                              NETWORK
                                            ───────│───────
                                                   │
 ┌──────────────────── SERVER SIDE ────────────────│─────┐
 │                                                 │     │
 │                    ┌────────────────────┐        │     │
 │                    │ UDPTransport       │◀───────┘     │
 │                    │ ::Receive()        │              │
 │                    └────────┬───────────┘              │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ NetworkManager              │           │
 │               │ ::ProcessMessages()         │           │
 │               └─────────────┬──────────────┘           │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ EntityReplicator            │           │
 │               │ ::ApplyInput()              │           │
 │               │ (server-authoritative)      │           │
 │               └─────────────┬──────────────┘           │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ Physics Simulation Step     │           │
 │               └─────────────┬──────────────┘           │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ EntityReplicator            │           │
 │               │ ::BuildSnapshot()           │           │
 │               └─────────────┬──────────────┘           │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ DeltaSnapshotManager        │           │
 │               │ ::Compress()                │           │
 │               │ (delta from last ACK'd)     │           │
 │               └─────────────┬──────────────┘           │
 │                             │                          │
 │                    ┌────────▼─────────┐                │
 │                    │ UDPTransport     │                │
 │                    │ ::Send()         │──────────┐     │
 │                    └──────────────────┘          │     │
 └─────────────────────────────────────────────────│─────┘
                                                   │
                                            ───────│───────
                                              NETWORK
                                            ───────│───────
                                                   │
 ┌──────────────────── CLIENT RECEIVE ─────────────│─────┐
 │                    ┌────────────────────┐        │     │
 │                    │ UDPTransport       │◀───────┘     │
 │                    │ ::Receive()        │              │
 │                    └────────┬───────────┘              │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ NetworkInterpolation        │           │
 │               │ ::BufferState()             │           │
 │               └─────────────┬──────────────┘           │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ Reconcile with Prediction   │           │
 │               │ (replay unACK'd inputs)     │           │
 │               └─────────────┬──────────────┘           │
 │                             │                          │
 │               ┌─────────────▼──────────────┐           │
 │               │ Render interpolated state   │           │
 │               └────────────────────────────┘           │
 └────────────────────────────────────────────────────────┘

 Server Architecture:
 ┌────────────────────────────────────────────────────────┐
 │                  WorldServer                            │
 │          (multi-area coordinator)                       │
 │   ┌──────────────┬──────────────┬──────────────┐       │
 │   │  AreaServer   │  AreaServer  │  AreaServer  │       │
 │   │  (Zone A)     │  (Zone B)    │  (Zone C)    │       │
 │   │  physics +    │  physics +   │  physics +   │       │
 │   │  entities     │  entities    │  entities    │       │
 │   └──────────────┴──────────────┴──────────────┘       │
 │   Entity handoff when player crosses area boundary      │
 └────────────────────────────────────────────────────────┘

 Channel Types:
 ┌─────────────────────────────────────────────┐
 │ Unreliable      — position updates (fast)   │
 │ Reliable        — ordered delivery           │
 │ ReliableOrdered — critical events (in order) │
 └─────────────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/Networking/NetworkManager.h` — Core multiplayer
- `SparkEngine/Source/Engine/Networking/EntityReplicator.h` — State replication
- `SparkEngine/Source/Engine/Networking/ClientPrediction.h` — Client prediction
- `SparkEngine/Source/Engine/Networking/AreaServer.h` — Single area simulation
- `SparkEngine/Source/Engine/Networking/WorldServer.h` — Multi-area coordination


## 12. Scripting (AngelScript)

Scripts are compiled from `.as` files, attached to entities via the `Script` component, and support hot-reload without restarting the engine.

```
 ┌──────────────────────── INITIALIZATION ──────────────────┐
 │                                                          │
 │  ┌───────────────────────────────┐                       │
 │  │ AngelScriptEngine::Initialize()│                       │
 │  └───────────────┬───────────────┘                       │
 │                  │                                       │
 │  ┌───────────────▼───────────────┐                       │
 │  │ RegisterEngineAPI()           │                       │
 │  │ ├─ print(string)             │                       │
 │  │ ├─ createEntity() → EntityID │                       │
 │  │ ├─ getTransform(id)          │                       │
 │  │ ├─ getKeyDown(key) → bool    │                       │
 │  │ ├─ getKey(key) → bool        │                       │
 │  │ └─ ... (full engine API)     │                       │
 │  └───────────────┬───────────────┘                       │
 │                  │                                       │
 │  ┌───────────────▼───────────────┐                       │
 │  │ CompileScripts(.as files)     │                       │
 │  │ Per-entity script instances    │                       │
 │  │ Cache method pointers:         │                       │
 │  │ Start(), Update(), OnCollision │                       │
 │  └───────────────────────────────┘                       │
 └──────────────────────────────────────────────────────────┘

 ┌──────────────────────── PER-FRAME ──────────────────────┐
 │                                                          │
 │  ┌──────────────────────────────┐                        │
 │  │ ScriptSystem::Update(dt)     │                        │
 │  └──────────────┬───────────────┘                        │
 │                 │                                        │
 │  ┌──────────────▼───────────────┐                        │
 │  │ For each Script component:   │                        │
 │  │ ├─ Get cached Update() ptr   │                        │
 │  │ ├─ Set dt argument           │                        │
 │  │ └─ Execute()                 │                        │
 │  └──────────────────────────────┘                        │
 └──────────────────────────────────────────────────────────┘

 ┌──────────────────────── HOT-RELOAD ─────────────────────┐
 │                                                          │
 │  FileWatcher detects .as change                          │
 │           │                                              │
 │  ┌────────▼────────────────────┐                         │
 │  │ ScriptHotReload             │                         │
 │  │ ├─ Call Serialize() on all  │                         │
 │  │ │  script instances         │                         │
 │  │ ├─ Discard old module       │                         │
 │  │ ├─ Recompile module         │                         │
 │  │ ├─ Re-attach to entities    │                         │
 │  │ └─ Call Deserialize() to    │                         │
 │  │    restore state            │                         │
 │  └─────────────────────────────┘                         │
 │                                                          │
 │  Context Separation:                                     │
 │  ┌────────┐ ┌────────┐ ┌────────┐                       │
 │  │ Shared │ │ Server │ │ Client │                        │
 │  │(both)  │ │(auth.) │ │(visual)│                        │
 │  └────────┘ └────────┘ └────────┘                        │
 └──────────────────────────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/Scripting/AngelScriptEngine.h` — VM wrapper
- `SparkEngine/Source/Engine/Scripting/ScriptHotReload.h` — Live recompilation
- `SparkEngine/Source/Engine/Scripting/ScriptHookManager.h` — Event binding


## 13. World Streaming & Origin Rebasing

Large worlds require streaming adjacent areas in/out and periodically rebasing the floating-point origin to maintain precision far from the origin.

```
 ┌──────────── SeamlessAreaManager ────────────────────────┐
 │                                                         │
 │  ┌──────────────────────────────────┐                   │
 │  │ Player Position + Velocity       │                   │
 │  │ + Camera Direction               │                   │
 │  └──────────────┬───────────────────┘                   │
 │                 │                                       │
 │  ┌──────────────▼───────────────────┐                   │
 │  │ Predictive Loading               │                   │
 │  │ (3-second lookahead)             │                   │
 │  │ Load radius: 500 units           │                   │
 │  │ Unload radius: 800 units         │                   │
 │  │ Max 2 concurrent loads           │                   │
 │  └──────────────┬───────────────────┘                   │
 │                 │                                       │
 │  ┌──────────────▼───────────────────┐                   │
 │  │ Area State Machine               │                   │
 │  │                                  │                   │
 │  │  Unloaded ──▶ Loading ──▶ Loaded │                   │
 │  │     ▲                      │     │                   │
 │  │     └── Unloading ◀────────┘     │                   │
 │  └──────────────────────────────────┘                   │
 │                                                         │
 │  SceneTransitionManager handles scene loads/unloads     │
 └─────────────────────────────────────────────────────────┘

 ┌──────────── WorldOriginSystem ──────────────────────────┐
 │                                                         │
 │  ┌──────────────────────────────────┐                   │
 │  │ Each frame: check player distance│                   │
 │  │ from current origin              │                   │
 │  └──────────────┬───────────────────┘                   │
 │                 │                                       │
 │           distance > 5000?                              │
 │           ┌─────┴─────┐                                 │
 │          NO           YES                               │
 │           │     ┌─────▼──────────────────────┐          │
 │           │     │ REBASE                     │          │
 │           │     │ ├─ newOrigin = playerPos    │          │
 │           │     │ ├─ Shift ALL entity pos     │          │
 │           │     │ ├─ Notify callbacks:        │          │
 │           │     │ │  ├─ Physics bodies        │          │
 │           │     │ │  ├─ Audio sources         │          │
 │           │     │ │  ├─ Particle systems      │          │
 │           │     │ │  └─ NavMesh               │          │
 │           │     │ └─ accumulatedOffset += old  │          │
 │           │     └────────────────────────────┘          │
 │           │                                             │
 │     (no action)    Transparent to gameplay:             │
 │                    LocalToAbsolute() / AbsoluteToLocal()│
 └─────────────────────────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.h` — Area streaming
- `SparkEngine/Source/Engine/World/WorldOriginSystem.h` — Origin rebasing
- `SparkEngine/Source/Engine/Streaming/DirectStorageLoader.h` — GPU-direct loading


## 14. Editor Architecture (SparkEditor)

The editor is a standalone executable that embeds the engine and adds a Dear ImGui-based interface with 57 specialized panels.

```
 ┌───────────────── EditorApplication ─────────────────────┐
 │                                                         │
 │  main() / wWinMain()                                    │
 │       │                                                 │
 │  ┌────▼─────────────────────────────────┐               │
 │  │ CreateWindow (Win32 / SDL2)           │               │
 │  └────┬─────────────────────────────────┘               │
 │       │                                                 │
 │  ┌────▼─────────────────────────────────┐               │
 │  │ InitGraphics (D3D11 / OpenGL)         │               │
 │  └────┬─────────────────────────────────┘               │
 │       │                                                 │
 │  ┌────▼─────────────────────────────────┐               │
 │  │ InitImGui (platform + renderer)       │               │
 │  └────┬─────────────────────────────────┘               │
 │       │                                                 │
 │  ┌────▼─────────────────────────────────┐               │
 │  │ InitEditorUI                          │               │
 │  │ ├─ CreateCorePanels()                 │               │
 │  │ └─ CreateToolAndContentPanels()       │               │
 │  └────┬─────────────────────────────────┘               │
 │       │                                                 │
 │  ┌────▼─────────────────────────────────┐               │
 │  │ InitPlugins (editor extensions)       │               │
 │  └────┬─────────────────────────────────┘               │
 │       │                                                 │
 │  ┌────▼─────────────────────────────────┐               │
 │  │           EDITOR MAIN LOOP           │               │
 │  │  ┌─────────────────────────────────┐ │               │
 │  │  │ ProcessMessages() (Win32/SDL2)  │ │               │
 │  │  └──────────────┬──────────────────┘ │               │
 │  │                 │                    │               │
 │  │  ┌──────────────▼──────────────────┐ │               │
 │  │  │ ImGui_NewFrame()                │ │               │
 │  │  └──────────────┬──────────────────┘ │               │
 │  │                 │                    │               │
 │  │  ┌──────────────▼──────────────────┐ │               │
 │  │  │ EditorUI::Render()              │ │               │
 │  │  │ (render all visible panels)     │ │               │
 │  │  └──────────────┬──────────────────┘ │               │
 │  │                 │                    │               │
 │  │  ┌──────────────▼──────────────────┐ │               │
 │  │  │ PluginManager::RenderAll()      │ │               │
 │  │  └──────────────┬──────────────────┘ │               │
 │  │                 │                    │               │
 │  │  ┌──────────────▼──────────────────┐ │               │
 │  │  │ ImGui::Render() + Present       │ │               │
 │  │  └─────────────────────────────────┘ │               │
 │  └──────────────────────────────────────┘               │
 └─────────────────────────────────────────────────────────┘

 Editor Panels (57 total):
 ┌────────────────────── CORE ─────────────────────────────┐
 │  Hierarchy, Inspector, SceneView, GameView, Console,    │
 │  AssetBrowser, ProjectBrowser, ProjectSettings, Search  │
 └─────────────────────────────────────────────────────────┘
 ┌────────────────────── TOOLS ────────────────────────────┐
 │  MaterialEditor, ParticleEditor, SplineEditor,          │
 │  DecalEditor, DestructionEditor, WeaponEditor,          │
 │  AbilityEditor, FPSTools, ObjectPlacement,              │
 │  PostProcessing, TriggerEditor, ConditionEditor         │
 └─────────────────────────────────────────────────────────┘
 ┌────────────────────── CONTENT ──────────────────────────┐
 │  ScriptEditor, DialogueEditor, CinematicSequencer,      │
 │  LocalizationPanel, TilemapEditor, SpriteEditor,        │
 │  SpriteAnimationEditor                                  │
 └─────────────────────────────────────────────────────────┘
 ┌────────────────────── DEBUG ────────────────────────────┐
 │  DebugVisualizer, EventMonitor, CoroutineDebug,         │
 │  SceneStatistics, Physics2D, Physics3D, AudioMixer      │
 └─────────────────────────────────────────────────────────┘
 ┌────────────────────── INFRASTRUCTURE ───────────────────┐
 │  BuildCook, BuildPipeline, PlayModeToolbar, Workflow,   │
 │  GameModuleSelector, Collaboration, DedicatedServer,    │
 │  SaveSystem, Streaming, Replay, Modding, VRConfig,      │
 │  WeatherFog, TimeOfDay, UndoHistory                     │
 └─────────────────────────────────────────────────────────┘

 Collaborative Editing:
 ┌─────────────────────────────────────────────────────────┐
 │ CollaborativeEditSession (WebSocket)                     │
 │ ├─ User cursors + selections shared in real-time        │
 │ ├─ Entity lock/unlock for conflict prevention           │
 │ └─ Operation transform for concurrent edits             │
 └─────────────────────────────────────────────────────────┘
```

**Key files:**
- `SparkEditor/Source/Core/EditorApplication.h` — Editor lifecycle
- `SparkEditor/Source/Core/EditorUI.h` — Panel registry and rendering
- `SparkEditor/Source/Panels/` — All 57 panel implementations
- `SparkEditor/Source/Communication/CollaborativeEditSession.h`


## 15. Game Module Lifecycle

Game modules are compiled as separate DLLs/shared libraries. The engine loads them at runtime via `ModuleManager`, giving each module access to the full engine through `EngineContext`.

```
 ┌──────────────────── ENGINE SIDE ────────────────────────┐
 │                                                         │
 │  ┌──────────────────────────────────┐                   │
 │  │ ModuleManager::LoadModule(path)   │                   │
 │  └──────────────┬───────────────────┘                   │
 │                 │                                       │
 │  ┌──────────────▼───────────────────┐                   │
 │  │ dlopen() / LoadLibrary()          │                   │
 │  └──────────────┬───────────────────┘                   │
 │                 │                                       │
 │  ┌──────────────▼───────────────────┐                   │
 │  │ CreateModule() (C export)         │                   │
 │  │ Returns IModule*                  │                   │
 │  └──────────────┬───────────────────┘                   │
 │                 │                                       │
 │  ┌──────────────▼───────────────────┐                   │
 │  │ IModule::OnLoad(EngineContext*)   │                   │
 │  │ Module initializes, registers     │                   │
 │  │ console commands, subscribes to   │                   │
 │  │ events, sets up gameplay          │                   │
 │  └──────────────┬───────────────────┘                   │
 │                 │                                       │
 │     ┌───────────▼────────────────────────────┐          │
 │     │          PER-FRAME CALLS               │          │
 │     │                                        │          │
 │     │  ModuleManager::UpdateAll(dt)          │          │
 │     │  ├─ IModule::OnUpdate(dt)              │          │
 │     │  ├─ IModule::OnFixedUpdate()           │          │
 │     │  └─ IModule::OnRender()                │          │
 │     │                                        │          │
 │     │  Also called when relevant:            │          │
 │     │  ├─ OnResize(width, height)            │          │
 │     │  ├─ OnPause() / OnResume()             │          │
 │     │  └─ OnImGui() (debug UI)               │          │
 │     └────────────────────────────────────────┘          │
 │                                                         │
 │  ┌──────────────────────────────────┐                   │
 │  │ SHUTDOWN                          │                   │
 │  │ ├─ IModule::OnUnload()            │                   │
 │  │ ├─ DestroyModule(ptr) (C export)  │                   │
 │  │ └─ dlclose() / FreeLibrary()      │                   │
 │  └──────────────────────────────────┘                   │
 └─────────────────────────────────────────────────────────┘

 IModule Interface:
 ┌─────────────────────────────────────────────────────────┐
 │  GetModuleInfo() → ModuleInfo (name, version, order)    │
 │  OnLoad(IEngineContext*)  → bool                        │
 │  OnUnload()                                             │
 │  OnUpdate(float deltaTime)                              │
 │  OnFixedUpdate()                                        │
 │  OnRender()                                             │
 │  OnResize(int w, int h)                                 │
 │  OnPause() / OnResume()                                 │
 │  OnImGui()                                              │
 └─────────────────────────────────────────────────────────┘

 Hot-Reload (development):
 ┌─────────────────────────────────────────────────────────┐
 │ PollModuleHotReload() each frame                        │
 │ ├─ Detect DLL timestamp change                          │
 │ ├─ OnUnload() old module                                │
 │ ├─ Copy DLL to temp, dlopen new copy                    │
 │ └─ OnLoad() new module                                  │
 └─────────────────────────────────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Core/ModuleManager.cpp` — Module loading/unloading
- `SparkEngine/Source/Core/ModuleManager.h` — Module interface and loading
- `GameModules/SparkGame/Source/Core/Main.cpp` — Example module entry
- `GameModules/SparkGame/Source/Core/SparkGame.h` — Example module class


## 16. Supporting Subsystems

Quick-reference flowcharts for the remaining engine subsystems.

### Save System

```
 SaveSystem::Save(slotName)
      │
      ├─ ComponentSerializerRegistry
      │  (per-component serialize lambdas)
      │       │
      ├───────▼──────────────────┐
      │  Serialize all entities   │
      │  Transform, Health,       │
      │  RigidBody, etc.          │
      └───────┬──────────────────┘
              │
      ┌───────▼──────────────────┐
      │ SaveData (JSON)           │
      │ + miniz deflate compress  │
      └───────┬──────────────────┘
              │
      ┌───────▼──────────┐
      │ Write .sav file   │
      └──────────────────┘

 Features: quicksave/quickload, rotating autosave (3 slots)
```

### Coroutine Scheduler

```
 Builder Pattern:
   StartCoroutine("name")
     .Do([]{...})
     .WaitForSeconds(2.0f)
     .Do([]{...})
     .Build()

 C++20 Native:
   GameCoroutine MyFunc() {
     co_await WaitForSeconds(1.0f);
     // resume here after 1 second
   }

 Scheduler: per-frame Update() advances all active coroutines
 Cancellation: by name or StopAll()
```

### UI System

```
 UICanvas (root, one per viewport)
   └─ UIPanel (layout: None / Horizontal / Vertical)
       ├─ UILabel (text display)
       ├─ UIButton (click handler)
       ├─ UIProgressBar (0.0 - 1.0)
       ├─ UIImageWidget (textured quad)
       └─ UIPanel (nested containers)

 Anchoring: 9 points + Stretch
 Properties: position, size, visibility, opacity, z-order
```

### Replay System

```
 RECORDING (configurable, default 20 fps):
   Each tick → ReplayFrame {
     timestamp, entity snapshots,
     flags (Alive, Firing, Crouching...)
   }

 PLAYBACK:
   Play / Pause / Rewind / FastForward / Seek
   Camera: FreeCam, FollowCam, FirstPerson, KillCam
   Kill cam: rewind N seconds, focus on entity

 File: magic "RPLY", max 1M frames, 100k entities
```

### Destruction System

```
 ApplyDamage(entity, amount, hitPoint, hitDir)
      │
      ├─ Update damage stage
      │
      ├─ health <= 0?
      │   YES → ForceDestroy()
      │          │
      │   ┌──────▼──────────────────┐
      │   │ FracturePattern lookup   │
      │   │ Spawn debris pieces as   │
      │   │ ECS entities + physics   │
      │   │ (max 500 debris limit)   │
      │   │ Trigger particles + SFX  │
      │   └─────────────────────────┘
      │
      └─ NO → visual damage stage update
```

### Dialogue System

```
 DialogueTriggerComponent → DialogueSystem::StartDialogue()
      │
      ├─ Load dialogue tree (nodes + edges)
      ├─ Display speaker + text
      ├─ Show player choices (branching)
      ├─ Evaluate conditions (quest state, items)
      └─ Execute actions (give item, advance quest)
```

### Localization

```
 LoadLanguage("en", "locales/en.json")
 SetCurrentLanguage("en")
      │
      ├─ StringTable: key → value mapping
      ├─ Get("hud.health") → "Health"
      ├─ Format("hud.ammo", 30, 120) → "Ammo: 30/120"
      └─ Fallback to "en" for missing keys

 OnLanguageChanged callback for UI refresh
```

### Loading Screen

```
 LoadingScreen::Begin(tasks)
      │
      ├─ LoadingTask { name, weight, execute() }
      ├─ Progress = weighted sum (0.0 → 1.0)
      ├─ Background image + rotating tips
      ├─ Minimum display time (no flash)
      └─ OnComplete(success) callback
```

### Mod System

```
 ModSystem::ScanForMods("mods/")
      │
      ├─ Discover mod.json files
      ├─ Parse metadata (name, version, deps)
      ├─ AreDependenciesMet() check
      │
      ├─ EnableMod(id) → load Scripts/, Assets/, Data/
      └─ DisableMod(id) → unload

 mod.json → Scripts/ → Assets/ → Data/ → Preview.png
```

### VR System

```
 VRSystem::Update()
      │
      ├─ Head tracking (position + orientation)
      ├─ Per-eye rendering:
      │   ├─ Left eye (view + proj matrix)
      │   └─ Right eye (view + proj matrix)
      ├─ Motion controllers (2x):
      │   ├─ Position, orientation, velocity
      │   ├─ Triggers, grips, thumbstick, buttons
      │   └─ Haptic feedback (amplitude + duration)
      └─ Tracking space: Seated or RoomScale
```

### Mobile Platform

```
 MobilePlatform::Update()
      │
      ├─ Touch events (Began/Moved/Ended/Cancelled)
      ├─ Gesture recognition:
      │   Tap, DoubleTap, LongPress, Swipe, Pinch, Rotate
      ├─ Quality presets (Low/Medium/High/Auto)
      │   (render scale, shadow res, draw budget, LOD bias)
      ├─ Screen: orientation, safe area (notch insets)
      └─ Battery-aware scaling (reduce quality as drops)
```

**Key files:**
- `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h`
- `SparkEngine/Source/Engine/Coroutine/CoroutineScheduler.h`
- `SparkEngine/Source/Engine/UI/UISystem.h`
- `SparkEngine/Source/Engine/Replay/ReplaySystem.h`
- `SparkEngine/Source/Engine/Destruction/DestructionSystem.h`
- `SparkEngine/Source/Engine/Dialogue/DialogueSystem.h` (in Engine/Dialogue/)
- `SparkEngine/Source/Engine/Localization/LocalizationSystem.h`
- `SparkEngine/Source/Engine/Loading/LoadingScreen.h`
- `SparkEngine/Source/Engine/Modding/ModSystem.h`
- `SparkEngine/Source/Engine/VR/VRSystem.h`
- `SparkEngine/Source/Engine/Mobile/MobilePlatform.h`


## 17. Shutdown Sequence

Shutdown mirrors initialization in reverse order. The `EngineShutdownEvent` gives subsystems a chance to save state before teardown.

```
 ┌────────────────────────────────────────────────┐
 │           ShutdownEngine()                      │
 └───────────────────┬────────────────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ Publish EngineShutdownEvent    │
         │ (subsystems save state)        │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ ShutdownGameplaySystems()      │
         │ (Weather, TimeOfDay, UI, etc.) │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ ShutdownDebugSystems()         │
         │ (profiler, debug draw)         │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ ModuleManager::ShutdownAll()   │
         │ (call OnUnload on each module) │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ SimpleConsole::Shutdown()       │
         │ (close pipe to SparkConsole)   │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ ConsoleProcessManager          │
         │ ::Shutdown()                   │
         │ (terminate SparkConsole.exe)   │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ EventBus::ClearAll()           │
         │ (remove all subscribers)       │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ ModuleManager::UnloadAll()     │
         │ (dlclose / FreeLibrary)        │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ AudioEngine.reset()            │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ ShutdownPhysics()              │
         │ (Jolt cleanup)                 │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ JobSystem::Shutdown()           │
         │ (drain queue, join threads)    │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ EngineContext::ResetOwned()     │
         │ EventBus.reset()               │
         │ Input.reset()                  │
         │ Graphics.reset()               │
         │ Timer.reset()                  │
         └───────────┬───────────────────┘
                     │
         ┌───────────▼───────────────────┐
         │ Clear debug hooks              │
         │ Process exits cleanly          │
         └───────────────────────────────┘
```

**Key files:**
- `SparkEngine/Source/Core/SparkEngine.cpp` — `ShutdownEngine()`
- `SparkEngine/Source/Core/GameplaySystemLifecycle.cpp` — `ShutdownGameplaySystems()`


## 18. SparkConsole (External Process)

SparkConsole is a separate executable that communicates with the engine via stdin/stdout pipes. It provides an interactive command-line interface without blocking the engine's main loop.

```
 ┌──────────── ENGINE SIDE ────────────────────────────────┐
 │                                                         │
 │  ┌──────────────────────────────────────┐               │
 │  │ ConsoleProcessManager::Initialize()   │               │
 │  │ ├─ CreateProcess("SparkConsole.exe")  │               │
 │  │ └─ SetupPipes(stdin, stdout)          │               │
 │  └──────────────────┬───────────────────┘               │
 │                     │                                   │
 │  Each frame:        │                                   │
 │  ┌──────────────────▼───────────────────┐               │
 │  │ ConsoleProcessManager                 │               │
 │  │ ::ProcessCommands()                   │               │
 │  │ ├─ Read pipe for incoming commands    │               │
 │  │ └─ Execute via SimpleConsole          │               │
 │  └──────────────────┬───────────────────┘               │
 │                     │                                   │
 │  ┌──────────────────▼───────────────────┐               │
 │  │ SimpleConsole::Update()               │               │
 │  │ ├─ Write log messages to pipe         │               │
 │  │ │  (engine → console process)         │               │
 │  │ └─ Flush buffered output              │               │
 │  └──────────────────────────────────────┘               │
 └──────────────────────────┬──────────────────────────────┘
                            │
                     stdin/stdout pipes
                            │
 ┌──────────── CONSOLE SIDE ───────────────┬───────────────┐
 │                                         │               │
 │  ┌──────────────────────────────────┐   │               │
 │  │ main() → ConsoleApp::Run()       │   │               │
 │  └──────────────┬───────────────────┘   │               │
 │                 │                       │               │
 │       ┌─────────▼──────────┐            │               │
 │       │ Detect pipe mode   │            │               │
 │       │ (stdin is a pipe?) │            │               │
 │       └────┬──────────┬────┘            │               │
 │           YES         NO                │               │
 │            │           │                │               │
 │  ┌─────────▼────┐  ┌──▼──────────────┐ │               │
 │  │ PIPE MODE    │  │ STANDALONE MODE  │ │               │
 │  │              │  │                  │ │               │
 │  │ Spawn:       │  │ Blocking getline │ │               │
 │  │ ├─ Engine    │  │ for user input   │ │               │
 │  │ │  input     │  │ Execute commands │ │               │
 │  │ │  thread    │  │ locally          │ │               │
 │  │ └─ Keyboard  │  │                  │ │               │
 │  │    input     │  │                  │ │               │
 │  │    thread    │  │                  │ │               │
 │  │              │  │                  │ │               │
 │  │ Main loop:   │  │                  │ │               │
 │  │ sleep 100ms  │  │                  │ │               │
 │  │ (threads do  │  │                  │ │               │
 │  │  the work)   │  │                  │ │               │
 │  └──────────────┘  └──────────────────┘ │               │
 └─────────────────────────────────────────────────────────┘

 Communication Protocol:
 ┌─────────────────────────────────────────────────────────┐
 │ Engine → Console: log messages (severity + text)        │
 │ Console → Engine: user commands (text strings)          │
 │                                                         │
 │ SimpleConsole: thread-safe (mutex-protected)             │
 │ Command registry: handler + description + category +    │
 │                   usage + permission level               │
 │ Permission: Player → Moderator → Admin → Developer      │
 └─────────────────────────────────────────────────────────┘
```

**Key files:**
- `SparkConsole/src/main.cpp` — Console entry point
- `SparkConsole/src/ConsoleApp.cpp` — Main loop and pipe I/O
- `SparkEngine/Source/Utils/ConsoleProcessManager.h` — Engine-side pipe management
- `SparkEngine/Source/Utils/SparkConsole.h` — Log sink and command registry
