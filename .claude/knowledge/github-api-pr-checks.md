# GitHub API — PR Checks Diagnosis

**Last updated:** 2026-04-17
**Type:** Pattern
**Status:** Active

## Description

The full workflow for polling PR checks, diagnosing failures, and rerunning jobs via `gh`. Paired with [build-optimizations.md](build-optimizations.md) (which explains *why* the steps below are ordered as they are) and [workflow-patterns.md](workflow-patterns.md) (which shows where this fits in the post-push flow).

---

## 1 — Poll

Always open with `gh pr checks --watch` **only** in an interactive terminal. In any scripted or MCP context, use the snapshot form:

```bash
gh pr checks                                # one-shot snapshot
gh pr checks --json name,state,conclusion   # machine-readable
```

Exit code 1 from `gh pr checks` while checks are still running is **not** a failure signal — ignore it and re-poll. See the `--watch` caveat in [build-optimizations.md](build-optimizations.md).

## 2 — Identify the failing job

```bash
gh run list --branch "$(git branch --show-current)" --limit 5
```

Columns: `STATUS · CONCLUSION · WORKFLOW · BRANCH · RUN_ID · STARTED`. Grab the RUN_ID of the failed run.

## 3 — Download only the failed job logs

```bash
gh run view <RUN_ID> --log-failed
```

`--log-failed` returns only the jobs whose conclusion is `failure`. Full logs (`--log`) can run 10–50 MB across the full matrix and are almost always unnecessary.

## 4 — Narrow to the first error line

The relevant failure is almost always the first `error:` or `FAILED` line in each job:

```bash
gh run view <RUN_ID> --log-failed 2>&1 | grep -nE "error:|FAILED|undefined" | head -20
```

If the root cause is buried (e.g. CMake reports a link error but the real issue was a missing symbol upstream), also scan for the first `ninja: build stopped` and read ~40 lines above.

## 5 — Reproduce locally

Pull the matching preset from `.github/workflows/build.yml`. Common presets:

```bash
cmake --preset linux-gcc-release && cmake --build build --parallel $(nproc)
cmake --preset linux-gcc-debug   && cmake --build build --parallel $(nproc)
```

See `.claude/knowledge/ci-reproducible-builds.md` for the full job ↔ preset table.

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
- If the failing job is marked `continue-on-error: true` in `build.yml` (e.g. `build-macos`, `build-windows-vs2026`, `clang-tidy`), the PR can still merge. Don't block on those unless the user explicitly asks.
- `gh pr view --json statusCheckRollup` returns the aggregate check state in one call — useful when you only care about red/green.

## See also

- [build-optimizations.md](build-optimizations.md) — `--log-failed`, `--watch` caveats, `--parallel $(nproc)` for rebuilds.
- [workflow-patterns.md](workflow-patterns.md) — post-push verification flow.
- Parent project guide: `CLAUDE.md` — "Post-PR checks" section.
