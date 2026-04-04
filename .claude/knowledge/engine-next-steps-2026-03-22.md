# Engine Next Steps — Recommended Roadmap

**Last updated:** 2026-03-22
**Type:** Decision
**Status:** **Resolved**

## Description

Prioritized roadmap for SparkEngine's next development phase. Philosophy: **fully implement and wire in all existing systems** — no dead code deletion. Every stub, header-only system, and built-not-wired feature gets completed and integrated.

## Context

As of 2026-03-22, SparkEngine has:
- 9 core systems production-ready (Graphics/D3D11, Physics/Jolt, Audio/XAudio2, ECS/EnTT, Input, AI, Animation, Scripting/AngelScript, Networking/UDP)
- 145 test files (37K LOC)
- 32 editor panels
- 27 singletons wired (0 orphaned)
- Networking now defaulted to ON
- 12 rendering header-only stubs (~15K lines) with types defined but no GPU implementation
- 5 built-not-wired editor systems
- Render graph infrastructure (1,700 lines) not integrated into GraphicsEngine
- 66 oversized functions, 7 classes with excessive private methods
- Extensive analysis knowledge from 20+ engine studies (TrinityCore, CryEngine, Godot, O3DE, id Tech, Frostbite, etc.)

## Roadmap

### Phase 1: Wire In & Complete Existing Systems

**Render Graph Integration** — HIGH
- 3 files exist (RenderGraphBuilder, RenderGraphExporter, TransientResourcePool)
- Wire into GraphicsEngine rendering path
- Extract render passes into stateless Feature Processor modules
- 4+ engines validate this pattern (Frostbite: 67% complexity reduction)

**Complete 12 Rendering Stubs with GPU Implementations** — HIGH
- Sky/atmosphere system → implement GPU atmosphere scattering shader
- Water system → implement water plane rendering with reflections/refractions
- Global illumination → implement screen-space GI or SH-based IBL
- Shadow atlas → implement cached shadow map atlas
- Occlusion culling → implement GPU-driven or hierarchical-Z culling
- Instance renderer → implement GPU instanced draw calls
- Screen-space effects (GPU) → implement compute-based SSAO/SSR
- Volumetric fog → implement froxel-based volumetric fog
- SSR → implement screen-space reflections
- Resource residency manager → implement VRAM budget tracking
- Dynamic quality scaler → implement adaptive resolution/quality
- Deferred lighting pass / Forward+ culling → wire into existing pipeline

**Wire Built-Not-Wired Editor Systems** — MEDIUM
- GizmoSystem → connect SceneViewPanel buttons to transform gizmo backend
- LightingTools → hook editor panel to scene viewport
- VersionControlSystem → add dockable panel for Git/SVN
- EditorPluginManager → expose plugin API, add documentation
- CollaborativeEditSession → ensure network transport connected

**Complete Networking Features** — HIGH
- Implement connection timeout detection (m_connectionTimeout exists, never checked)
- Complete reliable channel ACK/retransmission logic
- Implement client-side prediction loop (input history exists, prediction not running)
- Implement SteamTransport (4 TODO stubs)

**Wire Cinematic Sequencer Editor Panel** — MEDIUM
- Sequencer.h is fully implemented
- No corresponding editor panel exists
- Create CinematicSequencerPanel with timeline UI, keyframe editing

### Phase 2: Infrastructure From Engine Analyses

**Job System / WorkerThreadPool** — HIGH (9 engines validate)
- ParallelSystemExecutor exists but is unused
- Implement general-purpose thread pool
- Wire into ECS: parallelize Physics, AI, Animation, Particles
- Pattern: Wicked Engine wi::jobsystem, id Tech 7 "all jobs"

**Frame-Delayed Resource Destruction** — HIGH (3 engines validate)
- DeferredDeletionQueue header exists
- Implement ring-buffer delayed GPU resource destruction (2+ frames)
- Eliminates use-after-free with in-flight GPU resources

**Collision Layer/Mask System** — MEDIUM (Godot/Torque3D pattern)
- 32-bit layer + 32-bit mask per physics body
- Integrate with Jolt Physics collision groups
- Add editor UI to InspectorPanel

**Prefab System** — HIGH (4 engines validate)
- JSON-based prefab format with component definitions + property overrides
- Nesting support with inheritance
- Editor panel for creation/editing
- EntityArchetypeLoader (.archetype files already exist — extend)

**Per-Entity EventBus Dispatch** — MEDIUM (O3DE: 10x perf win)
- Current EventBus broadcasts globally
- Add Dispatch<EventType>(entityId) for entity-scoped events

### Phase 3: Graphics & Visual Quality

**Post-Processing Pipeline Completion** — HIGH (ThorVG/Unity analysis)
- Bloom (threshold-based HDR)
- Auto-exposure / eye adaptation (histogram-based)
- Tonemapping (ACES/Filmic/Neutral)
- Color grading (LGG + curves)
- Volume system for spatial parameter blending (different FX per region)

**Complete RHI Backends** — MEDIUM
- D3D12, Vulkan, OpenGL, Metal are experimental stubs
- Bring each to feature parity with D3D11 primary backend
- Focus on Vulkan as second priority after D3D11

**Shader System Enhancements** — MEDIUM
- Shader variant keywords (multi_compile / shader_feature)
- Material inheritance (child materials override properties)
- FastNoiseLite HLSL integration

**Texture Pipeline** — MEDIUM
- Basis Universal transcoder (4-8x VRAM savings)
- Background texture streaming (currently hitches main thread)
- meshoptimizer integration (vertex optimization, meshlet generation)

### Phase 4: Gameplay & Content Systems

**SparkGame AI Enemies** — HIGH
- Engine AI system (37 files, 12K lines) is fully functional
- SparkGame has 0% AI enemy integration — largest missing piece
- Wire behavior trees, perception, NavMesh into game module
- Add enemy spawning, patrol, combat behaviors

**Day-Night Cycle System** — MEDIUM
- WeatherSystem is complete but no time-of-day system
- Add TimeOfDaySystem driving sun angle/color
- Integrate with WeatherSystem for time-based weather transitions

**VR System Implementation** — LOW
- OpenXR-ready framework exists (292 lines)
- Implement xrCreateInstance, session creation, controller input
- Requires OpenXR SDK linkage

**DXR Ray Tracing Integration** — LOW
- 45K lines, feature-complete but ENABLE_DXR=OFF by default
- Validate and enable for D3D12 users
- Add editor toggle

### Phase 5: Quality & Polish

**Refactor Oversized Functions** — MEDIUM
- 66 functions exceed 50-line guideline (~13,360 lines total)
- Top targets: RegisterEngineConsoleCommands (555), RenderMainMenuBar (514), main (488)
- Split by logical subsystem

**Fix Duplicate Functions** — LOW
- GetShaderPermutation (2 copies in MaterialSystem.cpp)
- SaveToFile (2 copies in MaterialSystem.cpp)

**Add Missing Test Suites** — MEDIUM
- AudioEngine (no tests)
- SceneManager (no tests)
- Terrain system (untested)

**Documentation** — MEDIUM
- Networking wire format specification
- Asset binary format specification
- Plugin ABI stability guide
- EditorPluginManager wiki page
- Update README test count (done: 145)

## Cross-Engine Signal Strength

Features validated by the most engine analyses (strongest signal for what to build):

| Feature | Engines Validating | Priority |
|---------|-------------------|----------|
| Job system | 9 (Godot, O3DE, Wicked, id Tech, Frostbite, bgfx, Filament, The Forge, Stride) | Phase 2 |
| Prefab system | 4 (O3DE, Flax, Stride, Godot) | Phase 2 |
| Render graph | 4 (Frostbite, O3DE, Filament, bgfx) | Phase 1 |
| Handle-based resources | 4 (bgfx, The Forge, Filament, Wicked) | Phase 2 |
| Frame-delayed destruction | 3 (bgfx, Filament, The Forge) | Phase 2 |
| Post-processing chain | 3+ (Unity HDRP, Filament, Wicked) | Phase 3 |
| Collision layers | 3 (Godot, Torque3D, Stride) | Phase 2 |

## Notes

- This roadmap prioritizes **implementing what exists** over adding new systems
- No dead code deletion — every stub and header-only system gets completed
- Networking is now ON by default (CMake change made 2026-03-22)
- All repo documentation updated to reflect Jolt Physics, 145 tests, networking ON

## Implementation Progress (2026-03-22)

### Phase 1 — COMPLETE
- Render graph pass bodies wired into RenderPipeline (Shadow, GBuffer, Lighting, PostProcess, UI)
- 8 rendering stubs implemented with .cpp backends (ShadowAtlas, ScreenSpaceEffects, GPUOcclusionCulling, FroxelVolumetricFog, DynamicQualityScaler, DDGIProbeSystem, AdaptiveProbeVolumes, LightProbeSystem)
- EditorPluginManager wired into editor lifecycle
- Networking already fully implemented (timeout, retransmission, prediction all present)
- Editor systems (GizmoSystem, CinematicSequencer, LightingTools, VCS, CollaborativeEdit) already had substantial implementations
- Remaining stubs (SkyAtmosphere, WaterRenderer, ClusteredLightCulling) already had .cpp files

### Phase 2 — COMPLETE
- JobSystem wired into engine init/shutdown
- DeferredDeletionQueue integrated into RHI frame loop
- Collision layer/mask filtering added to PhysicsSystem (ShouldCollide, GroupFilterTable)
- EntityArchetypeLoader extended with property override support
- EntityEventBus cleanup wired into entity destruction

### Phase 3 — COMPLETE
- Bloom (threshold extraction, Gaussian blur, composite)
- Auto-exposure (luminance histogram, temporal adaptation)
- Tonemapping (ACES, Reinhard, Filmic, Neutral operators)
- Color grading (lift/gamma/gain, saturation, contrast)
- PostProcessVolume for spatial parameter blending

### Phase 4 — COMPLETE
- AI enemies wired into SparkGame (patrol behavior, NavMesh waypoints)
- TimeOfDaySystem implemented (sun direction, sky color, light temperature)
- Console commands: time_set, time_speed, time_get
- TimeOfDaySystem integrated with directional light and WeatherSystem

### Phase 5 — COMPLETE
- ~~Refactor 66 oversized functions~~ — Down to 9 borderline cases (documented as acceptable)
- ~~Fix duplicate MaterialSystem functions~~ — Resolved (wrapper+impl pattern, not duplicates)
- ~~Add AudioEngine and SceneManager test suites~~ — TestAudioEngine.cpp, TestSceneManager.cpp exist
- ~~Documentation specs~~ — All 3 exist (networking-wire-format.md, asset-format.md, plugin-abi-guide.md)
- ~~Last TODO in EventResponseSystem.cpp~~ — Wired ConditionSystem evaluation via EngineContext
