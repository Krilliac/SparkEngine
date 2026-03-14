# Persistence Context Database

This directory is Claude's persistent memory for the SparkEngine project. It is **not** game-engine code — it is a structured knowledge store that Claude reads and writes across sessions to accumulate learning: solutions to problems, effective workflows, codebase observations, and project-specific decisions.

## Why This Exists

Claude starts each session without memory of prior sessions. Without a persistence mechanism, the same issues get diagnosed repeatedly, effective workflows are rediscovered from scratch, and non-obvious codebase facts have to be re-inferred. This directory is the remedy.

## How Claude Should Use This

### At Session Start (mandatory)

After syncing with the upstream `Working` branch (per the "Session start" section in `CLAUDE.md`), Claude must:

1. Read `.claude/index.md` to load the table of contents.
2. Identify any entries relevant to the current task or domain.
3. Read those knowledge files before proceeding.

This prevents re-discovering solutions, re-experiencing dead ends, and re-learning project conventions.

### When to Write a New Entry

Write a new entry (or update an existing one) whenever Claude learns something worth preserving across sessions:

| Write when... | Entry type |
|---------------|-----------|
| A problem required multiple attempts to solve | **Issue** |
| A workflow or approach proved consistently effective | **Pattern** |
| A faster/better way to do something was discovered | **Optimization** |
| A non-obvious fact about the codebase was discovered | **Observation** |
| An architectural or stylistic decision was made for this project | **Decision** |

**Do not** write entries for trivial single-step tasks or things already clearly stated in CLAUDE.md.

### When to Update an Existing Entry

- A new method was tried and the status changed
- A workaround was superseded by a proper fix
- A codebase observation became outdated (e.g., a deprecated API was removed)
- Additional edge cases or caveats were discovered

## Entry Format

All entries use this structure. Sections marked _(Issue only)_ are omitted for non-Issue types; _(non-Issue)_ sections replace them.

```markdown
# [Topic Name]

**Last updated:** YYYY-MM-DD
**Type:** Issue | Pattern | Optimization | Observation | Decision
**Status:** Resolved | Active | Ongoing | Superseded

## Description
[What this entry is about. One paragraph.]

## Context
[When/where this applies — which tools, workflows, CI jobs, codebase areas]

## Methods Tried  ← Issue only
1. **[Approach]** → FAILED
   Reason: [why]
2. **[Approach]** → WORKED

## Approach  ← Pattern / Optimization only
[What to do. Actionable steps or commands.]

## Details  ← Observation / Decision only
[The fact, finding, or decision and its rationale.]

## Solution / Summary
[For Issues: exact working commands. For others: key takeaway in one paragraph.]

## Notes
- [Caveats, edge cases, related entries, links to CLAUDE.md sections]
```

**Status values:**
- `Resolved` — Issue fixed; no longer a problem
- `Active` — Pattern/Optimization/Observation currently in use
- `Ongoing` — Issue or situation that recurs and is being managed
- `Superseded` — Entry replaced by a better approach (keep for history)

## Rules

1. **Claude owns these files** — written by Claude sessions; humans may correct factual errors.
2. **Keep entries factual** — document what actually happened, not hypotheticals.
3. **Commit changes** — context files are tracked in git so future sessions on any branch benefit.
4. **Do not exclude from `.promptignore`** — this directory must remain visible to Claude.
5. **Update the index** — whenever you add or update a knowledge file, update `index.md`.
6. **Prefer updating over creating** — if an existing entry covers the topic, extend it.

## Entry Types Reference

| Type | Purpose | Mandatory sections |
|------|---------|-------------------|
| **Issue** | Problem + fix; records failed attempts | Description, Context, Methods Tried, Solution, Notes |
| **Pattern** | Repeatable workflow that works well | Description, Context, Approach, Notes |
| **Optimization** | Faster/better way to do something | Description, Context, Approach, Notes |
| **Observation** | Non-obvious codebase/tooling fact | Description, Context, Details, Notes |
| **Decision** | Project-specific architectural or style choice | Description, Context, Details, Notes |

## Directory Structure

```
.claude/
├── README.md                              # This file — system overview
├── index.md                               # Master index — READ THIS AT SESSION START
└── knowledge/
    ├── github-api-pr-checks.md            # [Issue] PR check status access methods
    ├── ci-failures.md                     # [Issue] CI job blocking rules, reproduction
    ├── git-rebase-conflicts.md            # [Issue] Rebase conflict resolution
    ├── clang-format.md                    # [Issue] clang-format issues and fixes
    ├── cmake-linux-build-failures.md      # [Issue] Linux CMake configure/build failures
    ├── windows-msvc-w4-warnings.md        # [Issue] MSVC /W4 warning-as-error patterns
    ├── workflow-patterns.md               # [Pattern] Effective SparkEngine dev workflows
    └── codebase-observations.md           # [Observation] Non-obvious SparkEngine facts
```
