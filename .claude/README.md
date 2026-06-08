# `.claude/` — Project AI Configuration

> **The knowledge base that used to live here has been retired.** Project
> knowledge now lives in the **wiki** (DuetOS-style), which is the single source
> of truth for humans and AI sessions alike.

## What changed

Previously this directory held a parallel `index.md` + `knowledge/*.md` store of
audits, engine analyses, patterns, and decisions. That duplicated the project
wiki and drifted out of date. On 2026-06-08 every entry was migrated into the
wiki, freshened against the current codebase, and the `.claude/knowledge/` store
was removed.

## Where knowledge lives now

Read the wiki at session start (see `CLAUDE.md` → "Session start"). The migrated
content lives under these sidebar sections (`wiki/_Sidebar.md`):

| Old `.claude/knowledge` content | New wiki location |
|---------------------------------|-------------------|
| Dev workflows, build/CI, git, clang-format, bloat pattern, cross-compilation, live-editor testing | `wiki/development/` (**Development & Process**) |
| Engine analyses, library evaluations, viability/feature/project recommendations, mac compat, DuetOS catalog, external research | `wiki/research/` (**Research & Analysis**) |
| Codebase audits/observations, system/module status, stub catalog, memory integrity/safety, GPU/physics/daemon/reflection notes, Wine tiers | `wiki/advanced/` (**Engineering Notes & Audits**) |

## What this directory is for now

`.claude/` is reserved for Claude Code **behavioral configuration** only — e.g.
`agents/`, `skills/`, `hooks/`, and `settings*.json` if/when added (the model
used by the sibling DuetOS project). It is **not** a knowledge store.

## How to record new knowledge

When a session learns something worth preserving across sessions (an issue's
root cause, an effective pattern, a non-obvious codebase fact, a decision):

1. Add or update the relevant **wiki page** (use `wiki/_Template.md` for new
   pages: Audience / Thread Context / Platform-Backend Scope header + canonical
   sections, ending with a `## Source & Freshness` note).
2. Add the page to `wiki/_Sidebar.md` under the right section.
3. Run `docs/sync-wiki.sh sync` (and `docs/update-all-docs.sh` for code changes)
   and commit alongside the change.

Per-developer, cross-session scratch memory (not project knowledge) still lives
in the host-level Claude memory directory and `.remember/`, which are separate
from this in-repo configuration.
