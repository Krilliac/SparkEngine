# GitHub API — PR Checks Diagnosis

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All platforms (`gh` CLI + git)

## Overview

The full workflow for polling PR checks, diagnosing failures, and rerunning jobs via the `gh` CLI. Paired with [Build Optimizations](Build-Optimizations.md) (which explains *why* the steps below are ordered as they are) and [Workflow Patterns](Workflow-Patterns.md) (which shows where this fits in the post-push flow).

## 1 — Poll

Use `gh pr checks --watch` **only** in an interactive terminal. In any scripted or non-TTY context, use the snapshot form:

```bash
gh pr checks                                # one-shot snapshot
gh pr checks --json name,state,conclusion   # machine-readable
```

Exit code 1 from `gh pr checks` while checks are still running is **not** a failure signal — ignore it and re-poll. See the `--watch` caveat in [Build Optimizations](Build-Optimizations.md).

## 2 — Identify the failing job

```bash
gh run list --branch "$(git branch --show-current)" --limit 5
```

Columns: `STATUS · CONCLUSION · WORKFLOW · BRANCH · RUN_ID · STARTED`. Grab the RUN_ID of the failed run.

## 3 — Download only the failed job logs

```bash
gh run view <RUN_ID> --log-failed
```

`--log-failed` returns only the jobs whose conclusion is `failure`. Full logs (`--log`) can run 10-50 MB across the full matrix and are almost always unnecessary.

## 4 — Narrow to the first error line

The relevant failure is almost always the first `error:` or `FAILED` line in each job:

```bash
gh run view <RUN_ID> --log-failed 2>&1 | grep -nE "error:|FAILED|undefined" | head -20
```

If the root cause is buried (e.g. CMake reports a link error but the real issue was a missing symbol upstream), also scan for the first `ninja: build stopped` and read ~40 lines above.

Note: this repo also has a `report-ci-errors` job that aggregates per-job error summaries and uploads them as artifacts (each build job uploads a `ci-errors-*` / `sanitizer-report-*` artifact). When present, downloading that artifact can be faster than scraping raw logs.

## 5 — Reproduce locally

Pull the matching configuration from `.github/workflows/build.yml`. The standard Linux jobs use manual flags, but for the common Debug/Release cases the presets match closely enough:

```bash
cmake --preset linux-gcc-release && cmake --build build --parallel $(nproc)
cmake --preset linux-gcc-debug   && cmake --build build --parallel $(nproc)
```

See [CI Reproducible Builds](CI-Reproducible-Builds.md) for the full job ↔ command table (including the sanitizer jobs, which use manual `cmake -B build` flag invocations, not presets).

## 6 — Fix, push, re-poll

```bash
git add -u && git commit --amend --no-edit
git push --force-with-lease
sleep 15
gh pr checks --fail-fast
```

`--force-with-lease` refuses the push if the remote moved since your last fetch — safer than plain `--force`.

## Notes

- Always fetch before polling if the PR is shared — someone else may have already pushed a fix.
- If the failing job is marked `continue-on-error: true` in `build.yml`, the PR can still merge. As of this writing those are `build-windows-vs2026`, `build-linux-mingw-wine`, `build-macos`, and `clang-tidy`. Don't block on those unless the user explicitly asks.
- `gh pr view --json statusCheckRollup` returns the aggregate check state in one call — useful when you only care about red/green.

## Source & Freshness

- Original entry: `GitHub API — PR Checks Diagnosis`, last updated 2026-04-17.
- Verified against codebase 2026-06-08.
- Updated / found stale:
  - Added the `report-ci-errors` job and the `ci-errors-*` / `sanitizer-report-*` artifacts as a faster alternative to scraping raw logs (new since the source was written).
  - Verified the `continue-on-error` job list against `.github/workflows/build.yml`: `build-windows-vs2026`, `build-linux-mingw-wine`, `build-macos`, `clang-tidy` (matches `CLAUDE.md`).
  - Clarified that the sanitizer jobs are reproduced via manual flags, not presets, pointing to the CI Reproducible Builds page.
  - Retargeted cross-references to the migrated wiki pages.

## Related Pages

- [Build Optimizations](Build-Optimizations.md) — `--log-failed`, `--watch` caveats, `--parallel $(nproc)` for rebuilds
- [CI Reproducible Builds](CI-Reproducible-Builds.md) — job ↔ command table
- [Workflow Patterns](Workflow-Patterns.md) — post-push verification flow
- [Project conventions (CLAUDE.md)](../../CLAUDE.md) — "Post-PR checks" section
