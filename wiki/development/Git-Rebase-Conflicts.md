# Git Rebase Conflict Resolution

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All platforms (git + bash doc scripts)

## Overview

Rebase-time conflicts in this repo cluster around a handful of predictable file types. This page documents the default resolution for each. The default upstream branch is `Working` (not `main`).

## Rule 1 — AUTO: sections in wiki pages

Wiki pages contain auto-generated blocks delimited by markers:

```
<!-- AUTO:stats -->
...live content produced by docs/sync-wiki.sh...
<!-- /AUTO:stats -->
```

When two branches edit these sections independently, git sees a conflict. **Always take the upstream side** — the local change will be rewritten by the next `docs/sync-wiki.sh sync` run, so there is nothing to preserve.

```bash
# From the conflict state
git checkout --theirs wiki/subsystems/Entity-Component-System.md
git add wiki/subsystems/Entity-Component-System.md
git rebase --continue

# Then regenerate to get the correct counts for HEAD
docs/sync-wiki.sh sync
git add -u && git commit --amend --no-edit
```

## Rule 2 — Stat counters in README / CLAUDE.md / `.claude/index.md`

These files have hardcoded counts (test cases, panels, components). Like wiki AUTO: sections, they are written by scripts:

- `docs/update-readme-badges.sh` → README.md
- `docs/update-context.sh` → CLAUDE.md, `.claude/index.md`

Resolution is identical: take upstream, then regenerate with `docs/update-all-docs.sh`.

## Rule 3 — Source code conflicts

Always resolve manually. Do not use `--theirs` / `--ours` blindly on `.h`, `.cpp`, or `CMakeLists.txt` files — a wrong pick silently drops functionality.

## Rule 4 — `docs/api/` (generated API pages)

This tree is gitignored, so you should never see a conflict here. If you do, something upstream committed `docs/api/` by mistake — report it and delete the tracked files.

## Pre-rebase safety check

Before starting, confirm how much you are about to pull in:

```bash
git fetch origin Working
git log --oneline HEAD..origin/Working | wc -l
```

See [Build Optimizations](Build-Optimizations.md) for the branch-delta thresholds that tell you whether to expect conflicts.

## Post-rebase cleanup

```bash
# Re-run doc generation so every AUTO: section matches the rebased HEAD
docs/update-all-docs.sh

# If anything changed, amend the rebase tip rather than adding a new commit
git diff --quiet || git commit -am "docs: regen after rebase"
```

## Source & Freshness

- Original entry: `Git Rebase Conflict Resolution`, last updated 2026-04-17.
- Verified against codebase 2026-06-08.
- Updated / found stale:
  - Confirmed `docs/sync-wiki.sh`, `docs/update-all-docs.sh`, `docs/update-readme-badges.sh`, and `docs/update-context.sh` all still exist with the documented roles — no command changes needed.
  - Added a leading note that the default upstream branch is `Working`.
  - Retargeted cross-references to the migrated wiki pages (was `build-optimizations.md` / `workflow-patterns.md`).

## Related Pages

- [Build Optimizations](Build-Optimizations.md) — pre-rebase branch-delta check
- [Workflow Patterns](Workflow-Patterns.md) — the full rebase → verify → push flow
- [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md)
- [Project conventions (CLAUDE.md)](../../CLAUDE.md) — "Git Sync Workflow" section
