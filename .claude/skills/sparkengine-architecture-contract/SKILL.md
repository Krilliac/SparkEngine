---
name: sparkengine-architecture-contract
description: >-
  The load-bearing design contract of the SparkEngine C++23 game engine: who
  owns each subsystem (EngineRuntime), how anything gets a subsystem pointer
  (EngineContext service locator), the lifecycle topology (startup, per-frame
  tick, shutdown), and the hard subsystem boundaries (game-module DLL seam, RHI,
  Platform.h). TRIGGER when you are about to add or wire a subsystem, touch
  EngineContext/EngineRuntime, add a g_ global or a GetInstance() singleton,
  decide where a new ECS system registers, cross the game-module DLL boundary,
  or you catch yourself asking "how is this engine supposed to fit together /
  where does X live / why is my subsystem null in a module". DO NOT TRIGGER for
  phase-execution mechanics, threading, or allocator details (use
  sparkengine-ecs-lifecycle-threading-and-memory), for module ABI / SDK export /
  hot-reload mechanics (use sparkengine-modules-sdk-abi-and-hot-reload), for
  build/CMake/CI failures (use sparkengine-build-ci-and-dependencies), or for
  self-contained bug fixes that cross no subsystem boundary.
---

# SparkEngine Architecture Contract

This is the **contract**, not a tutorial. It states the engine's load-bearing
design decisions as **invariants** (rules that must hold), the **WHY** behind
each, and the **failure mode** you cause by breaking it. It complements the
project manifest (`CLAUDE.md` at the repo root — Anti-Bloat table, "Before
Writing Code" checklist, "Wiring Things In" doctrine); it does not restate
those, it documents the *design* those rules protect.

**Definitions (used once here):**
- **Subsystem** — an engine-lifetime service (e.g. `GraphicsEngine`,
  `PhysicsSystem`, `AudioEngine`, `NetworkManager`).
- **Service locator** — the `EngineContext` object every subsystem is fetched
  through instead of a global.
- **Game module** — a genre DLL under `GameModules/*` (SparkGame, SparkGameFPS,
  SparkGameMMO, ...) that links the engine static library and is loaded at
  runtime by the host executable.
- **RHI** — Render Hardware Interface, the backend-agnostic GPU abstraction
  under `SparkEngine/Source/Graphics/RHI/`.
- **ECS** — Entity Component System (EnTT-backed); systems run each frame in a
  fixed phase order.

## When to use / when not to

| Situation | Go to |
|---|---|
| Adding a new subsystem — deciding ownership + access | this skill (Invariant 1) |
| A subsystem pointer is null inside a game-module DLL | this skill (Invariant 2) |
| Deciding *where* a new ECS system registers | this skill (Invariant 3) |
| Tempted to add a `g_*`/file-scope global or a `GetInstance()` singleton | this skill (Invariants 1–2) |
| Unsure whether new code must be wired into startup/loop | this skill (Invariant 4) |
| Phase execution order details, threading, job system, allocators | `sparkengine-ecs-lifecycle-threading-and-memory` |
| Module DLL exports, SDK ABI, hot reload, injection mechanics | `sparkengine-modules-sdk-abi-and-hot-reload` |
| Build/link/CMake/CI is red | `sparkengine-build-ci-and-dependencies` |
| Formatting-only or a localized bug fix crossing no boundary | just fix it; no skill needed |

If a change would violate an invariant below, **stop and reconsider the
design** — do not route around it. If the invariant itself is wrong, that is a
design change: say so explicitly and get sign-off, don't silently break it.

---

## Invariant 1 — Subsystems are owned by `EngineRuntime`, accessed via `EngineContext`. No new `g_*` subsystem globals.

**Rule.** Engine-lifetime ownership lives in one struct, `EngineRuntime`
(`SparkEngine/Source/Core/EngineRuntime.h`), which holds the owning
`std::unique_ptr`s (`graphics`, `input`, `timer`, `eventBus`, `moduleManager`,
`audioEngine`, `audioBackend`, `moduleHotReload`, and `physics` behind
`SPARK_JOLT_PHYSICS_AVAILABLE`). It is populated during startup
(`Core/SparkEngine.cpp` + the platform `SparkEngine{Windows,Linux}*.cpp` entry
files) and reached via `GetEngineRuntime()` — **only** by Core entry-point and
lifecycle files. Everything else fetches *live* subsystem pointers through the
service locator: `EngineContext::Get()->GetX()` (65 named getters, e.g.
`GetGraphics()`, `GetPhysics()`), or the generic `GetSystem<T>()` for types
without a named getter. You may **not** introduce a new file-scope `g_*`
subsystem global — `EngineRuntime`'s own header says it replaced exactly that
pattern.

**Why.** `EngineContext` uses one generic registry (`TypeId -> void*`, guarded
by `std::shared_mutex m_systemsMutex`) as the *single source of truth*; every
named getter delegates to `GetSystem<T>()` (`Core/EngineContext.h`, getters at
lines 169–289). One registry means one place to inject across the DLL boundary
(Invariant 2), one lock-guarded mutation point, and no duplicated pointer state
to desync.

**Failure mode if violated.** A `g_*` global is **per translation-image**: it
is null (or a dead duplicate) inside every game-module DLL, because each DLL
links its own copy of the engine static library. You get a null-deref or, worse,
a *second live instance* of a subsystem that silently diverges from the host's.

**Non-obvious detail — the type-id trick (do not "clean up").**
`GetTypeId<T>()` returns the address of a **non-`const`** function-local
`static char id;` (`Core/EngineContext.h` lines 58–68, with an in-code comment
saying exactly this). It must stay non-`const`: a `static const char id = 0`
compiles to an identical read-only COMDAT for every `T`, which MSVC's
`/OPT:ICF` (on in Release, off in Debug) folds to a **single address** —
collapsing every type id to one key so the locator returns the *wrong subsystem
in Release only*. Writable data is not ICF-folded. Adding `const` back is a
silent Release-only correctness bug.

**How to add a subsystem correctly:**
1. Add the owning `std::unique_ptr<T>` field to `EngineRuntime` (feature-gated
   systems get an `#ifdef` guard like `physics`).
2. Create it on the startup path and register it with dependency-aware
   registration:
   `ctx.RegisterSubsystem<T>(ptr, DependsOn<Dep1, Dep2>{}, initFn, shutdownFn)`
   so `InitializeAll()`/`ShutdownAll()` order it topologically. Follow the
   existing wiring in `Core/EngineSetup.h` (e.g.
   `RegisterSubsystem<Spark::UI::UISystem>(ui, DependsOn<Timer, Spark::EventBus>{})`).
   Plain `RegisterSystem<T>(ptr)` is only for systems with no init/deps.
3. Never store the pointer in a file-scope global. Fetch via
   `EngineContext::Get()->GetSystem<T>()` at the call site.
4. Wire its `Update()` into the real loop in the same change (Invariant 4).

---

## Invariant 2 — Inside a game-module DLL, resolve subsystems through the **injected** EngineContext, never a per-module singleton.

**Rule.** The engine library is linked **statically** into every game-module
DLL, so the context global is per-image and would be null inside a module. The
host injects its live context across the DLL boundary:
`EngineContext::Get()` **prefers the injected pointer** when one is set
(`Core/EngineContext.cpp` lines 29–43: `static EngineContext*
g_injectedContext`; `Get()` returns it first; `SetInjected(ctx)` sets it). The
DLL-side receiving export `SparkModuleInjectEngineContext` is defined in
`SparkSDK/Include/Spark/ModuleDllMain.h` and the host calls it via
`GetProcAddress` in `Core/ModuleManager.cpp` (grep the export name — line
numbers drift; injection now runs only after the mandatory `.sparkabi`
sidecar + in-image compatibility-descriptor gates pass). Module code must call
`EngineContext::Get()` — never a subsystem's own per-module singleton.

**Why.** Without injection, a module either sees `nullptr` or constructs its
own second copy of a subsystem. The injected pointer is **raw and non-owning**
on purpose: module teardown must not free the host's context.

**Failure mode if violated.** Cross-DLL null-deref, or two live copies of a
subsystem (e.g. two `NetworkManager`s) that never see each other's state —
network messages, ECS entities, or config silently vanish across the boundary.

**Boundary rule for `GetInstance()`.** Do not add **new** naive per-module
`static T instance;` singletons — that is the anti-pattern injection removes.
The retrofitted *context-routing* pattern is acceptable:
`NetworkManager::GetInstance()` (`NetworkManager.cpp` lines 130–145) resolves
through `EngineContext::Get()->GetNetworkService()` first and only falls back to
a local static. Known residual cleanups (open, verified 2026-08-23):
- `NetworkManager::GetInstance()`'s `static NetworkManager instance;` fallback
  still exists (context routing is first, but the dead fallback was not deleted).
- `SeamlessAreaManager::GetInstance()` is still called directly from module
  world-setup files (`GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp`,
  `SparkGameRPG/.../RPGWorldSetup.cpp`, `SparkGameOpenWorld/.../OWWorldSetup.cpp`).

For export mechanics, hot reload, ABI stability, and rebuilding stale module
binaries, use **sparkengine-modules-sdk-abi-and-hot-reload** — that skill owns
the seam's mechanics; this one owns only the rule.

---

## Invariant 3 — ECS systems run in a fixed phase order. The ONE canonical manager is `PhaseSystemManager`, created by `CreatePhaseSystemManager` and ticked by the gameplay lifecycle.

**Rule (ordering).** Downstream systems assume upstream ones have already
written this frame. The coarse contract order is:

`Physics -> Animation -> AI -> Audio -> Lifecycle/Gameplay -> Render`

The full `enum class Phase` (`Engine/ECS/Systems/PhaseSystemManager.h` lines
40–54) is a finer-grained superset of the same order:
`PrePhysics -> Physics -> PostPhysics -> Animation -> AI -> Audio -> Gameplay
-> PreRender -> Render -> PostRender`.

**Rule (registration).** **New ECS systems go into
`Spark::EngineSetup::CreatePhaseSystemManager()` (`Core/EngineSetup.h:175`)
with the correct `Phase::` bucket.** That is the single production registration
point (currently 12 `AddSystem` calls: PhysicsUpdate→`Physics`,
SplineFollower→`PostPhysics`, AnimationUpdate→`Animation`, AIUpdate→`AI`,
AudioUpdate→`Audio`, Lifecycle/AbilityUpdate/Projectile→`Gameplay`,
Particle/Decal/Terrain→`PreRender`, Render→`Render`).

**Rule (tick).** `Core/Lifecycle/GameplayLifecycleShared.cpp` owns the manager
(function-local static, line ~748), builds it from `CreatePhaseSystemManager`
during gameplay init (line ~766), pumps `UpdateAll(*world, dt)` every frame
(line ~1159, under the `"ECS_Phases"` guarded-update), and resets it at
shutdown (line ~1236). The regression test
`Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp` fails if the production init
path stops registering (≥5 systems), leaks systems across re-init, or stops
advancing component data. Separately, the platform entry loops drive
module-side systems via `GetEngineRuntime().moduleManager->UpdateAll(dt)` —
module OO logic is not phase-managed.

**Why.** `PhysicsUpdateSystem` writes simulation results into `Transform`;
`RenderSystem` and `AudioUpdateSystem` read it. If Render runs before Physics,
entities render at last frame's positions (one-frame lag / jitter). AI reads
positions produced by physics and spline followers, so it runs after them. The
ordering is the pipeline's correctness guarantee, not a style preference.

**History you must not re-litigate:**
- `StageBasedExecutor` (a formerly live tick over zero registered systems) was
  **deleted** in commit `4b42b8d9` ("wire ECS phase systems"). It no longer
  exists in engine source — do not resurrect it; older docs/wiki mentions are
  historical.
- An earlier path batched systems across *all* phases through one parallel
  executor, silently discarding phase order. `PhaseSystemManager::UpdateAll`
  now runs **serially** by design (its doc comment, `PhaseSystemManager.h`
  lines 113–121, says correct parallelism would need one executor *per phase*,
  which does not exist yet). Do not reintroduce cross-phase parallel batching.
- The flat `SystemManager` (`Engine/ECS/Systems/ECSystems.h:735`) still exists
  as **legacy** with no phase guarantee — do not register new systems there;
  migrating its remaining users onto `PhaseSystemManager` is an open cleanup
  `candidate`.

For per-phase execution mechanics, fixed-timestep details, threading, and
memory, use **sparkengine-ecs-lifecycle-threading-and-memory** — this skill
owns only the topology: which manager, which registration point, which order.

---

## Invariant 4 — Wiring is not optional. A system that exists but is never initialized/called/connected is worse than not existing.

**Rule.** Every `Initialize()` must be called on the startup path; every
`Update()`/`ProcessCommands()` must appear in the main loop; every sink must
have a source. If you build a system, you wire it in **in the same change**, or
you delete it. Wire with a direct call to the real function — no new wrapper
abstraction.

**Why.** Dead-but-present code reads as "done", is compiled, reviewed, and
maintained, but does nothing at runtime — the worst cost/value ratio. This is
stated project doctrine (`CLAUDE.md`, "Wiring Things In"), and the deleted
`StageBasedExecutor` (Invariant 3) is the canonical historical example.

**How to satisfy it.** After adding a system, grep for its
`Initialize`/`Update` call site and confirm it sits on a real path
(`Core/EngineSetup.h`, the platform `Core/SparkEngine*.cpp` entry files, or
`GameplayLifecycleShared.cpp`). Then run `tools/check-wiring.sh` (part of
`tools/validate-all.sh`), which flags systems whose `Initialize()` is never
called.

**Verified fixed instance (2026-08-23): Sequencer audio.** Sequences created by
`SequencerManager::CreateSequence` receive a private production dispatch callback
that copies crossed `AudioCue`s into the manager queue. `GameplayLifecycleShared.cpp`
drains that queue through `DispatchPendingAudioCues()` on the game/update thread,
after the cinematic worker joins. `EngineRuntime` owns `IAudioBackend`; the manager
holds only a non-owning pointer installed by Windows/Linux windowed startup and
explicitly cleared on headless startup and before audio teardown. With no backend,
cues are consumed silently rather than retained for a later surprise replay.
`Sequence::SetAudioCallback` remains an observer hook on the sequencer update thread;
it cannot replace production audio dispatch. Regression coverage is
`Tests/TestSequencerAudioWiring.cpp` (wired 2D/3D playback and thread affinity,
headless/no-service consumption, and seek/replay duplicate policy).

---

## Invariant 5 — Rendering goes through the RHI; there is always a GPU-less fallback, chosen automatically in `RHIFactory`.

**Rule.** All GPU work goes through the RHI abstraction
(`SparkEngine/Source/Graphics/RHI/`). D3D11 is the primary backend; D3D12,
Vulkan, Metal, OpenGL are experimental. Backend selection is centralized in
`RHIFactory.cpp` — do not hard-code a device type at a call site. When no GPU
backend is usable the engine falls back and **keeps running**; it does not
abort.

**Selection facts (verified in `RHI/RHIFactory.cpp`):**
- `Auto` priority: D3D11 on Windows, Metal on Apple, Vulkan on Linux, else the
  first available backend (lines 182–197).
- `SPARK_RHI_BACKEND` env var overrides selection (line 135); the value `none`
  forces `NullRHIDevice` (headless no-op device, `RHI/NullRHIDevice.h`).
- gVisor detection auto-recommends `None` to avoid Wine signal-handler hangs
  (line 166); override with `SPARK_RHI_BACKEND=<backend>`.
- `CreateDevice` with no usable backend returns `NullRHIDevice` (line ~210).
- Software substitutes live inside the backends, e.g. D3D11 falls back to WARP
  (Microsoft's software rasterizer) when hardware device creation fails, and
  skips WARP under Wine (`RHI/D3D11/D3D11Device.cpp` lines ~645–665).

**Why.** SparkEngine must come up on CI runners, headless servers
(AreaServer/WorldServer), and CPU-only/sandboxed hosts. A path that assumes a
real GPU breaks headless tests and dedicated-server builds.

**Failure mode if violated.** Hard-coding `D3D11CreateDevice` (or any concrete
backend) at a call site breaks Linux/macOS builds, headless CI, and dedicated
servers — exactly the scenarios the abstraction exists to serve.

---

## Invariant 6 — Cross-platform types come from `Core/Platform.h`. Do not include DirectX headers unguarded.

**Rule.** Math/vector types and platform stubs come from
`SparkEngine/Source/Core/Platform.h`. On Windows it includes real
`<DirectXMath.h>`; on other platforms it provides stubs via
`PlatformAudioStubs.h`, `PlatformDirectXMathStubs.h`, and `PlatformD3DStubs.h`.
Include platform-specific graphics headers only behind guards — the canonical
Linux CI jobs (GCC/Clang/sanitizers) compile the whole engine without the
Windows SDK.

**Failure mode if violated.** Green on Windows, red on `build-linux-gcc` /
`build-macos` with `fatal error: DirectXMath.h: No such file` — visible only in
CI.

---

## Quick self-check before committing an architecture-touching change

- [ ] No new `g_*` file-scope subsystem global (Invariant 1).
- [ ] New subsystem: owned in `EngineRuntime`, registered via
      `RegisterSubsystem<T>(..., DependsOn<...>{}, ...)`, fetched via
      `EngineContext::Get()` (Invariant 1).
- [ ] Did not add `const` to `GetTypeId<T>()`'s `static char id` (Invariant 1).
- [ ] Module-side access uses `EngineContext::Get()`; no new per-module
      `static T instance;` singleton (Invariant 2).
- [ ] New ECS system registered in `CreatePhaseSystemManager` with the correct
      `Phase::` bucket — not in legacy `SystemManager` (Invariant 3).
- [ ] `Test_lifecycle_ecs_phase_wiring` still passes if you touched lifecycle
      init (Invariant 3).
- [ ] Everything with `Initialize()`/`Update()` is called on the real path —
      grep the call site to prove it; run `tools/check-wiring.sh` (Invariant 4).
- [ ] No concrete GPU device hard-coded outside `RHIFactory` (Invariant 5).
- [ ] No unguarded DirectX/Windows-only include (Invariant 6).

## Open cleanup register (architecture-scoped; verified 2026-08-23)

| Item | Nature | Invariant |
|---|---|---|
| `NetworkManager::GetInstance()` local `static` fallback not deleted (context routing is first) | anti-pattern residue | 2 |
| `SeamlessAreaManager::GetInstance()` called directly in MMO/RPG/OpenWorld `*WorldSetup.cpp` | per-module singleton use | 2 |
| Legacy flat `SystemManager` (`ECSystems.h:735`) still present; migrate remaining users to `PhaseSystemManager` | parallel-system residue, `candidate` | 3 |
| Per-phase parallel executor does not exist; `UpdateAll` is serial by design | `open` (future work, do not hack around) | 3 |

---

## Provenance and maintenance

All claims verified on **2026-08-23** against the working tree of branch
`claude/whole-nine-yards-20260823` (which carries uncommitted changes ahead of
commit `0e1fe7e7`) by reading the cited sources — not by a full build, test
suite, or CI run at this exact tree. Re-verify volatile facts with these
one-liners (run from the repo root):

```bash
# Invariant 1 — ownership struct and accessor
grep -n "unique_ptr\|GetEngineRuntime" SparkEngine/Source/Core/EngineRuntime.h
# Invariant 1 — type-id must stay non-const; registry lock
grep -n "static char id\|shared_mutex" SparkEngine/Source/Core/EngineContext.h
# Invariant 1 — dependency-aware registration in use
grep -n "RegisterSubsystem<" SparkEngine/Source/Core/EngineSetup.h | head
# Invariant 2 — Get() prefers injected pointer; export + host consumption
grep -n "g_injectedContext\|SetInjected" SparkEngine/Source/Core/EngineContext.cpp
grep -n "SparkModuleInjectEngineContext" SparkSDK/Include/Spark/ModuleDllMain.h SparkEngine/Source/Core/ModuleManager.cpp
# Invariant 2 — open singleton residue
grep -rn "SeamlessAreaManager::GetInstance" GameModules | head
# Invariant 3 — canonical manager is registered AND ticked (expect hits for both)
grep -n "CreatePhaseSystemManager" SparkEngine/Source/Core/EngineSetup.h SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp
grep -n "GetPhaseSystemManagerImpl().UpdateAll" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp
# Invariant 3 — StageBasedExecutor stays deleted (expect ZERO hits in Source/)
grep -rn "StageBasedExecutor" SparkEngine/Source GameModules || echo "still deleted — good"
# Invariant 3 — regression gate exists
ls Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp
# Invariant 4 — production Sequencer audio queue, lifecycle drain, startup ownership, tests
grep -rn "SetAudioBackend\|DispatchPendingAudioCues\|SetAudioDispatchCallback" SparkEngine Tests --include='*.cpp' --include='*.h'
grep -n "SequencerAudio_" Tests/TestSequencerAudioWiring.cpp
# Invariant 5 — backend selection + null fallback
grep -n "SPARK_RHI_BACKEND\|NullRHIDevice\|IsRunningUnderGvisor" SparkEngine/Source/Graphics/RHI/RHIFactory.cpp | head
# Invariant 6 — non-Windows stubs
grep -n "PlatformDirectXMathStubs\|DirectXMath.h" SparkEngine/Source/Core/Platform.h
```

If any check disagrees with this document, **the code wins — update this
skill.** Phase-execution mechanics belong in
`sparkengine-ecs-lifecycle-threading-and-memory`; module ABI/injection
mechanics belong in `sparkengine-modules-sdk-abi-and-hot-reload`; keep the
three telling one story.
