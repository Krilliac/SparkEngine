---
name: sparkengine-architecture-contract
description: >-
  The load-bearing design decisions of the SparkEngine C++23 game engine: the
  invariants that MUST hold, WHY each exists, what breaks if you violate it, and
  the OPEN known-weak-points. TRIGGER when you are about to add or wire a
  subsystem, touch the service locator / EngineContext, change ECS system
  ordering or the system-manager plumbing, add a global, change RHI backend
  selection, cross the DLL/game-module boundary, or you catch yourself asking
  "how is this engine supposed to fit together / where does X live / can I add a
  g_ global / why is my subsystem null in a module". DO NOT TRIGGER for pure
  build/CMake/CI failures (use the global cmake-msvc / ci-troubleshoot skills),
  pure formatting, or a self-contained bug fix that touches no cross-subsystem
  boundary and adds no new system.
trilobite_compatible: true
trilobite_role: architecture
---

# SparkEngine Architecture Contract

This is the **contract**, not a tutorial. It states the engine's load-bearing
design decisions as **invariants** (rules that must hold), the **WHY** behind
each, and the **failure mode** you cause by breaking it. It assumes you have
already read `D:\SparkEngine\CLAUDE.md` (the project manifest with the Anti-Bloat
table and "Before Writing Code" checklist). This skill does not restate those;
it documents the *design* those rules protect.

**Definitions used once here:**
- **Subsystem** — an engine-lifetime service (e.g. `GraphicsEngine`,
  `PhysicsSystem`, `AudioEngine`, `NetworkManager`).
- **Service locator** — the `EngineContext` object every subsystem is fetched
  through instead of a global.
- **Game module** — a genre DLL under `GameModules/*` (SparkGame, SparkGameFPS,
  SparkGameMMO, ...) that links `SparkEngineLib` statically and is loaded at
  runtime by the host.
- **RHI** — Render Hardware Interface, the backend-agnostic GPU abstraction.
- **ECS** — Entity Component System (EnTT-backed); systems run each frame in a
  fixed phase order.

## When to use / when not to

| Use this skill when... | Use instead... |
|---|---|
| Adding a new subsystem and deciding how it is owned/accessed | this skill |
| A subsystem pointer is null inside a game-module DLL | this skill (Invariant 2) |
| Changing ECS system order, or picking a system-manager class | this skill (Invariant 3) |
| Tempted to add a `g_`/file-scope global for a subsystem | this skill (Invariant 1) |
| Adding code and unsure if it must be wired into startup/loop | this skill (Invariant 4) |
| Build/link/CMake/CI is red | global `cmake-msvc` (toolchain) / `ci-troubleshoot` (CI) |
| Formatting-only or a localized bug fix crossing no boundary | just fix it; no skill needed |
| You need the runtime module/DLL loading mechanics in depth | `sparkengine-plugin-abi` |

If a change would violate one of the invariants below, **stop and reconsider the
design** — do not route around it. If the invariant itself is wrong, that is a
design change: say so explicitly and get sign-off, don't silently break it.

---

## Invariant 1 — Subsystems are owned by `EngineRuntime`, accessed via `EngineContext`. No new `g_*` subsystem globals.

**Rule.** Engine-lifetime ownership lives in one struct, `EngineRuntime`
(`SparkEngine/Source/Core/EngineRuntime.h`), which holds the owning
`std::unique_ptr`s. Access to a *live* subsystem is always through the service
locator: `EngineContext::Get()->GetX()` (e.g. `GetPhysics()`, `GetAudio()`), or
the generic `GetSystem<T>()` for subsystems without a named getter. You may
**not** introduce a new file-scope `g_*` subsystem global.

**Why.** `EngineContext` uses a single generic registry (`TypeId -> void*`,
guarded by a `shared_mutex`) as the *single source of truth*; the ~50 named
getters (`GetGraphics()`, etc.) all delegate to `GetSystem<T>()`
(`Core/EngineContext.h`, lines 169-372). One registry means:
- One place to inject across the DLL boundary (Invariant 2).
- One place that is lock-guarded for runtime hot-reload registration.
- No duplicated/desynced pointer state to keep coherent.

A parallel `g_thing` global reintroduces exactly the desync + per-image-copy
problems the registry was built to kill.

**Failure mode if violated.** A `g_*` global is **per translation-image**: it is
null (or a dead duplicate) inside every game-module DLL, because the DLL links
its own copy of `SparkEngineLib`. You get a null-deref or, worse, a *second live
instance* of a subsystem that silently diverges from the host's. This is the
same class of bug Invariant 2 exists to prevent.

**Non-obvious detail — the type-id trick (do not "clean up").**
`GetTypeId<T>()` returns the address of a **non-`const`** function-local
`static char id;` (`Core/EngineContext.h`, lines 58-68). It must stay
non-`const`. A `static const char id = 0` compiles to an identical read-only
COMDAT for every `T`, which MSVC's `/OPT:ICF` (on in Release, off in Debug)
folds to a **single address** — collapsing every type id to the same key so the
locator returns the *wrong subsystem in Release only*. Writable data is not
ICF-folded. Adding `const` back is a silent Release-only correctness bug.
(Cross-ref: user memory `opt-icf-typeid-gate.md`.)

**How to add a subsystem correctly:**
1. Add the owning `std::unique_ptr<T>` field to `EngineRuntime` (or, for
   optional/feature-gated systems, guard it like `physics` is behind
   `SPARK_JOLT_PHYSICS_AVAILABLE`).
2. Create it during startup (`SparkEngine.cpp` /
   `SparkEngine{Windows,Linux}.cpp`) and register the raw pointer:
   `ctx.RegisterSystem<T>(ptr)` or a named `SetX()`.
3. If it has init/shutdown or dependencies, prefer
   `RegisterSubsystem<T>(ptr, DependsOn<Dep1, Dep2>{}, initFn, shutdownFn)` so
   `InitializeAll()`/`ShutdownAll()` order it topologically (see `EngineSetup.h`
   for the existing dependency wiring, e.g. UISystem/DialogueSystem
   `DependsOn<Timer, EventBus>`).
4. Never store the returned pointer in a file-scope global. Fetch via
   `EngineContext::Get()->GetSystem<T>()` at the call site.

---

## Invariant 2 — Inside a game-module DLL, resolve subsystems through the **injected** EngineContext, never a per-module singleton.

**Rule.** `SparkEngineLib` is a **static** library linked into every game-module
DLL, so the owning `g_engineContext` global is per-image and is null inside a
module. The host injects its live context across the DLL boundary via
`EngineContext::SetInjected(ctx)`, and `EngineContext::Get()` **prefers the
injected pointer** when one is set (`Core/EngineContext.cpp`: `if
(g_injectedContext) return g_injectedContext;`). Module code must call
`EngineContext::Get()` — never a subsystem's own `GetInstance()`/per-module
singleton fallback.

**Why.** Without injection, a module either sees `nullptr` or constructs its own
second copy of a subsystem. Injection passes a **raw, non-owning** pointer on
purpose (`SetInjected` doc, `EngineContext.h` lines 130-143): module teardown
must not free the host's context.

**Failure mode if violated.** Cross-DLL null-deref, or two live copies of a
subsystem (e.g. two `NetworkManager`s) that never see each other's state —
network messages, ECS entities, or config silently vanish across the boundary.

**STATUS (as of 2026-07-07): injection is WIRED IN SOURCE — rebuild stale
module DLLs to get it into the binaries.** The DLL-side export **has landed**:
`extern "C" __declspec(dllexport) void SparkModuleInjectEngineContext(void* ctx)`
that calls `EngineContext::SetInjected(...)` is defined at
`SparkSDK/Include/Spark/ModuleDllMain.h:88`, and the host consumes it at
`SparkEngine/Source/Core/ModuleManager.cpp:193` via
`GetProcAddress(..., "SparkModuleInjectEngineContext")`. This landed in commit
`52636dda` (an ancestor of the current tree). `SetInjected` + `Get()` preference
exist on the host side. So in **source**, injection is functionally wired
end-to-end. The canonical status write-up lives in
`sparkengine-module-injection-p0-campaign`; consult it before relying on
injection.

**Remaining genuine gaps (real, keep in mind):**
- **Stale binaries.** The module DLLs built on disk predate `52636dda`, so they
  **lack the export until rebuilt**. This is a binary gap, not a source gap —
  rebuild the modules (see the campaign skill) and verify with `dumpbin /exports`.
- **`NetworkManager::GetInstance()` static fallback not deleted.** It now routes
  through the injected context first (`NetworkManager.h:342` / `.cpp:130`), but
  the dead `static NetworkManager instance;` fallback was not removed.
- **`MMOWorldSetup.cpp` still uses `SeamlessAreaManager::GetInstance()`** — a
  streaming singleton, out of the injection-P0 scope. (The MMO DI fix for
  `MMOPlayerSystem` **has** landed — it resolves via `m_context->GetNetwork()`.)

Do not add **new** naive per-module `GetInstance()` singletons — those are the
anti-pattern the injection seam removes. The retrofitted context-routing
`GetInstance()` (which returns the injected host instance) and genuinely-shared
singletons are acceptable; see Invariant 2's cross-references and the plugin-abi
skill for the exact rule.

---

## Invariant 3 — ECS systems run in a fixed phase order. There is ONE canonical system-manager, and it is `PhaseSystemManager`.

**Rule (ordering).** The per-frame ECS pipeline runs in this order; downstream
systems assume upstream ones have already written this frame:

`Physics -> Animation -> AI -> Audio -> Lifecycle -> Render`

The full phase enum (`PhaseSystemManager.h`, `enum class Phase`) is finer-grained
but a superset of the same order:
`PrePhysics -> Physics -> PostPhysics -> Animation -> AI -> Audio -> Gameplay
(Lifecycle) -> PreRender -> Render -> PostRender`.

**Why.** `PhysicsUpdateSystem` writes simulation results back into the
`Transform` component; `RenderSystem` and `AudioUpdateSystem` *read* Transform.
If Render runs before Physics, entities render at last frame's positions (visible
jitter / one-frame lag). AI reads positions produced by Physics + spline
followers, so it runs after them. This ordering is the pipeline's core
correctness guarantee, not a style preference.

**Failure mode if violated.** One-frame lag, physics/render desync, AI reacting
to stale positions, audio emitters attached to not-yet-updated transforms.

**Rule (which manager) — RESOLVED 2026-07-18: `PhaseSystemManager` is the wired
canonical manager.** `GameplayLifecycleShared.cpp` creates it via
`EngineSetup.h::CreatePhaseSystemManager()` during gameplay init and pumps
`UpdateAll(*world, dt)` every frame in `Phase::` enum order
(Physics→Animation→AI→Audio→Gameplay→PreRender→Render). **New ECS systems go
into `CreatePhaseSystemManager` with the correct `Phase::` bucket.** The
regression test `Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp` fails if the
production init path stops registering/ticking phase systems. The shipping Core
loop still separately drives module systems via `moduleManager->UpdateAll(dt)` —
module OO behavior trees are unaffected. For query/registration idioms see
**sparkengine-ecs-query-patterns**. Keep those skills telling the same story.

**Executor consolidation (was OPEN, resolved 2026-07-18).** Previously three
classes did related jobs; `StageBasedExecutor` (a live tick over zero registered
systems) was **deleted** in the same change that wired `PhaseSystemManager`:

| Class | File | Status |
|---|---|---|
| `PhaseSystemManager` | `ECS/Systems/PhaseSystemManager.h` | **Canonical, wired** — created and ticked by `GameplayLifecycleShared.cpp`. Serial, phase-ordered. |
| `SystemManager` | `ECS/Systems/ECSystems.h` | Flat insertion-order manager; **legacy**, no phase guarantee — do not add new systems here. |
| `StageBasedExecutor` | *(deleted 2026-07-18)* | Was a dead tick (no `RegisterSystem` callers); removed when the phase pipeline was wired. |

Residual cleanup candidate: migrate any remaining `SystemManager` users onto
`PhaseSystemManager`. Until then:
- **Do not assume any of the three runs your system in the shipping loop.**
  `PhaseSystemManager` is not ticked at all; `StageBasedExecutor`'s tick is live
  but has zero registered systems. If you must add a system, verify the tick path
  end-to-end and raise the wiring gap — this is an unfinished architectural home.
- Note the parallel-execution history: an earlier path routed every phase through
  a single global `ParallelSystemExecutor` that batched by component read/write
  sets *across all phases*, silently discarding phase order (Render could run
  before Physics). It was removed; `PhaseSystemManager::UpdateAll` now runs
  **serially** (see its doc comment, `PhaseSystemManager.h` lines 116-121).
  Correct per-phase parallelism would need one executor **per phase** — that does
  not exist yet. Do not reintroduce cross-phase parallel batching.

---

## Invariant 4 — Wiring is not optional. A system that exists but is never initialized/called/connected is worse than not existing.

**Rule.** Every `Initialize()` must be called on the startup path; every
`Update()` / `ProcessCommands()` must appear in the main loop; every sink must
have a source. If you build a system, you wire it in **in the same change**, or
you delete it.

**Why.** Dead-but-present code is a trap: it reads as "done," is maintained,
compiled, and reviewed, but does nothing at runtime — the worst ratio of cost to
value. This is a stated project doctrine (CLAUDE.md "Wiring Things In"), and the
codebase has live examples of the failure it prevents.

**Failure mode if violated.** Silent no-ops that look implemented. Concrete
current instances (from `HARDEN_FLEET_HANDOFF.md`, OPEN as of 2026-07-07):
- `StageBasedExecutor` has a **dead tick** — never fed systems (Invariant 3).
- `SequencerManager` audio cues **never fire** — no `AudioCallback` wired.
- `BTNode::Clone()` deep-copy missing — cloned behavior trees have no root
  (silent no-op).
- `WeaponFireEvent.ownerEntity` is always `0` — owner never threaded through.
- 16 orphan `Test*.cpp` files are **never compiled** (no build registration).

**How to satisfy it.** After adding a system, grep for its `Initialize`/`Update`
call site and confirm it is on the real path (`EngineSetup.h`, the platform
`SparkEngine*.cpp` entry files, or the game loop). Wire with a **direct call** to
the real function — do not wrap it in a new abstraction layer. `tools/check-wiring.sh`
exists to catch systems whose `Initialize()` is never called.

---

## Invariant 5 — Rendering goes through the RHI; there is always a GPU-less fallback, and it is chosen automatically.

**Rule.** All GPU work goes through the RHI abstraction
(`SparkEngine/Source/Graphics/RHI/`). D3D11 is the primary backend; D3D12,
Vulkan, Metal, and OpenGL are experimental. When no GPU backend is usable the
engine falls back to a software/headless device and **keeps running** — it does
not abort. Backend selection is centralized in `RHIFactory`
(`RHI/RHIFactory.cpp`); do not hard-code a device type at a call site.

**Why.** SparkEngine must come up on CI runners, headless servers (AreaServer /
WorldServer), and CPU-only / sandboxed hosts. A rendering path that assumes a
real GPU would break headless tests and dedicated-server builds.

**Fallback tiers (verify in `RHIFactory.cpp`):**
- Priority when `Auto`: D3D11 on Windows, Metal on Apple, Vulkan on Linux,
  OpenGL as last GPU option.
- Software substitutes: WARP (D3D11/D3D12), Lavapipe (Vulkan), llvmpipe
  (OpenGL).
- `NullRHIDevice` (`RHI/NullRHIDevice.h`) — full headless no-op device when no
  backend is available.
- Override with the `SPARK_RHI_BACKEND` env var (`none` forces
  `NullRHIDevice`); `RHIFactory` also auto-recommends `None` under gVisor/CPU-only
  detection to avoid Wine signal-handler hangs.

**Failure mode if violated.** Hard-coding `D3D11CreateDevice` (or any concrete
backend) at a call site breaks Linux/macOS builds, headless CI, and dedicated
servers — the exact scenarios the abstraction and `NullRHIDevice` exist to serve.

---

## Invariant 6 — Cross-platform types come from `Core/Platform.h`. Do not include DirectX headers unguarded.

**Rule.** Math/vector types and platform stubs are provided by
`SparkEngine/Source/Core/Platform.h` (DirectXMath is stubbed on Linux). Include
platform-specific graphics headers only behind the appropriate guards; the
canonical Linux CI jobs (GCC/Clang) compile the whole engine without the Windows
SDK.

**Why.** Linux/macOS are supported build targets (there is a `build-macos` CI
job and Linux GCC/Clang/ASan/TSan/MSan jobs). An unguarded `#include
<DirectXMath.h>` or `<d3d11.h>` breaks every non-Windows job. Recent commits
(`2983dd34`, `710f797e`) were fixes for exactly this class of leak.

**Failure mode if violated.** Green on your Windows box, red on
`build-linux-gcc` / `build-macos` — a `fatal error: DirectXMath.h: No such file`
that you will only see in CI.

---

## Quick self-check before you commit an architecture-touching change

- [ ] No new `g_*` file-scope subsystem global (Invariant 1).
- [ ] New subsystem is owned in `EngineRuntime`, registered in `EngineContext`,
      fetched via `Get()->GetSystem<T>()` (Invariant 1).
- [ ] Did not add `const` to `GetTypeId<T>()`'s `static char id` (Invariant 1).
- [ ] Module-side access uses `EngineContext::Get()`, not a new `GetInstance()`
      fallback (Invariant 2).
- [ ] New ECS system: you verified the tick path end-to-end (no manager is
      cleanly wired — `PhaseSystemManager` is not ticked, `StageBasedExecutor`'s
      tick is live but empty) and flagged the wiring gap (Invariant 3).
- [ ] Anything with `Initialize()`/`Update()` is actually called on the real
      path — grep the call site to prove it (Invariant 4).
- [ ] No concrete GPU device hard-coded outside `RHIFactory` (Invariant 5).
- [ ] No unguarded DirectX/Windows-only include (Invariant 6).

---

## OPEN known-weak-points register (candidate cleanups, not fixed)

These are architecture-shaped debts recorded in
`D:\SparkEngine\HARDEN_FLEET_HANDOFF.md` as of 2026-07-07. Stated as **OPEN** —
do not assume any are resolved without re-checking.

| Item | Nature | Invariant touched |
|---|---|---|
| Module EngineContext injection: source LANDED (`52636dda`), but built module DLLs are STALE (predate the export) | rebuild binaries | 2 |
| `NetworkManager::GetInstance()` static fallback not deleted (now context-routed but the dead `static` remains); `MMOWorldSetup.cpp` still uses `SeamlessAreaManager::GetInstance()` (streaming, out of P0 scope) | anti-pattern cleanup | 2 |
| `SystemManager`/`PhaseSystemManager`/`StageBasedExecutor` triple | parallel systems, contradicts anti-bloat rule | 3 |
| `StageBasedExecutor` dead tick (ticked every frame, zero registered systems); `PhaseSystemManager` defined-not-wired (zero callers) | built-not-wired | 3, 4 |
| Sequencer audio callback never dispatched | built-not-wired | 4 |
| `BTNode::Clone()` deep-copy missing | silent no-op | 4 |
| 16 orphan `Test*.cpp` never compiled | built-not-wired | 4 |

---

## Provenance and maintenance

All claims below were verified by reading the repo at commit `710f797e` on
2026-07-07. Re-verify volatile facts with these one-liners (run from
`D:\SparkEngine`):

```bash
# Invariant 1 — registry is the single source of truth; getters delegate
grep -n "GetSystem<" SparkEngine/Source/Core/EngineContext.h | head
# Invariant 1 — type-id must stay non-const (do not add const)
grep -n "static char id" SparkEngine/Source/Core/EngineContext.h
# Invariant 2 — Get() prefers injected pointer; DLL-side export has LANDED
grep -n "g_injectedContext\|SetInjected" SparkEngine/Source/Core/EngineContext.cpp
grep -n "SparkModuleInjectEngineContext" SparkSDK/Include/Spark/ModuleDllMain.h   # a HIT now = LANDED (was OPEN)
# Invariant 3 — intended manager exists but is NOT wired (expect ZERO callers)
grep -n "class PhaseSystemManager\|enum class Phase" SparkEngine/Source/Engine/ECS/Systems/PhaseSystemManager.h
grep -rn "CreatePhaseSystemManager\|->UpdateAll" SparkEngine/Source --include='*.cpp'   # PhaseSystemManager: expect no live callers
# Invariant 3 — the tick that actually runs (StageBasedExecutor) vs its (absent) registration
grep -n "StageBasedExecutor::GetInstance().ExecuteAll" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp
grep -rn "RegisterSystem(" SparkEngine/Source | grep -i stage   # expect only the definition, no callers = dead tick
grep -n "class SystemManager\|class StageBasedExecutor" \
  SparkEngine/Source/Engine/ECS/Systems/ECSystems.h \
  SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h
# Invariant 5 — RHI backend selection + null fallback
grep -n "NullRHIDevice\|GraphicsBackend::None\|SPARK_RHI_BACKEND" SparkEngine/Source/Graphics/RHI/RHIFactory.cpp | head
# OPEN register — re-read the handoff for current status
grep -n "INERT\|consolidate\|GetInstance" D:/SparkEngine/HARDEN_FLEET_HANDOFF.md
```

If any check above disagrees with this document, the code wins — update this
skill. Validate edits with the repo-standard skill linter (`skill-lint.ps1`,
located under `~/.claude/skills/skill-linter/`), pointing `-Path` at this skill
folder.
