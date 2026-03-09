# SparkEngine Scripting (AngelScript) — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/Scripting/` (AngelScriptEngine.h — header-only, no .cpp)
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of `AngelScriptEngine.h` and related integration points.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Scripting subsystem wraps AngelScript to enable gameplay logic in script. `AngelScriptEngine.h` declares a well-structured API:
- Script compilation from files and strings
- Per-entity script binding via `AttachScript()` / `DetachScript()`
- Lifecycle callbacks: `Start()`, `Update(float)`, `OnCollision(EntityID)`
- Engine API registration: math types, component types, global functions
- Error handling with `GetLastError()`
- Cached method pointers for fast dispatch

However, **only a header file exists** — there is no `AngelScriptEngine.cpp`. The entire implementation is declared but not compiled.

---

## Critical Gaps

### GAP-S01 — No Implementation File Exists

**Files**: `Engine/Scripting/AngelScriptEngine.h` (only file in the directory)

**Impact**: The header declares 15+ methods and 5 private helper methods, but there is no corresponding `.cpp` file. None of the following are implemented:
- `Initialize()` — engine creation, API registration
- `CompileScriptFile()` / `CompileScriptFromString()` — compilation
- `AttachScript()` / `DetachScript()` — entity binding
- `CallStart()` / `CallUpdate()` / `CallOnCollision()` — callback dispatch
- `RegisterStandardLibrary()` / `RegisterEngineAPI()` / `RegisterMathTypes()` / `RegisterComponentTypes()` / `RegisterGlobalFunctions()` — API registration

**What is needed**: Create `AngelScriptEngine.cpp` implementing all declared methods. The AngelScript library itself is included via `#include <angelscript.h>` with add-on headers, suggesting the dependency exists.

---

### GAP-S02 — Only 5 Functions Exposed to Scripts

**File**: `Engine/Scripting/AngelScriptEngine.h` (lines 39-44, 277-305)

**Impact**: The engine API callable from scripts consists of only:
- `print(string)` — debug output
- `createEntity(string)` — create entity
- `getTransform(EntityID)` — get transform
- `getKeyDown(string)` — key press check
- `getKey(string)` — key held check

This is an extremely minimal API. Scripts cannot:
- Access physics (apply forces, raycast, set velocity)
- Play sounds
- Access AI components
- Access health/damage
- Spawn/destroy entities with components
- Access time, delta time, or frame count
- Do math operations (Vector3 add, normalize, dot, cross)
- Access the scene (find entities by name/tag)
- Access the console

**What is needed**: Register a comprehensive engine API:
- Math: Vector3, Quaternion, Matrix operations
- Entity: FindByName, FindByTag, Destroy, HasComponent, GetComponent
- Physics: ApplyForce, SetVelocity, Raycast, SetGravity
- Audio: PlaySound, PlaySound3D, StopSound, SetVolume
- Input: GetMousePosition, GetMouseDelta, GetAxis
- Time: GetDeltaTime, GetTime, GetFrameCount
- Scene: LoadScene, GetEntityCount
- Debug: DrawLine, DrawSphere, LogMessage

---

## Major Gaps

### GAP-S03 — No Hot-Reload Support

**File**: `Engine/Scripting/AngelScriptEngine.h`

**Impact**: When a script file changes on disk, the engine must be restarted to pick up the changes. For rapid iteration on gameplay logic, hot-reload is essential.

**What is needed**: Implement file watching on script directories. When a `.as` file changes:
1. Recompile the module
2. For each entity bound to that module, save script state
3. Destroy old script instances
4. Create new instances from recompiled module
5. Restore saved state

---

### GAP-S04 — No Script Debugging Support

**File**: `Engine/Scripting/AngelScriptEngine.h`

**Impact**: No breakpoint support, no variable inspection, no call stack display. Debugging script errors requires reading the error string from `GetLastError()`.

**What is needed**: At minimum, implement:
- Script exception handler with full stack trace
- Console command to list all active script instances
- Console command to inspect script variables on a given entity
- AngelScript line callback for performance profiling

---

### GAP-S05 — No Coroutine/Async Support in Scripts

**File**: `Engine/Scripting/AngelScriptEngine.h`

**Impact**: Only synchronous `Update(float)` is supported. Scripts cannot yield (e.g., "wait 2 seconds then do X", "wait until enemy is visible"). This makes writing sequential gameplay logic (cutscenes, tutorials, quest steps) extremely cumbersome.

**What is needed**: Implement AngelScript coroutine support:
- `yield()` — suspend until next frame
- `wait(float seconds)` — suspend for a duration
- `waitUntil(condition)` — suspend until condition is true

Use AngelScript's `asIScriptContext::Suspend()` and `Resume()` for this.

---

### GAP-S06 — Singleton Pattern, Not EngineContext

**File**: `Engine/Scripting/AngelScriptEngine.h` (line 188)

**Evidence**:
```cpp
static AngelScriptEngine* s_instance;
static AngelScriptEngine* GetInstance() { return s_instance; }
```

**Impact**: Uses raw singleton pointer instead of `EngineContext` service locator.

**What is needed**: Register with `EngineContext` for consistency and testability.

---

## Moderate Gaps

### GAP-S07 — No Script-to-Script Communication

**File**: `Engine/Scripting/AngelScriptEngine.h`

**Impact**: Scripts on different entities cannot communicate. There is no event system, message passing, or shared blackboard accessible from scripts.

**What is needed**: Expose an event/message system to scripts:
- `sendMessage(EntityID target, string event, any data)`
- `onMessage(string event, callback)` — register handler

---

### GAP-S08 — One Context Per Entity (Not Pooled)

**File**: `Engine/Scripting/AngelScriptEngine.h` (ScriptInstance struct)

**Impact**: Each entity with a script gets its own `asIScriptContext`. AngelScript contexts are relatively expensive. For scenes with many scripted entities (e.g., 100 enemies), this creates 100 contexts.

**What is needed**: Use a context pool. Before calling a script method, acquire a context from the pool. After the call, return it. This reduces memory overhead significantly.

---

### GAP-S09 — Only Three Lifecycle Callbacks

**File**: `Engine/Scripting/AngelScriptEngine.h`

**Impact**: Only `Start()`, `Update(float)`, and `OnCollision(EntityID)` are cached. Common gameplay callbacks are missing:
- `OnDestroy()` — cleanup on entity destruction
- `OnEnable()` / `OnDisable()` — activation changes
- `OnTriggerEnter(EntityID)` / `OnTriggerExit(EntityID)` — trigger zones
- `LateUpdate(float)` — post-physics update
- `FixedUpdate(float)` — fixed timestep for physics

**What is needed**: Cache and dispatch additional lifecycle callbacks, at minimum `OnDestroy` and trigger events.

---

### GAP-S10 — No Script Sandboxing

**File**: `Engine/Scripting/AngelScriptEngine.h`

**Impact**: Scripts have unrestricted access to whatever engine API is registered. No memory limits, no execution time limits, no restricted API surface for modding.

**What is needed**: For modding support, implement:
- Execution timeout (kill context after N milliseconds)
- Memory allocation limits per script
- Restricted API surface (no file I/O, no network access from scripts)

---

## Minor Gaps

### GAP-S11 — No Script Documentation Generator

**Impact**: No tool to auto-generate documentation of the script API from the registered functions.

---

### GAP-S12 — Thread Safety Warning Not Enforced

**File**: `Engine/Scripting/AngelScriptEngine.h` (line 71)

**Impact**: The header warns "Script contexts are not thread-safe" but there is no runtime assertion or guard. If called from a background thread, silent corruption would occur.

**What is needed**: Add `SPARK_ASSERT(IsMainThread())` in all public methods.

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-S01 | Critical | No .cpp implementation | Entire system non-functional |
| GAP-S02 | Critical | Only 5 script API functions | Unusable for gameplay |
| GAP-S03 | Major | No hot-reload | Slow iteration |
| GAP-S04 | Major | No debugging support | Hard to diagnose errors |
| GAP-S05 | Major | No coroutines | No async script logic |
| GAP-S06 | Major | Singleton pattern | Architecture inconsistency |
| GAP-S07 | Moderate | No script-to-script comm | Isolated scripts |
| GAP-S08 | Moderate | No context pooling | High memory at scale |
| GAP-S09 | Moderate | Only 3 lifecycle callbacks | Missing common events |
| GAP-S10 | Moderate | No script sandboxing | No modding safety |
| GAP-S11 | Minor | No API documentation tool | Developer experience |
| GAP-S12 | Minor | Thread safety not enforced | Silent corruption risk |

---

## Recommended Priority Order

1. **GAP-S01** — Create implementation file (unblocks scripting entirely)
2. **GAP-S02** — Comprehensive engine API registration (makes scripting useful)
3. **GAP-S05** — Coroutine support (gameplay authoring quality of life)
4. **GAP-S09** — Additional lifecycle callbacks (gameplay integration)
5. **GAP-S03** — Hot-reload (iteration speed)
6. **GAP-S08** — Context pooling (scalability)
7. **GAP-S04** — Debugging support
8. Everything else
