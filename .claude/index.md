# Persistence Context — Index

_Read this at every session start (after git sync). Each row links to a detailed knowledge file. If a topic matches your current task or domain, read the full file before proceeding._

## Knowledge Index

| Topic | File | Type | Status | Last Updated |
|-------|------|------|--------|--------------|
| GitHub API / PR check status | [knowledge/github-api-pr-checks.md](knowledge/github-api-pr-checks.md) | Issue | Resolved | 2026-03-14 |
| CI build failure patterns | [knowledge/ci-failures.md](knowledge/ci-failures.md) | Issue | Resolved | 2026-03-14 |
| Git rebase conflicts | [knowledge/git-rebase-conflicts.md](knowledge/git-rebase-conflicts.md) | Issue | Resolved | 2026-03-14 |
| clang-format issues | [knowledge/clang-format.md](knowledge/clang-format.md) | Issue | Resolved | 2026-03-14 |
| CMake Linux build failures | [knowledge/cmake-linux-build-failures.md](knowledge/cmake-linux-build-failures.md) | Issue | Resolved | 2026-03-14 |
| Windows MSVC /W4 warnings | [knowledge/windows-msvc-w4-warnings.md](knowledge/windows-msvc-w4-warnings.md) | Issue | Resolved | 2026-03-14 |
| **ConsoleProcessManager wired in** | [knowledge/consoleprocessmanager-wiring.md](knowledge/consoleprocessmanager-wiring.md) | Issue | Resolved | 2026-03-16 |
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| **SparkConsole refactor plan (critical)** | [knowledge/sparkconsole-refactor-plan.md](knowledge/sparkconsole-refactor-plan.md) | Pattern | Active | 2026-03-14 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |
| AI bloat pattern and countermeasures | [knowledge/ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md) | Observation | Active | 2026-03-14 |
| Codebase bloat audit (March 14) | [knowledge/codebase-bloat-audit-2026-03-14.md](knowledge/codebase-bloat-audit-2026-03-14.md) | Observation | Superseded | 2026-03-14 |
| Extended bloat audit (March 14) | [knowledge/bloat-audit-extended-2026-03-14.md](knowledge/bloat-audit-extended-2026-03-14.md) | Observation | Superseded | 2026-03-14 |
| **Comprehensive bloat audit (critical)** | [knowledge/codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md) | Observation | Active | 2026-03-15 |
| **30 orphaned headers (11K+ dead lines)** | [knowledge/orphaned-headers-audit.md](knowledge/orphaned-headers-audit.md) | Observation | Active | 2026-03-16 |
| **Editor panels: 4 restored, 9 deleted, all resolved** | [knowledge/editor-panel-bloat.md](knowledge/editor-panel-bloat.md) | Observation | Resolved | 2026-03-16 |
| **Feature restoration: 5 wrongly deleted features restored** | [knowledge/feature-restoration-2026-03-16.md](knowledge/feature-restoration-2026-03-16.md) | Decision | Resolved | 2026-03-16 |
| **5 duplicate systems, 2 of 3 ODR risks fixed** | [knowledge/duplicate-systems-audit.md](knowledge/duplicate-systems-audit.md) | Observation | Partially Resolved | 2026-03-16 |
| **11 orphaned tests, 14 untested subsystems** | [knowledge/test-suite-audit.md](knowledge/test-suite-audit.md) | Observation | Active | 2026-03-16 |
| **8 dead CMake options, duplicate imgui** | [knowledge/cmake-build-audit.md](knowledge/cmake-build-audit.md) | Observation | Active | 2026-03-16 |
| **26 singletons (12 orphaned), 74-member god object** | [knowledge/globals-singletons-audit.md](knowledge/globals-singletons-audit.md) | Observation | Active | 2026-03-16 |
| **66 oversized functions, 7 private-method violations, 4 duplicate functions** | [knowledge/code-quality-violations.md](knowledge/code-quality-violations.md) | Observation | Active | 2026-03-16 |
| **SECURITY: all 9 vulns fixed** | [knowledge/security-vulnerabilities.md](knowledge/security-vulnerabilities.md) | Issue | Resolved | 2026-03-16 |
| **THREADING: all critical races fixed** | [knowledge/thread-safety-issues.md](knowledge/thread-safety-issues.md) | Issue | Resolved | 2026-03-16 |
| **MEMORY/ERRORS: HRESULT, underflows, div-by-zero fixed; 3 low-risk OPEN** | [knowledge/memory-error-handling-issues.md](knowledge/memory-error-handling-issues.md) | Issue | Mostly Resolved | 2026-03-16 |
| **Rendering: 17 working, 12 header-only stubs (~15K dead lines)** | [knowledge/rendering-pipeline-status.md](knowledge/rendering-pipeline-status.md) | Observation | Active | 2026-03-16 |
| **Engine: 17 working, 7 orphaned systems (~90K+ dead lines)** | [knowledge/gameplay-systems-status.md](knowledge/gameplay-systems-status.md) | Observation | Active | 2026-03-16 |
| **Editor: 24 working, all panels resolved** | [knowledge/editor-functionality-status.md](knowledge/editor-functionality-status.md) | Observation | Mostly Resolved | 2026-03-16 |
| **SparkGame: 75% functional FPS, no AI enemies** | [knowledge/sparkgame-module-status.md](knowledge/sparkgame-module-status.md) | Observation | Active | 2026-03-16 |
| **SDK: ECS not exposed, unique_ptr DLL export, IGameModule gap** | [knowledge/sdk-api-surface-audit.md](knowledge/sdk-api-surface-audit.md) | Observation | Active | 2026-03-16 |
| **Docs: 53 wiki pages, 245/246 Doxygen, 10 critical gaps** | [knowledge/documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md) | Observation | Active | 2026-03-16 |
| **ThirdParty: 6 uninitialized submodules, curl dead code** | [knowledge/thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md) | Observation | Active | 2026-03-16 |

## Quick Reference by Topic

### Fixing problems (Issues)

**ConsoleProcessManager — RESOLVED** → Wired into all 5 startup paths. See [consoleprocessmanager-wiring.md](knowledge/consoleprocessmanager-wiring.md).

**Checking PR / CI status** → Use `gh run list` + `gh run view`, NOT `gh pr checks --watch`. See [github-api-pr-checks.md](knowledge/github-api-pr-checks.md).

**CI check failed** → Identify blocking vs. non-blocking jobs first. See [ci-failures.md](knowledge/ci-failures.md).

**Rebase conflict** → `<!-- AUTO:* -->` and `docs/api/` always take upstream. See [git-rebase-conflicts.md](knowledge/git-rebase-conflicts.md).

**clang-format failure** → Don't use `head -50` shortcut; match CI's Metal exclusion. See [clang-format.md](knowledge/clang-format.md).

**CMake configure/build fails on Linux** → Check submodules, apt packages, cache conflicts. See [cmake-linux-build-failures.md](knowledge/cmake-linux-build-failures.md).

**Windows CI fails but Linux passes** → MSVC `/W4` warnings. Use fix table. See [windows-msvc-w4-warnings.md](knowledge/windows-msvc-w4-warnings.md).

### Doing things well (Patterns & Optimizations)

**CRITICAL: 47 files violate size limits, 127 classes exceed method limit, 17 orphaned singletons** → See [codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md). 5 dead files (1,607 lines), 10,000+ removable lines total.

**CRITICAL: 30 orphaned headers never included anywhere (~11K lines)** → 19 graphics headers, 3 engine, 3 utils, 4 editor. See [orphaned-headers-audit.md](knowledge/orphaned-headers-audit.md).

**RESOLVED: All 10 unused editor panels resolved** → 4 restored (HierarchyPanel, ConsolePanel, MaterialEditor, PlayModeToolbar), 6 deleted (Dialogue, AssetDependency, AudioMixer, Profiler, Particle, RuntimeInspector). All 4 duplicate pairs resolved. See [editor-panel-bloat.md](knowledge/editor-panel-bloat.md).

**PARTIALLY RESOLVED: ODR risks** → AudioMixer renamed (fixed), AnimationStateMachine deduped (fixed), dual EventBus still open. See [duplicate-systems-audit.md](knowledge/duplicate-systems-audit.md).

**MEDIUM: 11 orphaned tests not in CMake, 14 untested subsystems** → See [test-suite-audit.md](knowledge/test-suite-audit.md).

**MEDIUM: 8 dead CMake build options** → ENABLE_LUA, ENABLE_PHYSX (no backend), 6 flags with no code guards. See [cmake-build-audit.md](knowledge/cmake-build-audit.md).

**INFO: 26 singletons total (12 orphaned), GraphicsEngine has 74 member variables** → See [globals-singletons-audit.md](knowledge/globals-singletons-audit.md).

**HIGH: 66 functions exceed 50-line limit, 4 duplicate functions in same files** → RegisterEngineConsoleCommands (555 lines), RenderMainMenuBar (514), main() (488). See [code-quality-violations.md](knowledge/code-quality-violations.md).

**HIGH: 7 classes exceed 10 private-method limit** → PostProcessingPipeline (84 private methods!), PhysicsSystem (42), SimpleConsole (42). See [code-quality-violations.md](knowledge/code-quality-violations.md).

**CRITICAL: SparkConsole needs refactoring** → 6,000+ lines of embedded UI bloat that should be in SparkConsole.exe instead. See [sparkconsole-refactor-plan.md](knowledge/sparkconsole-refactor-plan.md) for 2-session plan.

**BEFORE writing any code** → Check file size. If over 400 lines (.cpp) or 200 lines (.h), trim first. See [ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md).

**BEFORE adding a new method/class** → Search for existing one. Remove a duplicate if adding. See [workflow-patterns.md](knowledge/workflow-patterns.md).

**System is built but Initialize() is never called** → Either wire it in immediately or delete it. ConsoleProcessManager is an active example. See [ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md).

**Starting a task involving multiple codebase areas** → Launch parallel Explore agents. See [workflow-patterns.md](knowledge/workflow-patterns.md).

**After any structural code change** → Run `generate-api-docs.sh check` + `sync-wiki.sh sync`. See [workflow-patterns.md](knowledge/workflow-patterns.md).

### Working faster (Optimizations)

**Slow cmake build** → Add `--parallel $(nproc)`. See [build-optimizations.md](knowledge/build-optimizations.md).

**CI failure log is huge** → Use `gh run view <RUN_ID> --log-failed` for just the failures. See [build-optimizations.md](knowledge/build-optimizations.md).

**About to rebase** → Check `git log --oneline HEAD..origin/Working | wc -l` first. See [build-optimizations.md](knowledge/build-optimizations.md).

**Verify CMake preset flags without configuring** → `cmake --preset linux-gcc-release -N`. See [build-optimizations.md](knowledge/build-optimizations.md).

### Security & correctness (Issues)

**RESOLVED: Security vulnerabilities (9/9 fixed)** → DLL injection, command injection, path traversal, deserialization — all patched. See [security-vulnerabilities.md](knowledge/security-vulnerabilities.md).

**RESOLVED: Thread safety** → 11 highest-risk `atomic<bool>` conversions done, EventBus m_nextId atomic, recursion guard added, lock ordering documented. See [thread-safety-issues.md](knowledge/thread-safety-issues.md).

**MOSTLY RESOLVED: Memory/error handling** → HRESULT, underflows, div-by-zero, const-correctness all fixed. 3 low-risk items remain open (naked new in Physics, RHI .release() pattern, COM manual release). See [memory-error-handling-issues.md](knowledge/memory-error-handling-issues.md).

### Understanding the codebase (Observations)

**Networking/graphics don't compile** → Likely disabled by CMake toggles. See [codebase-observations.md](knowledge/codebase-observations.md).

**Using legacy globals like `g_graphics`** → Deprecated; use `EngineContext`. See [codebase-observations.md](knowledge/codebase-observations.md).

### Functional audit — what works vs scaffolding

**Rendering pipeline**: 17 features working (D3D11, forward/deferred/forward+, materials, lighting, particles, decals, LOD, FSR/DLSS). 12 header-only stubs (~15K dead lines): sky, water, GI, shadow atlas, render graph, occlusion culling, instance renderer. No terrain system. DXR complete but disabled. See [rendering-pipeline-status.md](knowledge/rendering-pipeline-status.md).

**Engine systems**: 17 working (ECS, physics, AI, animation, audio, input, camera, scripting, save, UI, modding, events, coroutines, 2D, dialogue, weather, world origin). 7 orphaned (~90K+ lines): streaming, procedural gen (52KB), cinematic sequencer (29KB), replay, destruction, achievement, localization. Missing engine-level: inventory, quest, weapon. See [gameplay-systems-status.md](knowledge/gameplay-systems-status.md).

**Editor**: 14 working panels (viewport, inspector, hierarchy, assets, physics debug, console, profiler, animation, PIE, undo, build, stats, toolbar). 8 built-not-shown (~10K lines): MaterialEditor (1,832), DialogueEditor (1,781), AssetDependency (1,551), AudioMixer (1,467), ParticleEditor (1,235), RuntimeInspector (1,003). 4 duplicate panel pairs. Missing: shader graph, AI debug, cinematic editor, project settings. See [editor-functionality-status.md](knowledge/editor-functionality-status.md).

**SparkGame**: 75% functional FPS framework. Working: module loading, player controller, weapons (18 types), HUD (production quality), vehicles (70%), level loading. Missing: AI enemies (0%). SparkConsole.exe and SparkShaderCompiler.exe are 100% functional tools. See [sparkgame-module-status.md](knowledge/sparkgame-module-status.md).

### SDK, docs, and dependencies

**SDK/API**: 6 headers (365 LOC), 18 subsystem getters via IEngineContext. CRITICAL: `std::unique_ptr<Game>` exported across DLL boundary (ABI risk). ECS not exposed (major gap). IGameModule.h is internal but used as public. See [sdk-api-surface-audit.md](knowledge/sdk-api-surface-audit.md).

**Documentation**: 53 wiki pages (29,583 lines), 245/246 headers with Doxygen (99.6%). Missing: threading model, networking protocol spec, asset format specs, 19 editor subsystem pages. README says "35 tests" but CLAUDE.md says "82+". See [documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md).

**Third-party deps**: 7 deps total, all permissive licenses (MIT/ZLib). ALL 6 git submodules uninitialized — build silently disables features. curl is dead code (declared but never built/linked). No version tracking file. Duplicate imgui target. See [thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md).

---

_To add a new entry: create a file in `knowledge/`, add a row to the table above, add a quick-reference line if relevant, then commit both files. See `.claude/README.md` for entry format._
