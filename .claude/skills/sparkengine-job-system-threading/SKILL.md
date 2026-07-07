---
name: sparkengine-job-system-threading
description: >-
  SparkEngine concurrency and threading-discipline runbook: the real JobSystem /
  ThreadSafeQueue / StageBasedExecutor / ParallelPerception APIs, the ECS phase
  barriers, per-subsystem thread-safety rules, and a checklist for parallelizing a
  new system without data races. TRIGGER when: "parallelize this system", "is it
  safe to call from a worker thread", "add a job", "JobSystem", "Submit / ParallelFor",
  "which stage does my system run in", "data race / crash under TSan", "can I touch the
  ECS from a background thread", "thread-safe queue", "Jolt physics threads". DO NOT
  TRIGGER for: performance profiling / measuring / optimizing hot paths (use the global
  perf-hotpath skill), rendering-backend RHI work, or networking protocol logic.
trilobite_compatible: true
trilobite_role: concurrency
---

# SparkEngine — Job System & Threading Discipline

This is a **correctness runbook** for concurrency in SparkEngine. It tells you which
threading primitives actually exist, where the synchronization barriers are, what each
subsystem's thread-safety contract is, and how to add parallel work without introducing
a data race. It is not a performance guide — for measuring or tuning hot paths use the
global **perf-hotpath** skill instead.

Jargon defined once:
- **Job** — a callable (lambda) handed to the thread pool to run on a worker thread.
- **Worker thread** — one of N background threads owned by `JobSystem` that pull jobs.
- **Main thread** — the game/render thread that owns the ECS registry, physics, and D3D.
- **Barrier / join point** — a place where the main thread waits for all outstanding
  jobs to finish (`future.get()`) before continuing. This is what makes parallel code
  deterministic between phases.
- **ECS stage/phase** — a bucket in the fixed engine update order.

---

## 0. The one invariant you must never break

The ECS update order is fixed and load-bearing (stated in `CLAUDE.md`, enforced by
`SystemStage` in `ParallelSystemExecutor.h`):

```
Physics -> Animation -> AI -> Audio -> Lifecycle -> Render
```

Systems **within** one stage may run in parallel. Stages **never** overlap: the engine
joins all jobs from a stage before starting the next. Do not reorder stages, and do not
let one stage's work leak into the next without a join.

---

## 1. The real primitives (read the headers, don't invent)

There is **no** custom "task graph" or "fiber" abstraction. There are exactly these
building blocks. Every claim below was verified against the source on 2026-07-07.

### 1a. `Spark::JobSystem` — the one thread pool
`SparkEngine/Source/Utils/JobSystem.h` (header-only singleton).

| Call | Signature | Notes |
|------|-----------|-------|
| Get | `JobSystem& JobSystem::Get()` | Meyers singleton. |
| Submit | `auto Submit(F&& f, Args&&...) -> std::future<...>` | Enqueue one job, get a future. |
| ParallelFor | `void ParallelFor(int begin, int end, Func&& body, int minBatchSize = 1)` | Splits `[begin,end)` into batches, submits each, **blocks until all done**. Runs inline if no workers or range `<= minBatchSize`. |
| WaitForAll | `void WaitForAll()` | Submits an empty barrier job and blocks on it. |
| GetWorkerCount | `uint32_t GetWorkerCount() const` | 0 means "no pool — run inline". |
| IsInitialized | `bool IsInitialized() const` | Check before dispatching. |
| GetPendingJobCount | `size_t GetPendingJobCount() const` | Queue depth snapshot. |
| Initialize | `void Initialize(uint32_t numThreads = 0)` | `0` = `hardware_concurrency() - 1`. **You almost never call this** — see below. |
| Shutdown | `void Shutdown()` | Joins all workers; pending jobs are drained first. |

Lifecycle you do NOT own:
- Initialized **once** at engine boot via `EngineBootstrap` in
  `SparkEngine/Source/Core/EngineSetup.h` (`js.Initialize(0)`).
- Shut down in `SparkEngine/Source/Core/SparkEngine.cpp` (`JobSystem::Get().Shutdown()`),
  after all subsystems that submit jobs. **Do not call `Initialize` or `Shutdown`
  yourself** in gameplay/system code — just `Get()` and use it.

Safety properties baked into the pool (verified in `JobSystem.h`):
- A job that throws does **not** kill the worker: the worker `try/catch`es and logs via
  `SPARK_LOG_ERROR`. A fire-and-forget exception is swallowed after logging; an exception
  in a `Submit`-with-future job is re-thrown at `future.get()`.
- The pool has a **fixed** worker count. See the deadlock rule in §5.

### 1b. `Spark::ThreadSafeQueue<T>` — producer/consumer FIFO
`SparkEngine/Source/Utils/ThreadSafeQueue.h`.

Mutex-guarded bounded FIFO. **No condition variable** — consumers poll or drain each
frame; nobody blocks waiting for an item. Verified signatures (note: pop is
out-parameter + `bool`, NOT `std::optional`):

```cpp
bool TryPush(T item);      // false if at capacity
void ForcePush(T item);    // ignores capacity
bool TryPop(T& out);       // false if empty (never blocks)
void PopAll(std::vector<T>& out);  // drain all, batch-efficient
size_t Size() const; bool IsEmpty() const; void Clear();
```

Constructor: `explicit ThreadSafeQueue(size_t maxSize = 0)` — `0` = unbounded.
Used by `NetworkManager` (message I/O), `JobSystem` distribution, and audio command
buffering.

### 1c. `Spark::ECS::StageBasedExecutor` — the live ECS stage runner
`SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h` + `.cpp` (singleton).

This is the executor actually ticked by the main loop. Runs systems **within** a stage
in parallel on `JobSystem`, joins, then advances to the next stage — preserving the §0
order.

```cpp
enum class SystemStage : uint8_t { Physics, Animation, AI, Audio, Lifecycle, Render, Count };

StageBasedExecutor& StageBasedExecutor::GetInstance();
void RegisterSystem(const std::string& name, SystemStage stage, std::function<void(float)> fn);
void ExecuteAll(float dt);   // stages sequential; systems in a stage parallel-then-joined
void Shutdown();
std::string Console_GetStatus() const;
```

Wiring status (verified 2026-07-07): `ExecuteAll(dt)` **is** called every frame from
`SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp`
(`StageBasedExecutor::GetInstance().ExecuteAll(dt)`), and `Shutdown()` is called on
teardown. **However, no `RegisterSystem(...)` call exists anywhere in the codebase**
(only the definition). So today this is a **dead tick**: the executor is invoked but has
zero registered systems. This is exactly the ECS-consolidation debt called out in
`HARDEN_FLEET_HANDOFF.md` ("register systems into `StageBasedExecutor` (dead tick)").
If you are adding a system to run in a stage, this is the API to register it against —
but be aware nothing else is registered yet, so verify the tick path end-to-end.

### 1d. `Spark::ECS::ParallelSystemExecutor` — access-set batching (present, NOT wired)
Same header. A **different, non-singleton** class that auto-derives parallel batches
from declared component read/write sets (not from stages):

```cpp
void DeclareAccess(ISystem*, unordered_set<type_index> reads, unordered_set<type_index> writes);
template<class...> void DeclareReads(ISystem*);   // convenience
template<class...> void DeclareWrites(ISystem*);
void BuildSchedule();                 // groups into conflict-free batches
void Execute(World& world, float dt); // batches sequential; systems in a batch parallel-joined
size_t GetBatchCount() const;
std::string Console_GetScheduleInfo() const;
```

Conflict rule enforced by `CanAddToBatch`: two systems may share a batch only if there
is **no** write-write, write-read, or read-write overlap on any component type.

Wiring status (verified 2026-07-07): defined and configured by
`ConfigureParallelExecution(...)` in `SparkEngine/Source/Engine/ECS/ECSIntegration.h`,
but `ConfigureParallelExecution` is **never called** from any `.cpp`, and no live
`Execute(world, dt)` call exists. Treat this class as **candidate / not wired into the
live tick** — do not assume registering here runs anything. `StageBasedExecutor` (§1c) is
the live path.

> Relationship note (accurate, not a guess): the handoff's phrase
> "`SystemManager`/`PhaseSystemManager`/`StageBasedExecutor`" refers to three *ordering*
> managers. `ParallelSystemExecutor` is a **fourth**, related class — the access-set
> batcher. `PhaseSystemManager` (`PhaseSystemManager.h`) once dispatched through a single
> global `ParallelSystemExecutor` and that parallel path was **removed** because batching
> across all phases silently discarded phase ordering (Render could beat Physics); it now
> runs **serially** (see the `UpdateAll` note in `PhaseSystemManager.h`). Correct
> per-phase parallelism would need one executor per phase.

### 1e. `Spark::AI::ParallelPerceptionSystem` — gather / process / writeback (WIRED)
`SparkEngine/Source/Engine/AI/ParallelPerception.h` + `.cpp`. This is the reference
example of doing ECS work in parallel **safely**:

1. `RebuildSpatialIndex(world)` — main thread builds an Octree from entity positions.
2. `GatherAgentData(world)` — main thread snapshots each agent into an `AgentPerceptionJob`
   struct. **Workers never touch the ECS registry.**
3. `UpdateAllAgents(world, time)` — dispatches `JobSystem::ParallelFor` over the agent
   array; each worker reads the immutable Octree and writes only its own job struct.
4. `WriteBackResults(world)` — main thread copies results back into ECS components.

Wired 2026-04-14: owned/ticked by `Spark::AI::AIIntegratedSystem` (`AIIntegration.h`),
initialized from `GameplayLifecycleShared.cpp`. `SetMinBatchSize(int)` gates against
thread-pool overhead for small agent counts.

### 1f. Jolt physics job pool (separate from `Spark::JobSystem`)
`SparkEngine/Source/Physics/PhysicsSystem.cpp` creates its **own** internal pool:
`JPH::JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, hardware_concurrency()-1)`.
This is Jolt's internal parallelism for the simulation step — it is **not**
`Spark::JobSystem`, and you do not submit to it. See §3 for the caller-facing contract.

---

## 2. Where the barriers are

A "barrier" here = the main thread blocking until a group of jobs finishes. Know these
so you never read a result before it's produced:

| Barrier | Code | What it guarantees |
|---------|------|--------------------|
| Between ECS stages | `StageBasedExecutor::ExecuteAll` loops stages; per stage it `Submit`s each system then `for (f : futures) f.get();` before the next stage | Stage N fully completes before stage N+1 starts. |
| Between access-batches | `ParallelSystemExecutor::Execute` joins each batch's futures before the next batch | No conflicting batch overlaps (when wired). |
| End of a parallel loop | `JobSystem::ParallelFor` collects all batch futures and `.get()`s them before returning | Every index processed before the call returns. |
| Explicit drain | `JobSystem::WaitForAll()` | All currently-queued jobs done (submits a sentinel and blocks). |
| Any single result | `future.get()` on the future from `Submit` | That one job is done; re-throws its exception if it threw. |

Single-system stages/batches run **inline on the main thread** (no dispatch), so their
"barrier" is trivially the function returning.

Fault isolation wrapper: system updates dispatched by the executors are wrapped in
`SPARK_GUARDED_UPDATE(name, category, { ... })` (`SparkEngine/Source/Core/FaultIsolation.h`).
It `try/catch`es the body and reports the fault instead of propagating — so an exception
in a stage system is contained, not a crash. Wrap your own dispatched system body the
same way for parity.

---

## 3. Per-subsystem thread-safety contract

From `CLAUDE.md`, cross-checked against the actual code. "Main-thread-only" means: call
its public methods **only** from the game/render thread, never from a `JobSystem` worker.

| Subsystem | Contract | Verified detail |
|-----------|----------|-----------------|
| `Spark::JobSystem` | Thread-safe to `Submit`/query from any thread | Mutex + CV around the queue. |
| `Spark::ThreadSafeQueue<T>` | Thread-safe | Single mutex; no CV (poll/drain). |
| `SimpleConsole` | Thread-safe | Mutex-guarded log sink. |
| `Logger` / `SPARK_LOG_*` | Thread-safe from any thread | Async writer thread + atomic levels. |
| `NetworkManager` | Queue mutex for message I/O + handler registration | `CLAUDE.md`. The wiki documents a 4-mutex lock order (`state -> queue -> handler`, with `clients`/`input` separate) — treat that ordering as the deadlock-avoidance rule if you touch it. |
| `GraphicsEngine` | **Main-thread render**; `std::atomic` frame state | Do not issue draws/resource creation off-thread. |
| `PhysicsSystem` | **Public API is main-thread-only**, even though the *simulation* is multithreaded | `CLAUDE.md` says "supports multithreaded job dispatch" — that refers to Jolt's **internal** `JPH::JobSystemThreadPool` used during the step, NOT the caller API. All raycasts, body edits, and queries must come from the main thread. Do not call `PhysicsSystem` from a worker. |
| ECS registry (EnTT `World`) | **Main-thread-only** | Never read/iterate/mutate from a worker. Use the gather/writeback pattern (§1e). |
| `ParallelSystemExecutor` / `StageBasedExecutor` | The executor object is **not** thread-safe; call from main thread only. The *systems it dispatches* run on workers. | Header `@threadsafety` note. |
| `ParallelPerceptionSystem` | Gather/Writeback on main; per-agent processing on workers | Workers touch only snapshot structs + immutable Octree. |
| `CoroutineScheduler` | Main-thread-only | Resumes on main thread each frame. |

The single most common bug this skill prevents: **touching the EnTT `World` or
`PhysicsSystem` from inside a `JobSystem::Submit`/`ParallelFor` body.** Don't. Snapshot on
the main thread first.

---

## 4. Checklist — "I want to parallelize a new system safely"

Work top to bottom. Stop at the first row that says stop.

1. **Do you even need threads?** If the work is < a few hundred microseconds or touches
   the ECS/physics/graphics directly, keep it on the main thread. Parallelism has join
   overhead. (For "is it actually a hot path", measure first — see **perf-hotpath**.)

2. **Does your job body touch shared mutable state?** The EnTT `World`, `PhysicsSystem`,
   `GraphicsEngine`, any singleton without a documented thread-safe contract in §3?
   - If **yes** → you cannot dispatch it as-is. Use the **gather / process / writeback**
     pattern: on the main thread, copy the inputs each job needs into per-job POD structs
     (see `AgentPerceptionJob` in `ParallelPerception.h`); dispatch jobs that read only
     those structs + immutable shared data; write results back on the main thread after
     the join. Each job must write only to **its own** slot — never a shared container
     without a lock.
   - If **no** (pure computation over private/snapshot data) → continue.

3. **Pick the dispatch primitive:**
   - Uniform work over an index range → `JobSystem::Get().ParallelFor(0, n, body, minBatch)`.
     Set `minBatch` so tiny ranges run inline (perception uses 4).
   - A handful of independent one-off tasks → `JobSystem::Get().Submit(lambda)` per task,
     then `future.get()` each before you use results (pattern used in
     `GameplayLifecycleShared.cpp` `UpdateNonECSSystems`).
   - A whole ECS system that should run inside a stage → register it with
     `StageBasedExecutor::GetInstance().RegisterSystem(name, SystemStage::X, fn)`.
     **Caveat (see §1c):** this is the *intended-but-unfinished* parallel path.
     Its `ExecuteAll(dt)` tick is live (called every frame from
     `GameplayLifecycleShared.cpp`), but **zero other systems are registered into
     it today**, and the intended-canonical `PhaseSystemManager` is not wired at
     all — so "where an ECS system runs" is a known OPEN architectural weak point
     (see `sparkengine-architecture-contract` Invariant 3). Registering here does
     tick, but verify the whole path end-to-end and flag the wiring gap rather
     than assuming this is a settled home.

4. **Gate on pool availability.** Before dispatching, check
   `jobs.IsInitialized() && jobs.GetWorkerCount() > 0`; fall back to an inline loop
   otherwise (this is exactly what `StageBasedExecutor::ExecuteAll` and
   `UpdateNonECSSystems` do). `ParallelFor` already handles the zero-worker case.

5. **Never block a worker on another job.** Do not call `future.get()` (or otherwise wait
   on other jobs) *from inside* a job body. The pool has a fixed worker count; a job that
   waits for a job that has no free worker to run = deadlock (deadlock rule 3 in the wiki).
   Fan out and join **only** on the main thread.

6. **Respect the stage boundary.** Anything that reads another stage's output must run in
   a later stage (or after the join). Do not assume ordering *within* a parallel batch.

7. **Wrap the dispatched body** in `SPARK_GUARDED_UPDATE(name, category, { ... })` if it's
   an engine system update, so a fault is isolated rather than crashing (matches §2).

8. **Prove it.** Build and run the ASan and TSan CI jobs — `build-linux-asan` and
   `build-linux-tsan` (see `CLAUDE.md` CI table). TSan is the one that catches data
   races. Add/adjust a test under `Tests/`.

Copy-paste skeleton (snapshot + ParallelFor + writeback):

```cpp
// --- main thread ---
std::vector<MyJobData> jobs;                 // POD snapshot, one slot per work item
GatherInputsFromECS(world, jobs);            // main-thread reads only

auto& js = Spark::JobSystem::Get();
if (js.IsInitialized() && js.GetWorkerCount() > 0)
{
    js.ParallelFor(0, (int)jobs.size(),
        [&jobs](int i) { ProcessOne(jobs[i]); },   // touches ONLY jobs[i] + const data
        /*minBatchSize*/ 4);                        // ParallelFor joins before returning
}
else
{
    for (auto& j : jobs) ProcessOne(j);          // inline fallback
}

WriteResultsBackToECS(world, jobs);          // main-thread writes only
```

---

## 5. Quick "is this safe?" decision table

| You want to... | Safe? | Do this |
|----------------|-------|---------|
| Read the ECS `World` inside a `Submit`/`ParallelFor` body | NO | Snapshot on main thread first (§1e/§4). |
| Call any `PhysicsSystem` method from a worker | NO | Queue it; run on main thread. |
| Issue a D3D draw / create a GPU resource off-thread | NO | Main thread only. |
| `SPARK_LOG_*` from a worker | YES | Logger is async + thread-safe. |
| Push/pop a `ThreadSafeQueue` from two threads | YES | That's its job. |
| `future.get()` inside a worker job | NO | Deadlock risk (fixed pool). Join on main. |
| Reorder ECS stages to "optimize" | NO | §0 invariant. |
| Register a system into `StageBasedExecutor` and expect it to tick | PARTIAL | The tick is called, but nothing is registered yet (§1c) — verify end-to-end. |

---

## 6. When NOT to use this skill (use a sibling instead)

- **Measuring/optimizing a hot path, picking what to parallelize for speed** → global
  **perf-hotpath** skill. This skill only covers *correctness* of concurrency.
- **RHI / render-backend threading, GPU command lists** → no dedicated skill in this
  tree; read `SparkEngine/Source/Graphics/RHI/` directly (`RHIFactory`, backend devices).
- **Network protocol, packet handling, AreaServer/WorldServer logic** → no dedicated
  skill in this tree; read `SparkEngine/Source/Engine/Networking/` and
  `wiki/advanced/Threading-Model.md` (this skill only states `NetworkManager`'s
  thread-safety contract).
- **Full threading reference / every primitive in the engine** → the wiki page
  `wiki/advanced/Threading-Model.md` is the exhaustive catalog; this skill is the
  action-oriented subset for adding parallel work safely.

---

## Provenance and maintenance

All facts verified against source on **2026-07-07** (branch `Working`). Re-verify with:

```bash
# JobSystem API surface (Submit / ParallelFor / Initialize / Shutdown)
grep -nE 'Submit|ParallelFor|void (Initialize|Shutdown)|GetWorkerCount|IsInitialized' \
  SparkEngine/Source/Utils/JobSystem.h

# ThreadSafeQueue signatures (TryPop is bool + out-param, not optional)
grep -nE 'TryPush|ForcePush|TryPop|PopAll' SparkEngine/Source/Utils/ThreadSafeQueue.h

# Stage order + StageBasedExecutor / ParallelSystemExecutor API
grep -nE 'enum class SystemStage|RegisterSystem|ExecuteAll|DeclareReads|DeclareWrites|BuildSchedule|void Execute' \
  SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h

# Is StageBasedExecutor still a DEAD TICK? (definition should be the ONLY hit)
grep -rn 'RegisterSystem(' SparkEngine/Source | grep -i stage

# Confirm the live tick site
grep -n 'StageBasedExecutor::GetInstance().ExecuteAll' \
  SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp

# Is ParallelSystemExecutor wired yet? (ConfigureParallelExecution should have NO .cpp caller)
grep -rn 'ConfigureParallelExecution' SparkEngine/Source --include='*.cpp'

# Jolt's own thread pool (separate from Spark::JobSystem)
grep -n 'JobSystemThreadPool' SparkEngine/Source/Physics/PhysicsSystem.cpp

# Where the Spark JobSystem is actually initialized/shut down (do NOT do this yourself)
grep -n 'js.Initialize' SparkEngine/Source/Core/EngineSetup.h
grep -n 'JobSystem::Get().Shutdown' SparkEngine/Source/Core/SparkEngine.cpp
```

Volatile claims to re-check when they may have changed:
- **`StageBasedExecutor` dead-tick status** (§1c) — flips the moment someone adds a
  `RegisterSystem` call. Re-run the grep above.
- **`ParallelSystemExecutor` unwired status** (§1d) — flips when `ConfigureParallelExecution`
  gets a caller.
- **`PhaseSystemManager` serial-only** (§1d note) — re-read the `UpdateAll` comment in
  `PhaseSystemManager.h`; per-phase parallelism was an open item.

Companion reference (exhaustive, not a substitute): `wiki/advanced/Threading-Model.md`,
`wiki/subsystems/Job-System.md`. If this skill and the wiki disagree, trust the **header
source** and update both.
