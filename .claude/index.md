# Persistence Context — Index

_Read this at every session start (after git sync). Each row links to a detailed knowledge file._

## Knowledge Index

| Topic | File | Type | Status | Last Updated |
|-------|------|------|--------|--------------|
| Five-engine analysis (Cocos/Defold/Panda3D/S&box/Halley) | [knowledge/five-engine-analysis.md](knowledge/five-engine-analysis.md) | Decision | Active | 2026-03-19 |
| 11-engine + 3-framework analysis (30 recommendations) | [knowledge/eleven-engine-analysis.md](knowledge/eleven-engine-analysis.md) | Decision | Active | 2026-03-20 |
| 13 closed/proprietary engine analysis (15 recommendations) | [knowledge/closed-engines-analysis.md](knowledge/closed-engines-analysis.md) | Decision | **Resolved** | 2026-03-21 |
| ThorVG + Unity Graphics + 33 libraries analysis | [knowledge/thorvg-unity-graphics-analysis.md](knowledge/thorvg-unity-graphics-analysis.md) | Decision | Active | 2026-03-22 |
| Engine next steps: 5-phase roadmap (Phases 1-4 complete) | [knowledge/engine-next-steps-2026-03-22.md](knowledge/engine-next-steps-2026-03-22.md) | Decision | Mostly Resolved | 2026-03-22 |
| Jolt Physics integration (migrated from Bullet3) | [knowledge/jolt-physics-integration.md](knowledge/jolt-physics-integration.md) | Observation | Active | 2026-03-22 |
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |
| AI bloat pattern and countermeasures | [knowledge/ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md) | Observation | Active | 2026-03-14 |
| Comprehensive bloat audit | [knowledge/codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md) | Observation | Active | 2026-03-18 |
| 145 tests (suite audit) | [knowledge/test-suite-audit.md](knowledge/test-suite-audit.md) | Observation | Active | 2026-03-19 |
| 66 oversized functions, 7 private-method violations | [knowledge/code-quality-violations.md](knowledge/code-quality-violations.md) | Observation | Active | 2026-03-16 |
| Memory/error handling (3 low-risk items) | [knowledge/memory-error-handling-issues.md](knowledge/memory-error-handling-issues.md) | Issue | Mostly Resolved | 2026-03-16 |
| Rendering pipeline (all 12 stubs now implemented) | [knowledge/rendering-pipeline-status.md](knowledge/rendering-pipeline-status.md) | Observation | **Resolved** | 2026-03-22 |
| Engine systems (29+ working, all wired) | [knowledge/gameplay-systems-status.md](knowledge/gameplay-systems-status.md) | Observation | Active | 2026-03-22 |
| SparkGame module (AI enemies now wired) | [knowledge/sparkgame-module-status.md](knowledge/sparkgame-module-status.md) | Observation | Active | 2026-03-22 |
| Documentation coverage (64 wiki pages, 99.6% Doxygen) | [knowledge/documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md) | Observation | Active | 2026-03-24 |
| ThirdParty dependencies | [knowledge/thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md) | Observation | Active | 2026-03-17 |
| Load test baseline (frame/CPU/memory benchmarks) | [knowledge/load-test-baseline.md](knowledge/load-test-baseline.md) | Observation | Active | 2026-03-26 |
| Live editor testing (Xvfb + Mesa llvmpipe) | [knowledge/live-editor-testing.md](knowledge/live-editor-testing.md) | Pattern | Active | 2026-03-28 |
## Quick Reference

### Current Engine State (2026-03-22)

- **Physics**: Jolt Physics (migrated from Bullet3). Use `EngineContext::Get()->GetPhysics()`
- **Networking**: Enabled by default (`ENABLE_NETWORKING=ON`), UDP sockets, no external deps
- **Tests**: 170 test files, 1,989 tests passing
- **Editor**: 52 panels, all wired including GizmoSystem, CollaborativeEditSession, CinematicSequencer, TimeOfDay, AbilityEditor, TriggerEditor, ConditionEditor, DecalEditor
- **Rendering**: All 12 former stubs now have .cpp implementations (ShadowAtlas, ScreenSpaceEffects, GPUOcclusionCulling, FroxelVolumetricFog, DynamicQualityScaler, DDGIProbeSystem, AdaptiveProbeVolumes, LightProbeSystem, SkyAtmosphere, WaterRenderer, ClusteredLightCulling, DynamicQualityTypes)
- **Post-processing**: Bloom, auto-exposure, tonemapping (ACES/Filmic/Neutral/Reinhard), color grading (LGG)
- **Infrastructure**: JobSystem wired, DeferredDeletionQueue in RHI, collision layer filtering, EntityEventBus cleanup, archetype spawn overrides
- **Gameplay**: TimeOfDaySystem, AI enemies in SparkGame, WeatherSystem integration

### Before Writing Code

1. Check file size — if over 500 lines (.cpp) or 300 lines (.h), consider splitting
2. Search for existing implementations before adding new ones
3. Use `EngineContext` service locator, not deprecated globals
4. Run `clang-format -i` on modified files before committing

### CI Quick Reference

- `gh run list` + `gh run view <ID> --log-failed` for CI failures
- Metal files excluded from clang-format checks
- `build-windows-vs2026` and `clang-tidy` are `continue-on-error` (not blockers)

### Phase 5 Remaining Work

- Refactor 66 oversized functions (see code-quality-violations.md)
- Fix duplicate functions in MaterialSystem.cpp
- Add AudioEngine and SceneManager test suites
- Documentation specs (networking wire format, asset format, plugin ABI)

---

_To add a new entry: create a file in `knowledge/`, add a row to the table above, then commit both._
