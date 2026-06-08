# Code Quality Violations — Functions, Methods, and Conventions

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All (codebase-wide audit)

## Overview

This page records a March–April 2026 audit of `CLAUDE.md` quality-guideline violations *beyond* file/class size: oversized functions, classes with too many private helpers, and naming conventions. It is a historical snapshot with a verified-current status column — most of the originally-cited violations have since been resolved, and the codebase has been substantially reorganized (several cited files have moved or been split).

The original audit was marked **"Mostly Resolved."** As of 2026-06-08 verification, that remains accurate and most specifics have moved further toward resolution.

## Progress Timeline (from original entry)

- **2026-03-31:** Down from 66 oversized functions to 9 remaining (86% reduction). `RegisterDefaultCommands` split into 3 focused functions. `InspectorComponentRenderers.cpp` split into 4 domain files. `MaterialSystem` "duplicates" confirmed as wrapper+impl, not true duplicates.
- **2026-03-31 (optimization pass):** `SaveSystem::RegisterBuiltins()` (425 lines) split into 3 domain functions; `SaveSystem::SerializeWorld()` (124 lines) refactored with a `TrySerialize<T>` template; `ConsoleApp` Run/ReadEngineInput/ReadUserInput split into focused helpers.
- **2026-04-04:** All 9 remaining oversized functions reviewed and annotated with `// NOTE: Intentionally exceeds 50-line guideline` comments explaining why each is left as-is (linear initialization, rendering pipeline, event wiring, UI layout).

## 1. Functions Over the Length Guideline (originally 66)

The original audit used a 50-line trigger. The current `CLAUDE.md` guideline is ~60 lines, and explicitly treats clear linear flow as acceptable. The remaining intentionally-long functions carry `// NOTE: Intentionally exceeds ...` annotations — verified present (10 occurrences across files including `GraphicsEngineWindows.cpp`, `SparkEngineWindows.cpp`, `EditorUI.cpp`, `EditorNotificationManager.cpp`).

### Original top offenders — current status

Many cited files have been **split or relocated**, so the original line counts no longer apply:

| Function (as cited) | Original file | Original lines | Current reality (2026-06-08) |
|---------------------|---------------|----------------|------------------------------|
| `RegisterEngineConsoleCommands()` | SparkEngine.cpp | 555 | `SparkEngine.cpp` is now 443 lines total — split out |
| `main()` / `wWinMain()` | SparkEngine.cpp | 488 / 330 | Platform entry points split into `SparkEngineWindows.cpp` / `SparkEngineLinux.cpp` |
| `RenderMainMenuBar()` / `CreatePanels()` | EditorUI.cpp | 514 / 412 | `EditorUI.cpp` still large (1,516 lines) — see live offenders below |
| `RegisterHealthCommands()` etc. | SparkConsole.cpp | 453 + | `SparkConsole.cpp` trimmed to 641 lines total |
| `LoadFromFile()` / `Console_ValidateMaterials()` | MaterialSystem.cpp | 407 / 247 | `MaterialSystem.cpp` is now 486 lines total |
| `CreateCombatArena()` / `Initialize()` | Game.cpp | 379 / 364 | `Game.cpp` relocated to `GameModules/SparkGameFPS/Source/Game/Game.cpp` |
| Various | GraphicsEngine.cpp | — | `GraphicsEngine.cpp` is now a 13-line shim; platform code lives in `GraphicsEngineWindows.cpp` |

The headline number ("66 oversized functions, 9 remaining, all annotated") was accurate at audit time. The codebase has since been refactored heavily enough that a fresh audit would need new line counts.

## 2. Classes With Many Private Helper Methods

The original audit flagged 7 classes against a "10 max private helpers" guideline. The current `CLAUDE.md` guideline is ~15 *public* methods per class, framed as a "does each method earn its place?" question rather than a hard cap. Several of the worst offenders have been cut down:

| Class | File | Original count | Current (2026-06-08) |
|-------|------|----------------|----------------------|
| PostProcessingPipeline | `Graphics/PostProcessingPipeline.h` (434 lines) | 84 | ~13 private methods — **drastically reduced** |
| PhysicsSystem | `Physics/PhysicsSystem.h` (685 lines, relocated from `Engine/Physics/`) | 42 | Still data-heavy; not re-counted |
| RenderGraph | `Graphics/RenderGraph.h` (1,125 lines, relocated from `Graphics/RenderGraph/`) | 26 | Still large |
| SimpleConsole | `Utils/SparkConsole.h` (193 lines) | 42 | **Reduced** — header trimmed to 193 lines |
| PlatformInputManager, WaterSystem, BehaviorTree | various | 53 / 23 / 16+ | Not re-counted |

Note the relocations: `PhysicsSystem.h` moved from `Engine/Physics/` to `Source/Physics/`, and `RenderGraph.h` moved from `Graphics/RenderGraph/` to `Source/Graphics/`.

## 3. Naming Convention Violations

`CLAUDE.md`: *PascalCase classes/methods, camelCase locals, `m_` prefix members, UPPER_SNAKE macros.*

### Struct members without `m_` prefix

Systematic across data-transfer (POD) structs. The `m_` prefix is consistently used for **class private members** but generally omitted for **public struct fields** — a recognized gray area, since POD structs commonly omit `m_`. Affected headers historically included `Input/InputBindings.h`, `Input/GamepadInput.h`, `Input/PlatformInput.h`, `Input/InputManager.h`, `Engine/Scripting/ScriptHotReload.h`, `Engine/Scripting/VisualScriptSystem.h`, `Engine/Procedural/SplatmapSystem.h`, and `Utils/SparkConsole.h`.

**Open question (unchanged):** whether `m_` applies to POD/data structs or only to private class members. Treated as low severity.

### Methods and macros

- Public methods are consistently PascalCase — no violations found.
- Macros are consistently UPPER_SNAKE; platform stubs like `_In_` are acceptable.

## 4. Commented-Out Code

`CLAUDE.md`: *commented-out code → delete it.* The original audit found only 2 minor instances (`SeamlessAreaManager.cpp` planned integration; `SparkConsole.cpp` incomplete state assignments). **Both confirmed resolved** — re-checking `SeamlessAreaManager.cpp` on 2026-06-08 finds no leftover commented integration block, and `SparkConsole.cpp` is fully trimmed.

CI also gates TODO/FIXME/HACK/XXX comment count at a threshold of 20 via the `todo-count` job.

## Summary

| Violation type | Original count | Status 2026-06-08 |
|----------------|----------------|-------------------|
| Functions over length guideline | 66 (9 remaining, annotated) | Annotations present; many cited files split/relocated |
| Duplicate functions in same file | 0 (all resolved) | Resolved |
| Classes with many private helpers | 7 | Reduced (PostProcessingPipeline 84→~13, SparkConsole trimmed) |
| Struct `m_` prefix | ~80+ fields | Open gray area, low severity |
| Commented-out code | 0 | Resolved |

## Source & Freshness

- **Original entry date:** 2026-04-02 (`.claude/knowledge/code-quality-violations.md`, type: Observation, status: Mostly Resolved)
- **Verified against codebase 2026-06-08.**
- **UPDATED — many cited files moved or shrank:** `SparkEngine.cpp` 443 lines (entry implied ~1,660 in oversized functions); `GraphicsEngine.cpp` is now a 13-line shim with platform code in `GraphicsEngineWindows.cpp`; `MaterialSystem.cpp` 486 lines; `SparkConsole.cpp` 641 / `SparkConsole.h` 193. `Game.cpp` relocated to `GameModules/SparkGameFPS/Source/Game/`; `PhysicsSystem.h` to `Source/Physics/`; `RenderGraph.h` to `Source/Graphics/`.
- **UPDATED — private-method counts:** `PostProcessingPipeline.h` dropped from 84 to ~13 private methods (now 434 lines). SimpleConsole/SparkConsole header trimmed.
- **VERIFIED — STALE claims re-checked:** commented-out code in `SeamlessAreaManager.cpp` is gone (resolved, as the entry claimed). The `// NOTE: Intentionally exceeds ...` annotations still exist (10 occurrences). `EditorUI.cpp` remains genuinely large at 1,516 lines.
- **UNVERIFIABLE / not re-counted:** PhysicsSystem, RenderGraph, PlatformInputManager, WaterSystem, BehaviorTree private-method counts and the per-file oversized-function totals were not re-tallied; treat the original numbers as historical.

## Related Pages

- [AI-Bloat-Pattern.md](AI-Bloat-Pattern.md) — why these violations accumulate and how the thresholds work
- [Clang-Format.md](Clang-Format.md) — the formatting layer of code-quality enforcement
