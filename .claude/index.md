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
## Quick Reference

### Current Engine State (2026-04-11)

- **Physics**: Jolt Physics (migrated from Bullet3). Use `EngineContext::Get()->GetPhysics()`
- **Networking**: Enabled by default (`ENABLE_NETWORKING=ON`), UDP sockets, no external deps
- **Tests**: 356 test files, 4605 tests on SparkTests (Phase I adds 8 GTAO tests); all pass on native Linux (1 pre-existing flaky replication test now in TestWarnings.h); total 118831+ assertions
- **Editor**: 59 panels, all wired including GizmoSystem, CollaborativeEditSession, CinematicSequencer, TimeOfDay, AbilityEditor, TriggerEditor, ConditionEditor, DecalEditor. `NetworkDebugPanel` now auto-polls `NetworkManager::GetStats()` each frame. `SelectionManager` is now the single source of truth for editor selection: `HierarchyPanel` mirrors its state into the singleton (NotifySelectionChanged → SelectMultiple) and `InspectorPanel` observes it (OnSelectionChanged → SetInspectedObjectByID); `SceneViewPanel` has no selection state of its own.
- **Rendering**: 6 RHI backends (D3D11, D3D12, Vulkan, OpenGL, Metal, NullRHI). `FoliageRenderer::UploadToSceneBuffer` is wired from `GraphicsEngine::EndFrame()` so the foliage CPU batch reaches the GPU each frame. The `FoliageImpostorAtlas` is now lazily baked from `FoliageRenderer::CollectFromFoliageManager` whenever the species count grows — `FoliageManager::GetSpeciesByGlobalIndex` enables registry walking and `FoliageImpostorAtlas::BakeAllRegisteredSpecies` does layout + per-species bake in one call. The atlas SRV is exposed via `GetImpostorAtlas().GetSRV()` but the foliage VS/PS pair does not yet sample it (separate session). `DXRSupport` finished: per-PSO shader tables, real DXIL blob loading from `.cso` files, lazy output texture, per-frame constant buffer. CMake DXC build step now compiles `Shaders/HLSL/RayTracing/DXR*.hlsl → .cso` with `lib_6_3` profile when `find_program(dxc)` succeeds on Windows MSVC builds; missing dxc is logged but non-fatal. Top-level `Shaders/HLSL/` tree (~91 files) is now copied to the runtime directory so all engine shaders are reachable. Remaining Tier 1 stubs with `@warning` headers: `VRSystem` (awaiting OpenXR SDK), `SteamTransport` (awaiting Steamworks SDK), `SteamPlatform`/`EpicPlatform`/`ConsolePlatform` in `OnlineServices`. ~25 Graphics utility headers intentionally demand-driven (see `stub-and-abandoned-features-2026-04-10.md`).
- **Passive registries (demand-driven, not in lifecycle)**: `NavMeshManager`, `NavMeshObstacleManager`, `LODManager`, `AnimationManager` — each has a header `@note` explaining the pattern. Consumed on demand by AI / render / animation / level-streaming code, exercised by dedicated tests.
- **Post-processing**: 15 passes (GTAO, Bloom, AutoExposure, Tonemapping, ColorGrading, FXAA, DOF, MotionBlur, Vignette, ChromaticAberration, FilmGrain, LensDistortion, LightShafts, LensFlare, Sharpen). GTAO is Phase I's first Tier 2 graphics-orphan activation — the horizon-based AO pass from `GTAOEffect.h` is wired as the 15th pipeline pass and slotted first so it modulates scene lighting at HDR resolution before Bloom.
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
