# Thread Safety Issues — Race Conditions and Data Races

**Last updated:** 2026-03-16
**Type:** Issue
**Status:** Resolved

## Description

30+ subsystems used unprotected `bool m_initialized` pattern. EventBus had a data race on subscription IDs and a recursive publish risk. NetworkManager had undocumented lock ordering.

---

## Critical: Systemic `bool m_initialized` Race — RESOLVED

**30+ subsystems** declared `bool m_initialized = false;` without atomic protection.

**FIXED — 11 highest-risk subsystems converted to `std::atomic<bool>`:**
- `Engine/Scripting/ScriptHotReload.h` — `m_running`
- `Engine/Networking/NetworkManager.h` — `m_initialized`
- `Engine/Networking/NetworkIntegration.h` — `m_initialized`
- `Engine/Streaming/AsyncAssetLoader.h` — `m_initialized`
- `Utils/SparkConsole.h` — `m_initialized`
- `Utils/FileLogger.h` — `m_initialized`
- `Engine/AI/AIIntegration.h` — `m_initialized`
- `Engine/AI/ParallelPerception.h` — `m_initialized`
- `Engine/Mobile/MobilePlatform.h` — `m_initialized`
- `Engine/VR/VRSystem.h` — `m_initialized`

**Remaining raw `bool` (lowest risk, mostly single-threaded Graphics stubs):**
- 20+ Graphics headers (ConstantBufferRing, LightManager, SkyAtmosphere, etc.)
- These are single-threaded rendering code; atomic is unnecessary.

---

## Critical: EventBus::m_nextId Data Race — FIXED

**File:** `Utils/EventBus.h`

Changed `uint64_t m_nextId = 0` to `std::atomic<uint64_t> m_nextId{0}`. Added `<atomic>` include.

---

## High: EventBus Recursive Publish — FIXED

**File:** `Utils/EventBus.h`

**Fix applied:** Added per-event-type `thread_local int s_publishDepth` guard. If a handler publishes the same event type recursively, the nested publish is silently ignored to prevent infinite recursion. The snapshot-based design already prevents mutex deadlock (handlers run outside the lock).

---

## High: Logger Shutdown Race — ANALYZED, NO FIX NEEDED

**File:** `Utils/Logger.cpp:123-150`

**Analysis:** `m_writerThread.join()` is a full synchronization point. After `join()` returns, the writer thread has completely exited (including releasing all locks). The subsequent `m_sinkMutex` acquisition at line 144 is guaranteed to succeed without contention. No actual deadlock is possible.

---

## High: NetworkManager Lock Ordering — DOCUMENTED

**File:** `Engine/Networking/NetworkManager.h:459,477-478`

**Fix applied:** Added documentation comments establishing strict lock ordering:
`m_stateMutex` → `m_queueMutex` → `m_handlerMutex` (never reverse).

---

## Medium: Logger CallbackSink Thread Contract — ACCEPTABLE

**File:** `Utils/Logger.h:298-317`

Callbacks fire from the writer thread when async logging is enabled. This is an intentional design choice. Users must ensure their callbacks are thread-safe.

---

## Medium: CVarRegistry Compound Type Races — ACCEPTABLE

**File:** `Utils/ConsoleVariable.h:137-147`

Individual CVar `SetFromString()` on compound types is not atomic, but CVars are intended to be set from the main thread (console input). This is an acceptable design constraint.

---

## What's Correct

- ThreadSafeQueue — proper mutex + condition variable
- JobSystem — correct atomic flags, proper thread pool shutdown
- GraphicsEngine — `std::atomic<bool> m_frameInProgress` with compare_exchange
- ConsoleProcessManager — `std::atomic<bool> m_shouldStopThread`
- Logger async writer — correct condition_variable pattern
