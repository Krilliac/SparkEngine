# Thread Safety Issues — Race Conditions and Data Races

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Active
**Severity:** High

## Description

30+ subsystems use unprotected `bool m_initialized` pattern. EventBus has a data race on subscription IDs. NetworkManager has undocumented lock ordering. Logger shutdown has a race window.

---

## Critical: Systemic `bool m_initialized` Race — PARTIALLY FIXED

**30+ subsystems** declare `bool m_initialized = false;` without atomic protection.

**FIXED (2026-03-16) — 7 highest-risk subsystems converted to `std::atomic<bool>`:**
- `Engine/Scripting/ScriptHotReload.h` — `m_running`
- `Engine/Networking/NetworkManager.h` — `m_initialized`
- `Engine/Networking/NetworkIntegration.h` — `m_initialized`
- `Engine/Streaming/AsyncAssetLoader.h` — `m_initialized`
- `Utils/SparkConsole.h` — `m_initialized`
- `Utils/FileLogger.h` — `m_initialized`

**Still raw `bool` (lower risk, mostly single-threaded Graphics stubs):**
- 20+ Graphics headers (ConstantBufferRing, LightManager, SkyAtmosphere, etc.)
- `Engine/Streaming/SeamlessAreaManager.h`
- `Engine/Mobile/MobilePlatform.h`
- `Engine/AI/ParallelPerception.h`, `AIIntegration.h`
- `Engine/VR/VRSystem.h`

---

## Critical: EventBus::m_nextId Data Race — FIXED

**File:** `Utils/EventBus.h`

**FIXED (2026-03-16):** Changed `uint64_t m_nextId = 0` to `std::atomic<uint64_t> m_nextId{0}`. Added `<atomic>` include.

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
