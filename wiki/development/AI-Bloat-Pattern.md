# AI-Assisted Development — Bloat Pattern and Countermeasures

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All (process/architecture discipline, not platform-specific)

## Overview

AI-assisted development has a structural, recurring tendency toward bloat. This is not a defect of any single session — it is a systemic property of how AI assistants make changes. Understanding *why* it happens is required to prevent it. This page is the rationale behind the Anti-Bloat Guidelines in `CLAUDE.md`.

The pattern was discovered through a full audit of the SparkEngine codebase in March 2026, after the engine had been AI-assisted for several months. The original audit found, among other things, a `SparkConsole.cpp` that had grown to roughly 261 KB, a `ConsoleProcessManager` that was fully built but never wired in, 25+ command-registration functions in `SimpleConsole`, and `SimpleConsole::Initialize()` being called from five different code paths.

Most of those specific instances have since been fixed (see Source & Freshness). The *pattern* remains the durable lesson.

## Why AI Creates Bloat

**1. No pain from complexity.** A human developer feels the cost of a 261 KB file when they spend three hours debugging it. An AI never does. Each session starts fresh; the accumulated mess is invisible until it is catastrophic.

**2. Addition feels productive; removal does not.** Every new feature, method, or class looks like forward progress. Deleting code looks like going backwards. There is no natural counter-pressure.

**3. "What if we need this later?" thinking.** AI defaults to comprehensive solutions. A simple logging function becomes a full logging system with severity enums, color coding, history persistence, tab completion, and watch variables — "just in case."

**4. Systems built but not integrated.** AI builds things; it does not always wire them in. The original `ConsoleProcessManager` had fully implemented pipes, process launching, and command queuing — but `Initialize()` was never called. Pure overhead, zero functionality.

**5. Parallel duplication.** Two systems doing overlapping work get built independently and neither is removed, because "we might need both."

**6. Each session sees only a small change.** No single session adds an outrageous amount — 50 lines here, a new method there. After 20 sessions you have 261 KB files and systems nobody can explain.

## The Compounding Effect

Bloat compounds. A bloated file is harder to read, so the next session adds another helper instead of understanding the existing code, which makes it more bloated. After N sessions the file is incomprehensible and nobody touches it — it just accumulates more wrappers.

## Confirmed Bloat Instances (March 2026 audit, now largely resolved)

| Location | Bloat Type | Status as of 2026-06-08 |
|----------|-----------|--------------------------|
| `SparkConsole.cpp` | Embedded console UI never used (cursor, ANSI, Win32 handles) | **Resolved** — file is now 641 lines |
| `SparkConsole.h` | 25+ `Register*()` methods | **Resolved** — header now 193 lines |
| `ConsoleProcessManager` | Fully implemented, zero call sites for `Initialize()` | **Resolved** — now wired in via `SparkEngine.cpp` / `SparkEngineWindows.cpp` / `SparkEngineLinux.cpp` |
| `SparkEngine.cpp` | `SimpleConsole::Initialize()` called 5x | **Resolved** — `SparkEngine.cpp` is now 443 lines |
| `SimpleConsole::WatchEntry` system | Built, no external callers | Reviewed during trim |
| `SimpleConsole` tab-completion state | Dead members | Reviewed during trim |

The takeaway: every one of these was eventually fixed by a human-reviewed pass focused on removal. That pass is the real safeguard.

## Countermeasures (enforced via CLAUDE.md)

### Sensible thresholds — guidelines, not hard limits

These are signals to pause and think, not absolute caps. A clean 450-line `.cpp` is fine; a cryptic 200-line one is not.

| Thing | Threshold | Action |
|-------|-----------|--------|
| `.cpp` file size | ~500 lines | Split if doing multiple jobs; leave if one coherent unit |
| `.h` file size | ~300 lines | Split if unrelated types; data-heavy headers are fine |
| Public methods per class | ~15 | Ask whether each earns its place |
| Function length | ~60 lines | Split if nested branching; clear linear flow is fine |
| Command registration functions | 1 per subsystem | Consolidate before adding |
| Parallel singletons doing the same thing | 0 | Remove the duplicate |

> Earlier revisions of this guidance cited hard limits of 400 lines per `.cpp` and 200 per `.h`. The current `CLAUDE.md` uses the softer ~500/~300 thresholds above, with readability as the overriding test. Tooling: `tools/check-bloat.sh` (part of `tools/validate-all.sh`).

### The readability principle

Never sacrifice readability to hit a line count. Keep "why" comments, descriptive names (`brushRadius` > `br`), vertical whitespace between logical sections, braces on non-trivial loop bodies, one statement per line. The question is always: *"Does this make sense to someone reading it for the first time?"*

### Per-session rules

1. **Wire-in requirement:** if a system has `Initialize()`, it must be called in the startup path — or delete the system. (`tools/check-wiring.sh` enforces this.)
2. **No orphaned code:** dead code is deleted immediately, not commented out (git history exists).
3. **Removal mindset:** a PR that adds code should usually also remove code; aim for a net-negative or small-positive line count.
4. **Bloat check at session start:** `find ... -name '*.cpp' | xargs wc -l | sort -rn | head -15`.

### When you notice bloat mid-task

Do not defer. If the file you are editing is over the threshold:

1. Trim it first — delete dead code, consolidate duplicate methods, remove unused members.
2. Then make your actual change.

### What "minimal" means

The `ConsoleProcessManager` wire-in is the canonical example:

- **Wrong:** add 20 lines wrapping `Initialize()` in a helper class with retry logic and callbacks.
- **Right:** add 2 lines — `Initialize()` in startup, `ProcessCommands()` in the main loop.

If the fix is more than ~10 lines, ask: *"What am I adding that I don't need?"*

## Notes

- This problem is not fixable with AI alone — **human review focused on removal** is the real safeguard.
- The CLAUDE.md Anti-Bloat Guidelines section is the living enforcement mechanism; update it when new patterns are found.
- *"Looks good, simplify it"* is a valid and important review comment — use it often.

## Source & Freshness

- **Original entry date:** 2026-03-14 (`.claude/knowledge/ai-bloat-pattern.md`, type: Observation)
- **Verified against codebase 2026-06-08.**
- **UPDATED:** Nearly all cited bloat instances are now resolved — `SparkConsole.cpp` is 641 lines (was ~261 KB), `SparkConsole.h` is 193 lines, and `ConsoleProcessManager` is wired in (referenced from `SparkEngine.cpp`, `SparkEngineWindows.cpp`, `SparkEngineLinux.cpp`, with per-platform implementations). Marked the table accordingly.
- **UPDATED:** Replaced the obsolete hard limits (400/.cpp, 200/.h) with the current `CLAUDE.md` ~500/~300 readability-first thresholds and the full guideline table; noted `tools/check-bloat.sh` and `tools/check-wiring.sh` as the enforcing scripts.
- **VERIFIED:** The "why AI creates bloat" analysis and the per-session discipline remain accurate and unchanged in intent.

## Related Pages

- [Code-Quality-Violations.md](Code-Quality-Violations.md) — concrete audit of function/method-size violations and their resolution
- [Clang-Format.md](Clang-Format.md) — formatting half of pre-commit hygiene
- [Live-Editor-Testing.md](Live-Editor-Testing.md) — wiring-and-verification discipline applied to runtime testing
