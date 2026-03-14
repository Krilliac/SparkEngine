# AI-Assisted Development — Bloat Pattern and Countermeasures

**Last updated:** 2026-03-14
**Type:** Observation
**Status:** Active

## Description

AI-assisted development has a structural, unavoidable tendency toward bloat. This is not a bug in any single session — it is a systemic property. Understanding why it happens is required to prevent it.

## Context

Discovered through full audit of SparkEngine codebase in March 2026. The engine had been AI-assisted for multiple months. The cumulative result: `SparkConsole.cpp` grew to 261KB, `ConsoleProcessManager` was built but never wired in, `SimpleConsole` accumulated 25+ command registration functions, and `SimpleConsole::Initialize()` was called 5 times in different code paths.

## Details

### Why AI Creates Bloat

**1. No pain from complexity**
A human developer feels the cost of a 261KB file when they spend 3 hours debugging it. AI never feels that. Each session starts fresh; the accumulated mess is invisible until it's catastrophic.

**2. Addition feels productive; removal does not**
Every new feature, method, or class looks like forward progress. Deleting code looks like going backwards. AI has no natural counter-pressure.

**3. "What if we need this later?" thinking**
AI defaults to comprehensive solutions. A simple logging function becomes a full logging system with severity enums, color coding, history persistence, tab completion, and watch variables — because "it might be needed."

**4. Systems built but not integrated**
AI builds things. AI may not always wire them in. ConsoleProcessManager: fully implemented pipes, process launching, command queuing — never initialized. Pure overhead with zero functionality.

**5. Parallel duplication**
Two systems doing overlapping things get built independently and neither gets removed. SimpleConsole and ConsoleProcessManager both exist with overlapping command registration. Neither is cleaned up because "we might need both."

**6. Each session sees a small change**
No single session adds an outrageous amount. 50 lines here, a new method there. After 20 sessions: 261KB files and systems nobody can understand.

### Confirmed Bloat Instances Found in SparkEngine

| Location | Bloat Type | Estimated Waste |
|----------|-----------|-----------------|
| `SparkConsole.cpp` | Embedded console UI never used (cursor, ANSI, Win32 handles) | ~150 lines |
| `SparkConsole.h` | 25+ `Register*()` methods, could be 3 | ~20 method declarations |
| `ConsoleProcessManager` | Fully implemented, zero call sites for `Initialize()` | Entire file is dead |
| `SparkEngine.cpp` | `SimpleConsole::Initialize()` called 5x | 4 redundant call sites |
| `SimpleConsole::WatchEntry` system | Built, no callers outside the class | Entire subsystem |
| `SimpleConsole` tab completion state | 3 members, never used externally | Dead state |

### The Compounding Effect

Bloat compounds. A bloated file is harder to read, so the next session adds another helper function instead of understanding the existing code. That makes it more bloated. After N sessions, the file is incomprehensible and nobody touches it — it just accumulates more wrappers.

## Solution / Summary

### Per-Session Rules (enforced in CLAUDE.md)

1. **Hard line limits**: `.cpp` max 400 lines, `.h` max 200 lines
2. **Removal mandate**: every PR that adds code must also remove code
3. **Wire-in requirement**: if a system has `Initialize()`, it must be called, or delete the system
4. **No orphaned code**: dead code is deleted immediately, not commented out
5. **Bloat check at session start**: `find ... | xargs wc -l | sort -rn | head -15`

### When You Notice Bloat Mid-Task

Do not defer. If the file you're editing is over the limit:
1. Trim it first — delete dead code, consolidate duplicate methods, remove unused members
2. Then make your actual change
3. The PR should show a net negative line count or small positive

### What "Minimal" Means Here

The SparkConsole fix is the canonical example:
- **Wrong**: Add 20 lines wrapping `Initialize()` in a helper class with retry logic and callbacks
- **Right**: Add 2 lines — `Initialize()` in startup, `ProcessCommands()` in main loop

If the fix is more than 10 lines, ask: "What am I adding that I don't need?"

## Notes

- This problem is not fixable with AI alone — human review focused on *removal* is the real safeguard
- The CLAUDE.md Anti-Bloat Rules section is the living enforcement mechanism; update it when new patterns are found
- "Looks good, simplify it" is a valid and important review comment — it should be used often
