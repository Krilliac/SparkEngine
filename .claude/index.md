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
| **ConsoleProcessManager unwired (critical)** | [knowledge/consoleprocessmanager-wiring.md](knowledge/consoleprocessmanager-wiring.md) | Issue | Active | 2026-03-14 |
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| **SparkConsole refactor plan (critical)** | [knowledge/sparkconsole-refactor-plan.md](knowledge/sparkconsole-refactor-plan.md) | Pattern | Active | 2026-03-14 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |
| AI bloat pattern and countermeasures | [knowledge/ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md) | Observation | Active | 2026-03-14 |
| Codebase bloat audit (March 14) | [knowledge/codebase-bloat-audit-2026-03-14.md](knowledge/codebase-bloat-audit-2026-03-14.md) | Observation | Superseded | 2026-03-14 |
| Extended bloat audit (March 14) | [knowledge/bloat-audit-extended-2026-03-14.md](knowledge/bloat-audit-extended-2026-03-14.md) | Observation | Superseded | 2026-03-14 |
| **Comprehensive bloat audit (critical)** | [knowledge/codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md) | Observation | Active | 2026-03-15 |
| **30 orphaned headers (11K+ dead lines)** | [knowledge/orphaned-headers-audit.md](knowledge/orphaned-headers-audit.md) | Observation | Active | 2026-03-16 |
| **8 unused editor panels (11K+ dead lines)** | [knowledge/editor-panel-bloat.md](knowledge/editor-panel-bloat.md) | Observation | Active | 2026-03-16 |
| **5 duplicate systems, 3 ODR risks** | [knowledge/duplicate-systems-audit.md](knowledge/duplicate-systems-audit.md) | Observation | Active | 2026-03-16 |
| **11 orphaned tests, 14 untested subsystems** | [knowledge/test-suite-audit.md](knowledge/test-suite-audit.md) | Observation | Active | 2026-03-16 |
| **8 dead CMake options, duplicate imgui** | [knowledge/cmake-build-audit.md](knowledge/cmake-build-audit.md) | Observation | Active | 2026-03-16 |
| **26 singletons (12 orphaned), 74-member god object** | [knowledge/globals-singletons-audit.md](knowledge/globals-singletons-audit.md) | Observation | Active | 2026-03-16 |
| **66 oversized functions, 7 private-method violations, 4 duplicate functions** | [knowledge/code-quality-violations.md](knowledge/code-quality-violations.md) | Observation | Active | 2026-03-16 |

## Quick Reference by Topic

### Fixing problems (Issues)

**CRITICAL: ConsoleProcessManager never called** → It's unwired. 15-min fix: add Initialize() at startup + ProcessCommands() in main loop. See [consoleprocessmanager-wiring.md](knowledge/consoleprocessmanager-wiring.md).

**Checking PR / CI status** → Use `gh run list` + `gh run view`, NOT `gh pr checks --watch`. See [github-api-pr-checks.md](knowledge/github-api-pr-checks.md).

**CI check failed** → Identify blocking vs. non-blocking jobs first. See [ci-failures.md](knowledge/ci-failures.md).

**Rebase conflict** → `<!-- AUTO:* -->` and `docs/api/` always take upstream. See [git-rebase-conflicts.md](knowledge/git-rebase-conflicts.md).

**clang-format failure** → Don't use `head -50` shortcut; match CI's Metal exclusion. See [clang-format.md](knowledge/clang-format.md).

**CMake configure/build fails on Linux** → Check submodules, apt packages, cache conflicts. See [cmake-linux-build-failures.md](knowledge/cmake-linux-build-failures.md).

**Windows CI fails but Linux passes** → MSVC `/W4` warnings. Use fix table. See [windows-msvc-w4-warnings.md](knowledge/windows-msvc-w4-warnings.md).

### Doing things well (Patterns & Optimizations)

**CRITICAL: 47 files violate size limits, 127 classes exceed method limit, 17 orphaned singletons** → See [codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md). 5 dead files (1,607 lines), 10,000+ removable lines total.

**CRITICAL: 30 orphaned headers never included anywhere (~11K lines)** → 19 graphics headers, 3 engine, 3 utils, 4 editor. See [orphaned-headers-audit.md](knowledge/orphaned-headers-audit.md).

**CRITICAL: 8 editor panels built but never shown (11,257 lines)** → MaterialEditor, Dialogue, AssetDependency, AudioMixer, Profiler, Particle, RuntimeInspector, PlayModeToolbar. Plus engine-depends-on-editor violation in AllEnums.h. See [editor-panel-bloat.md](knowledge/editor-panel-bloat.md).

**HIGH: 3 ODR violation risks** → AudioMixer (2 defs), AnimationStateMachine (2 defs), dual EventBus implementations. See [duplicate-systems-audit.md](knowledge/duplicate-systems-audit.md).

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

### Understanding the codebase (Observations)

**Networking/graphics don't compile** → Likely disabled by CMake toggles. See [codebase-observations.md](knowledge/codebase-observations.md).

**Using legacy globals like `g_graphics`** → Deprecated; use `EngineContext`. See [codebase-observations.md](knowledge/codebase-observations.md).

---

_To add a new entry: create a file in `knowledge/`, add a row to the table above, add a quick-reference line if relevant, then commit both files. See `.claude/README.md` for entry format._
