# Thread Safety Issues — Race Conditions and Data Races

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Active
**Severity:** High

## Description

30+ subsystems use unprotected `bool m_initialized` pattern. EventBus has a data race on subscription IDs. NetworkManager has undocumented lock ordering. Logger shutdown has a race window.

---

## Critical: Systemic `bool m_initialized` Race

**30+ subsystems** declare `bool m_initialized = false;` without atomic protection. If `Initialize()` is called from multiple threads (or checked from a different thread than where it's set), this is a data race.

**Affected files include:**
- `Engine/Scripting/ScriptHotReload.h:177` — `bool m_running`
- `Engine/Streaming/AsyncAssetLoader.h:351` — `bool m_initialized`
- `Graphics/ConstantBufferRing.h:345`
- `Engine/Networking/NetworkIntegration.h:229`
- `Engine/Streaming/SeamlessAreaManager.h:292`
- `Graphics/TextureSystem.h` — `bool m_loaded`, `bool m_streaming`
- `Utils/FileLogger.h:402`
- And 20+ more subsystems

**Fix:** Convert all to `std::atomic<bool>`. This is a mechanical fix.

---

## Critical: EventBus::m_nextId Data Race

**File:** `Utils/EventBus.h:178,314`

```cpp
uint64_t m_nextId = 0;  // Line 314 — NOT atomic
uint64_t id = ++m_nextId;  // Line 178 — race on increment
```

`m_nextId` is global to the bus but only protected by per-channel mutexes. Two threads subscribing to different event types race on the shared counter.

**Fix:** Change to `std::atomic<uint64_t> m_nextId{0};`

---

## High: EventBus Recursive Publish Deadlock

**File:** `Utils/EventBus.h`

`Publish()` takes a snapshot under `ch.mutex` lock, then invokes handlers outside the lock. But if a handler calls `Publish<SameType>()`, it tries to acquire `ch.mutex` again — deadlock on non-recursive mutex.

Documentation warns against this but doesn't enforce it.

**Fix:** Use `std::recursive_mutex` for channel locks, or add runtime recursion guard.

---

## High: Logger Shutdown Race

**File:** `Utils/Logger.cpp:123-150`

Shutdown sequence:
1. Sets `m_stopThread = true`
2. Notifies condition variable
3. Joins writer thread
4. Acquires `m_sinkMutex` to flush sinks

If the writer thread is dispatching to sinks (holding `m_sinkMutex`) while the main thread calls `Shutdown()`, potential deadlock when main thread tries to acquire `m_sinkMutex` after join.

**Fix:** Ensure writer thread processes all pending messages and releases locks before join returns.

---

## High: NetworkManager Lock Ordering

**File:** `Engine/Networking/NetworkManager.h:459,477-478`

Three mutexes with no documented ordering:
- `m_stateMutex` — protects role, connection state, client ID
- `m_queueMutex` — protects message queues
- `m_handlerMutex` — protects message handlers

If different code paths acquire these in different orders, deadlock is possible.

**Fix:** Document and enforce strict ordering: `m_stateMutex` → `m_queueMutex` → `m_handlerMutex`.

---

## Medium: Logger CallbackSink Thread Contract

**File:** `Utils/Logger.h:298-317`

`CallbackSink::Write()` invokes user callback without specifying which thread it runs on. If async logging is enabled, callbacks fire from the writer thread. No documentation warns users.

**Fix:** Document that callbacks must be thread-safe, or add per-sink mutex.

---

## Medium: CVarRegistry Compound Type Races

**File:** `Utils/ConsoleVariable.h:137-147`

Registry uses mutex for registration, but individual CVar `SetFromString()` on compound types (std::string, structs) is not atomic.

---

## What's Correct

- ThreadSafeQueue — proper mutex + condition variable
- JobSystem — correct atomic flags, proper thread pool shutdown
- GraphicsEngine — `std::atomic<bool> m_frameInProgress` with compare_exchange
- ConsoleProcessManager — `std::atomic<bool> m_shouldStopThread`
- Logger async writer — correct condition_variable pattern
