# Extended Bloat Audit — Additional Findings

**Last updated:** 2026-03-14
**Type:** Observation
**Status:** Active
**Supplements:** codebase-bloat-audit-2026-03-14.md

---

## Dead-Code Utility Headers (0 usages, delete entirely)

Three header-only utility systems have **zero call sites** anywhere in the codebase (no `.cpp` includes them, no macros are invoked):

| File | Lines | Singletons / Macros | Usage count |
|------|-------|---------------------|-------------|
| `SparkEngine/Source/Utils/ChromeTracing.h` | 211 | `ChromeTracing::GetInstance()` | **0** |
| `SparkEngine/Source/Utils/MemoryDebugger.h` | 433 | `SPARK_TRACK_ALLOC`, `SPARK_TRACK_FREE`, `SPARK_PRINT_LEAK_REPORT` | **0** |
| `SparkEngine/Source/Utils/FrameInspector.h` | 408 | `FrameInspector::GetInstance()` | **0** |

**Combined dead code: 1,052 lines.**

**Action:** Delete all three files. They implement functionality that:
- ChromeTracing: Never enabled/called; Profiler.h likely supersedes it.
- MemoryDebugger: All tracking macros uncalled; valgrind/ASan does this better.
- FrameInspector: Never instantiated; GPU timestamp logic is redundant with Profiler.

TweenManager (`Tween.h`, 455 lines) has 7 references but all are within the header itself (the singleton accessor). Check for actual game-side callers before deleting.

---

## Duplicate AudioMixer in Same Namespace — ODR Risk

**Two classes named `AudioMixer` in `namespace Spark::Audio`:**

| File | Size | Pattern | GetInstance |
|------|------|---------|-------------|
| `Audio/AudioMixer.h` | 331 lines | Instance-based; Initialize()/Update(listenerPos, dt) | No |
| `Audio/MusicManager.h` line 57 | Part of 292 lines | Singleton; SetBusVolume/FadeBus/Update | Yes |

**Problem:** Both live in `namespace Spark::Audio`. If a translation unit includes both headers, this is an ODR violation. Current safety: only `AudioMixer.cpp` includes `AudioMixer.h`, only `MusicManager.cpp` includes `MusicManager.h`. But this is fragile — adding a feature that needs both will break.

**Action:** Rename MusicManager's internal mixer to `BusMixer` or `AudioBusMixer` and update references in `MusicManager.cpp`.

---

## Two Parallel Visual Scripting Systems

| System | File | Lines | Purpose |
|--------|------|-------|---------|
| `VisualScriptSystem` | `Engine/Scripting/VisualScriptSystem.{h,cpp}` | 2,406 | Blueprint-to-AngelScript compiler, runtime execution |
| `VisualScriptingSystem` | `SparkEditor/VisualScripting/VisualScriptingSystem.{h,cpp}` | 4,937 | Blueprint-style node editor (UI panel) |

**Combined: 7,343 lines.** These are *partially* different (one is engine-runtime, one is editor-UI) but they duplicate:
- Node type definitions (`ScriptVariableType` vs `ScriptPinType`)
- Graph data structures (both have nodes, pins, edges)
- Serialization logic
- Compilation/validation pipeline

**Status:** Unclear if they communicate. Neither includes the other. Neither is initialized from the SparkEngine startup (VisualScriptSystem::Initialize() is a singleton but it's not called in SparkEngine.cpp).

**Action:**
1. Verify VisualScriptSystem::Initialize() is wired in — if not, it's orphaned.
2. Consolidate graph/node types into a shared header used by both.
3. Long-term: editor system should depend on engine system, not implement a parallel graph runtime.

---

## SparkEngine.cpp — Duplicate Startup Paths (copy-paste bloat)

`SparkEngine.cpp` is 2,115 lines with **two entry points**:
- `wWinMain` (line 228): Windows GUI/game startup
- `main` (line 1626): Linux/headless/console startup

Each path duplicates the **full system initialization sequence** instead of sharing a common init function:

| System | Creation count in wWinMain | Notes |
|--------|---------------------------|-------|
| `PhysicsSystem` | 3× (`make_unique`) | Lines 282, 397 (normal), 1649 (play mode) |
| `GraphicsEngine` | 2× (`make_unique`) | Lines 609, 1768 |
| `SimpleConsole::Initialize()` | 3× | Lines 294, 635, 1661 |

`main()` adds 2 more `PhysicsSystem` and `GraphicsEngine` instantiations (lines 1783, 2035, 2043).

**Root cause:** Each startup variant (headless, normal, windowed, play mode, restart) copy-pastes rather than calling a shared `InitSystems()` helper.

**Action:** Extract one `InitCoreSystems()` function, one `ShutdownSystems()` function, and call them from all entry paths. Estimated reduction: ~600 lines from SparkEngine.cpp.

---

## PlatformInputManager — Alive but Separate from InputManager

`PlatformInput.h` defines `PlatformInputManager` (a backend abstraction over `Win32InputBackend` and `SDL2InputBackend`). `PlatformInputManager` is used in 51 `.cpp` files. `InputManager` (separate class) is used throughout the engine context.

**Relationship unclear:** It's not confirmed whether InputManager wraps PlatformInputManager, or whether they operate independently. `PlatformInput.h` is 897 lines — 4.5× over the header limit — suggesting over-engineering.

**Action:** Audit whether InputManager calls PlatformInputManager internally. If not, one is dead.

---

## Shader.cpp — Duplicate Include

`SparkEngine/Source/Graphics/Shader.cpp` includes `SparkConsole.h` **twice**:
- Line 7: `#include "../Utils/SparkConsole.h"`
- Line 1298: `#include "../Utils/SparkConsole.h"` (duplicate, ~1,291 lines later)

This is harmless thanks to `#pragma once` but indicates the file was copy-pasted or spliced together.

**Action:** Remove the duplicate include at line 1298.

---

## SparkEngine.cpp — g_graphics Global Still in Active Use

The CLAUDE.md architecture says to use `EngineContext` service locator, not `g_graphics`/`g_input` globals. Confirmed: `SparkEngine.cpp` still defines and uses `g_graphics` (`std::unique_ptr<GraphicsEngine>`) as a file-scope global, not through EngineContext. This global is referenced in 56 command lambdas and console hooks via `GetGfx()` helper.

**Status:** `g_graphics` is accessed via a `GetGfx()` inline that returns the raw pointer — fine for internal use. But the raw global prevents proper lifecycle management via EngineContext. Lower priority than the above issues.

---

## Stubs That Are Legitimate (Don't Delete)

| File | Reason |
|------|--------|
| `PhysicsSystemStub.cpp` | Compiled when `SPARK_BULLET_PHYSICS_AVAILABLE` is not set. Valid no-op fallback. |
| `CrashHandlerStub.cpp` | Compiled when miniz not available. Valid no-op fallback. |
| `OpenALAudioEngine.*` | Cross-platform fallback when XAudio2 unavailable (Linux/macOS). Active, not dead. |

---

## Confirmed Priority Order (Combined with First Audit)

### P0 — Delete immediately (no risk)
1. `ChromeTracing.h` — 211 lines, 0 usages
2. `MemoryDebugger.h` — 433 lines, 0 usages
3. `FrameInspector.h` — 408 lines, 0 usages
4. Duplicate `#include` in `Shader.cpp` line 1298

### P1 — Fix before touching related code
5. Wire `ConsoleProcessManager` into startup + main loop (15 min)
6. Rename `AudioMixer` in `MusicManager.h` → `AudioBusMixer` (ODR risk)
7. Verify `VisualScriptSystem::Initialize()` is wired in; delete if not
8. Consolidate duplicate startup paths in `SparkEngine.cpp`

### P2 — Structural refactors
9. SparkConsole.cpp: Delete embedded UI, collapse 52 Register* methods
10. Extract `PhysicsSystem.h` inline defs to `.inl` (1,909 → ~200 lines)
11. PlatformInputManager/InputManager relationship audit

---

## Total Removable Lines (Running Estimate)

| Category | Lines |
|---------|-------|
| Dead utility headers (P0) | 1,052 |
| SparkConsole embedded UI | ~4,200 |
| SparkEngine.cpp duplicate startup | ~600 |
| PhysicsSystem.h over-inline | ~1,500 |
| RenderGraph.h over-inline | ~1,300 |
| Duplicate `#include` | 1 |
| **Running total** | **~8,650 lines** |

---

## Metrics Snapshot (2026-03-14)

```
Total source lines (all .h + .cpp): 269,770
Files violating .cpp 400-line limit: 22
Files violating .h 200-line limit: 10+
Confirmed dead headers: 3 (1,052 lines)
ODR-risky duplicate class names: 1 (AudioMixer)
Parallel system implementations: 2 (VisualScripting)
Unwired singletons confirmed: 1 (ConsoleProcessManager)
Unwired singletons suspected: 1 (VisualScriptSystem::Initialize)
Duplicate system creation in startup: 3 systems × 3 paths = 9 duplicate inits
```
