# Persistence Context — Index

_Read this at every session start (after git sync). Each row links to a detailed knowledge file._

## Knowledge Index

| Topic | File | Type | Status | Last Updated |
|-------|------|------|--------|--------------|
| Five-engine analysis (Cocos/Defold/Panda3D/S&box/Halley) | [knowledge/five-engine-analysis.md](knowledge/five-engine-analysis.md) | Decision | Active | 2026-03-19 |
| 11-engine + 3-framework analysis (30 recommendations) | [knowledge/eleven-engine-analysis.md](knowledge/eleven-engine-analysis.md) | Decision | Active | 2026-03-20 |
| ThorVG + Unity Graphics + 33 libraries analysis | [knowledge/thorvg-unity-graphics-analysis.md](knowledge/thorvg-unity-graphics-analysis.md) | Decision | Active | 2026-03-22 |
| Jolt Physics integration (migrated from Bullet3) | [knowledge/jolt-physics-integration.md](knowledge/jolt-physics-integration.md) | Observation | Active | 2026-03-22 |
| CI reproducible builds (local reproduction commands) | [knowledge/ci-reproducible-builds.md](knowledge/ci-reproducible-builds.md) | Pattern | Active | 2026-03-30 |
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |
| AI bloat pattern and countermeasures | [knowledge/ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md) | Observation | Active | 2026-03-14 |
| clang-format rules and Metal exclusion | [knowledge/clang-format.md](knowledge/clang-format.md) | Pattern | Active | 2026-03-14 |
| Git rebase conflict resolution | [knowledge/git-rebase-conflicts.md](knowledge/git-rebase-conflicts.md) | Pattern | Active | 2026-03-14 |
| GitHub API / PR checks | [knowledge/github-api-pr-checks.md](knowledge/github-api-pr-checks.md) | Pattern | Active | 2026-03-14 |
| Comprehensive bloat audit | [knowledge/codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md) | Observation | Active | 2026-03-18 |
| Deep test coverage analysis | [knowledge/test-suite-audit.md](knowledge/test-suite-audit.md) | Observation | Active | 2026-04-04 |
| 66 oversized functions, 7 private-method violations | [knowledge/code-quality-violations.md](knowledge/code-quality-violations.md) | Observation | Active | 2026-03-16 |
| Engine systems (29+ working, all wired) | [knowledge/gameplay-systems-status.md](knowledge/gameplay-systems-status.md) | Observation | Active | 2026-03-22 |
| SparkGame module status | [knowledge/sparkgame-module-status.md](knowledge/sparkgame-module-status.md) | Observation | Active | 2026-03-22 |
| Documentation coverage (125 wiki pages, 100% Doxygen) | [knowledge/documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md) | Observation | Active | 2026-04-06 |
| ThirdParty dependencies | [knowledge/thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md) | Observation | Active | 2026-04-09 |
| Load test baseline (frame/CPU/memory benchmarks) | [knowledge/load-test-baseline.md](knowledge/load-test-baseline.md) | Observation | Active | 2026-03-26 |
| Live editor testing (Xvfb + Mesa llvmpipe) | [knowledge/live-editor-testing.md](knowledge/live-editor-testing.md) | Pattern | Active | 2026-04-07 |
| MinGW + Wine cross-compilation (D3D11 on Linux, 64 fixes) | [knowledge/mingw-wine-cross-compilation.md](knowledge/mingw-wine-cross-compilation.md) | Pattern | Active | 2026-03-29 |
| Hardware acceleration (8 GPU systems, 72 tests) | [knowledge/hardware-acceleration-systems.md](knowledge/hardware-acceleration-systems.md) | Decision | Active | 2026-03-28 |
| Mac compatibility analysis (gaps, roadmap, ProcessDrawList ported end-to-end) | [knowledge/mac-compatibility-analysis.md](knowledge/mac-compatibility-analysis.md) | Observation | Active | 2026-04-18 |
| Third-party library evaluation (5 added, 7 rejected) | [knowledge/third-party-library-evaluation.md](knowledge/third-party-library-evaluation.md) | Decision | Active | 2026-03-31 |
| Engine viability evaluation (can you make a game?) | [knowledge/engine-viability-evaluation.md](knowledge/engine-viability-evaluation.md) | Observation | Active | 2026-04-01 |
| Project recommendations (13+3 production systems) | [knowledge/project-recommendations-2026-04-04.md](knowledge/project-recommendations-2026-04-04.md) | Decision | Active | 2026-04-04 |
| Engine feature recommendations (7 new game-making systems) | [knowledge/engine-feature-recommendations-2026-04-04.md](knowledge/engine-feature-recommendations-2026-04-04.md) | Decision | Active | 2026-04-04 |
| Memory integrity system (branch guards, code scanning) | [knowledge/memory-integrity-system.md](knowledge/memory-integrity-system.md) | Observation | Active | 2026-04-05 |
| Memory safety evaluation & C++26 bridge (Contracts, NonNull, SafeCast) | [knowledge/memory-safety-evaluation.md](knowledge/memory-safety-evaluation.md) | Decision | Active | 2026-04-06 |
| Stub & abandoned features catalog (Tier 1–4, Graphics orphans, VR/Steam stubs) | [knowledge/stub-and-abandoned-features-2026-04-10.md](knowledge/stub-and-abandoned-features-2026-04-10.md) | Observation | Active | 2026-04-14 |
| Wine role in SparkEngine + layered fallback tiers (live execution path) | [knowledge/wine-role-and-fallback-tiers-2026-04-14.md](knowledge/wine-role-and-fallback-tiers-2026-04-14.md) | Decision | Active | 2026-04-14 |
| Wine `-no-jobsystem` breakthrough — 80% RC=0 on gVisor; recipe baked into `tools/wine-run.sh` | [knowledge/wine-no-jobsystem-breakthrough-2026-04-16.md](knowledge/wine-no-jobsystem-breakthrough-2026-04-16.md) | Issue + Pattern | Active | 2026-04-16 |
| GPU/CPU separation plan (portability + wiring + RHI parity roadmap — Phase 1–4 shipped) | [knowledge/gpu-cpu-separation-plan-2026-04-12.md](knowledge/gpu-cpu-separation-plan-2026-04-12.md) | Plan | Reference | 2026-04-12 |
| Reflection & polymorphism refactoring (Phase 1/2/8B done, FieldInfo attrs, ReflectionSerializer) | [knowledge/reflection-polymorphism-refactoring-plan-2026-04-12.md](knowledge/reflection-polymorphism-refactoring-plan-2026-04-12.md) | Plan | Reference | 2026-04-12 |
| SparkDaemon services architecture (Asset, Shader, Collab Broker, Build Monitor — 6 phases shipped) | [knowledge/daemon-services-architecture-2026-04-16.md](knowledge/daemon-services-architecture-2026-04-16.md) | Plan | Reference | 2026-04-16 |
| Advanced techniques catalog (34 items across Rendering/ECS/AI/Physics/Net/Streaming/Scripting; #15 priority-tiered QueuedEventBus shipped) | [knowledge/advanced-techniques-catalog-2026-04-18.md](knowledge/advanced-techniques-catalog-2026-04-18.md) | Decision + Plan | Active | 2026-04-18 |

## Quick Reference

### Current Engine State (2026-04-18)

- **Physics**: Jolt Physics (migrated from Bullet3). Use `EngineContext::Get()->GetPhysics()`.
- **Networking**: Enabled by default (`ENABLE_NETWORKING=ON`), UDP sockets, no external deps.
- **Tests**: 482+ test files, ~5930 tests on SparkTests. Full suite runs green.
- **Editor**: 59 panels, all wired including GizmoSystem, CollaborativeEditSession, CinematicSequencer, TimeOfDay, AbilityEditor, TriggerEditor, ConditionEditor, DecalEditor. `NetworkDebugPanel` auto-polls `NetworkManager::GetStats()` each frame. `SelectionManager` is the single source of truth for editor selection.
- **Rendering**: 6 RHI backends (D3D11, D3D12, Vulkan, OpenGL, Metal, NullRHI). Foliage CPU→GPU pipeline wired through `GraphicsEngine::EndFrame()`. `FoliageImpostorAtlas` lazily baked. DXR finished with per-PSO shader tables, real DXIL loading, lazy output texture. CMake DXC compiles `Shaders/HLSL/RayTracing/DXR*.hlsl → .cso` when dxc is present. ~25 Graphics utility headers intentionally demand-driven.
- **Passive registries (demand-driven)**: `NavMeshManager`, `NavMeshObstacleManager`, `LODManager`, `AnimationManager` — consumed on demand by AI/render/animation/level-streaming code, exercised by dedicated tests.
- **Post-processing**: 16 passes — GTAO, SSAOTemporal, Bloom, AutoExposure, Tonemapping, ColorGrading, FXAA, DOF, MotionBlur, Vignette, ChromaticAberration, FilmGrain, LensDistortion, LightShafts, LensFlare, Sharpen. Per-pass GPU timestamps via `GPUTimestampQuery`, `ScopedGPUEvent` PIX/RenderDoc markers, `VolumeManager` blends spatial post-process volumes each frame.
- **Events**: `QueuedEventBus` has priority tiers — Critical / Normal / Low. Dispatch order is Critical → Normal → Low; Low is evicted first under queue pressure, Critical only as last-resort safety valve.
- **ECS**: 75 component types across 17 headers, 25 systems.
- **Game modules**: 10 (SparkGame, FPS, MMO, RPG, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript).
- **Infrastructure**: JobSystem wired, DeferredDeletionQueue in RHI, collision layer filtering, EntityEventBus cleanup, archetype spawn overrides.
- **Gameplay**: TimeOfDaySystem, AI enemies in SparkGame (driven by `AIBudgetLimiter` distance-prioritised tick), WeatherSystem integration.
- **Codebase**: ~566K lines of C++ across 1843 source files, 144 wiki pages.

### Before Writing Code

1. Check file size — if over 500 lines (.cpp) or 300 lines (.h), consider splitting.
2. Search for existing implementations before adding new ones.
3. Use `EngineContext` service locator, not deprecated globals.
4. Run `clang-format -i` on modified files before committing.

### CI Quick Reference

- `gh run list` + `gh run view <ID> --log-failed` for CI failures.
- Metal files excluded from clang-format checks.
- `build-windows-vs2026`, `build-linux-mingw-wine`, `build-macos`, `clang-tidy` are `continue-on-error` (not blockers).

---

_To add a new entry: create a file in `knowledge/`, add a row to the table above, then commit both. Delete completed single-shot session logs — the code is in the repo and the history is in git._
