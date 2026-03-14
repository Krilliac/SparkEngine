# Build and CI Workflow Optimizations

**Last updated:** 2026-03-14
**Type:** Optimization
**Status:** Active

## Description

Concrete time and effort savers for build, CI diagnosis, and git workflows. These are faster or more reliable alternatives to the obvious first approach.

## Context

Applies to any session involving building, testing, CI diagnosis, or git operations in SparkEngine. These are not correctness fixes — the default approaches work — they are efficiency improvements.

---

## Optimization: Always Use `--parallel $(nproc)` for CMake Builds

### Approach

Always pass `--parallel $(nproc)` to `cmake --build` to exploit all available CPU cores:

```bash
cmake --build build --config Release --parallel $(nproc)
```

Without this flag, CMake builds single-threaded by default on some configurations, which is dramatically slower on multi-core hosts.

### Notes

- `$(nproc)` is Linux/bash-specific. On macOS use `$(sysctl -n hw.logicalcpu)`.
- All CI jobs already use `--parallel $(nproc)` — this makes local builds match CI speed.

---

## Optimization: Use `--log-failed` to Jump Straight to CI Failure Output

### Approach

When a CI run fails, skip reading the full log. Use `--log-failed` to download only the failed job output:

```bash
# Get run ID first
gh run list --branch "$(git branch --show-current)" --limit 3

# Download only failed logs (much smaller, faster to scan)
gh run view <RUN_ID> --log-failed
```

### Notes

- Full logs (`gh run view <RUN_ID> --log`) can be very large — often 10–50 MB for all jobs.
- `--log-failed` downloads only the logs from jobs with a `failure` conclusion.
- **See also:** [github-api-pr-checks.md](github-api-pr-checks.md) for the full PR check diagnosis workflow.

---

## Optimization: Check Branch Delta Before Rebasing

### Approach

Before running `git rebase origin/Working`, check how many commits you're behind. This tells you whether to expect conflicts and how many:

```bash
git log --oneline HEAD..origin/Working | wc -l
```

| Output | Action |
|--------|--------|
| `0` | Branch is up to date — skip rebase entirely |
| `1–3` | Straightforward rebase, unlikely to conflict |
| `4–10` | Moderate delta — read commit messages before rebasing |
| `10+` | Large delta — review commits first with `git log --oneline HEAD..origin/Working` |

### Notes

- **See also:** [git-rebase-conflicts.md](git-rebase-conflicts.md) for conflict resolution strategies.
- Checking first prevents surprises mid-rebase on large divergences.

---

## Optimization: `gh pr checks` Snapshot Over `--watch` Loop

### Approach

For a quick status snapshot without risk of hanging, use plain `gh pr checks` (no flags) rather than `--watch`:

```bash
# Quick snapshot — does not hang
gh pr checks

# Then use run list + view for details on failures
gh run list --branch "$(git branch --show-current)" --limit 3
gh run view <RUN_ID>
```

`gh pr checks --watch` is designed for interactive terminals and can hang indefinitely in non-TTY environments.

### Notes

- **See also:** [github-api-pr-checks.md](github-api-pr-checks.md) for the full polling workflow.
- Ignore exit code 1 from `gh pr checks` if checks are still running — the non-zero exit is not meaningful while checks are in progress.

---

## Optimization: CMake Dry-Run to Verify Preset Flags

### Approach

Use the `-N` flag to do a CMake dry-run that prints all resolved options without actually configuring:

```bash
cmake --preset linux-gcc-release -N
```

Useful for verifying which CMake toggles (`ENABLE_NETWORKING`, `ENABLE_DXR`, etc.) are set by a preset before committing to a full configure.

### Notes

- Much faster than a full configure when you just need to check a flag.
- **See also:** [codebase-observations.md](codebase-observations.md) for the list of systems that are OFF by default.
