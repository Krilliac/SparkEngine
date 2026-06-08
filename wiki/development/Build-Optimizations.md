# Build and CI Workflow Optimizations

> **Audience:** Programmers
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All platforms (build/CI/git; commands shown for Linux/bash)

## Overview

Concrete time and effort savers for build, CI diagnosis, and git workflows. These are faster or more reliable alternatives to the obvious first approach. They apply to any session involving building, testing, CI diagnosis, or git operations in SparkEngine. These are not correctness fixes — the default approaches work — they are efficiency improvements.

## Always Use `--parallel $(nproc)` for CMake Builds

Always pass `--parallel $(nproc)` to `cmake --build` to exploit all available CPU cores:

```bash
cmake --build build --config Release --parallel $(nproc)
```

Without this flag, CMake builds single-threaded by default on some configurations, which is dramatically slower on multi-core hosts.

**Notes:**

- `$(nproc)` is Linux/bash-specific. On macOS use `$(sysctl -n hw.logicalcpu)`. On Windows PowerShell, the MSVC generator parallelizes by default with `--parallel` (no count needed), or use `$env:NUMBER_OF_PROCESSORS`.
- All Linux CI jobs already use `--parallel $(nproc)` (macOS uses `$(sysctl -n hw.logicalcpu)`) — this makes local builds match CI speed.

## Use `--log-failed` to Jump Straight to CI Failure Output

When a CI run fails, skip reading the full log. Use `--log-failed` to download only the failed job output:

```bash
# Get run ID first
gh run list --branch "$(git branch --show-current)" --limit 3

# Download only failed logs (much smaller, faster to scan)
gh run view <RUN_ID> --log-failed
```

**Notes:**

- Full logs (`gh run view <RUN_ID> --log`) can be very large — often 10-50 MB for all jobs.
- `--log-failed` downloads only the logs from jobs with a `failure` conclusion.
- This repo also uploads per-job error-summary artifacts (`ci-errors-*`) and a `report-ci-errors` aggregation job — downloading those can be even faster than `--log-failed`.
- See [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md) for the full PR check diagnosis workflow.

## Check Branch Delta Before Rebasing

Before running `git rebase origin/Working`, check how many commits you are behind. This tells you whether to expect conflicts and how many:

```bash
git log --oneline HEAD..origin/Working | wc -l
```

| Output | Action |
|--------|--------|
| `0` | Branch is up to date — skip rebase entirely |
| `1-3` | Straightforward rebase, unlikely to conflict |
| `4-10` | Moderate delta — read commit messages before rebasing |
| `10+` | Large delta — review commits first with `git log --oneline HEAD..origin/Working` |

**Notes:**

- See [Git Rebase Conflicts](Git-Rebase-Conflicts.md) for conflict resolution strategies.
- Checking first prevents surprises mid-rebase on large divergences.

## `gh pr checks` Snapshot Over `--watch` Loop

For a quick status snapshot without risk of hanging, use plain `gh pr checks` (no flags) rather than `--watch`:

```bash
# Quick snapshot — does not hang
gh pr checks

# Then use run list + view for details on failures
gh run list --branch "$(git branch --show-current)" --limit 3
gh run view <RUN_ID>
```

`gh pr checks --watch` is designed for interactive terminals and can hang indefinitely in non-TTY environments.

**Notes:**

- See [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md) for the full polling workflow.
- Ignore exit code 1 from `gh pr checks` if checks are still running — the non-zero exit is not meaningful while checks are in progress.

## CMake Dry-Run to Verify Preset Flags

Use the `-N` flag to do a CMake dry-run that prints all resolved options without actually configuring:

```bash
cmake --preset linux-gcc-release -N
```

Useful for verifying which CMake toggles (`ENABLE_NETWORKING`, `ENABLE_DXR`, etc.) are set by a preset before committing to a full configure.

**Notes:**

- Much faster than a full configure when you just need to check a flag.
- Many toggles are OFF by default (e.g. `ENABLE_VULKAN`, `ENABLE_OPENGL`, `ENABLE_METAL`, `ENABLE_DXR`, `SPARK_DOUBLE_PRECISION_PHYSICS`); `ENABLE_NETWORKING`, `ENABLE_EDITOR`, `ENABLE_GRAPHICS`, and `BUILD_GAME_MODULES` are ON. See `CLAUDE.md` "Build" for the authoritative toggle list.

## Compiler Caching in CI (ccache / sccache)

CI uses compiler caches to speed up incremental builds: ccache on the Linux jobs and sccache on the Windows MSVC jobs (passed via `-DCMAKE_C_COMPILER_LAUNCHER` / `-DCMAKE_CXX_COMPILER_LAUNCHER`). If you reproduce a CI job locally and have ccache/sccache installed, adding the same launcher flags makes repeat builds far faster; otherwise omit them.

## Source & Freshness

- Original entry: `Build and CI Workflow Optimizations`, last updated 2026-03-14.
- Verified against codebase 2026-06-08.
- Updated / found stale:
  - Added a Windows/PowerShell note for `--parallel` (the `$(nproc)` form is bash-only).
  - Added the `ci-errors-*` artifacts / `report-ci-errors` job as a faster-than-`--log-failed` option (new since source).
  - Added a ccache/sccache section documenting the compiler-cache launcher flags CI now uses.
  - Refreshed the default-OFF/ON toggle list against current `CLAUDE.md` (source pointed at a `codebase-observations.md` file not migrated here).
  - Retargeted cross-references to the migrated wiki pages.

## Related Pages

- [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md) — full PR check diagnosis workflow
- [Git Rebase Conflicts](Git-Rebase-Conflicts.md) — conflict resolution strategies
- [CI Reproducible Builds](CI-Reproducible-Builds.md) — per-job local reproduction commands
- [Workflow Patterns](Workflow-Patterns.md) — pre-push and session-start checklists
- [Project conventions (CLAUDE.md)](../../CLAUDE.md) — "Build" toggle list
