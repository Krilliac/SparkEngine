# Git Rebase Conflict Resolution

**Last updated:** 2026-04-17
**Type:** Pattern
**Status:** Active

## Description

Rebase-time conflicts in this repo cluster around a handful of predictable file types. This entry documents the default resolution for each.

---

## Rule 1 — AUTO: sections in wiki pages

Wiki pages contain auto-generated blocks delimited by markers:

```
<!-- AUTO:stats -->
…live content produced by docs/sync-wiki.sh…
<!-- /AUTO:stats -->
```

When two branches edit these sections independently, git sees a conflict. **Always take the upstream side** — the local change will be rewritten by the next `docs/sync-wiki.sh sync` run, so there's nothing to preserve.

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

Before starting, confirm how much you're about to pull in:

```bash
git fetch origin Working
git log --oneline HEAD..origin/Working | wc -l
```

See [build-optimizations.md](build-optimizations.md) for the branch-delta thresholds that tell you whether to expect conflicts.

## Post-rebase cleanup

```bash
# Re-run doc generation so every AUTO: section matches the rebased HEAD
docs/update-all-docs.sh

# If anything changed, amend the rebase tip rather than adding a new commit
git diff --quiet || git commit -am "docs: regen after rebase"
```

## See also

- [build-optimizations.md](build-optimizations.md) — pre-rebase branch-delta check.
- [workflow-patterns.md](workflow-patterns.md) — the full rebase → verify → push flow.
- Parent project guide: `CLAUDE.md` — "Git Sync Workflow" section.
