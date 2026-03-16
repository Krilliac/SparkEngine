# Persistence Context — Index

_Read this at every session start (after git sync). Each row links to a detailed knowledge file. If a topic matches your current task or domain, read the full file before proceeding._

## Knowledge Index

| Topic | File | Type | Status | Last Updated |
|-------|------|------|--------|--------------|
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| **SparkConsole refactor plan (critical)** | [knowledge/sparkconsole-refactor-plan.md](knowledge/sparkconsole-refactor-plan.md) | Pattern | Active | 2026-03-14 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |
| AI bloat pattern and countermeasures | [knowledge/ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md) | Observation | Active | 2026-03-14 |
| **Comprehensive bloat audit (critical)** | [knowledge/codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md) | Observation | Active | 2026-03-15 |
| **30 orphaned headers (11K+ dead lines)** | [knowledge/orphaned-headers-audit.md](knowledge/orphaned-headers-audit.md) | Observation | Active | 2026-03-16 |
| **5 duplicate systems, 2 of 3 ODR risks fixed** | [knowledge/duplicate-systems-audit.md](knowledge/duplicate-systems-audit.md) | Observation | Partially Resolved | 2026-03-16 |
| **11 orphaned tests, 14 untested subsystems** | [knowledge/test-suite-audit.md](knowledge/test-suite-audit.md) | Observation | Active | 2026-03-16 |
| **8 dead CMake options, duplicate imgui** | [knowledge/cmake-build-audit.md](knowledge/cmake-build-audit.md) | Observation | Active | 2026-03-16 |
| **26 singletons (12 orphaned), 74-member god object** | [knowledge/globals-singletons-audit.md](knowledge/globals-singletons-audit.md) | Observation | Active | 2026-03-16 |
| **66 oversized functions, 7 private-method violations, 4 duplicate functions** | [knowledge/code-quality-violations.md](knowledge/code-quality-violations.md) | Observation | Active | 2026-03-16 |
| **MEMORY/ERRORS: 3 low-risk items remain** | [knowledge/memory-error-handling-issues.md](knowledge/memory-error-handling-issues.md) | Issue | Mostly Resolved | 2026-03-16 |
| **Rendering: 17 working, 12 header-only stubs (~15K dead lines)** | [knowledge/rendering-pipeline-status.md](knowledge/rendering-pipeline-status.md) | Observation | Active | 2026-03-16 |
| **Engine: 17 working, 7 orphaned systems (~90K+ dead lines)** | [knowledge/gameplay-systems-status.md](knowledge/gameplay-systems-status.md) | Observation | Active | 2026-03-16 |
| **Editor: 24 working, all panels resolved** | [knowledge/editor-functionality-status.md](knowledge/editor-functionality-status.md) | Observation | Mostly Resolved | 2026-03-16 |
| **SparkGame: 75% functional FPS, no AI enemies** | [knowledge/sparkgame-module-status.md](knowledge/sparkgame-module-status.md) | Observation | Active | 2026-03-16 |
| **SDK: ECS not exposed, unique_ptr DLL export, IGameModule gap** | [knowledge/sdk-api-surface-audit.md](knowledge/sdk-api-surface-audit.md) | Observation | Active | 2026-03-16 |
| **Docs: 53 wiki pages, 245/246 Doxygen, 10 critical gaps** | [knowledge/documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md) | Observation | Active | 2026-03-16 |
| **ThirdParty: 6 uninitialized submodules, curl dead code** | [knowledge/thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md) | Observation | Active | 2026-03-16 |

## Quick Reference by Topic

### Fixing problems (Issues)

**Checking PR / CI status** → Use `gh run list` + `gh run view`, NOT `gh pr checks --watch`.

**CI check failed** → Identify blocking vs. non-blocking jobs first. See CLAUDE.md CI jobs table.

**Rebase conflict** → `<!-- AUTO:* -->` and `docs/api/` always take upstream.

**clang-format failure** → Match CI's Metal exclusion; don't use `head -50` shortcut.

**CMake configure/build fails on Linux** → Check submodules, apt packages, cache conflicts.

**Windows CI fails but Linux passes** → MSVC `/W4` warnings. See CLAUDE.md CI section.

**MOSTLY RESOLVED: Memory/error handling** → 3 low-risk items remain open (naked new in Physics, RHI .release() pattern, COM manual release). See [memory-error-handling-issues.md](knowledge/memory-error-handling-issues.md).

### Doing things well (Patterns & Optimizations)

**CRITICAL: 47 files violate size limits, 127 classes exceed method limit, 10 orphaned singletons (7 wired in this session)** → See [codebase-bloat-audit-2026-03-15.md](knowledge/codebase-bloat-audit-2026-03-15.md). All 7 dead utility headers now wired in. Remaining: DecalSystem, DXRSupport, PlatformInputManager, NavMesh.

**CRITICAL: 30 orphaned headers never included anywhere (~11K lines)** → See [orphaned-headers-audit.md](knowledge/orphaned-headers-audit.md).

**PARTIALLY RESOLVED: ODR risks** → Dual EventBus still open. See [duplicate-systems-audit.md](knowledge/duplicate-systems-audit.md).

**MEDIUM: 11 orphaned tests not in CMake, 14 untested subsystems** → See [test-suite-audit.md](knowledge/test-suite-audit.md).

**MEDIUM: 8 dead CMake build options** → See [cmake-build-audit.md](knowledge/cmake-build-audit.md).

**HIGH: 66 functions exceed 50-line limit, 4 duplicate functions** → See [code-quality-violations.md](knowledge/code-quality-violations.md).

**CRITICAL: SparkConsole needs refactoring** → 6,000+ lines of embedded UI bloat. See [sparkconsole-refactor-plan.md](knowledge/sparkconsole-refactor-plan.md).

**BEFORE writing any code** → Check file size. If over 400 lines (.cpp) or 200 lines (.h), trim first. See [ai-bloat-pattern.md](knowledge/ai-bloat-pattern.md).

**BEFORE adding a new method/class** → Search for existing one. Remove a duplicate if adding. See [workflow-patterns.md](knowledge/workflow-patterns.md).

### Working faster (Optimizations)

**Slow cmake build** → Add `--parallel $(nproc)`. See [build-optimizations.md](knowledge/build-optimizations.md).

**CI failure log is huge** → Use `gh run view <RUN_ID> --log-failed` for just the failures.

**About to rebase** → Check `git log --oneline HEAD..origin/Working | wc -l` first.

### Understanding the codebase (Observations)

**Networking/graphics don't compile** → Likely disabled by CMake toggles. See [codebase-observations.md](knowledge/codebase-observations.md).

**Using legacy globals like `g_graphics`** → Deprecated; use `EngineContext`. See [codebase-observations.md](knowledge/codebase-observations.md).

### Functional audit — what works vs scaffolding

**Rendering pipeline**: 17 working, 12 header-only stubs (~15K dead lines). See [rendering-pipeline-status.md](knowledge/rendering-pipeline-status.md).

**Engine systems**: 17 working, 7 orphaned (~90K+ lines). See [gameplay-systems-status.md](knowledge/gameplay-systems-status.md).

**Editor**: 24 working panels, all resolved. See [editor-functionality-status.md](knowledge/editor-functionality-status.md).

**SparkGame**: 75% functional FPS. See [sparkgame-module-status.md](knowledge/sparkgame-module-status.md).

**SDK/API**: ECS not exposed, unique_ptr DLL export risk. See [sdk-api-surface-audit.md](knowledge/sdk-api-surface-audit.md).

**Documentation**: 53 wiki pages, 99.6% Doxygen coverage. See [documentation-coverage-audit.md](knowledge/documentation-coverage-audit.md).

**Third-party deps**: 6 uninitialized submodules, curl dead code. See [thirdparty-dependencies-audit.md](knowledge/thirdparty-dependencies-audit.md).

## Resolved Issues (cleaned up 2026-03-16)

_These issues were fully resolved and their knowledge files removed. Key learnings are preserved inline above._

- **Security vulnerabilities** (9/9 fixed) — DLL injection, command injection, path traversal, deserialization all patched
- **Thread safety** — 11 atomic conversions, EventBus m_nextId atomic, recursion guard, lock ordering documented
- **ConsoleProcessManager** — Wired into all 5 startup paths
- **Editor panel bloat** — 4 panels restored, 9 deleted, all duplicate pairs resolved
- **Feature restoration** — 5 wrongly deleted features restored
- **GitHub API/PR checks**, **CI failures**, **rebase conflicts**, **clang-format**, **CMake Linux builds**, **MSVC /W4 warnings** — all resolved, key tips kept in quick reference above

---

_To add a new entry: create a file in `knowledge/`, add a row to the table above, add a quick-reference line if relevant, then commit both files. See `.claude/README.md` for entry format._
