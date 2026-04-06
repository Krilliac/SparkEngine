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
| Documentation coverage (89 wiki pages, 100% Doxygen) | [knowledge/documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md) | Observation | Active | 2026-04-04 |
| ThirdParty dependencies | [knowledge/thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md) | Observation | Active | 2026-03-17 |
| Load test baseline (frame/CPU/memory benchmarks) | [knowledge/load-test-baseline.md](knowledge/load-test-baseline.md) | Observation | Active | 2026-03-26 |
| Live editor testing (Xvfb + Mesa llvmpipe) | [knowledge/live-editor-testing.md](knowledge/live-editor-testing.md) | Pattern | Active | 2026-03-28 |
| MinGW + Wine cross-compilation (D3D11 on Linux, 64 fixes) | [knowledge/mingw-wine-cross-compilation.md](knowledge/mingw-wine-cross-compilation.md) | Pattern | Active | 2026-03-29 |
| Hardware acceleration (8 GPU systems, 72 tests) | [knowledge/hardware-acceleration-systems.md](knowledge/hardware-acceleration-systems.md) | Decision | Active | 2026-03-28 |
| Mac compatibility analysis (gaps, roadmap, changes) | [knowledge/mac-compatibility-analysis.md](knowledge/mac-compatibility-analysis.md) | Observation | Active | 2026-03-28 |
| Third-party library evaluation (5 added, 7 rejected) | [knowledge/third-party-library-evaluation.md](knowledge/third-party-library-evaluation.md) | Decision | Active | 2026-03-31 |
| Engine viability evaluation (can you make a game?) | [knowledge/engine-viability-evaluation.md](knowledge/engine-viability-evaluation.md) | Observation | Active | 2026-04-01 |
| Project recommendations (13+3 production systems) | [knowledge/project-recommendations-2026-04-04.md](knowledge/project-recommendations-2026-04-04.md) | Decision | Active | 2026-04-04 |
| Engine feature recommendations (7 new game-making systems) | [knowledge/engine-feature-recommendations-2026-04-04.md](knowledge/engine-feature-recommendations-2026-04-04.md) | Decision | Active | 2026-04-04 |
| Engine next steps (15 recommendations, 4 tiers) | [knowledge/engine-recommendations-2026-04-04.md](knowledge/engine-recommendations-2026-04-04.md) | Decision | Active | 2026-04-04 |
| Memory integrity system (branch guards, code scanning) | [knowledge/memory-integrity-system.md](knowledge/memory-integrity-system.md) | Observation | Active | 2026-04-05 |
| Header namespace issues (Animation, Network, PostProcess) | [knowledge/header-namespace-issues.md](knowledge/header-namespace-issues.md) | Observation | **Resolved** | 2026-04-05 |
| Memory safety evaluation & C++26 bridge (Contracts, NonNull, SafeCast) | [knowledge/memory-safety-evaluation.md](knowledge/memory-safety-evaluation.md) | Decision | Active | 2026-04-06 |
## Quick Reference

### Current Engine State (2026-04-06)

- **Physics**: Jolt Physics (migrated from Bullet3). Use `EngineContext::Get()->GetPhysics()`
- **Networking**: Enabled by default (`ENABLE_NETWORKING=ON`), UDP sockets, no external deps
- **Tests**: 323 test files, 3955 tests (all pass on native Linux except 1 pre-existing MMO test)
- **Editor**: 59 panels, all wired including GizmoSystem, CollaborativeEditSession, CinematicSequencer, TimeOfDay, AbilityEditor, TriggerEditor, ConditionEditor, DecalEditor
- **Rendering**: All 12 former stubs now have .cpp implementations. 6 RHI backends (D3D11, D3D12, Vulkan, OpenGL, Metal, NullRHI)
- **Post-processing**: 14 passes (Bloom, AutoExposure, Tonemapping, ColorGrading, FXAA, DOF, MotionBlur, Vignette, ChromaticAberration, FilmGrain, LensDistortion, LightShafts, LensFlare, Sharpen)
- **ECS**: 75 component types across 17 headers, 25 systems
- **Game modules**: 10 (SparkGame, FPS, MMO, RPG, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript)
- **Infrastructure**: JobSystem wired, DeferredDeletionQueue in RHI, collision layer filtering, EntityEventBus cleanup, archetype spawn overrides
- **Gameplay**: TimeOfDaySystem, AI enemies in SparkGame, WeatherSystem integration
- **Codebase**: ~504K lines of C++ across 1551 source files, 117 wiki pages

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
