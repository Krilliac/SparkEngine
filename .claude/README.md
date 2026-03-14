# Persistence Context Database

This directory is Claude's persistent memory for the SparkEngine project. It is **not** game-engine code — it is a structured knowledge store that Claude reads and writes across sessions to avoid re-discovering solutions to recurring issues.

## Why This Exists

Claude starts each session without memory of prior sessions. Without a persistence mechanism, the same issues get diagnosed repeatedly: the same failed GitHub API commands are tried, the same rebase pitfalls are hit, the same clang-format gotchas are rediscovered. This directory eliminates that waste.

## How Claude Should Use This

### At Session Start (after git sync)

Read `.claude/index.md` to load the current knowledge index. Scan the table — if any topic matches what you're about to work on, read that knowledge file before proceeding.

### When Solving a Recurring Issue

If you encounter a problem that took multiple attempts to solve (or one that is likely to recur), write or update the relevant knowledge file in `knowledge/`. Then update the `index.md` table. Commit these context files alongside your code changes.

### When to Write a New Entry

Write a new entry when:
- You tried more than one method before finding a working approach
- A command or workflow behaved unexpectedly
- You discovered a repo-specific quirk not documented in CLAUDE.md
- You found a reliable workaround for a flaky tool or CI behavior

Do **not** duplicate information already clearly documented in CLAUDE.md — only add entries for things that required discovery.

## Entry Format

All knowledge files use this standard structure:

```markdown
# [Topic Name]

**Last updated:** YYYY-MM-DD
**Status:** Resolved | Workaround | Ongoing

## Issue

[One paragraph: what the problem is]

## Context

[When/where this occurs — which commands, which CI jobs, which workflows]

## Methods Tried

1. **[Command or approach]** → FAILED
   Reason: [why it failed]

2. **[Command or approach]** → PARTIALLY WORKED
   Limitation: [what it couldn't do]

3. **[Command or approach]** → WORKED

## Solution (Reliable Method)

[Prose explanation of the working approach]

```bash
# Exact commands that work
```

## Notes

- [Caveats, edge cases, conditions where this might not apply]
- [Related issues or links to CLAUDE.md sections]
```

## Rules

1. **Claude owns these files** — they are written by Claude sessions, not humans. Humans may correct factual errors, but should not add entries manually (Claude won't know the full context of what was tried).
2. **Keep entries factual** — document what actually happened, not hypotheticals.
3. **Commit changes** — context files are tracked in git so future sessions on any branch benefit from prior discoveries.
4. **Do not exclude from `.promptignore`** — this directory must remain visible to Claude's context window.
5. **Update the index** — whenever you add or update a knowledge file, update `index.md` accordingly.

## Directory Structure

```
.claude/
├── README.md           # This file — system overview
├── index.md            # Master index — READ THIS AT SESSION START
└── knowledge/
    ├── github-api-pr-checks.md        # GitHub API / PR check status access
    ├── ci-failures.md                 # CI build failure patterns
    ├── git-rebase-conflicts.md        # Rebase conflict resolution
    ├── clang-format.md                # clang-format issues and fixes
    ├── cmake-linux-build-failures.md  # Linux CMake configure/build failures
    └── windows-msvc-w4-warnings.md    # MSVC /W4 warning-as-error patterns
```
