# SparkEngine Coroutine System — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/Coroutine/` (CoroutineScheduler, Coroutine, YieldInstruction)
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Coroutine/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Coroutine system provides a lightweight cooperative task scheduler for gameplay code. It supports sequencing actions with yields (`WaitForSeconds`, `WaitForFrames`, `WaitUntil`, `WaitForEndOfFrame`) via a builder-pattern API. The system is implemented entirely in the header (`CoroutineScheduler.h`) with inline method bodies. `CoroutineScheduler` is a singleton that ticks all active coroutines each frame. The implementation is simple and functional for basic use cases, but has significant gaps in error handling, documented-but-missing features, and integration with the rest of the engine.

---

## Major Gaps

### GAP-CO01 — Documented Features Not Implemented

**Files**:
- `Coroutine/CoroutineScheduler.h` (lines 33–37, usage example in doc comment)

**Impact**: The file-level documentation shows usage of `Repeat(5, callback)` and `.WithInterval(10.0f)` in the "wave spawner" example, but neither method exists in the `Coroutine` class API. The builder pattern only has `Do()`, `WaitForSeconds()`, `WaitForFrames()`, `WaitUntil()`, and `YieldFrame()`. Any developer following the documented usage example will get a compile error.

**Evidence**: The `Coroutine` class (lines 159–275) has no `Repeat()` or `WithInterval()` method. The doc comment at line 34 shows `scheduler.StartCoroutine("wave_spawner").Repeat(5, [&](int wave) { SpawnWave(wave); }).WithInterval(10.0f);` — this code does not compile.

**What is needed**: Either implement `Repeat(int count, std::function<void(int)>)` and `WithInterval(float seconds)` methods, or update the documentation to remove the example and accurately describe the current API.

---

### GAP-CO02 — Entity Binding Documented But Not Implemented

**Files**:
- `Coroutine/CoroutineScheduler.h` (line 18, feature list)

**Impact**: The feature list documents "Entity binding — auto-cancel when an entity is destroyed" but the `Coroutine` class has no entity reference, no `BindToEntity()` method, and no mechanism to detect entity destruction. Coroutines referencing destroyed entities will continue running and may access invalid data.

**Evidence**: The `Coroutine` class members (lines 258–274) have `m_name`, `m_steps`, `m_currentStep`, and `m_cancelled` — no entity ID or World reference. The `CoroutineScheduler` has no entity integration.

**What is needed**: Add `Coroutine& BindToEntity(uint32_t entityID)` that stores an entity reference. In `CoroutineScheduler::Update()`, check if bound entities still exist and cancel orphaned coroutines. Requires a `World&` reference or callback mechanism.

---

### GAP-CO03 — No Error Handling in Action Callbacks

**Files**:
- `Coroutine/CoroutineScheduler.h` (lines 238–243, `Update()` method)

**Impact**: If a `Do()` callback throws an exception, the coroutine's `Update()` method propagates the exception to the `CoroutineScheduler::Update()` loop, which will abort iteration over the remaining coroutines. All coroutines after the throwing one are skipped for that frame. The scheduler state is not corrupted (the erase-remove cleanup at line 366 still runs), but the behavior is unpredictable.

**Evidence**: `step.action()` at line 242 is called with no try/catch. The enclosing `while` loop and the `for` loop in `CoroutineScheduler::Update()` have no exception handling.

**What is needed**: Wrap `step.action()` in a try/catch. On exception, log the error, cancel the offending coroutine, and continue processing remaining coroutines. Consider a per-coroutine error callback.

---

### GAP-CO04 — Singleton Pattern Outside EngineContext

**Files**:
- `Coroutine/CoroutineScheduler.h` (lines 290–294, `GetInstance()`)

**Impact**: `CoroutineScheduler` is a global singleton not registered in `EngineContext`. This creates a hidden dependency, makes testing difficult (can't inject a mock), and means game modules must access it directly rather than through the service locator.

**Evidence**: `EngineContext.h` has no `GetCoroutineScheduler()` method. Free functions `StartCoroutine()` and `StopCoroutine()` (lines 385–394) directly reference the global singleton.

**What is needed**: Register `CoroutineScheduler` in `EngineContext`. The free-function convenience wrappers can remain but should route through the context when available.

---

## Moderate Gaps

### GAP-CO05 — Not Using C++20 Coroutines

**Files**:
- `Coroutine/CoroutineScheduler.h` (full file)

**Impact**: The engine targets C++20 but the coroutine system uses a custom callback-based builder pattern rather than C++20 `co_yield`/`co_await` coroutines. While the current approach works, C++20 coroutines would provide more natural syntax, stack-like local variable preservation across yields, and compiler-optimized suspension/resumption.

**Evidence**: No `#include <coroutine>`, no `co_yield`, no `co_await`, no `promise_type`. The system uses `std::function<void()>` callbacks and explicit yield instruction objects.

**What is needed**: This is an enhancement, not a bug. Consider adding a C++20 coroutine wrapper as an alternative API (e.g., `Task<void> MyCoroutine()`) that coexists with the current builder API. The builder API is simpler for basic sequences; C++20 coroutines are better for complex logic with local state.

---

### GAP-CO06 — No Pause/Resume for Individual Coroutines

**Files**:
- `Coroutine/CoroutineScheduler.h` (lines 159–275, `Coroutine` class)

**Impact**: Individual coroutines can only be cancelled (`Cancel()`), not paused and resumed. If the game pauses (menu, cutscene), there is no way to freeze a specific coroutine without cancelling it. `StopAll()` cancels everything permanently.

**Evidence**: `Coroutine` has `Cancel()` and `IsCancelled()` but no `Pause()`/`Resume()`/`IsPaused()` methods.

**What is needed**: Add `Pause()` and `Resume()` methods to `Coroutine`. In `Update()`, skip paused coroutines without removing them. Add `PauseAll()`/`ResumeAll()` to `CoroutineScheduler` for game pause scenarios.

---

### GAP-CO07 — No Coroutine Tags or Group Management

**Files**:
- `Coroutine/CoroutineScheduler.h` (lines 302–306, `StartCoroutine`)

**Impact**: Coroutines are identified only by name. There is no tag or group system to cancel or pause categories of coroutines (e.g., "stop all UI coroutines" or "pause all gameplay coroutines"). `StopCoroutine(name)` requires knowing the exact name.

**What is needed**: Add an optional tag parameter: `StartCoroutine(name, tag)`. Add `StopByTag(tag)` and `PauseByTag(tag)` to `CoroutineScheduler`.

---

## Minor Gaps

### GAP-CO08 — No Priority or Execution Order Guarantees

**Files**:
- `Coroutine/CoroutineScheduler.h` (lines 354–370, `Update()`)

**Impact**: Coroutines are ticked in insertion order (the order they were created). There is no priority system to ensure certain coroutines run before others. For most gameplay use cases this is fine, but ordering-sensitive systems (e.g., a coroutine that must run after physics but before rendering) have no control.

**What is needed**: Optional priority parameter on `StartCoroutine()`. Sort coroutines by priority before ticking.

---

### GAP-CO09 — No Coroutine Nesting or Sub-Coroutines

**Files**:
- `Coroutine/CoroutineScheduler.h` (full file)

**Impact**: A coroutine cannot yield on another coroutine. In Unity, `yield return StartCoroutine(OtherCoroutine())` allows composition. The current system has no equivalent — starting a sub-coroutine from within a `Do()` callback works but the parent does not wait for it to finish.

**What is needed**: Add `WaitForCoroutine(name)` yield instruction that pauses until the named coroutine completes.

---

### GAP-CO10 — Header-Only Implementation May Increase Compile Times

**Files**:
- `Coroutine/CoroutineScheduler.h` (full file — all method bodies inline)

**Impact**: The entire coroutine system is implemented inline in the header. Every translation unit that includes this header gets the full implementation, increasing compile times. For a header this size (~400 lines) the impact is modest, but it prevents link-time deduplication of template instantiations.

**What is needed**: Move non-template method implementations to a `.cpp` file. Keep only the `YieldInstruction` subclasses and short inline methods in the header.

---
