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
| Effective dev workflows | [knowledge/workflow-patterns.md](knowledge/workflow-patterns.md) | Pattern | Active | 2026-03-14 |
| Codebase non-obvious facts | [knowledge/codebase-observations.md](knowledge/codebase-observations.md) | Observation | Active | 2026-03-14 |
| Build and CI workflow speedups | [knowledge/build-optimizations.md](knowledge/build-optimizations.md) | Optimization | Active | 2026-03-14 |

## Quick Reference by Topic

### Fixing problems (Issues)

**Checking PR / CI status** → Use `gh run list` + `gh run view`, NOT `gh pr checks --watch`. See [github-api-pr-checks.md](knowledge/github-api-pr-checks.md).

**CI check failed** → Identify blocking vs. non-blocking jobs first. See [ci-failures.md](knowledge/ci-failures.md).

**Rebase conflict** → `<!-- AUTO:* -->` and `docs/api/` always take upstream. See [git-rebase-conflicts.md](knowledge/git-rebase-conflicts.md).

**clang-format failure** → Don't use `head -50` shortcut; match CI's Metal exclusion. See [clang-format.md](knowledge/clang-format.md).

**CMake configure/build fails on Linux** → Check submodules, apt packages, cache conflicts. See [cmake-linux-build-failures.md](knowledge/cmake-linux-build-failures.md).

**Windows CI fails but Linux passes** → MSVC `/W4` warnings. Use fix table. See [windows-msvc-w4-warnings.md](knowledge/windows-msvc-w4-warnings.md).

### Doing things well (Patterns & Optimizations)

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
