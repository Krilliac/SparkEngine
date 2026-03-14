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
| **Codebase bloat audit (critical)** | [knowledge/codebase-bloat-audit-2026-03-14.md](knowledge/codebase-bloat-audit-2026-03-14.md) | Observation | Active | 2026-03-14 |

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

**CRITICAL: 22 files violate size limits** → See [codebase-bloat-audit-2026-03-14.md](knowledge/codebase-bloat-audit-2026-03-14.md). Largest: SparkConsole.cpp (6,996 lines), GraphicsEngine.cpp (4,579), MaterialSystem.cpp (4,326).

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
