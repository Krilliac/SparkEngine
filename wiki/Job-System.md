# Job System

SparkEngine provides a lightweight, thread-safe job system built on a fixed-size thread pool. The `Spark::JobSystem` class lives in `SparkEngine/Source/Utils/JobSystem.h` and is the primary mechanism for parallelizing CPU-bound work across the engine.

## Overview

The job system is a **singleton** that manages a pool of worker threads. Workers pull jobs from a shared FIFO queue and execute them. Any callable (lambdas, function pointers, `std::bind` results) can be submitted as a job, and callers receive a `std::future` for retrieving results or detecting completion.

Key characteristics:

- **Fixed-size thread pool** created once at engine startup
- **`std::mutex` + `std::condition_variable`** for queue synchronization
- **`std::packaged_task` + `std::future`** for result propagation
- **Automatic batch sizing** in `ParallelFor` based on worker count
- **Inline fallback** when the pool has no workers or the workload is too small
- **Non-copyable singleton** with RAII shutdown in the destructor

```
                      +--------------------------+
                      |     Main Thread          |
                      |                          |
                      |   Submit() / ParallelFor |
                      +--------|-----------|-----+
                               |           |
                               v           v
                      +------------------------+
                      |      Job Queue         |
                      |  (mutex + cond_var)    |
                      +--|------|------|-------+
                         |      |      |
                    +----v+ +---v-+ +--v---+
                    | W0  | | W1  | | W2   | ...  (worker threads)
                    +-----+ +-----+ +------+
```

## Header Location

```
SparkEngine/Source/Utils/JobSystem.h
```

Namespace: `Spark`

## Initialization and Shutdown

The job system must be initialized before any subsystem submits work. SparkEngine handles this automatically during engine startup via `EngineSetup::InitializeJobSystem()`, which is called after core subsystem registration and before any system that uses parallel processing (AI, perception, parallel system executor).

### Automatic initialization (engine startup)

The engine bootstrap system registers the `JobSystem` as a named subsystem with dependency tracking. It is initialized before the AI subsystem and any other subsystem that declares a dependency on it:

```cpp
// From EngineSetup.h — the engine does this for you
bootstrap.Register({"JobSystem",
    []() {
        auto& js = Spark::JobSystem::Get();
        if (!js.IsInitialized())
            js.Initialize(0);
        return true;
    },
    []() {
        auto& js = Spark::JobSystem::Get();
        js.Shutdown();
    },
    {}});

// AI declares a dependency on JobSystem
bootstrap.Register({"AI", []() { return true; }, []() {}, {"Timer", "Physics", "JobSystem"}});
```

### Manual initialization

If you need to control the thread count explicitly (for example, in a tool or test harness), call `Initialize` directly:

```cpp
auto& jobs = Spark::JobSystem::Get();
jobs.Initialize(4);  // 4 worker threads

// ... use the job system ...

jobs.Shutdown();  // blocks until all pending jobs drain, then joins threads
```

Passing `0` (the default) creates `std::thread::hardware_concurrency() - 1` workers, reserving one core for the main thread. Calling `Initialize` on an already-initialized instance is a no-op.

### Shutdown behavior

`Shutdown()` sets the stop flag, notifies all waiting workers, and blocks until every worker thread has completed its current job and exited. Any jobs still in the queue at the time of shutdown are executed before threads exit. The destructor calls `Shutdown()` automatically, so the system is safe even if you forget to shut down explicitly.

## API Reference

### Static Access

| Method | Description |
|--------|-------------|
| `static JobSystem& Get()` | Returns the singleton instance (Meyers singleton, thread-safe) |

### Lifecycle

| Method | Description |
|--------|-------------|
| `void Initialize(uint32_t numThreads = 0)` | Create the worker thread pool. `0` means `hardware_concurrency - 1`. No-op if already initialized. |
| `void Shutdown()` | Signal stop, drain the queue, join all threads. Blocks until complete. No-op if not initialized. |
| `bool IsInitialized() const` | Returns `true` if the thread pool is running |

### Job Submission

| Method | Description |
|--------|-------------|
| `auto Submit(F&& f, Args&&... args) -> std::future<R>` | Submit any callable. Returns a future holding the result type `R = std::invoke_result_t<F, Args...>`. |
| `void ParallelFor(int begin, int end, Func&& body, int minBatchSize = 1)` | Split `[begin, end)` into chunks, submit each as a job, block until all complete. |
| `void WaitForAll()` | Submit a barrier job and block until the queue is drained up to that point. |

### Diagnostics

| Method | Description |
|--------|-------------|
| `uint32_t GetWorkerCount() const` | Number of worker threads in the pool |
| `size_t GetPendingJobCount() const` | Current number of jobs waiting in the queue (snapshot) |

## Usage Patterns

### Fire-and-forget jobs

When you do not need the result and just want work to happen asynchronously, submit a lambda and discard the future:

```cpp
auto& jobs = Spark::JobSystem::Get();

// The future is discarded — the job runs in the background
jobs.Submit([]
{
    RegenerateLightProbes();
});
```

**Caution**: If the fire-and-forget job captures references or pointers, you must ensure the referenced data outlives the job. Use `WaitForAll()` or `Shutdown()` before destroying shared state.

### Future-based results

Submit a job that returns a value and retrieve it later via the future:

```cpp
auto& jobs = Spark::JobSystem::Get();

// Submit a computation that returns a value
auto future = jobs.Submit([]() -> float
{
    return ComputePathCost(startPos, endPos);
});

// Do other work on the main thread while the job runs...
UpdateUI();

// Block until the result is ready
float pathCost = future.get();
```

You can also submit jobs with arguments:

```cpp
auto future = jobs.Submit([](int a, int b) { return a + b; }, 10, 20);
int result = future.get();  // 30
```

### Parallel for loops

`ParallelFor` is the most common pattern for data-parallel workloads. It splits a range into batches, submits each batch as a separate job, and blocks until all batches complete:

```cpp
auto& jobs = Spark::JobSystem::Get();

std::vector<Entity> entities = GetVisibleEntities();
std::vector<float> distances(entities.size());

jobs.ParallelFor(0, static_cast<int>(entities.size()),
    [&](int i)
    {
        distances[i] = ComputeDistanceToCamera(entities[i]);
    });

// All distances are computed by this point
```

#### Batch sizing

The `minBatchSize` parameter (default 1) controls the minimum number of items per batch. For very cheap per-item work, increase this to reduce job submission overhead:

```cpp
// Each item is trivial — use a larger batch to amortize scheduling cost
jobs.ParallelFor(0, 100000,
    [&](int i) { output[i] = input[i] * 2.0f; },
    256);  // at least 256 items per batch
```

The actual batch size is computed as `max(minBatchSize, totalItems / (numWorkers + 1))`, which distributes work evenly across all workers plus the submitting thread.

#### Inline fallback

If the job system has no workers (not initialized, or initialized with 0 workers) or the total range is smaller than `minBatchSize`, `ParallelFor` executes the loop inline on the calling thread. This means code using `ParallelFor` works correctly even when the job system is disabled or in single-threaded test environments.

### Collecting multiple futures

For heterogeneous parallel work where different jobs have different return types or logic:

```cpp
auto& jobs = Spark::JobSystem::Get();

auto meshFuture = jobs.Submit([] { return LoadMesh("soldier.fbx"); });
auto texFuture  = jobs.Submit([] { return LoadTexture("soldier_diffuse.dds"); });
auto navFuture  = jobs.Submit([] { return BakeNavMeshRegion(region); });

// All three load in parallel on different worker threads
auto mesh    = meshFuture.get();
auto texture = texFuture.get();
auto navData = navFuture.get();
```

## Engine Integration Patterns

### AI system (parallel agent updates)

The AI system uses `ParallelFor` to tick behavior trees and update steering for all live agents in parallel. Each agent's perception, behavior tree tick, and movement computation are independent, making this an ideal parallelization target:

```cpp
// From AISystem.cpp
auto& jobSystem = Spark::JobSystem::Get();
jobSystem.ParallelFor(
    0, agentCount,
    [&](int i)
    {
        auto& agent = liveAgents[i];
        UpdatePerception(agent, world, deltaTime);
        TickBehaviorTree(agent, deltaTime);
        ApplySteering(agent, deltaTime);
    });
```

### Parallel perception (Octree + JobSystem)

The `ParallelPerceptionSystem` dispatches per-agent perception queries across worker threads. It follows a three-phase pattern that avoids ECS access from worker threads:

1. **BuildSpatialIndex** (main thread) -- Insert perceivable entities into an Octree.
2. **GatherAgentData** (main thread) -- Snapshot per-agent state into `AgentPerceptionJob` structs.
3. **UpdateAllAgents** (parallel) -- Dispatch `JobSystem::ParallelFor` over the agent array; each worker queries the Octree and runs sight/hearing checks.

This pattern is important: ECS reads/writes happen only on the main thread (phases 1 and 2), while the parallel phase (3) operates exclusively on pre-gathered, thread-local data structures.

```cpp
// From ParallelPerception.cpp
auto& jobSystem = JobSystem::Get();
const int agentCount = static_cast<int>(m_agentJobs.size());

if (jobSystem.IsInitialized() && agentCount >= m_minBatchSize)
{
    jobSystem.ParallelFor(0, agentCount, [&](int i)
    {
        ProcessAgentPerception(m_agentJobs[i], m_octree, currentTime);
    });
}
```

### Parallel ECS system execution

The `ParallelSystemExecutor` (in `SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h`) analyzes component read/write sets per system and groups non-conflicting systems into parallel batches. Each batch runs its systems concurrently on the JobSystem thread pool, while batches execute sequentially to preserve data dependencies:

```cpp
ParallelSystemExecutor executor;

// Declare component access patterns
executor.DeclareAccess<PhysicsUpdateSystem>({typeid(Transform)}, {typeid(RigidBodyComponent)});
executor.DeclareAccess<AIUpdateSystem>({typeid(Transform)}, {typeid(AIComponent)});
executor.DeclareAccess<AudioUpdateSystem>({typeid(Transform)}, {}); // read-only

// Build schedule (determines which systems can run in parallel)
executor.BuildSchedule();

// Per frame: batches run sequentially, systems within a batch run in parallel
executor.Execute(world, deltaTime);
```

Systems within a batch are submitted via `JobSystem::Submit` and collected with futures:

```
Batch 0: [PhysicsUpdateSystem, AudioUpdateSystem]  -- run in parallel
Batch 1: [AIUpdateSystem]                           -- runs after batch 0
```

## Thread Safety Rules

The `JobSystem` class is fully thread-safe. `Submit`, `ParallelFor`, `WaitForAll`, and all diagnostic methods can be called from any thread.

However, the **jobs themselves** must be written with thread safety in mind:

| Rule | Rationale |
|------|-----------|
| Do not access the ECS registry from worker threads | EnTT is not thread-safe for concurrent mutation. Gather data on the main thread first. |
| Do not call `PhysicsSystem` methods from worker threads | Jolt Physics supports multithreaded job dispatch, but direct API calls should be synchronized. |
| Avoid shared mutable state without synchronization | Standard data-race rules apply. |
| Prefer per-index output arrays over shared containers | `ParallelFor` with per-index writes to a pre-allocated array is data-race free. |
| Use `WaitForAll()` before reading results from fire-and-forget jobs | Without the future, there is no other synchronization point. |
| Keep `GraphicsEngine` calls on the main thread | GPU submission is single-threaded; only `std::atomic` frame state is safe cross-thread. |

## Performance Considerations

- **Job granularity**: Each `Submit` call allocates a `std::packaged_task` and acquires the queue mutex. For very fine-grained work (nanosecond-scale operations), prefer `ParallelFor` with a larger `minBatchSize` over many individual `Submit` calls.
- **Worker count**: The default (`hardware_concurrency - 1`) is generally optimal. Over-subscribing the CPU with more workers than cores causes context-switch overhead.
- **Queue contention**: The single shared queue with a single mutex is adequate for moderate submission rates. Extremely high-frequency submission (thousands of micro-jobs per frame) may benefit from per-thread work-stealing queues in a future iteration.
- **Profiling**: Use the `Profiler` to measure the wall-clock time of `ParallelFor` calls and compare against the serial baseline. Parallelization only helps when per-item work significantly exceeds the scheduling overhead.

## Diagnostics and Debugging

Query the job system state at runtime via console commands or debug overlays:

```cpp
auto& js = Spark::JobSystem::Get();
std::stringstream ss;
ss << "JobSystem: " << (js.IsInitialized() ? "YES" : "NO");
if (js.IsInitialized())
{
    ss << " (" << js.GetWorkerCount() << " workers)";
    ss << " pending: " << js.GetPendingJobCount();
}
```

The engine's `engine.status` console command includes JobSystem status automatically, reporting whether it is initialized and how many worker threads are active.

For the `ParallelSystemExecutor`, use `Console_GetScheduleInfo()` to print the parallel batch layout and verify that systems are grouped as expected.

## Companion Utility: ThreadSafeQueue

The `Spark::ThreadSafeQueue<T>` template (in `SparkEngine/Source/Utils/ThreadSafeQueue.h`) provides a mutex-guarded bounded FIFO queue used by several subsystems alongside the JobSystem:

- **NetworkManager**: message I/O buffering
- **AudioEngine**: command buffering from game thread to audio thread

While `JobSystem` uses a raw `std::queue` internally with its own mutex, `ThreadSafeQueue` is available for your own producer/consumer patterns between the main thread and worker threads.

## Limitations and Future Work

- **No job dependencies or continuations**: Jobs cannot declare "run after job X completes." Use futures or manual synchronization for ordering.
- **No priority queues**: All jobs are FIFO. High-priority work cannot preempt queued low-priority work.
- **No work stealing**: Workers are bound to a single shared queue. A per-thread deque with work stealing would improve cache locality for fine-grained workloads.
- **No main-thread dispatch**: There is no mechanism to schedule a callback back onto the main thread from a worker. Results must be polled via futures.

These limitations are documented in the [Codebase Health](Codebase-Health) wiki page under architectural considerations.

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Utils/JobSystem.h` | JobSystem singleton (header-only) |
| `SparkEngine/Source/Utils/ThreadSafeQueue.h` | Companion thread-safe queue template |
| `SparkEngine/Source/Core/EngineSetup.h` | Bootstrap registration and `InitializeJobSystem()` |
| `SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h` | Parallel ECS system batching |
| `SparkEngine/Source/Engine/AI/AISystem.cpp` | AI parallel agent updates |
| `SparkEngine/Source/Engine/AI/ParallelPerception.h` | Parallel perception with Octree |
| `SparkEngine/Source/Engine/AI/ParallelPerception.cpp` | Parallel perception implementation |

---

## See Also

- [Architecture Overview](Architecture-Overview) -- Engine architecture, subsystem layout, and key patterns
- [Entity Component System](Entity-Component-System) -- ECS architecture, components, systems, and the `ParallelSystemExecutor`
- [AI and Navigation](AI-and-Navigation) -- Behavior trees, NavMesh, and parallel perception
- [Testing](Testing) -- Unit tests and CTest integration
- [Profiler and Debugging](Profiler-and-Debugging) -- Profiler, debug overlays, and performance measurement
