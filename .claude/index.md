# Persistence Context — Index

_Read this at every session start (after git sync). Each row links to a detailed knowledge file._

## Knowledge Index

| Topic | File | Type | Status | Last Updated |
|-------|------|------|--------|--------------|
| Five-engine analysis (Cocos/Defold/Panda3D/S&box/Halley) | [knowledge/five-engine-analysis.md](knowledge/five-engine-analysis.md) | Decision | Active | 2026-03-19 |
| 11-engine + 3-framework analysis (30 recommendations) | [knowledge/eleven-engine-analysis.md](knowledge/eleven-engine-analysis.md) | Decision | Active | 2026-03-20 |
| 13 closed/proprietary engine analysis (15 recommendations) | [knowledge/closed-engines-analysis.md](knowledge/closed-engines-analysis.md) | Decision | **Resolved** | 2026-03-21 |
| ThorVG + Unity Graphics + 33 libraries analysis | [knowledge/thorvg-unity-graphics-analysis.md](knowledge/thorvg-unity-graphics-analysis.md) | Decision | Active | 2026-03-22 |
| Jolt Physics integration (migrated from Bullet3) | [knowledge/jolt-physics-integration.md](knowledge/jolt-physics-integration.md) | Observation | Active | 2026-03-22 |
| CI reproducible builds (local reproduction commands) | [knowledge/ci-reproducible-builds.md](knowledge/ci-reproducible-builds.md) | Pattern | Active | 2026-03-30 |
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |
| AI bloat pattern and countermeasures | [knowledge/ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md) | Observation | Active | 2026-03-14 |
| Comprehensive bloat audit | [knowledge/codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md) | Observation | Active | 2026-03-18 |
| Deep test coverage analysis (297 files, 3647 tests) | [knowledge/test-suite-audit.md](knowledge/test-suite-audit.md) | Observation | Active | 2026-04-04 |
| 66 oversized functions, 7 private-method violations | [knowledge/code-quality-violations.md](knowledge/code-quality-violations.md) | Observation | Active | 2026-03-16 |
| Engine systems (29+ working, all wired) | [knowledge/gameplay-systems-status.md](knowledge/gameplay-systems-status.md) | Observation | Active | 2026-03-22 |
| SparkGame module (AI enemies now wired) | [knowledge/sparkgame-module-status.md](knowledge/sparkgame-module-status.md) | Observation | Active | 2026-03-22 |
| Documentation coverage (125 wiki pages, 100% Doxygen) | [knowledge/documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md) | Observation | Active | 2026-04-06 |
| ThirdParty dependencies | [knowledge/thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md) | Observation | Active | 2026-04-09 |
| Load test baseline (frame/CPU/memory benchmarks) | [knowledge/load-test-baseline.md](knowledge/load-test-baseline.md) | Observation | Active | 2026-03-26 |
| Live editor testing (Xvfb + Mesa llvmpipe) | [knowledge/live-editor-testing.md](knowledge/live-editor-testing.md) | Pattern | Active | 2026-04-07 |
| MinGW + Wine cross-compilation (D3D11 on Linux, 64 fixes) | [knowledge/mingw-wine-cross-compilation.md](knowledge/mingw-wine-cross-compilation.md) | Pattern | Active | 2026-03-29 |
| Hardware acceleration (8 GPU systems, 72 tests) | [knowledge/hardware-acceleration-systems.md](knowledge/hardware-acceleration-systems.md) | Decision | Active | 2026-03-28 |
| Mac compatibility analysis (gaps, roadmap, changes) | [knowledge/mac-compatibility-analysis.md](knowledge/mac-compatibility-analysis.md) | Observation | Active | 2026-03-28 |
| Third-party library evaluation (5 added, 7 rejected) | [knowledge/third-party-library-evaluation.md](knowledge/third-party-library-evaluation.md) | Decision | Active | 2026-03-31 |
| Engine viability evaluation (can you make a game?) | [knowledge/engine-viability-evaluation.md](knowledge/engine-viability-evaluation.md) | Observation | Active | 2026-04-01 |
| Project recommendations (13+3 production systems) | [knowledge/project-recommendations-2026-04-04.md](knowledge/project-recommendations-2026-04-04.md) | Decision | Active | 2026-04-04 |
| Engine feature recommendations (7 new game-making systems) | [knowledge/engine-feature-recommendations-2026-04-04.md](knowledge/engine-feature-recommendations-2026-04-04.md) | Decision | Active | 2026-04-04 |
| Memory integrity system (branch guards, code scanning) | [knowledge/memory-integrity-system.md](knowledge/memory-integrity-system.md) | Observation | Active | 2026-04-05 |
| Memory safety evaluation & C++26 bridge (Contracts, NonNull, SafeCast) | [knowledge/memory-safety-evaluation.md](knowledge/memory-safety-evaluation.md) | Decision | Active | 2026-04-06 |
| ConnectionScopeFilter wired into replication path | [knowledge/connection-scope-wiring-2026-04-10.md](knowledge/connection-scope-wiring-2026-04-10.md) | Observation | Resolved | 2026-04-10 |
| Stub & abandoned features catalog (Tier 1–4, ~30 Graphics orphans, VR/Steam stubs) | [knowledge/stub-and-abandoned-features-2026-04-10.md](knowledge/stub-and-abandoned-features-2026-04-10.md) | Observation | Active | 2026-04-10 |
| Engine next-steps Phase A (NavMesh/LOD notes, NetworkDebugPanel poll) | [knowledge/engine-next-steps-2026-04-10.md](knowledge/engine-next-steps-2026-04-10.md) | Observation | Active | 2026-04-10 |
| Engine next-steps Phase B (stub @warnings, AnimMgr, SelectionMgr id widen) | [knowledge/engine-next-steps-phase-b-2026-04-10.md](knowledge/engine-next-steps-phase-b-2026-04-10.md) | Observation | Active | 2026-04-10 |
| Engine next-steps Phase C (DXR finish, foliage GPU bake, SelectionMgr panel migration) | [knowledge/engine-next-steps-phase-c-2026-04-10.md](knowledge/engine-next-steps-phase-c-2026-04-10.md) | Observation | Active | 2026-04-10 |
| Engine next-steps Phase D (DXR .cso build step, foliage lifecycle bake, DXR tests) | [knowledge/engine-next-steps-phase-d-2026-04-10.md](knowledge/engine-next-steps-phase-d-2026-04-10.md) | Observation | Active | 2026-04-10 |
| Engine next-steps Phase G (billboard aspect, FXC validation, Windows smoke procedure) | [knowledge/engine-next-steps-phase-g-2026-04-11.md](knowledge/engine-next-steps-phase-g-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase I (GTAO wired as 15th post-process pass — first Tier 2 orphan activation) | [knowledge/engine-next-steps-phase-i-2026-04-11.md](knowledge/engine-next-steps-phase-i-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase J (SSAOTemporal + RenderTargetPool + GPUDebugMarkers + GPUTimestampQuery — 4 more Tier 2 orphans) | [knowledge/engine-next-steps-phase-j-2026-04-11.md](knowledge/engine-next-steps-phase-j-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase K (VolumeManager activated with spatial post-process volumes + ApplyVolumeStack binding) | [knowledge/engine-next-steps-phase-k-2026-04-11.md](knowledge/engine-next-steps-phase-k-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase L (MeshOptimizer ACMR reporting in LODManager + BVHAccelerator first-level cull in SceneRenderer) | [knowledge/engine-next-steps-phase-l-2026-04-11.md](knowledge/engine-next-steps-phase-l-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase M (ReflectionProbeCache + CachedShadowAtlas wired into LightingSystem lifecycle) | [knowledge/engine-next-steps-phase-m-2026-04-11.md](knowledge/engine-next-steps-phase-m-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase N (RTHandleSystem + ConstantBufferRing wired into PostProcessingPipeline) | [knowledge/engine-next-steps-phase-n-2026-04-11.md](knowledge/engine-next-steps-phase-n-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase O (ShaderVariantSystem wired into Shader class lifecycle) | [knowledge/engine-next-steps-phase-o-2026-04-11.md](knowledge/engine-next-steps-phase-o-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase P (PersistentMaterialCBManager wired into MaterialSystem lifecycle) | [knowledge/engine-next-steps-phase-p-2026-04-11.md](knowledge/engine-next-steps-phase-p-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase Q (DenoiserInterface + SoftwareDenoiser wired into GraphicsEngine) | [knowledge/engine-next-steps-phase-q-2026-04-11.md](knowledge/engine-next-steps-phase-q-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase R (UICompositor wired into UISystem after wire-or-delete audit) | [knowledge/engine-next-steps-phase-r-2026-04-11.md](knowledge/engine-next-steps-phase-r-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase S (FastNoise2SIMD NoiseGraph wired into GraphicsEngine) | [knowledge/engine-next-steps-phase-s-2026-04-11.md](knowledge/engine-next-steps-phase-s-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase T (VoxelConeTracing VCTSystem wired into GraphicsEngine — final Tier 2 orphan) | [knowledge/engine-next-steps-phase-t-2026-04-11.md](knowledge/engine-next-steps-phase-t-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase U+ plan (deferred — shader hot-reload / RHI parity / editor panels) | [knowledge/engine-next-steps-phase-u-plan-2026-04-11.md](knowledge/engine-next-steps-phase-u-plan-2026-04-11.md) | Plan | Deferred | 2026-04-11 |
| Engine next-steps Phase U (ShaderHotReload singleton wired into Shader lifecycle + GraphicsEngine BeginFrame pump) | [knowledge/engine-next-steps-phase-u-2026-04-11.md](knowledge/engine-next-steps-phase-u-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase V (ShaderDiskCache singleton wired into Shader lifecycle + LoadShaderFromSource lookup/store) | [knowledge/engine-next-steps-phase-v-2026-04-11.md](knowledge/engine-next-steps-phase-v-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase W (ShaderCrossCompiler singleton wired into Shader::Initialize — Theme 3A complete) | [knowledge/engine-next-steps-phase-w-2026-04-11.md](knowledge/engine-next-steps-phase-w-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase X (RHIHandlePool + TransientBufferAllocator real-class test coverage — Theme 3B first phase) | [knowledge/engine-next-steps-phase-x-2026-04-11.md](knowledge/engine-next-steps-phase-x-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phases Y + Z (NullRHIDevice full wire-up + TransientBufferAllocator in all real RHI backends) | [knowledge/engine-next-steps-phase-y-z-2026-04-11.md](knowledge/engine-next-steps-phase-y-z-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase AA (Theme 3C — LevelStreamingSystem + VersionControlSystem panels wired) | [knowledge/engine-next-steps-phase-aa-2026-04-11.md](knowledge/engine-next-steps-phase-aa-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase BB (ScriptHookManager + DynamicQualityScaler singleton orphans wired into GameplayLifecycleShared.cpp) | [knowledge/engine-next-steps-phase-bb-2026-04-11.md](knowledge/engine-next-steps-phase-bb-2026-04-11.md) | Observation | Active | 2026-04-11 |
| Engine next-steps Phase CC (GPUStallProfiler + AsyncComputeScheduler singleton orphans wired into GameplayLifecycleShared.cpp) | [knowledge/engine-next-steps-phase-cc-2026-04-11.md](knowledge/engine-next-steps-phase-cc-2026-04-11.md) | Observation | Active | 2026-04-11 |
## Quick Reference

### Current Engine State (2026-04-11)

- **Physics**: Jolt Physics (migrated from Bullet3). Use `EngineContext::Get()->GetPhysics()`
- **Networking**: Enabled by default (`ENABLE_NETWORKING=ON`), UDP sockets, no external deps
- **Tests**: 368 test files, 4751 tests on SparkTests on Linux (Phase U–CC add 164 cumulative new tests; ImGui-gated Phase AA tests inactive without libimgui). Theme 3A **complete**; Theme 3B **complete for compilable-on-Linux scope**; Theme 3C **2 of 4 outside-Panels/ orphans wired**; Theme 3D Phase BB landed ScriptHookManager + DynamicQualityScaler (25 tests) and Phase CC landed GPUStallProfiler + AsyncComputeScheduler (22 tests) — all four are touch-based lifecycle wire-ups in GameplayLifecycleShared.cpp.
- **Editor**: 59 panels, all wired including GizmoSystem, CollaborativeEditSession, CinematicSequencer, TimeOfDay, AbilityEditor, TriggerEditor, ConditionEditor, DecalEditor. `NetworkDebugPanel` now auto-polls `NetworkManager::GetStats()` each frame. `SelectionManager` is now the single source of truth for editor selection: `HierarchyPanel` mirrors its state into the singleton (NotifySelectionChanged → SelectMultiple) and `InspectorPanel` observes it (OnSelectionChanged → SetInspectedObjectByID); `SceneViewPanel` has no selection state of its own.
- **Rendering**: 6 RHI backends (D3D11, D3D12, Vulkan, OpenGL, Metal, NullRHI). `FoliageRenderer::UploadToSceneBuffer` is wired from `GraphicsEngine::EndFrame()` so the foliage CPU batch reaches the GPU each frame. The `FoliageImpostorAtlas` is now lazily baked from `FoliageRenderer::CollectFromFoliageManager` whenever the species count grows — `FoliageManager::GetSpeciesByGlobalIndex` enables registry walking and `FoliageImpostorAtlas::BakeAllRegisteredSpecies` does layout + per-species bake in one call. The atlas SRV is exposed via `GetImpostorAtlas().GetSRV()` but the foliage VS/PS pair does not yet sample it (separate session). `DXRSupport` finished: per-PSO shader tables, real DXIL blob loading from `.cso` files, lazy output texture, per-frame constant buffer. CMake DXC build step now compiles `Shaders/HLSL/RayTracing/DXR*.hlsl → .cso` with `lib_6_3` profile when `find_program(dxc)` succeeds on Windows MSVC builds; missing dxc is logged but non-fatal. Top-level `Shaders/HLSL/` tree (~91 files) is now copied to the runtime directory so all engine shaders are reachable. Remaining Tier 1 stubs with `@warning` headers: `VRSystem` (awaiting OpenXR SDK), `SteamTransport` (awaiting Steamworks SDK), `SteamPlatform`/`EpicPlatform`/`ConsolePlatform` in `OnlineServices`. ~25 Graphics utility headers intentionally demand-driven (see `stub-and-abandoned-features-2026-04-10.md`).
- **Passive registries (demand-driven, not in lifecycle)**: `NavMeshManager`, `NavMeshObstacleManager`, `LODManager`, `AnimationManager` — each has a header `@note` explaining the pattern. Consumed on demand by AI / render / animation / level-streaming code, exercised by dedicated tests.
- **Post-processing**: 16 passes (GTAO, SSAOTemporal, Bloom, AutoExposure, Tonemapping, ColorGrading, FXAA, DOF, MotionBlur, Vignette, ChromaticAberration, FilmGrain, LensDistortion, LightShafts, LensFlare, Sharpen). GTAO (Phase I) and SSAOTemporal (Phase J) are Tier 2 graphics-orphan activations — the horizon-based AO pass from `GTAOEffect.h` and the variance-clipped denoiser from `SSAOTemporal.h` run first so they modulate scene lighting at HDR resolution before Bloom. Phase J also wires `RenderTargetPool`, `GPUDebugMarkers`, and `GPUTimestampQuery` into the pipeline: the pool ticks each frame and exposes a public query surface, every pass is bracketed by a `ScopedGPUEvent` for PIX/RenderDoc captures, and per-pass GPU timestamps replace the previous `std::chrono` CPU-side measurements when a D3D11 device is attached. Phase K activates `VolumeManager` as a spatial post-process volume system — users create global or local volumes via `PostProcessingPipeline::GetVolumeManager()`, set the camera position with `SetCameraPosition()`, and on each `Process()` call the blended `VolumeStack` (exposure compensation, bloom, color grading, fog) is pushed into the pipeline's effect settings via `ApplyVolumeStack()`. Only fields whose `overrideState` was set by a real volume land on the settings, so hand-authored values survive volume-less frames.
- **ECS**: 75 component types across 17 headers, 25 systems
- **Game modules**: 10 (SparkGame, FPS, MMO, RPG, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript)
- **Infrastructure**: JobSystem wired, DeferredDeletionQueue in RHI, collision layer filtering, EntityEventBus cleanup, archetype spawn overrides
- **Gameplay**: TimeOfDaySystem, AI enemies in SparkGame, WeatherSystem integration
- **Codebase**: ~529K lines of C++ across 1620 source files, 125 wiki pages

### Before Writing Code

1. Check file size — if over 500 lines (.cpp) or 300 lines (.h), consider splitting
2. Search for existing implementations before adding new ones
3. Use `EngineContext` service locator, not deprecated globals
4. Run `clang-format -i` on modified files before committing

### CI Quick Reference

- `gh run list` + `gh run view <ID> --log-failed` for CI failures
- Metal files excluded from clang-format checks
- `build-windows-vs2026` and `clang-tidy` are `continue-on-error` (not blockers)

### Phase 5 — COMPLETE

All Phase 5 items resolved:
- ~~Refactor 66 oversized functions~~ — 9 remain, documented as acceptable (clear linear code)
- ~~Fix duplicate MaterialSystem functions~~ — Resolved (wrapper+impl, not duplicates)
- ~~15 critical missing test suites~~ — All exist and pass (257 test files, 3311+ tests)
- ~~Documentation specs~~ — All 3 complete (networking-wire-format.md, asset-format.md, plugin-abi-guide.md)
- ~~Last TODO (EventResponseSystem.cpp)~~ — Wired ConditionSystem via EngineContext
- **0 TODOs remaining in source tree**

---

_To add a new entry: create a file in `knowledge/`, add a row to the table above, then commit both._
