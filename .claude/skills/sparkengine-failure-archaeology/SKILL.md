---
name: sparkengine-failure-archaeology
description: >-
  Chronological case-file of SparkEngine's hardest, already-solved bugs — each with symptom,
  root cause, the exact fix commit, and current landed/open status. TRIGGER when you are about to
  re-investigate something that "feels like it was fixed before", when you say "has this ICF /
  type-id / C++23 gate / blue-screen / macOS build / camera race / AngelScript hot-reload thing
  happened before", when a Windows Release-only CI failure reappears, when you want the STORY and
  commit hash of a past incident, or when auditing which harden-fleet items are still deferred.
  DO NOT TRIGGER for a brand-new live symptom you just hit and want a fix recipe for — use the
  sibling sparkengine-debugging-playbook (symptom->fix triage) instead; this skill is a history
  book, not a first-responder.
trilobite_compatible: true
trilobite_role: archaeology
---

# SparkEngine Failure Archaeology

This is a **chronological case-file**, not a triage table. Each entry tells the *story* of a bug that
already cost real investigation time: what broke, why, the commit that fixed it, and whether it is
still fixed today. Read it to avoid re-solving a solved problem and to know the exact commit to `git
show` for the full reasoning.

## When to use this / when NOT to

| Use this skill when… | Use a sibling instead |
|---|---|
| "Didn't we already fix a Release-only service-locator bug?" | New crash with no history → `sparkengine-debugging-playbook` |
| A Windows Release CI test fails but Debug passes | You need a symptom→fix decision tree → `sparkengine-debugging-playbook` |
| You want the commit hash + full reasoning of a past incident | You're doing a normal build → global `cmake-msvc` (toolchain) / `ci-troubleshoot` (CI) |
| Auditing which harden-fleet P1/P2 items are still deferred | — |

**Jargon defined once:**
- **ICF** = Identical COMDAT Folding, MSVC linker flag `/OPT:ICF` (on in Release, off in Debug). It merges byte-identical read-only data/functions to one address.
- **TU** = translation unit (one `.cpp`/`.mm` after preprocessing).
- **UAF** = use-after-free.
- **`.mm`** = Objective-C++ source (macOS Metal backends only).
- **trilobite** = this repo's ad-hoc name for a deferred-verification queue / cross-cutting audit pass (see `HARDEN_FLEET_HANDOFF.md`). There is no central registry file; `trilobite_*` frontmatter is the local convention.
- **harden-fleet** = a multi-agent hardening branch (`claude/harden-fleet`), merged into `Working`.

All commits below are confirmed ancestors of `Working` HEAD unless marked otherwise. Verify any entry with `git show <hash>`.

---

## Era 1 — The 2026-06-08 Windows-Release-CI unblock sprint

One day, one engineer (Krill), a cascade of Release-only failures on `build-windows-vs2022`. These
are the classic "passes in Debug, fails in Release" traps. Ordered by commit time that day.

### 1. C++23 feature gate wrongly rejected MSVC  — `d8685a2a` (14:28)
- **Symptom:** After merging dependabot/feature branches, the engine refused to compile on the
  *supported* MSVC toolchain — the `#error` C++23 gate in `Core/Platform.h` fired even though the
  compiler had the needed features.
- **Root cause:** MSVC reports `_MSVC_LANG == 202004L` for `/std:c++latest` until VS 2022 17.12, so
  it never reaches the `202100L` C++23 threshold the gate checked. The gate contradicted its own
  `#error` text (which documents MSVC 19.36+ / VS 2022 17.6+ as supported).
- **Fix:** Detect MSVC C++23 via `_MSC_VER >= 1936` in addition to `_MSVC_LANG`. Same commit also
  re-pinned `ThirdParty/UI/imgui` from the no-docking 1.92.8 tag back to the docking-branch tip
  (dependabot had dropped `DockSpace`/`ImGuiCol_Docking*` that SparkEditor needs).
- **Status:** **Landed.** Still present — `Core/Platform.h:115` today reads
  `_MSVC_LANG >= 202100L || (_MSVC_LANG > 202002L && ... _MSC_VER >= 1936)`.

### 2. `/OPT:ICF` collapsed all `GetTypeId<T>()` to one address  — `21b1a689` (15:42)
- **Symptom:** In **Release only**, the `EngineContext` service locator returned the *wrong*
  subsystem pointer. 14 tests failed (`ServiceLocator_*`, `Reflection_*`, `SubsystemInit_*`,
  `Adversarial_*`); the editor's play-mode hung. Debug was fine.
- **Root cause:** `GetTypeId<T>()` returned the address of a per-type `static const char id = 0`.
  Every `T` produced a byte-identical read-only COMDAT, so `/OPT:ICF` (Release-on, Debug-off) folded
  them all to a single address — making `GetTypeId<A>() == GetTypeId<B>()` for all types.
- **Fix:** Use a **non-const** `static char id;`. Writable data is not ICF-folded, so each `T` keeps a
  distinct address. Applied to `Core/EngineContext.h` and the three test helpers that mirror the
  pattern (`TestAdversarialEngine.cpp`, `TestEngineContext.cpp`, `TestReflection.cpp`).
- **Status:** **Landed.** `EngineContext.h:66` today is `static char id;` with a load-bearing comment
  at line 60 warning it must stay non-const. This is also captured in user memory `opt-icf-typeid-gate.md`.
- **Rule that came out of it:** type-id marker statics must be **non-const**; never "tidy" them to `const`.

### 3. Scene View blue-screened the whole editor  — `28f4ae8f` (16:21)
- **Symptom:** Opening the editor with the **Scene View** tab active turned the entire editor window
  a flat blue (clear color) with no visible UI. **Game View worked**; switching to Scene View broke it.
- **Root cause:** `SceneViewPanel::RenderSceneContent()` rendered to its own texture on the editor's
  shared D3D11 immediate context, then called `OMSetRenderTargets(0, nullptr, nullptr)` — leaving the
  render target **unbound**. Because the panel renders *inside* ImGui's frame build, the following
  `ImGui_ImplDX11_RenderDrawData()` drew to no target, so only the clear color showed. Game View was
  immune because it only uses ImGui draw lists and never touches the RT (this exactly matched the report).
- **Fix (two layers):** (a) `SceneViewPanel` saves the bound RT with `OMGetRenderTargets` and restores
  it afterward instead of unbinding; (b) `EditorApplication::Render()` re-binds the backbuffer right
  before `ImGui::Render()` as defense-in-depth. Files: `SparkEditor/Source/Core/EditorApplicationWindows.cpp`,
  `SparkEditor/Source/Panels/SceneViewPanel.cpp`.
- **Status:** **Landed.**

### 4. `Training_GradientCheckHuber` flaky in Release  — `368c20fb` (20:03)
- **Symptom:** `build-windows-vs2022 (Release)` failed on exactly one test. Passed locally under
  VS 17.10.
- **Root cause:** *Not* an engine bug. Floating-point cancellation in the central-difference gradient
  check. Relative error is U-shaped in the perturbation step (measured: eps `1e-4`→0.021, `1e-3`→0.0044,
  `1e-2`→0.00042). The `1e-3` step sat in the roundoff-dominated region with only ~2x margin and
  tipped over the `1e-2` bound under CI's MSVC Release toolset. `Training_HuberLossSwitchesAtDelta`
  (exact values) passed, proving the gradient itself was correct.
- **Fix:** Move the Huber finite-difference check to a `1e-2` step (~24x margin, the sweet spot). MSE
  check keeps `1e-3` (smoother gradients). File: `Tests/TestCpuNeuralTraining.cpp`.
- **Status:** **Landed.**
- **Lesson:** a gradient-check failure is usually the *epsilon*, not the gradient — confirm with an
  exact-value sibling test before touching the math.

### 5–7. macOS Metal/OpenAL compile cascade (`.mm`-only, continue-on-error CI)
These only compile on macOS, so the bugs were latent until the `build-macos` job (which is
`continue-on-error` — does **not** gate the required matrix) surfaced them. Fixed in layers the same evening:

| Commit | Time | Bug | Fix |
|---|---|---|---|
| `1b531e9f` | 20:47 | `PlatformTypes.h` `using BOOL = int` clashed with Objective-C's `typedef bool BOOL` in `.mm` TUs → "type alias redefinition (bool vs int)". Also `MetalRayTracing.mm` used `SPARK_LOG_*` without including `Utils/LogMacros.h`. | Guard the `BOOL` alias with `#ifndef __OBJC__`; add the `LogMacros.h` include. |
| `409c84e8` | 21:24 | `OpenALAudioEngine.h` forward-declared `typedef struct ALCdevice ALCdevice`; Apple's `alc.h` uses `struct ALCdevice_struct` → "typedef redefinition with different types". Plus `MetalRayTracing.mm` missing `<cstdio>` for `snprintf`. | Forward-declare `ALCdevice` per platform so the tag matches; add `<cstdio>`. |
| `0d0dae01` | 22:13 | Two `SPARK_LOG_*` calls in `MetalRayTracing.mm` passed a `std::string` as the format arg; the macro forwards to `snprintf(...fmt...)` needing `const char*` → "no matching function for call to snprintf". | Pass `"%s"` + `.c_str()`. |

- **Status:** all three **Landed**.
- **Still-open platform decision (NOT a bug):** `CMakeLists.txt` pins
  `CMAKE_OSX_DEPLOYMENT_TARGET=11.0` (Big Sur), but C++23 `std::format<float>` needs libc++'s float
  `to_chars`, available only on **macOS 13.3+**. This blocks every `std::format<float>` TU on macOS.
  Deliberately **deferred** to a maintainer with a Mac (raise the floor to 13.3, or swap to fmtlib).
  Documented in `0d0dae01`'s commit body. `build-macos` is continue-on-error so it does not gate CI.
- **Earlier related macOS commit:** `a40ac4aa` (2026-04-20) fixed system-vs-Homebrew OpenAL header
  paths (`OpenAL/al.h` vs `AL/al.h`), a `RHIStatistics` field mismatch in `MetalDevice.mm`, and a
  `/MT` vs `/MD` CRT clash (`LNK1169`) in `SparkBuild/CMakeLists.txt`.

---

## Era 2 — The 2026-07-06/07 harden-fleet audit (14 lanes, 130 findings)

A multi-agent hardening pass on branch `claude/harden-fleet`, later merged into `Working`. It found
real concurrency/lifetime bugs and left a documented deferred queue.

### 8. Data race in `SparkEngineCamera` state callback  — `0d3cbcbf` (checkpoint, 2026-07-06 20:19)
- **Symptom:** Concurrency finding (audit "camera/profiler races"). `m_isTransitioning` and callback
  dispatch were accessed inconsistently w.r.t. `m_stateMutex`; firing `NotifyStateChange()` *inside*
  the lock risked re-entrant deadlock (the callback can re-enter the camera, e.g. `Console_GetState`,
  and `m_stateMutex` is non-recursive).
- **Root cause:** State callbacks were invoked while holding the non-recursive `m_stateMutex`;
  `m_isTransitioning` lacked a documented locking discipline.
- **Fix:** In `SetPosition` (and the transition path), mutate state under the lock, then release and
  call `NotifyStateChange()` **outside** the lock. `m_isTransitioning` documented as "always accessed
  under `m_stateMutex`". Files: `Camera/SparkEngineCamera.cpp`, `Camera/SparkEngineCamera.h`.
- **Status:** **Landed.** Today `SparkEngineCamera.cpp` fires `NotifyStateChange()` outside the lock
  ("Fire the callback outside the lock (see NotifyStateChange)"). Note: the "line 162" locator from
  the audit lead is approximate — the file was heavily rewritten in this commit (~389 lines changed);
  match on the *comment/pattern*, not the line number.

### 9. AngelScript hot-reload left live scripts dangling  — `0d3cbcbf` (same commit)
- **Symptom:** A single typo in a script during hot-reload could corrupt/dangle all live entity
  scripts of that module (use-after-free class): the old approach detached and recompiled first, so a
  failed compile left entity scripts referencing a discarded module.
- **Root cause:** `ReloadScript` detached instances and discarded/recompiled the module **before**
  knowing the new source compiled. A compile error then left live `asIScriptObject`s pointing into a
  gone module.
- **Fix:** Pre-validate — compile the new source into a throwaway staging module
  (`moduleName + "$hotreload_stage"`) **before touching any live instances**; on failure, abort with
  `SetLastError` and leave all live scripts intact; only on success detach/recompile/re-attach.
  Also added per-class `[server]`/`[client]`/`[shared]` context recording. File:
  `Engine/Scripting/AngelScriptEngine.cpp` / `.h`.
- **Status:** **Landed.** Note: per-instance script state is **not** preserved across reload
  (constructors re-run, fields reset) — by design, no `Serialize`/`Deserialize` hooks.

### 10. The deferred "trilobite" queue  — handoff `1e45b87b`, mostly resolved by `52636dda`
`HARDEN_FLEET_HANDOFF.md` (committed at `1e45b87b`, 2026-07-06 20:41) listed P0-inert and deferred
P1/P2 items. **That doc is now stale** — most were fixed the next hour in `52636dda`
(2026-07-06 21:44, "Phase 3 deferred cross-cutting fixes", suite green 5933 OK / 0 fail). Current status:

| Item (from handoff) | Fixed in | Verified-present today | Status |
|---|---|---|---|
| Module `EngineContext` DLL-side export | `52636dda` | `SparkModuleInjectEngineContext` at `SparkSDK/Include/Spark/ModuleDllMain.h:88` | **Landed (part 1)** |
| BT `Clone()` deep-copy no-op | `52636dda` | `BehaviorTreeNodes.h` / `BehaviorTreeTypes.h` | **Landed** |
| `WeaponManager` `ownerEntity` always 0 | `52636dda` | `WeaponManager.cpp/.h` | **Landed** |
| EditorUI UAF funnel (`SwapWorld`) | `52636dda` | `EditorUI::SwapWorld(...)` at `EditorUI.h:335` | **Landed** |
| `HasUnsavedChanges` false-negative | `52636dda` | `UndoRedoManager.h` edit-sequence counter | **Landed** |
| MMO DI `GetInstance()` fallback | `52636dda` | `MMOPlayerSystem.cpp`, `MMOWorldSetup.cpp` | **Landed** |
| Localization `GetString` dangling ref | `52636dda` | `GetString`/`GetEntry` return `std::string` by value (`LocalizationSystem.h:71,137`) | **Landed** |
| Sequencer audio never wired | `52636dda` | `Sequencer.cpp/.h` audio-cue dispatch | **Landed** |

**Still genuinely deferred (open):**
- **`NetworkManager::GetInstance()` per-module fallback removal** — `52636dda` explicitly kept it:
  "load-bearing in bootstrap". Still present at `Networking/NetworkManager.cpp:130`. **Open / intentional.**
- **ECS `SystemManager`/`PhaseSystemManager`/`StageBasedExecutor` triple** — the handoff asked to
  consolidate; it is **NOT done**. All three still exist: `SystemManager` (`ECSystems.h:735`),
  `PhaseSystemManager` (`PhaseSystemManager.h`), and `StageBasedExecutor`
  (`ParallelSystemExecutor.h:378`). Worse, none is a cleanly-wired home: `StageBasedExecutor::ExecuteAll(dt)`
  **is ticked every frame** (`GameplayLifecycleShared.cpp:1062`) but has **zero `RegisterSystem`
  callers** (dead tick), while `PhaseSystemManager` is **defined-not-wired** (`CreatePhaseSystemManager`
  has zero callers, `UpdateAll` never invoked). The shipping Core loop drives module systems via
  `moduleManager->UpdateAll(dt)`. **Open** — an unresolved architectural weak point; see
  `sparkengine-architecture-contract` Invariant 3 and `sparkengine-job-system-threading §1c`.
- **Tooling/tests hygiene** (split `ConsoleApp.cpp`, single-source version string, wire 16 orphan
  `Test*.cpp`) — treat as **open** unless a later commit says otherwise.

### 11. Pre-existing Debug test-suite abort blocker (not from harden-fleet)
- **Symptom:** The full **Debug** test run aborts at `TestConstantBufferRing_InitializeNullDeviceFails`
  — `Initialize(nullptr)` hits `Assert::Fail` → `std::abort()` (asserts are active in Debug). Because
  harden tests register last, they never execute in Debug.
- **Workaround (from the handoff):** run the suite in **Release** (asserts are no-ops), or guard/skip
  the null-device assert tests. This is a pre-existing condition on `Working`, not introduced by the branch.
- **Status:** **Open** (workaround known). Re-check whether still present before relying on this.

---

## How to add a new entry (keep this file honest)

When a hard bug is solved, append an entry with **all** of: symptom, root cause, fix commit hash +
one-line description, and current status (Landed / Open / Candidate). **Never** write a claim you did
not confirm with `git show`. Prefer matching on comment/pattern over line numbers — this codebase
rewrites files wholesale during audits.

---

## Provenance and maintenance

- **Authored:** 2026-07-07. All commit hashes confirmed ancestors of `Working` HEAD (`710f797e`) at
  that date via `git merge-base --is-ancestor <hash> HEAD`.
- **Not independently verifiable from this Windows host:** the macOS `.mm` fixes (`1b531e9f`,
  `409c84e8`, `0d0dae01`, `a40ac4aa`) rely on `build-macos` CI; their commit bodies are the evidence.
- **Could NOT locate in git history / left out:** no separate commit was found isolating the
  "AngelScript UAF" or "camera race" as standalone fixes — both were part of the `0d3cbcbf` checkpoint
  and are attributed there, not to a distinct "trilobite" commit (the word appears only in
  `1e45b87b`'s message and `HARDEN_FLEET_HANDOFF.md`).

Re-verify any entry with these one-liners (run from repo root, Git Bash):

```bash
# Full reasoning of any incident:
git show 21b1a689   # ICF type-id      | git show d8685a2a  # C++23 gate
git show 28f4ae8f   # editor bluescreen | git show 368c20fb  # Huber epsilon
git show 0d3cbcbf   # camera race + AngelScript hot-reload (harden-fleet checkpoint)
git show 52636dda   # Phase 3 deferred-queue fixes

# Confirm a hash is still in Working:
git merge-base --is-ancestor <hash> HEAD && echo IN || echo NOT

# Spot-check that named fixes are still in the tree:
grep -n "static char id" SparkEngine/Source/Core/EngineContext.h                 # ICF fix present
grep -n "_MSC_VER >= 1936" SparkEngine/Source/Core/Platform.h                    # C++23 gate present
grep -n "SwapWorld" SparkEditor/Source/Core/EditorUI.h                           # editor UAF funnel present
grep -n "SparkModuleInjectEngineContext" SparkSDK/Include/Spark/ModuleDllMain.h  # module DLL export present
grep -n "GetInstance" SparkEngine/Source/Engine/Networking/NetworkManager.cpp    # still-open fallback

# The (now-stale) deferred queue:
cat HARDEN_FLEET_HANDOFF.md
```

Before committing, lint this skill with the `skill-linter` skill (its script lives under
`~/.claude/skills/skill-linter/`), passing `-Path` = this skill's folder.
