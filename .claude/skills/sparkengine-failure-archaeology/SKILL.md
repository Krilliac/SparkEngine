---
name: sparkengine-failure-archaeology
description: >-
  Chronological case-file of SparkEngine's hardest, already-solved bugs — each with symptom,
  root cause, the exact fix commit, and current landed/open status. TRIGGER when you are about to
  re-investigate something that "feels like it was fixed before", when you ask "has this ICF /
  type-id / DLL-unload crash / teardown AV / RCON-bypass / Great-Revert / hot-reload / orphan-test
  thing happened before", when a Windows Release-only CI failure reappears, when you want the STORY
  and commit hash of a past incident or a failed approach that must not be retried, or when auditing
  which harden-fleet items are still deferred. DO NOT TRIGGER for a brand-new live symptom you just
  hit and want a fix recipe for — use the sibling sparkengine-debugging-playbook (symptom->fix
  triage) instead; this skill is a history book, not a first-responder.
trilobite_compatible: true
trilobite_role: archaeology
---

# SparkEngine Failure Archaeology

This is a **chronological case-file**, not a triage table. Each entry tells the *story* of a bug or
failed approach that already cost real investigation time: what broke, why, the commit that fixed
it, and whether it is still fixed today. Read it to avoid re-solving a solved problem — or
re-attempting an approach that already failed — and to know the exact commit to `git show` for the
full reasoning.

## When to use this / when NOT to

| Use this skill when… | Use a sibling instead |
|---|---|
| "Didn't we already fix a Release-only service-locator bug?" | New live symptom, no history → `sparkengine-debugging-playbook` |
| "Was a big abstraction migration tried before? How did it go?" | You need build/CI mechanics → `sparkengine-build-ci-and-dependencies` |
| You want the commit hash + full reasoning of a past incident | DLL/plugin ABI *rules* going forward → `sparkengine-modules-sdk-abi-and-hot-reload` |
| Auditing which harden-fleet deferred items are still open | Test-suite health / QA gates → `sparkengine-validation-and-qa` |

This skill is the **canonical home for chronology and failed approaches only**. Symptom→fix
decision trees live in `sparkengine-debugging-playbook`; if you find yourself writing a triage
recipe here, move it there.

**Jargon defined once:**
- **ICF** = Identical COMDAT Folding, MSVC linker flag `/OPT:ICF` (on in Release, off in Debug). Merges byte-identical read-only data/functions to one address.
- **TU** = translation unit (one `.cpp`/`.mm` after preprocessing).
- **UAF** = use-after-free. **AV** = access violation (Windows segfault).
- **`.mm`** = Objective-C++ source (macOS Metal backends only).
- **RCON** = remote console: privileged server admin commands over the network.
- **harden-fleet** = a multi-agent hardening branch (`claude/harden-fleet`), merged into `Working`.
- **trilobite** = this repo's ad-hoc name for the harden-fleet deferred-verification queue (the word appears only in `1e45b87b`'s message and `HARDEN_FLEET_HANDOFF.md`).

All commits below are confirmed ancestors of `Working`-line HEAD `0e1fe7e7` (2026-08-23). Verify
any entry with `git show <hash>`.

---

## Era 0 — The Great Revert (2026-03-30) and the incremental recovery (2026-04-17)

The canonical **failed approach** in this repo's history. Do not retry it in the same shape.

### 0a. Big-bang abstraction migration reverted wholesale — `7851ee38`
- **What was attempted (PRs #313 + #314):** in one stroke — DirectXMath SIMD on Linux/macOS via a
  submodule + a hand-written `ThirdParty/sal/sal.h` shim, new interface abstractions
  (`IAudioBackend`, `IAnimationSystem`, …), singleton deprecation markers, a repo-wide `SPARK_LOG`
  macro migration, `[[nodiscard]]` additions, and CI workflow restructuring.
- **What happened:** "too many cascading issues across builds." The whole thing was reverted back
  to the `cf5d3a96` baseline in `7851ee38` — 223 files, 2920 deletions.
- **Lesson:** cross-cutting migrations in this codebase land **one contained seam at a time**, each
  independently green. Every goal of #313/#314 was later achieved incrementally (see below and
  Era 4) — none of it required the big-bang shape.

### 0b. The recovery pattern — `37778c15`
- Three weeks later, the *singleton-deprecation* goal landed as a contained token-swap: the 9
  free-floating `g_graphics`/`g_input`/`g_timer`/… globals were folded into one `EngineRuntime`
  ownership struct (`Core/EngineRuntime.h`, accessed via `GetEngineRuntime()`), migration confined
  to `Core/SparkEngine{,Windows,Linux}.cpp`, ~zero net lines, all 5869 tests passing.
- **Status:** **Landed.** This is now project law (CLAUDE.md: no new file-scope `g_*` subsystem
  globals). The *DirectXMath-on-Linux* goal landed later via guarded stubs, not a submodule —
  see Era 4 (`2962441e`).

---

## Era 1 — The 2026-06-08 Windows-Release-CI unblock sprint

One day, one engineer, a cascade of Release-only failures on `build-windows-vs2022`. The classic
"passes in Debug, fails in Release" traps. Ordered by commit time that day.

### 1. C++23 feature gate wrongly rejected MSVC — `d8685a2a` (14:28)
- **Root cause:** MSVC reports `_MSVC_LANG == 202004L` for `/std:c++latest` until VS 2022 17.12, so
  the `#error` gate in `Core/Platform.h` never saw the `202100L` C++23 threshold, rejecting the
  *supported* toolchain. Same commit re-pinned `ThirdParty/UI/imgui` to the docking branch
  (dependabot had dropped `DockSpace` that SparkEditor needs).
- **Fix / status:** detect MSVC C++23 via `_MSC_VER >= 1936` as well. **Landed** — verified today at
  `Core/Platform.h:114`. Also in user memory `opt-icf-typeid-gate.md`.

### 2. `/OPT:ICF` collapsed all `GetTypeId<T>()` to one address — `21b1a689` (15:42)
- **Symptom:** **Release only**, the `EngineContext` service locator returned the *wrong* subsystem
  pointer; 14 tests failed (`ServiceLocator_*`, `Reflection_*`, `SubsystemInit_*`, `Adversarial_*`);
  editor play-mode hung. Debug was fine.
- **Root cause:** `GetTypeId<T>()` returned the address of a per-type `static const char id = 0`.
  Every `T` produced a byte-identical read-only COMDAT, so `/OPT:ICF` folded them all to a single
  address — `GetTypeId<A>() == GetTypeId<B>()` for all types.
- **Fix:** a **non-const** `static char id;` — writable data is not ICF-folded. Applied to
  `Core/EngineContext.h` and the three test helpers mirroring the pattern.
- **Status:** **Landed** — verified today at `EngineContext.h:66`.
- **Rule that came out of it:** type-id marker statics must be **non-const**; never "tidy" them to `const`.

### 3. Scene View blue-screened the whole editor — `28f4ae8f` (16:21)
- **Root cause:** `SceneViewPanel::RenderSceneContent()` rendered to its own texture on the shared
  D3D11 immediate context, then left the render target **unbound**
  (`OMSetRenderTargets(0, nullptr, nullptr)`) inside ImGui's frame build — so
  `ImGui_ImplDX11_RenderDrawData()` drew to no target and only the clear color showed. Game View
  (pure ImGui draw lists) was immune.
- **Fix / status:** save + restore the bound RT in the panel; re-bind the backbuffer in
  `EditorApplication::Render()` as defense-in-depth. **Landed.**

### 4. `Training_GradientCheckHuber` flaky in Release — `368c20fb` (20:03)
- **Root cause:** *not* an engine bug — floating-point cancellation in the central-difference
  gradient check; the `1e-3` epsilon sat in the roundoff-dominated region with ~2x margin.
- **Fix / status:** epsilon `1e-2` (~24x margin) for the Huber check. **Landed.**
- **Lesson:** a gradient-check failure is usually the *epsilon*, not the gradient — confirm with an
  exact-value sibling test before touching the math.

### 5. macOS Metal/OpenAL compile cascade (`.mm`-only, continue-on-error CI)
Latent until the non-gating `build-macos` job surfaced them; fixed in layers the same evening:
`1b531e9f` (Objective-C `BOOL` clash with `PlatformTypes.h`'s `using BOOL = int` → guarded with
`#ifndef __OBJC__`; missing `LogMacros.h`), `409c84e8` (`ALCdevice` typedef-tag mismatch vs Apple's
`alc.h`; missing `<cstdio>`), `0d0dae01` (`std::string` passed to an `snprintf`-backed log macro →
`"%s"` + `.c_str()`). All **landed**. Earlier related: `a40ac4aa` (2026-04-20, OpenAL header paths +
`/MT` vs `/MD` CRT clash).
- **Still-open platform decision (NOT a bug):** `CMakeLists.txt` pins
  `CMAKE_OSX_DEPLOYMENT_TARGET=11.0` (verified today, root `CMakeLists.txt:56`), but C++23
  `std::format<float>` needs libc++ float `to_chars` (macOS 13.3+). Deliberately **deferred** to a
  maintainer with a Mac; `build-macos` is continue-on-error so it does not gate CI.

---

## Era 2 — The harden-fleet audit and its deferred queue (2026-07-06 → 2026-07-18)

A multi-agent hardening pass on `claude/harden-fleet`, merged into `Working`. It found real
concurrency/lifetime bugs, left a documented deferred queue ("trilobite"), and — unlike when this
file was first written — **that queue is now almost fully drained**.

### 6. Camera state-callback race + AngelScript hot-reload dangling scripts — `0d3cbcbf` (07-06 20:19)
- **Camera:** `NotifyStateChange()` fired while holding the non-recursive `m_stateMutex` → re-entrant
  deadlock risk. Fixed: mutate under lock, fire callback outside it. **Landed.**
- **Hot-reload:** `ReloadScript` detached instances and recompiled *before* knowing the new source
  compiled; one typo dangled every live `asIScriptObject` of that module. Fixed: pre-validate into a
  throwaway staging module (`moduleName + "$hotreload_stage"`) before touching live instances.
  **Landed.** Per-instance script state is *not* preserved across reload — by design.
- Match on comment/pattern, not line numbers — files were heavily rewritten in this commit.

### 7. Debug test-suite abort blocker — RESOLVED by `96c68e54` (07-06)
The old blocker (full Debug run aborted at `TestConstantBufferRing_InitializeNullDeviceFails`
because `Initialize(nullptr)` hit an assert → `std::abort()`) was fixed the same day the handoff
recorded it: `SPARK_EXPECTS(device)` dropped in favor of the graceful `return false` path, plus
three sibling abort/crash fixes (NarrowCast float narrowing, `ValidatePBRProperties`, a nullptr
registry in a harden test). **Landed** — verified today: `ConstantBufferRing.h` `Initialize`
returns false on null device with a "do NOT assert" comment. The "run Debug tests in Release"
workaround in `HARDEN_FLEET_HANDOFF.md` is obsolete.

### 8. The deferred "trilobite" queue — final ledger
`HARDEN_FLEET_HANDOFF.md` (`1e45b87b`) is **stale as a status document**; most items fell within
the hour in `52636dda` ("Phase 3 deferred cross-cutting fixes": module `EngineContext` DLL export →
`SparkModuleInjectEngineContext`, BT `Clone()` deep-copy, `WeaponManager` owner, `EditorUI`
`SwapWorld` UAF funnel, `HasUnsavedChanges` counter, MMO DI fallbacks, `LocalizationSystem`
return-by-value). Those are spot-verified landed. **Sequencer audio follow-up (2026-08-23):**
`52636dda` originally added only the public callback mechanism, but the current working tree now
wires manager-created sequences to the `EngineRuntime`-owned `IAudioBackend` through a copied-cue
queue and game-thread `DispatchPendingAudioCues()` drain. Headless/no-service consumes cues
silently; scrubs do not synthesize cues; later forward crossings and explicit replay are eligible.
`Tests/TestSequencerAudioWiring.cpp` verifies service delivery/thread affinity, headless behavior,
and replay/seek duplicate policy (3 tests, 18 assertions). Status: **Landed in the current working
tree**; forward-looking ownership rules live in `sparkengine-architecture-contract` Invariant 4 and
diagnosis in `sparkengine-debugging-playbook` row 7. The other stragglers:

| Item (from handoff) | Outcome | Status |
|---|---|---|
| ECS `SystemManager`/`PhaseSystemManager`/`StageBasedExecutor` triple — no cleanly-wired home | `4b42b8d9` (07-18): `PhaseSystemManager` created via `EngineSetup::CreatePhaseSystemManager` in gameplay init, `UpdateAll` pumped each frame in Physics→Animation→AI→Audio→Lifecycle→Render order; dead `StageBasedExecutor` **deleted**; regression test `Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp` fails if unwired again | **Landed** — verified today in `Core/Lifecycle/GameplayLifecycleShared.cpp` (`CreatePhaseSystemManager` at :766, `UpdateAll` at :1159) |
| 16 orphan `Test*.cpp` never in the SparkTests target | `c412f05b` (07-13): all 16 registered — see Era 4 for the 3 real bugs this surfaced | **Landed** — verified in `Tests/CMakeLists.txt` |
| `NetworkManager::GetInstance()` per-module fallback removal | `52636dda` explicitly kept it: "load-bearing in bootstrap" | **Open / intentional** — verified today at `Networking/NetworkManager.cpp:130` (EngineContext lookup first, `static NetworkManager` fallback second) |
| Split `ConsoleApp.cpp`; single-source the version string | Not done — `SparkConsole/src/ConsoleApp.cpp` is 1103 lines; `v1.0.0` still hardcoded in `SparkConsole/src/main.cpp:55` | **Open** (hygiene, low priority) |

---

## Era 3 — Editor undo convergence (2026-07-10) — `34714393`

- **Symptom:** Ctrl+Z, the Edit menu, the command palette, and `UndoHistoryPanel` were all **inert**
  — and worse, invisible to `SwapWorld()`'s UAF-guard history-clear (the `52636dda` fix), re-opening
  the very UAF class that fix closed.
- **Root cause:** `EditorUI` held a **private `UndoRedoManager` instance** while real edits were
  recorded on the `Spark::Editor::CommandHistory` singleton. Two parallel undo stacks: one empty
  and wired to the UI, one live and unreachable from it.
- **Fix:** delete the private instance; every undo surface operates on the process-wide singleton
  (`GetUndoRedoManager()` returns `&UndoRedoManager::GetInstance()`); `io.WantTextInput` guard so
  Ctrl+Z in a text field doesn't undo scene edits; monotonic edit/saved sequence counters so dirty
  state survives `Clear()`.
- **Status:** **Landed** — verified today in `EditorUI.h:99` and `UndoRedoManager.h:171-175`.
- **Lesson:** "parallel singleton systems doing the same thing: 0" (CLAUDE.md anti-bloat table) is
  not aesthetic — the duplicate stack was simultaneously a dead feature *and* a latent UAF.

---

## Era 4 — The 2026-07-13 marathon: dead tests, then the portability onion

One session (same Claude session ID on all four commits), two arcs.

### 9. 16 dead test files registered; 3 real engine bugs fell out — `c412f05b` (16:41)
- **Symptom:** 16 substantial test files (76–469 lines) existed under `Tests/` but were never in the
  SparkTests target — they silently never ran in CI. Registering them required fixing three
  *hallucinated* macro conventions (`SPARK_TEST`/Catch2-style/`ASSERT_FLOAT_EQ`) to the real
  `TEST`/`EXPECT_*` framework.
- **Bugs surfaced by actually running them:**
  1. `LODGenerator::Simplify` was accidentally O(collapses × triCount) — a 540k-vert scene never
     finished (>150 s). Adjacency map → ~6.5 s.
  2. `HitchDetector` included the current frame in the rolling baseline it was compared against —
     a large spike inflated its own baseline and masked itself.
  3. `FontSystem::LayoutText` emitted quads for zero-area glyphs (spaces), inflating vertex counts.
- **Status:** **Landed.**
- **Lesson (the one rule again):** *a test that never runs reports no failures.* A `find 0 tests`
  suite is indistinguishable from a green one. When adding tests, verify they are **registered and
  executed**, not just present on disk.

### 10. The Linux/macOS portability onion — `2962441e` → `cfa9a337` → `752896fa` (20:01–21:53)
Each fix let the build get exactly one stage further; all three were **pre-existing** breakage:
1. **`2962441e`** — 179 files included `<DirectXMath.h>` directly (absent on Linux) instead of
   `Core/Platform.h`, which conditionally pulls real DirectXMath on Windows or
   `PlatformDirectXMathStubs.h` elsewhere. Bulk-rewrote every include; the Windows verify build
   caught a self-include bug (the pass had rewritten Platform.h's own include) before it shipped.
   This is how the Great Revert's DirectXMath goal (Era 0) finally landed: guarded stubs, no
   submodule, no SAL shim.
2. **`cfa9a337`** — compile now reached the MSVC secure-CRT gap: `sscanf_s`/`localtime_s`/`gmtime_s`
   in cross-platform files. Shims added to `PlatformTypes.h`'s non-Windows section (deliberately
   avoiding POSIX `*_r`, which strict `-std=c++NN` can hide under glibc).
3. **`752896fa`** — compile clean, link now failed: `GetOrLoadTextureSRV`/`InvalidateBasicMaterial`
   declared unconditionally but defined only in a Windows-only TU. Non-Windows counterparts added
   (null-SRV is a legal "no texture" to every caller). Validated via CI, not locally — on a Windows
   host even g++ defines `_WIN32` and compiles the `#ifndef` block out.
- **Status:** all three **Landed**.

---

## Era 5 — The DLL/module lifetime cluster (2026-07-06 → 2026-08-23)

Four commits, months apart, one law: **any object whose vtable, destructor, or allocator lives in a
DLL must be destroyed while that DLL is still mapped** — and CRT-exit statics are the recurring
assassin. Forward-looking rules live in `sparkengine-modules-sdk-abi-and-hot-reload`; the
incidents are here.

### 11. Probe loads + legacy adapter leak — `2e4b4c2f` (07-06)
`DiscoverModules` probed DLLs with `DONT_RESOLVE_DLL_REFERENCES` (documented-dangerous: no
init, no dependency resolution) then *called into them* — and `LegacyModuleAdapter`'s destructor
never called the module's `destroyFn` (leak of the DLL-allocated object). Both fixed in
`Core/ModuleManager.cpp`. **Landed.**

### 12. Engine lifecycle wave — `8d57de1b` (07-10)
Four root causes from live cdb stacks on a frozen client:
1. `InitPhysics` **never called** `PhysicsSystem::Initialize` — `m_joltSystem` stayed null and every
   consumer silently took its no-physics fallback while builds stayed green (a wiring ghost; see
   CLAUDE.md "Functionality Is Not Optional").
2. Module DLLs statically link **their own Jolt copy** — per-image allocator pointers,
   `Factory::sInstance`, `RegisterTypes` tables were never registered, so the first module-side
   shape creation AV'd inside `JPH operator new`. Fixed idempotently per image via the
   `SparkModuleInjectEngineContext` hook (`SparkSDK/Include/Spark/ModuleDllMain.h:90` today).
3. `NetworkManager::ClearHandlers` moved to the **pre-`FreeLibrary`** teardown block: handler
   `std::function`s carry module-DLL lambdas; the CRT-exit destructor of the static fallback
   instance freed them **after unload** and wedged the crash handler (the frozen-window zombie).
4. `ShutdownPhysics` moved **before** module unload: module-created Jolt bodies/shapes must release
   while module code is still mapped.
**Landed.**

### 13. W11 teardown AV — `8a863dec` (07-11)
A module whose `OnLoad` failed was destroyed at `UnloadAll` time, after physics/ECS/event-bus were
gone. Fixed: prompt-destroy the failed instance while subsystems are alive (DLL stays mapped until
`UnloadAll`); `PhysicsSystem::RemoveBody` becomes a safe one-shot-WARN no-op after Jolt teardown;
per-config archive dirs (`build/lib/<Config>`) so Debug/Release vendor archives coexist. Regression
test `Tests/TestPhysicsTeardownGuard.cpp`. **Landed.**

### 14. Editor plugins: destroy through the DLL export before unload — `a6fd96d9` (08-23)
Same law, editor side: plugin-provided **panel** implementations (vtables in the plugin DLL) are now
shut down and released before the plugin instance is destroyed, and the instance is destroyed
through its DLL `destroyFn` before `FreeLibrary` — including on every registration-failure path
(duplicate name, exceptions). `ModuleManager` got the matching refused-legacy-module fix
(`adapterOwner.reset()` before `FreeLibrary`). **Landed.**

---

## Era 6 — Networking trust boundary night (2026-08-23, 01:41–01:59)

Three commits in eighteen minutes, one theme: **nothing an unauthenticated or rejected peer sends
may reach privileged code or trigger amplified work.**

### 15. Chat is not an admin transport — `68998265` (01:41)
- **Root cause:** a chat message starting with `/` was routed straight into `ExecuteRcon` —
  **bypassing the RCON password entirely**. Any connected client could run privileged commands.
- **Fix:** the chat→RCON gate is deleted outright; `ExecuteRcon` is a trusted in-process API until a
  separately-authenticated remote-admin protocol exists. Bonus: RCON handlers are now invoked
  **outside** the registry mutex (the built-in `help` handler re-enters the registry — self-deadlock).
- **Status:** **Landed** (`Networking/DedicatedServer.cpp`).

### 16. Ingress validation hardened — `5556061d` (01:50)
- Raw packet `channel` byte range-checked before the `ChannelType` cast (previously any u8 was
  cast blind); `ConnectAccepted`/`Ack` schemas tightened to exact sizes; **unauthenticated senders
  of custom (module-defined) message types are now rejected** — custom types stay schema-optional
  but only for authenticated peers. New adversarial tests in `TestPacketValidatorReal.cpp` /
  `TestNetworkManagerIntegration.cpp`; wire format doc updated (`docs/specs/networking-wire-format.md`).
- **Status:** **Landed.**

### 17. Rejected clients got a full entity sync — `8c096003` (01:59)
- **Root cause:** `HandleConnect` returned `void`, so the caller pushed the *pending* ID into
  `newlyConnected` even when the connect was rejected — the server then ran a full replicated-entity
  walk for an endpoint it had just refused (O(connects × entities) CPU amplification for an
  attacker).
- **Fix:** `HandleConnect` returns the admitted `ClientID` or `INVALID_CLIENT`; `SendFullSync`
  independently refuses targets not in `m_clients`.
- **Status:** **Landed** (`NetworkConnection.cpp`, `NetworkReplication.cpp`).

---

## How to add a new entry (keep this file honest)

When a hard bug is solved — or an approach fails expensively — append an entry with **all** of:
symptom, root cause, fix commit hash + one-line description, and current status (Landed / Open /
Candidate). **Never** write a claim you did not confirm with `git show` or a grep of the current
tree. Prefer matching on comment/pattern over line numbers — this codebase rewrites files wholesale
during audits. When an "Open" item gets fixed, don't delete it: record the resolving commit (the
Era-2 ledger shows the shape).

---

## Provenance and maintenance

- **Refreshed:** 2026-08-23 on branch `claude/whole-nine-yards-20260823` (working tree with
  uncommitted changes ahead of HEAD `0e1fe7e7`). Every commit hash above confirmed an ancestor via
  `git merge-base --is-ancestor <hash> HEAD`; every "verified today" claim grepped against the
  live working tree, not just the committed HEAD.
- **Resolved since the 2026-07-07 edition:** ECS phase wiring (`4b42b8d9`), 16 orphan tests
  (`c412f05b`), Debug-suite abort blocker (`96c68e54`). The old "still open" claims for those are
  gone — do not resurrect them from `HARDEN_FLEET_HANDOFF.md`, which remains stale by design
  (historical artifact).
- **Not independently verifiable from a Windows host:** the macOS `.mm` fixes (`1b531e9f`,
  `409c84e8`, `0d0dae01`, `a40ac4aa`) and the non-Windows `#ifndef _WIN32` paths in `cfa9a337`/
  `752896fa` rely on CI; the commit bodies are the evidence.

Re-verify with these one-liners (repo root, Git Bash):

```bash
# Full reasoning of any incident:
git show 7851ee38   # Great Revert        | git show 37778c15  # EngineRuntime recovery
git show 21b1a689   # ICF type-id         | git show 96c68e54  # Debug-suite unblock
git show 4b42b8d9   # ECS phase wiring    | git show c412f05b  # 16 orphan tests
git show 34714393   # undo convergence    | git show 8d57de1b  # DLL lifecycle wave
git show 5556061d 68998265 8c096003       # trust-boundary night

# Confirm a hash is still in the mainline:
git merge-base --is-ancestor <hash> HEAD && echo IN || echo NOT

# Spot-check that named fixes are still in the tree:
grep -n "static char id" SparkEngine/Source/Core/EngineContext.h                  # ICF fix
grep -n "_MSC_VER >= 1936" SparkEngine/Source/Core/Platform.h                     # C++23 gate
grep -n "CreatePhaseSystemManager" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp  # phase wiring
grep -n "do NOT assert" SparkEngine/Source/Graphics/ConstantBufferRing.h          # Debug unblock
grep -rn "StageBasedExecutor" SparkEngine/Source --include='*.cpp'                # expect: nothing (deleted)
grep -n "GetInstance" SparkEngine/Source/Engine/Networking/NetworkManager.cpp     # still-open fallback (:130)
grep -n "SparkModuleInjectEngineContext" SparkSDK/Include/Spark/ModuleDllMain.h   # per-image inject hook
ls Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp Tests/TestPhysicsTeardownGuard.cpp
wc -l SparkConsole/src/ConsoleApp.cpp                                             # open hygiene item (1103 at refresh)
```

Before committing, lint this skill with the installed `skill-linter` skill, pointing its
`-Path` parameter at this repository's `.claude/skills` tree.
