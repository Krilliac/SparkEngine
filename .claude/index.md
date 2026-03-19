# Persistence Context — Index

_Read this at every session start (after git sync). Each row links to a detailed knowledge file. If a topic matches your current task or domain, read the full file before proceeding._

## Knowledge Index

| Topic | File | Type | Status | Last Updated |
|-------|------|------|--------|--------------|
| TrinityCore architecture analysis | [knowledge/trinitycore-analysis.md](knowledge/trinitycore-analysis.md) | Decision | **Resolved** | 2026-03-18 |
| CryEngine feature analysis (16 systems adopted) | [knowledge/cryengine-analysis.md](knowledge/cryengine-analysis.md) | Decision | **Resolved** | 2026-03-19 |
| Five-engine analysis (Cocos/Defold/Panda3D/S&box/Halley — 20 recommendations) | [knowledge/five-engine-analysis.md](knowledge/five-engine-analysis.md) | Decision | Active | 2026-03-19 |
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| SparkConsole refactor | [knowledge/sparkconsole-refactor-plan.md](knowledge/sparkconsole-refactor-plan.md) | Pattern | **Resolved** | 2026-03-17 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |
| AI bloat pattern and countermeasures | [knowledge/ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md) | Observation | Active | 2026-03-14 |
| Comprehensive bloat audit (P0 resolved) | [knowledge/codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md) | Observation | Active | 2026-03-18 |
| 30 orphaned headers (27 deleted, 3 intentional) | [knowledge/orphaned-headers-audit.md](knowledge/orphaned-headers-audit.md) | Observation | **Resolved** | 2026-03-16 |
| Duplicate systems (3/3 ODR risks fixed) | [knowledge/duplicate-systems-audit.md](knowledge/duplicate-systems-audit.md) | Observation | **Mostly Resolved** | 2026-03-17 |
| 97 tests (12 new: 9 TrinityCore + ConsoleRBAC + NetworkIntegration) | [knowledge/test-suite-audit.md](knowledge/test-suite-audit.md) | Observation | Active | 2026-03-19 |
| CMake build (curl removed, dead options gone) | [knowledge/cmake-build-audit.md](knowledge/cmake-build-audit.md) | Observation | **Mostly Resolved** | 2026-03-17 |
| 27 singletons (0 orphaned, 22 wired, 5 passive caches) | [knowledge/globals-singletons-audit.md](knowledge/globals-singletons-audit.md) | Observation | **Resolved** | 2026-03-18 |
| 66 oversized functions, 7 private-method violations | [knowledge/code-quality-violations.md](knowledge/code-quality-violations.md) | Observation | Active | 2026-03-16 |
| MEMORY/ERRORS: 3 low-risk items remain | [knowledge/memory-error-handling-issues.md](knowledge/memory-error-handling-issues.md) | Issue | Mostly Resolved | 2026-03-16 |
| Rendering: 17 working, 12 header-only stubs (~15K dead lines) | [knowledge/rendering-pipeline-status.md](knowledge/rendering-pipeline-status.md) | Observation | Active | 2026-03-16 |
| Engine: 29 working (10 TrinityCore new), 0 orphaned | [knowledge/gameplay-systems-status.md](knowledge/gameplay-systems-status.md) | Observation | Active | 2026-03-18 |
| Editor: 24 working, all panels resolved | [knowledge/editor-functionality-status.md](knowledge/editor-functionality-status.md) | Observation | **Resolved** | 2026-03-16 |
| SparkGame: 75% functional FPS, no AI enemies | [knowledge/sparkgame-module-status.md](knowledge/sparkgame-module-status.md) | Observation | Active | 2026-03-16 |
| SDK: unique_ptr fixed, ECS exposed, IGameModule gap | [knowledge/sdk-api-surface-audit.md](knowledge/sdk-api-surface-audit.md) | Observation | **Mostly Resolved** | 2026-03-17 |
| Docs: 58 wiki pages, 245/246 Doxygen, 5 critical gaps remaining | [knowledge/documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md) | Observation | Active | 2026-03-19 |
| ThirdParty: 6 uninitialized submodules, curl removed | [knowledge/thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md) | Observation | Active | 2026-03-17 |

## Quick Reference by Topic

### Fixing problems (Issues)

**Checking PR / CI status** → Use `gh run list` + `gh run view`, NOT `gh pr checks --watch`.

**CI check failed** → Identify blocking vs. non-blocking jobs first. See CLAUDE.md CI jobs table.

**Rebase conflict** → `<!-- AUTO:* -->` and `docs/api/` always take upstream.

**clang-format failure** → Match CI's Metal exclusion; don't use `head -50` shortcut.

**CMake configure/build fails on Linux** → Check submodules, apt packages, cache conflicts.

**Windows CI fails but Linux passes** → MSVC `/W4` warnings. See CLAUDE.md CI section.

**MOSTLY RESOLVED: Memory/error handling** → 3 low-risk items remain open. See [memory-error-handling-issues.md](knowledge/memory-error-handling-issues.md).

### Doing things well (Patterns & Optimizations)

**RESOLVED: SparkConsole** → Refactored from ~7,000 to 551 lines. Now a clean log sink + command registry.

**RESOLVED: EventBus ODR** → EventSystem.h now delegates to Utils/EventBus.h. Single canonical implementation.

**RESOLVED: SDK unique_ptr** → Changed to raw pointer with manual lifecycle. ECS World exposed via GetWorld().

**RESOLVED: curl dependency** → Removed from .gitmodules and CMakeLists.txt.

**RESOLVED: 9 TrinityCore systems now have tests (2026-03-18):**
- TestAbilitySystem, TestConditionSystem, TestInstanceManager, TestMovementSystem
- TestSpatialGrid, TestReplicationFields, TestAsyncDatabase, TestScriptHookManager, TestModuleHotReload
- All 9 added to Tests/CMakeLists.txt (94 total test files)

**Full gap analysis completed and P1 items resolved (2026-03-18):**
- See `docs/gap-analysis/COMPREHENSIVE_GAP_ANALYSIS_2026-03-18.md`
- Covers all 18 subsystem categories with DONE/PARTIAL/STUB/MISSING/ORPHANED status
- P1 items completed: PlatformInputManager deleted, WeaponSystem wired, CoroutineScheduler wired, MusicManager wired, Camera + Persistence wiki pages created, ConsoleRBAC tested
- P2 items completed: Threading wiki page, SparkEditor.md expanded (32 panels), network integration tests (30 tests), API docs generated
- Key remaining gaps: terrain rendering, audio tests, connection timeout detection

**Active: 20 recommendations from 5-engine analysis (2026-03-19):**
- Cocos Engine, Defold, Panda3D, S&box, Halley
- Top 5 quick wins: DrawMask visibility, FixedUpdate accumulator, network instability simulator, interpolation buffer, plugin registration macros
- Top networking: delta snapshots, interpolation buffer, instability simulator, FROM/INTO collision masks
- Top rendering: clustered light culling, light probes + SH, RHI validation layer, age-based resource pooling
- See [five-engine-analysis.md](knowledge/five-engine-analysis.md) for full prioritized list

**RESOLVED: 16 CryEngine-inspired systems (2026-03-19):**
- TerrainRenderer, TacticalPointSystem, CoverSystem, MaterialEffectSystem (HIGH)
- FormationSystem, GroupAISystem, CollisionAvoidanceSystem, DynamicResponseSystem, EntityArchetypeSystem, ProximityTriggerSystem (MEDIUM)
- SkyAtmosphereSystem, WaterRenderer, OcclusionCullingSystem, PoseModifierStack, AnimCompression, BlendSpace2D (LOW)
- All wired into SparkEngine.cpp Init/Update/Shutdown paths with tests

**RESOLVED: 10 TrinityCore-inspired systems (2026-03-18):**
- AbilitySystem (spells/auras/procs), SpatialGrid (cell partitioning), AsyncDatabase (persistence)
- ReplicationFields + EntityReplicator (dirty tracking), ScriptHookManager (28 hook types)
- MovementSystem (priority generator stack), ConditionSystem (universal conditions)
- InstanceManager (encounters/lockouts), ConsoleRBAC (permission levels), ModuleHotReload (DLL watcher)
- All wired into SparkEngine.cpp Init/Update/Shutdown paths. AbilitySystem.cpp split into 2 files.

**RESOLVED: 12 major file splits (2026-03-18):**
- GraphicsEngine.cpp (4,949 → 5 files), MaterialSystem.cpp (4,283 → 3 files)
- EditorUI.cpp (2,691 → 3 files), AssetPipeline.cpp (2,557 → 3 files)
- Shader.cpp (2,344 → 2 files), LightingSystem.cpp (2,239 → 2 files)
- PhysicsSystem.cpp (2,029 → 2 files), InspectorPanel.cpp (2,373 → 2 files)
- VulkanDevice.cpp (2,327 → 3 files), D3D12Device.cpp (1,893 → 2 files)
- AdvancedAssetPipeline.cpp (2,324 → 3 files), Game.cpp (2,042 → 2 files)
- LightingTools.cpp (1,963 → 2 files)
- Pattern: extract Console_* methods + class implementations + private helpers into separate TUs
- **No files over 2,000 lines remain.** Largest: Shader.cpp (1,922).

**Active: 66 functions exceed 50-line limit** → See [code-quality-violations.md](knowledge/code-quality-violations.md).

**BEFORE writing any code** → Check file size. If over 500 lines (.cpp) or 300 lines (.h), consider splitting. See [ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md).

**BEFORE adding a new method/class** → Search for existing one. See [workflow-patterns.md](knowledge/workflow-patterns.md).

### Working faster (Optimizations)

**Slow cmake build** → Add `--parallel $(nproc)`. See [build-optimizations.md](knowledge/build-optimizations.md).

**CI failure log is huge** → Use `gh run view <RUN_ID> --log-failed` for just the failures.

**About to rebase** → Check `git log --oneline HEAD..origin/Working | wc -l` first.

### Understanding the codebase (Observations)

**Networking/graphics don't compile** → Likely disabled by CMake toggles. See [codebase-observations.md](knowledge/codebase-observations.md).

**Using legacy globals like `g_graphics`** → Deprecated; use `EngineContext`. See [codebase-observations.md](knowledge/codebase-observations.md).

### Functional audit — what works vs scaffolding

**All 9 core subsystems are FUNCTIONAL** (confirmed 2026-03-17): Graphics (D3D11), Physics (Bullet3), Audio (XAudio2), ECS (EnTT), Input, Scene Management, AI (behavior trees + NavMesh), Animation (skeletal + IK), Main Game Loop.

**Rendering pipeline**: 17 working, 12 header-only stubs (~15K dead lines). See [rendering-pipeline-status.md](knowledge/rendering-pipeline-status.md).

**Engine systems**: 29 working (10 TrinityCore new), all tested. See [gameplay-systems-status.md](knowledge/gameplay-systems-status.md).

**Editor**: 24 working panels, all resolved. See [editor-functionality-status.md](knowledge/editor-functionality-status.md).

**SparkGame**: 75% functional FPS. See [sparkgame-module-status.md](knowledge/sparkgame-module-status.md).

**SDK/API**: unique_ptr fixed, ECS exposed. See [sdk-api-surface-audit.md](knowledge/sdk-api-surface-audit.md).

**Documentation**: 53 wiki pages, 99.6% Doxygen coverage. See [documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md).

**Third-party deps**: 5 uninitialized submodules (curl removed). See [thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md).

## Resolved Issues (cleaned up 2026-03-17)

_These issues were fully resolved. Key learnings are preserved inline above._

- **Security vulnerabilities** (9/9 fixed) — DLL injection, command injection, path traversal, deserialization all patched
- **Thread safety** — 11 atomic conversions, EventBus m_nextId atomic, recursion guard, lock ordering documented
- **ConsoleProcessManager** — Wired into all startup paths
- **Editor panel bloat** — 4 panels restored, 9 deleted, all duplicate pairs resolved
- **Feature restoration** — 5 wrongly deleted features restored
- **SparkConsole refactor** — From ~7,000 to 551 lines
- **EventBus ODR violation** — EventSystem.h now delegates to canonical EventBus.h
- **SDK unique_ptr DLL export** — Changed to raw pointer, ABI crash risk eliminated
- **ECS not in SDK** — GetWorld() added to IEngineContext
- **curl dead dependency** — Removed from .gitmodules and CMakeLists.txt
- **Dead CMake options** — ENABLE_LUA, ENABLE_PHYSX and 6 unused flags removed
- **12 major file splits** — No files over 2,000 lines remain (was 4,949)
- **Startup path analysis** — 3 platform entry points (WinMain/headless/SDL2) verified correct, not duplicate
- **Orphaned system triage** — DecalSystem wired; DXRManager partially gated; NavMesh/AnimationSystem/Sequencer need deeper integration
- **GitHub API/PR checks**, **CI failures**, **rebase conflicts**, **clang-format**, **CMake Linux builds**, **MSVC /W4 warnings** — all resolved

---

_To add a new entry: create a file in `knowledge/`, add a row to the table above, add a quick-reference line if relevant, then commit both files. See `.claude/README.md` for entry format._
