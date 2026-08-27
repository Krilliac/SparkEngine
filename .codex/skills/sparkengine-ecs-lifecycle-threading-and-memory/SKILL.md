---
name: sparkengine-ecs-lifecycle-threading-and-memory
description: >-
  SparkEngine runtime-core runbook: ECS/EnTT query semantics, PhaseSystemManager
  phase wiring and the one live tick site, world/entity/component lifecycle,
  JobSystem threading and per-subsystem thread affinity, synchronization rules,
  allocators and memory ownership (EngineRuntime), world-origin rebase callbacks,
  and the physics stepping/lock contract. TRIGGER when: "add a new ECS system",
  "which phase does X run in", "why doesn't my system tick", "iterate entities
  with components", "destroy entity during iteration", "parallelize this loop",
  "is X thread-safe", "who owns this subsystem", "frame allocator", "origin
  rebasing", "who steps physics", "double-tick", "data race in Update".
  DO NOT TRIGGER when: the question is module DLL injection/ABI or hot-reload
  seams (use sparkengine-modules-sdk-abi-and-hot-reload), compiler/CMake flags or
  CI jobs (use sparkengine-build-ci-and-dependencies), incident history and
  regression chronology (use sparkengine-failure-archaeology), or live debugging
  of a crash (use sparkengine-debugging-playbook).
---

# SparkEngine — ECS, lifecycle, threading, and memory

> **Live-tree guard (reviewed 2026-08-24).** SparkEngine's `Working` line is
> changing daily. Re-read root `AGENTS.md`, `CLAUDE.md`, the relevant wiki
> pages, and current `git status`/`git log` before acting. Branch labels, counts,
> line numbers, open-gap/status claims, and binary inventories below are dated
> evidence, not current truth. Preserve unrelated dirty work. During read-only
> orientation, do not build, generate, format, update submodules, or run Git
> mutations. On Nathan's Windows setup, route an authorized MSVC build through
> `C:\Users\Nathan\.claude\scripts\build.ps1` so vcvars and `CC`/`CXX`
> pinning are applied.

This is the single home for: ECS query semantics, `PhaseSystemManager` phase
wiring, world/entity/component lifecycle, jobs and thread affinity,
synchronization, allocators and memory ownership, world-origin callbacks, and
physics-lock/stepping interaction.

**When NOT to use this skill — sibling routing:**

| Your question is about… | Go to |
|---|---|
| Module DLL loading, `SparkModuleInjectEngineContext`, ABI, hot-reload seams | `sparkengine-modules-sdk-abi-and-hot-reload` |
| Compiler flags, CMake presets, CI jobs, sanitizers, dependency setup | `sparkengine-build-ci-and-dependencies` |
| "How did we get here" — incident chronology, past regressions | `sparkengine-failure-archaeology` |
| A live crash/hang you are diagnosing right now | `sparkengine-debugging-playbook` |
| Engine-wide invariants and change-approval rules | `sparkengine-architecture-contract` |
| Test coverage policy, validation scripts, QA gates | `sparkengine-validation-and-qa` |

Jargon used below, defined once:
- **EnTT** — the third-party header-only ECS registry library; `World` wraps one `entt::registry`.
- **Phase** — a named bucket in `Spark::ECS::Phase` (enum) that fixes execution order.
- **Tick / pump** — one per-frame `Update()` call driven by the main loop.
- **Wired** — registered *and* actually called from the production frame path (project doctrine: a built-but-unwired system must be wired in or deleted).

---

## 1. The one live ECS tick path (ground truth, verified 2026-08-23)

`StageBasedExecutor` is **deleted** — zero references remain anywhere under
`SparkEngine/Source`, `SparkEditor/Source`, `GameModules`, `Tests`, `SparkSDK`
(only historical wiki/docs prose mentions it). `PhaseSystemManager` is the live
executor. Any doc claiming the reverse describes the pre-2026-07-18 state.

The production chain, file by file:

```
Platform loop (SparkEngine/Source/Core/SparkEngineWindowsWin32.cpp:186,
               SparkEngineWindowsHeadless.cpp:319, SparkEngineLinuxInit.cpp:161,
               SparkEngineLinuxHeadless.cpp:134)
  → UpdateGameplaySystems(dt)            Core/GameplaySystemLifecycle.cpp:177
  → UpdateGameplaySystemsImpl(dt)        Core/Lifecycle/UpdateStage.cpp:16
  → SPARK_GUARDED_UPDATE("ECS_Phases", "Core",
        { GetPhaseSystemManagerImpl().UpdateAll(*world, dt); })
                                         Core/Lifecycle/GameplayLifecycleShared.cpp:1159
```

- **Registration**: `InitializeEcsPhaseSystemsImpl()`
  (`GameplayLifecycleShared.cpp:~766`) move-assigns a fresh manager from
  `Spark::EngineSetup::CreatePhaseSystemManager(*ctx)`
  (`SparkEngine/Source/Core/EngineSetup.h:175`). It is called **last** in
  `InitializeGameplaySystemsImpl` so physics/audio/graphics context pointers are
  already set. Re-init **replaces** the manager instead of accumulating
  duplicates (duplicates would double-tick component data) — this is tested.
- **The manager instance**: a function-local static inside
  `GetPhaseSystemManagerImpl()` (`GameplayLifecycleShared.cpp:748`), declared in
  `Core/Lifecycle/GameplayLifecycleShared.h:29`.
- **Shutdown ordering**: `GameplayLifecycleShared.cpp:1236` clears the manager
  (`= PhaseSystemManager{}`) **before** platform teardown, because systems hold
  non-owning pointers to physics/audio/graphics.
- **CI-enforced**: `Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp`
  (registered in `Tests/CMakeLists.txt:713`) drives the exact production
  registration entry point and the exact lifecycle-owned manager, and fails if
  the phase systems are ever un-wired again.
  `Tests/harden/Test_tests_ecsystemordering_real.cpp` covers the real class's
  ordering guarantees.

### Phase order and what runs where

`Spark::ECS::Phase` (`SparkEngine/Source/Engine/ECS/Systems/PhaseSystemManager.h`):

```
PrePhysics → Physics → PostPhysics → Animation → AI → Audio → Gameplay → PreRender → Render → PostRender
```

Within a phase, systems run in insertion order. Phased systems run first; the
legacy flat (unphased) list runs after all phases, in insertion order.
`UpdateAll` is **serial** — an earlier parallel path was removed because it
silently discarded phase ordering (documented in the `UpdateAll` doc comment).

Production registration (`EngineSetup.h::CreatePhaseSystemManager`):

| Phase | Systems (insertion order) | Conditional on |
|---|---|---|
| Physics | `PhysicsUpdateSystem` | `ctx.GetPhysics() != nullptr` |
| PostPhysics | `SplineFollowerSystem` | — |
| Animation | `AnimationUpdateSystem` | — |
| AI | `AIUpdateSystem` | — |
| Audio | `AudioUpdateSystem` | `ctx.GetAudio() != nullptr` |
| Gameplay | `LifecycleSystem`, `AbilityUpdateSystem`, `ProjectileSystem` | — |
| PreRender | `ParticleUpdateSystem`, `DecalSystem`, `TerrainSystem` | — |
| Render | `RenderSystem` | `ctx.GetGraphics() != nullptr` |

Known documentation quirk (code wins): the comment above the
`SplineFollowerSystem` registration says "after animation, before AI", but
`Phase::PostPhysics` executes **before** `Phase::Animation` in enum order. The
actual order is Physics → SplineFollower → Animation → AI. Another stale
comment: `ECSystems.h` still says "Bullet Physics" in two forward-declaration
comments — the physics backend is **Jolt** (see `Physics/PhysicsSystem.h`).

Headless/tests: with no physics/audio/graphics in the context, those phase
entries are skipped by design; the ≥5 unconditional core systems must always be
present (asserted by the harden test).

Do **not** add an ad-hoc tick for anything already in the phase pipeline —
e.g. `TerrainSystem` is ticked in `Phase::PreRender`; a second tick in
`GameplayLifecycleShared.cpp` would advance terrain streaming/LOD twice per
frame (there is a comment at the old site saying exactly this).

### Executor inventory — what is live vs. shelf-ware

| Class | File (repo-relative) | Status |
|---|---|---|
| `PhaseSystemManager` | `SparkEngine/Source/Engine/ECS/Systems/PhaseSystemManager.h` | **Live** — the only production ECS executor; serial; tested + CI-enforced |
| `SystemManager` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` | **Legacy** flat insertion-order manager. Implemented; do not register new engine systems here |
| `StageBasedExecutor` | *(deleted)* | Gone from all source; do not resurrect. History: `sparkengine-failure-archaeology` |
| `ParallelSystemExecutor` | `.../Systems/ParallelSystemExecutor.h` | Implemented + unit-tested, **not wired**: `ECSIntegration.h::ConfigureParallelExecution` has zero production callers |
| `FixedTimestepPhysicsSystem` | `.../Systems/FixedTimestepPhysics.h` | Implemented, **not wired** in production (tests only). It *steps* the sim via `StepFixed` — registering it while a module also steps = double-stepping (§4) |
| `CreateGameSystems` / `CreateReactiveSystems` | `SparkEngine/Source/Engine/ECS/ECSIntegration.h` | Convenience factories, **no production callers** (`CreatePhaseSystemManager` in `EngineSetup.h` is the production factory). `candidate` status |
| `ReactiveSystem<T>` | `SparkEngine/Source/Engine/ECS/ReactiveSystem.h` | Base class for EnTT-signal change tracking; use only for rarely-changing components (its header says when not to) |

**Decision rule — adding a new engine ECS system:**
1. Implement `Spark::ECS::ISystem` (`Update`, `GetName`; name must be stable/unique — `GetSystem(name)` does linear string match).
2. Register it in `EngineSetup.h::CreatePhaseSystemManager` in the correct phase. That is the **only** production registration site.
3. Prove it ticks: add/extend a test in `Tests/harden/` that drives `InitializeEcsPhaseSystemsImpl` and asserts observable component change (pattern: `Test_lifecycle_ecs_phase_wiring.cpp`).
4. Zero registered callers, or a green run that never executed your system, counts as **not done**. A check that stops checking is indistinguishable from a check that passes.

---

## 2. World, queries, and entity/component lifecycle

`World` (`SparkEngine/Source/Engine/ECS/Components.h`, global namespace) is a
thin wrapper over one `entt::registry`. `EntityID = entt::entity`
(`Components/CoreComponents.h:21`).

| Operation | API | Contract enforced (SPARK_REQUIRE fires on violation) |
|---|---|---|
| Create | `world.CreateEntity("name")` | non-null id; name → `NameComponent` |
| Destroy | `world.DestroyEntity(id)` | invalid id → warn + no-op; also calls `EntityEventBus::Global().RemoveEntity(id)` before `registry.destroy` |
| Add | `world.AddComponent<T>(id, args...)` | entity valid **and does not already have T** (duplicate add is a contract violation, not an overwrite) |
| Get | `world.GetComponent<T>(id)` → `T*` | entity valid; returns nullptr if component absent (`try_get`) |
| Has / Remove | `HasComponent<T>` / `RemoveComponent<T>` | entity valid; Remove requires the component to exist |
| Query | `world.GetEntitiesWith<A, B>()` → EnTT view | iterate `for (auto e : view)` + `view.get<A>(e)`, or `.each()` |

**Iteration-safety decision rules** (this is why `Spark::DeferredQueue` exists):
- Never call `DestroyEntity` or `RemoveComponent`/`AddComponent` **for a
  component type you are iterating** from inside the view loop. Collect first,
  mutate after.
- Use `Spark::DeferredQueue<entt::entity>`
  (`SparkEngine/Source/Utils/DeferredDeletion.h`): `MarkForDeletion()` during
  iteration, `Flush(callback)` after. `Flush` moves the pending list local
  first, so the callback may safely enqueue new items (processed next flush)
  and exceptions in the callback are caught and logged, not propagated.
  `DeferredQueue` is **not thread-safe** — synchronize externally.
- Production examples: `LifecycleSystem` (death), `DecalSystem` (expiry),
  `ProjectileSystem` (expiry) each keep a persistent member queue to avoid
  per-frame heap allocation.

**Lifecycle events:** `LifecycleSystem` fires its `DeathCallback` once per
entity per `HealthComponent::isDead == true` detection; the callback (single
slot, last-set wins) is responsible for destroying the entity or clearing the
flag, otherwise it fires again next frame. Registration:
`lifecycleSys->SetDeathCallback(...)` via
`GetPhaseSystemManagerImpl().GetSystem("LifecycleSystem")`.

**Hierarchy:** `Transform.parent` (`entt::null` = root) + `children` vector.
Origin rebasing (§5) shifts **root transforms only**; children inherit.

Sibling note: a retired query-patterns skill historically held query
micro-patterns and anti-patterns beyond this section; its wiring claims
("PhaseSystemManager defined-not-wired", "StageBasedExecutor is ticked") were
**stale** and it has been folded into this file — this skill is the sole home
for ECS query semantics now.

---

## 3. Threading: JobSystem, affinity, and synchronization

`Spark::JobSystem` (`SparkEngine/Source/Utils/JobSystem.h`) — fixed-size thread
pool singleton (`JobSystem::Get()`).

- **Initialized in production** at `Core/SparkEngineWindowsInit.cpp:88`,
  `Core/SparkEngineWindowsHeadless.cpp:135`, `Core/SparkEngineLinuxInit.cpp:232`
  via `EngineSetup::InitializeJobSystem(g_maxWorkerThreads)`
  (`g_maxWorkerThreads` parsed from the command line; `0` →
  `hardware_concurrency - 1`). Shutdown at `Core/SparkEngine.cpp:347`.
- API: `Submit(f, args...)` → `std::future`; `ParallelFor(begin, end, body,
  minBatchSize)` (runs inline if uninitialized or the range is small — so code
  using it still works headless); `WaitForAll()` (waits for queue drain **and**
  in-flight jobs — a FIFO barrier job is documented as insufficient).
- Worker exceptions are caught and logged; a throwing fire-and-forget job does
  **not** kill the worker; a `Submit`-with-future job stores the exception in
  the future.

**Thread-affinity table** (verified against source doc comments):

| Component | Affinity / safety |
|---|---|
| EnTT registry / `World` | **Main thread only.** Never read, iterate, or mutate from a worker. Pattern: gather snapshots on main → process on workers → write back on main (see `Engine/AI/ParallelPerception.h`, wired 2026-04-14) |
| `PhaseSystemManager` / `SystemManager` | `AddSystem` and `UpdateAll` same-thread; systems may launch background tasks but must not call manager methods from them |
| `ParallelSystemExecutor` | Executor object main-thread only; the systems it dispatches run on workers (header `@threadsafety`) — but it is not wired (§1) |
| `PhysicsSystem` | **Public API is main-thread only** (`Physics/PhysicsSystem.h:98`: "All physics simulation, raycasting, and collision shape operations must be called from the main thread"). Jolt parallelizes *internally* during the step. Internal mutexes exist (`m_contactMutex` for Jolt contact callbacks which arrive on Jolt worker threads, `m_surfaceVelocityMutex`, `m_metricsMutex`) — they protect those specific structures, they do **not** make the API thread-safe |
| `SimpleConsole` | Thread-safe (mutex) |
| `NetworkManager` | Queue mutex for message I/O + handler registration |
| `GraphicsEngine` | Main-thread render; `std::atomic` frame state |
| `SceneRenderer::Submit` | Main thread only (single writer; header note) |
| `JobSystem` | Thread-safe (that is its job) |
| `SubsystemFaultIsolator` | Thread-safe (mutex + atomic fast path) |
| `DeferredQueue`, `FrameAllocator` | Not thread-safe |
| `LockFreeRingAllocator` | Single-writer / multi-reader by design |

**Fault isolation:** every lifecycle tick site is wrapped in
`SPARK_GUARDED_UPDATE(name, category, {...})` (`Core/FaultIsolation.h`) —
try/catch + fault counting; a repeatedly-throwing subsystem is auto-disabled
and auto-recovers after a cooldown (`SubsystemFaultIsolator::Update` is pumped
in `UpdateGameplaySystemsImpl`). Consequence for you: **a system that throws
every frame goes silent instead of crashing** — check fault-isolator state
before concluding a system "isn't wired".

---

## 4. Physics stepping contract and lock interaction

From `SparkEngine/Source/Physics/PhysicsSystem.h` (class doc, "Stepping
contract"):

- Physics stepping is **module-driven**. The engine core never advances the
  simulation. Exactly ONE owner per process may advance it per frame, via
  `Update(deltaTime)` OR `StepFixed(stepCount, alpha)` — never both.
- Verified actual steppers today:
  `GameModules/SparkGameFPS/Source/Game/Game.cpp:496` (`physics->Update(dt)`)
  and `GameModules/SparkGameMMOFPS/Source/Net/TFServerSim.cpp:100`
  (`physics->StepFixed(1, 1.0f)`, server sim). The header's "sole caller is
  SparkGameFPS" note is dated 2026-07 and predates the MMOFPS server sim.
- `PhysicsUpdateSystem` (the `Phase::Physics` ECS system) does **NOT** step the
  simulation. Verified in `ECSystems.cpp:92–201`: it only (a) auto-creates
  Jolt bodies for `Transform`+`RigidBodyComponent` entities, (b) reads Dynamic
  body pose/velocity back into ECS, (c) pushes ECS transforms into Kinematic
  bodies. Its header declares accumulator/interpolation fields
  (`m_accumulator`, `m_interpolationAlpha`) but the implementation never uses
  them — `GetInterpolationAlpha()` always returns 0. Label: `open` doc/code
  drift; do not rely on that alpha.
- Deterministic fixed ticks: the sole owner should drive
  `Spark::FixedTimestepAccumulator` (`Core/FixedTimestepAccumulator.h`) +
  `StepFixed`, per the header's code sample.
- **Failure mode — double-stepping**: registering `FixedTimestepPhysicsSystem`
  (which calls `StepFixed` itself) into the phase manager while a module also
  steps runs the sim at 2× speed and breaks determinism.
- **Authority rule** (`AIUpdateSystem` doc, `ECSystems.h`): a Dynamic body is
  authority-owned by physics — writing its `Transform` directly fights the
  solver (read-back overwrites it next Physics phase). Drive Dynamic agents
  through forces; Kinematic/bodyless entities move via Transform writes.
- AI double-tick guard: `Engine/AI/AIIntegration.h` gates the heavy
  `Spark::AI::AISystem::Update` behind `runCoreAISystem` (default **false**) so
  it does not double-tick behavior trees alongside the phase-registered
  `AIUpdateSystem`.

---

## 5. World-origin rebasing and its callbacks

`Spark::World::WorldOriginSystem`
(`SparkEngine/Source/Engine/World/WorldOriginSystem.h/.cpp`) — floating-origin
rebasing for large worlds. Not a phase system; owners pump
`Update(registry, referencePos)` themselves.

Semantics (verified in the .cpp):
- When `|referencePos| ≥ threshold` (default 5000): shifts **root** `Transform`
  positions by `-offset`, accumulates the offset, then fires every callback
  registered via `RegisterRebaseCallback` (each wrapped in
  `SPARK_GUARDED_UPDATE("WorldOrigin:Callback", ...)`).
  `LocalToAbsolute`/`AbsoluteToLocal` convert using the accumulated offset.
  Non-finite reference positions are rejected with an error log.
- **Who actually pumps it** (verified): only
  `GameModules/SparkGameMMOFPS/Source/World/TFWorldSetupUpdate.cpp:107`
  (`m_origin->Update(world->GetRegistry(), pawnPos)`, client-side, threshold
  from `kOriginRebaseThreshold`). RPG (threshold 3000), OpenWorld (4000), and
  MMO (5000) world setups **configure** their `WorldOriginSystem` members but
  never call `Update`/`ForceRebase` — configured-but-not-pumped. Status: `open`
  (violates the wiring doctrine; either pump or delete when touching those
  modules).
- **Open hazard — no production rebase callbacks exist.**
  `RegisterRebaseCallback` has zero callers outside `Tests/` (verified). A
  rebase therefore shifts ECS transforms but NOT Jolt body positions, audio
  sources, or particles; on the next Physics phase, Dynamic-body read-back
  snaps those entities back to un-rebased physics positions. If you enable
  rebasing in a world with Dynamic bodies, you must register a callback that
  shifts physics bodies (main thread — `PhysicsSystem` API affinity, §3), or
  restrict rebasing to worlds where physics state is server-owned (the MMOFPS
  client case). Tested at unit level (`Tests/TestWorldOriginSystem.cpp`);
  end-to-end physics interaction is **not** tested anywhere. Do not present
  rebasing as release-ready for physics-heavy worlds.

---

## 6. Memory ownership and allocators

Ownership doctrine (project-wide, enforced in review): `std::unique_ptr` owns,
raw pointers are non-owning, no naked `new`/`delete`.

- **Engine-lifetime ownership** lives in the `EngineRuntime` struct
  (`SparkEngine/Source/Core/EngineRuntime.h`) — unique_ptrs for graphics,
  input, timer, event bus, module manager, audio (+backend), hot-reload
  manager, and physics (under `SPARK_JOLT_PHYSICS_AVAILABLE`). It replaced
  per-subsystem file-scope globals; **do not introduce new `g_*` subsystem
  globals**. Access live pointers through `EngineContext::Get()->GetX()`, never
  the struct fields.
- **System ownership**: `PhaseSystemManager`/`SystemManager` exclusively own
  registered systems via `unique_ptr`; `AddSystem<T>` returns a non-owning
  raw pointer. Systems hold non-owning pointers to subsystems (hence the
  shutdown ordering in §1).
- **`Spark::FrameAllocator`** (`Utils/FrameAllocator.h`) — per-frame linear
  bump allocator; `Alloc` during the frame, `Reset()` at end of frame; not
  thread-safe. Production consumer: `Graphics/SceneRenderer.h:187` (4 MB draw
  list). Unit-tested (`Tests/TestFrameAllocator.cpp`).
- **`Spark::LockFreeRingAllocator<CapacityBytes>`**
  (`Utils/LockFreeRingAllocator.h`) — power-of-2 ring, size-prefixed blocks,
  single-writer/multi-reader atomics. **No engine consumer today** — only
  tests reference it. Status: `candidate` (wire it or expect it to be
  challenged under the anti-bloat rules).
- **`Spark::DeferredQueue<T>`** — see §2; the sanctioned replacement for
  scattered `m_pendingDeletion` vectors.
- Graphics-side transient memory (`Graphics/RHI/TransientBufferAllocator.h`,
  constant-buffer rings) belongs to rendering work; it is listed here only so
  you don't reinvent it.

---

## 7. Failure modes checklist

| Symptom | Likely cause | Check |
|---|---|---|
| New system never runs | Registered in the wrong place (not `CreatePhaseSystemManager`), or only in the unwired `CreateGameSystems` | `grep -n "YourSystem" SparkEngine/Source/Core/EngineSetup.h` |
| System ran, then went silent | Fault isolator auto-disabled it after repeated throws | logs for `SPARK_GUARDED_UPDATE` fault reports; `SubsystemFaultIsolator` |
| Component data advances twice per frame | Duplicate registration (re-init accumulation) or an ad-hoc tick beside the phase tick | harden test `LifecycleEcsPhases_ReinitReplacesInsteadOfAccumulating`; grep for second `Update` call site |
| Physics runs 2× speed / nondeterministic | Two steppers in one process (module + `FixedTimestepPhysicsSystem`, or two modules) | grep `->Update(dt)` / `StepFixed` across `GameModules` |
| Entity snaps back after you moved it | You wrote `Transform` on a Dynamic-body entity; physics read-back overwrote it | `RigidBodyComponent::Type` — use forces or Kinematic |
| Crash/corruption during entity iteration | Structural mutation inside a view loop | use `DeferredQueue`, mutate after the loop |
| Assert on `AddComponent` | Component already present (duplicate add is a contract violation) | `HasComponent<T>` first, or fix the double-add |
| Data race in a "parallelized" system | Worker touched the EnTT registry or `PhysicsSystem` API | gather/writeback pattern; affinity table §3 |
| World jitter far from origin | Rebasing configured but never pumped (RPG/OpenWorld/MMO), or pumped without physics callbacks | §5 |
| `WaitForAll` returned but work seems unfinished | You waited on a barrier job instead of `JobSystem::WaitForAll()` | use `WaitForAll` — it counts in-flight jobs |

Status ladder used above — **implemented** (code exists), **tested** (unit
tests in `Tests/`), **CI-enforced** (a harden/regression test fails the build
if reverted), **release-ready** (all of the above + no `open` hazards). E.g.
phase wiring is CI-enforced; origin rebasing is implemented + unit-tested but
not release-ready for physics worlds; `ParallelSystemExecutor` and
`LockFreeRingAllocator` are implemented + tested but unwired (`candidate`).

---

## Provenance and maintenance

Authored 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (post-`0e1fe7e7`). Every claim above was
verified by reading the named files on that date. Volatile facts most likely to
drift: the phase registration list in `EngineSetup.h`, the set of physics
steppers, and the (currently absent) production `RegisterRebaseCallback`
callers.

Re-verify from the repo root (bash):

```bash
# StageBasedExecutor stays deleted (expect 0)
grep -rln "StageBasedExecutor" SparkEngine/Source SparkEditor/Source GameModules Tests SparkSDK | wc -l
# The one live tick site (expect 1 hit at GameplayLifecycleShared.cpp)
grep -rn 'SPARK_GUARDED_UPDATE("ECS_Phases"' SparkEngine/Source
# Production registration factory + per-phase list
grep -n "AddSystem<" SparkEngine/Source/Core/EngineSetup.h
# Lifecycle wiring + regression gate registered in the test target
grep -n "CreatePhaseSystemManager\|GetPhaseSystemManagerImpl" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp
grep -n "Test_lifecycle_ecs_phase_wiring" Tests/CMakeLists.txt
# Physics steppers (expect FPS Game.cpp + MMOFPS TFServerSim.cpp only)
grep -rn "physics->Update(\|->StepFixed(" GameModules SparkEngine/Source/Core
# Origin rebasing: pumps and callbacks (currently: 1 pump in MMOFPS, 0 prod callbacks)
grep -rn "m_origin->Update\|RegisterRebaseCallback" GameModules SparkEngine/Source
# JobSystem init sites
grep -rn "InitializeJobSystem" SparkEngine/Source/Core
# Run the regression tests (build first per sparkengine-build-ci-and-dependencies;
# use your actual build dir — build/<preset> for preset configures, build/ for raw -B build)
ctest --test-dir build/windows-release -C Release -R SparkEngineTests --output-on-failure
```

If any command's output contradicts this file, the repo wins — update this
skill, and log incident history in `sparkengine-failure-archaeology`.
