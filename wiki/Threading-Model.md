# Threading Model

This page documents every threading primitive, thread-safety contract, and concurrency pattern in SparkEngine. Understanding these rules is critical for avoiding deadlocks, data races, and main-thread stalls.

**Source:** Threading infrastructure spans `Utils/`, `Engine/Persistence/`, `Engine/Networking/`, `Engine/AI/`, `Engine/ECS/Systems/`, and `Graphics/`.

---

## Overview

SparkEngine uses a **main-thread-primary** architecture. The main thread owns:
- The ECS registry (EnTT)
- Physics simulation (Jolt Physics)
- Graphics rendering (D3D11/D3D12/Vulkan)
- ImGui editor UI
- Coroutine scheduling

Background threads handle:
- Job system work stealing (CPU-bound tasks)
- Texture streaming (async I/O)
- Database queries (connection pool)
- Console subprocess I/O (pipe reader)
- Async logging (writer thread)

### Thread Safety Summary

| Subsystem | Thread-Safe? | Pattern |
|-----------|-------------|---------|
| `JobSystem` | Yes | Thread pool + mutex + CV |
| `ThreadSafeQueue` | Yes | Bounded FIFO + mutex |
| `SimpleConsole` | Yes | Mutex-guarded log sink |
| `Logger` | Yes | Async writer thread + atomic levels |
| `ConsoleProcessManager` | Partial | Pipe threads + command queue |
| `NetworkManager` | Partial | 4-mutex hierarchy |
| `AsyncDatabasePool` | Yes | Worker pool + callbacks on main |
| `TextureStreaming` | Yes | Streaming threads + mutex |
| `PhysicsSystem` | No | Main thread only |
| `GraphicsEngine` | No* | Main thread render, atomic frame state |
| `ParallelSystemExecutor` | No* | Dispatches to JobSystem from main |
| `ParallelPerceptionSystem` | Partial | Snapshot → parallel → writeback |
| `CoroutineScheduler` | No | Main thread only |
| `ECS (EnTT registry)` | No | Main thread only (use snapshots) |

\* Call from main thread only; internal state may use atomics.

---

## 1. Job System (Thread Pool)

**Source:** `SparkEngine/Source/Utils/JobSystem.h`

The `JobSystem` singleton provides a fixed-size worker pool for CPU-bound tasks.

### Architecture

```
Main Thread                    Worker Threads (N = hardware_concurrency)
    |                               |
    +-- Submit(task) ------------> m_jobQueue (mutex + CV)
    |                               |
    +-- ParallelFor(range) ------> splits into batches → Submit each
    |                               |
    +-- future.get() <------------ worker completes, promise fulfilled
```

### Threading Primitives

| Primitive | Purpose |
|-----------|---------|
| `std::atomic<bool> m_initialized` | Guard against double-init |
| `std::atomic<bool> m_stop` | Shutdown signal |
| `std::mutex m_queueMutex` | Guards `m_jobQueue` |
| `std::condition_variable m_condition` | Wakes workers on new jobs |
| `std::vector<std::thread> m_workers` | Worker thread pool |

### Usage

```cpp
// Submit a single task
auto future = JobSystem::Get().Submit([]{ return ExpensiveComputation(); });
auto result = future.get();  // Blocks until complete

// Parallel loop
JobSystem::Get().ParallelFor(0, 1000, [](int i){ ProcessItem(i); });
```

### Shutdown

`Shutdown()` sets `m_stop = true`, notifies all workers via `m_condition.notify_all()`, and joins all threads. Pending jobs in the queue are abandoned.

---

## 2. Thread-Safe Queue

**Source:** `SparkEngine/Source/Utils/ThreadSafeQueue.h`

A bounded FIFO queue used by NetworkManager and AudioEngine for cross-thread message passing.

| Method | Behavior |
|--------|----------|
| `TryPush(item)` | Returns `false` if at capacity |
| `ForcePush(item)` | Always enqueues (unbounded variant) |
| `TryPop()` | Returns `std::optional<T>`, non-blocking |
| `PopAll()` | Drains entire queue into vector (batch efficiency) |

Single `std::mutex` guards all operations. No condition variable — callers poll or use `PopAll()` each frame.

---

## 3. Network Manager

**Source:** `SparkEngine/Source/Engine/Networking/NetworkManager.h`

### Lock Hierarchy (CRITICAL)

NetworkManager uses four mutexes with a strict acquisition order:

```
m_stateMutex → m_queueMutex → m_handlerMutex
                                     ↑
m_clientsMutex (separate hierarchy)
m_inputMutex   (separate hierarchy)
```

**Rule:** Never acquire a higher-order mutex while holding a lower-order one. Violating this order causes deadlock.

| Mutex | Protects |
|-------|----------|
| `m_stateMutex` | `m_role`, `m_connectionState`, `m_localClientID` |
| `m_queueMutex` | `m_outgoingQueue`, `m_incomingQueue` |
| `m_handlerMutex` | `m_handlers` (message handler map) |
| `m_clientsMutex` | `m_clients` (connected client map), `m_nextClientID` |
| `m_inputMutex` | `m_pendingInputs` (client input states) |

### Atomics

- `std::atomic<uint32_t> m_nextNetworkID{1}` — Lock-free network ID generation

### Threading Model

Socket operations run on the main thread during `Update()`. The mutexes protect against concurrent `Send()` calls from game systems that may run on JobSystem workers.

---

## 4. Async Database Pool

**Source:** `SparkEngine/Source/Engine/Persistence/AsyncDatabase.h/.cpp`

TrinityCore-inspired architecture: one database connection per worker thread, plus a dedicated sync connection for the main thread.

### Architecture

```
Main Thread                         Worker Threads (1 per connection)
    |                                    |
    +-- AsyncQuery(stmt, params) -----> m_workQueue (mutex + CV)
    |                                    |
    +-- ProcessCallbacks() <----------- m_completedCallbacks (mutex)
    |                                    |
    +-- SyncQuery(stmt, params) ------> m_syncConnection (m_syncMutex)
```

### Threading Primitives

| Primitive | Purpose |
|-----------|---------|
| `std::vector<std::thread> m_workers` | Worker pool |
| `std::mutex m_queueMutex` + `m_queueCV` | Work queue synchronization |
| `std::mutex m_syncMutex` | Guards main-thread sync connection |
| `std::mutex m_callbackMutex` | Guards completed callback list |
| `std::atomic<bool> m_open` | Pool state |
| `std::atomic<bool> m_stopping` | Graceful shutdown |
| `std::atomic<int> m_pendingCount` | Outstanding query count |

### Critical Rule

**`ProcessCallbacks()` must be called on the main thread every frame.** Async query results accumulate in `m_completedCallbacks`; without draining, callbacks never fire and memory grows.

### Shutdown Sequence

1. `m_stopping.store(true)`
2. `m_queueCV.notify_all()` — wakes all workers
3. Workers exit their wait loops and terminate
4. Join all worker threads
5. Close all connections

---

## 5. Texture Streaming

**Source:** `SparkEngine/Source/Graphics/TextureStreaming.cpp`

### Architecture

Dedicated streaming threads load textures from disk without blocking the main thread.

| Primitive | Purpose |
|-----------|---------|
| `std::vector<std::thread> m_streamingThreads` | Loader threads |
| `std::mutex m_streamingMutex` + `m_streamingCondition` | Request queue sync |
| `std::mutex m_texturesMutex` | Guards texture cache |
| `std::atomic<bool> m_shouldStop` | Shutdown flag |

### Thread Function

`StreamingThreadFunction()` waits on `m_streamingCondition` for new requests, pops from `m_streamingQueue`, loads the texture, and stores in the cache under `m_texturesMutex`.

---

## 6. Logger (Async Writer)

**Source:** `SparkEngine/Source/Utils/Logger.h/.cpp`

### Architecture

Log messages are queued and written by a background thread. This prevents logging from stalling the main thread during file I/O.

| Primitive | Purpose |
|-----------|---------|
| `std::thread m_writerThread` | Background log writer |
| `std::mutex m_logMutex` + `m_queueCV` | Message queue sync |
| `std::atomic<bool> m_initialized` | Init guard |
| `std::atomic<bool> m_asyncEnabled` | Async mode toggle |
| `std::atomic<bool> m_stopThread` | Shutdown signal |
| `std::atomic<LogLevel> m_globalLevel` | Level filter (lock-free reads) |
| `std::atomic<LogLevel> m_categoryLevels[]` | Per-category levels |

### Thread Safety

`SPARK_LOG_*()` macros are safe to call from any thread. Messages are queued under `m_logMutex` and dispatched by the writer thread to registered sinks (stderr, file, editor).

---

## 7. Console Process Manager

**Source:** `SparkEngine/Source/Utils/ConsoleProcessManager.h`

Manages the SparkConsole.exe subprocess via stdin/stdout pipes.

| Primitive | Purpose |
|-----------|---------|
| `std::thread m_consoleThread` | Pipe reader thread |
| `std::atomic<bool> m_shouldStopThread` | Shutdown flag |
| `std::atomic<bool> m_initialized` | Init guard |
| `std::atomic<bool> m_consoleRunning` | Subprocess alive state |
| `std::mutex m_messageMutex` | Guards `m_messageQueue` (outgoing) |
| `std::mutex m_commandMutex` | Guards `m_commandQueue` (incoming) |

### Threading Model

The console thread reads from the subprocess pipe and pushes commands into `m_commandQueue`. The main thread calls `ProcessCommands()` each frame to drain the queue and execute commands.

---

## 8. Parallel ECS Execution

**Source:** `SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h`

### Conflict Detection

Systems declare their read/write component access:

```cpp
executor.DeclareReads<Transform>(renderSystem);
executor.DeclareWrites<Transform>(physicsSystem);
executor.BuildSchedule();
```

`BuildSchedule()` groups systems into **parallel batches** where no write-write or read-write conflicts exist.

### Execution

```
Batch 0: [PhysicsSystem, AudioSystem]     ← parallel (no conflicts)
Batch 1: [AnimationSystem]                ← sequential (writes Transform)
Batch 2: [RenderSystem, AISystem]         ← parallel (no conflicts)
```

- Single-system batches execute directly on the main thread
- Multi-system batches submit to `JobSystem` and wait for all futures
- **The executor itself is NOT thread-safe** — call only from the main thread

---

## 9. Parallel AI Perception

**Source:** `SparkEngine/Source/Engine/AI/ParallelPerception.h/.cpp`

### Gather-Process-Writeback Pattern

```
Main Thread:
  1. RebuildSpatialIndex(world)     — build Octree from entity positions
  2. GatherAgentData(world)         — snapshot agent state into m_agentJobs
  3. UpdateAllAgents()              — dispatch to JobSystem

Worker Threads:
  4. ProcessAgentPerception(job)    — query Octree (read-only), check sight/hearing

Main Thread:
  5. WriteBackResults(world)        — copy results back to ECS
```

**Key insight:** Workers never touch the ECS registry. They read from the Octree (immutable during processing) and write to per-agent job data (no sharing between agents). This eliminates all data races without locks.

---

## 10. Physics System

**Source:** `SparkEngine/Source/Physics/PhysicsSystem.h`

```
@note PhysicsSystem is NOT thread-safe. Call all public methods from the
main game thread. The physics simulation runs on the calling thread
inside Update().
```

No threading primitives. Jolt Physics supports multithreaded job dispatch. All raycasts, collision queries, and body manipulation must happen on the main thread.

---

## 11. Coroutine System

**Source:** `SparkEngine/Source/Engine/Coroutine/CoroutineTypes.h`

C++20 coroutines (WaitForSeconds, WaitForFrames, WaitUntil) yield and resume on the main thread only. The `CoroutineScheduler` ticks each frame and resumes ready coroutines. No threading involved.

---

## Main Thread Constraints

These subsystems **must only be called from the main thread:**

1. **PhysicsSystem** — all public methods
2. **ECS registry** (EnTT) — entity creation, component access, iteration
3. **GraphicsEngine** — render passes, resource creation
4. **CoroutineScheduler** — tick and resume
5. **ParallelSystemExecutor** — call `Execute()` from main; workers run on JobSystem
6. **ParallelPerceptionSystem** — Gather/WriteBack from main; processing on workers
7. **Editor UI** — all ImGui rendering

---

## Background Thread Inventory

| Thread | Owner | Purpose |
|--------|-------|---------|
| N worker threads | `JobSystem` | CPU-bound tasks, ECS parallel batches |
| M streaming threads | `TextureStreaming` | Async texture loading |
| K database workers | `AsyncDatabasePool` | Async SQL queries |
| 1 writer thread | `Logger` | Async log sink dispatch |
| 1 console thread | `ConsoleProcessManager` | Subprocess pipe I/O |

---

## Safe Patterns

### Submitting work to the job system

```cpp
auto future = JobSystem::Get().Submit([]{ return ComputeNavMesh(); });
// ... do other work ...
auto mesh = future.get();  // Block when result needed
```

### Parallel ECS iteration (via executor)

```cpp
executor.DeclareReads<Transform>(renderSystem);
executor.DeclareWrites<Velocity>(physicsSystem);
executor.BuildSchedule();
executor.Execute(world, dt);  // Main thread — workers handle batches
```

### Async database queries

```cpp
auto future = db.AsyncQuery(STMT_LOAD_PLAYER, {playerId});
// Later, on main thread:
db.ProcessCallbacks();  // Fires completion handlers
```

### Thread-safe logging

```cpp
SPARK_LOG_INFO(LogCategory::Core, "Message from any thread");
```

## Unsafe Patterns (Never Do)

| Pattern | Why It's Wrong | Fix |
|---------|---------------|-----|
| Access ECS from worker thread | EnTT is not thread-safe | Use snapshots or the Gather-Process-Writeback pattern |
| Call Physics from non-main thread | Jolt Physics dispatch is thread-safe; query from any thread | Queue physics commands for main thread |
| Reverse NetworkManager lock order | Deadlock | Always: state → queue → handler |
| Hold mutex during long I/O | Main thread stalls | Use async patterns (JobSystem, AsyncDB) |
| Busy-wait on atomics | CPU waste | Use `std::condition_variable` |
| Create `std::thread` directly | Unmanaged lifetime | Use `JobSystem::Submit()` instead |

---

## Deadlock Prevention Rules

1. **NetworkManager:** Acquire locks in order: `m_stateMutex` → `m_queueMutex` → `m_handlerMutex`
2. **General:** Never hold two mutexes from different subsystems simultaneously
3. **JobSystem:** Never submit a job that blocks waiting for another job (potential pool exhaustion)
4. **AsyncDatabase:** Never call `SyncQuery()` from a worker thread (deadlocks on `m_syncMutex`)
5. **Main thread:** Never block indefinitely on a `std::future` — use timeouts or poll

---

## See Also

- [Job System](Job-System) — Detailed job system API and usage
- [Persistence System](Persistence-System) — AsyncDatabasePool architecture
- [Networking](Networking) — Network message threading
- [Entity Component System](Entity-Component-System) — ECS execution order
- [AI and Navigation](AI-and-Navigation) — Parallel perception system
