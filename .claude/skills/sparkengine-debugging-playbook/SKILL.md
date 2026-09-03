---
name: sparkengine-debugging-playbook
description: >-
  SparkEngine symptom-driven debugging: you observed a concrete wrong behavior in a running
  SparkEngine subsystem (AI behavior trees, weapons, the ImGui editor, undo/redo, MMO modules,
  localization, cinematic sequencer, ECS system ticking, or client-side network prediction) and
  need to know where to look, what the correct code should look like, and a discriminating
  experiment to confirm the cause before changing anything. Also the first-response toolkit:
  log macros, debug hooks, fault isolation, crash dumps, and test filters.
  TRIGGER when the user says things like "cloned behavior tree does nothing", "weapon damage is
  attributed to entity 0", "editor crashes when I open a scene", "editor won't prompt to save",
  "MMO module uses the wrong NetworkManager", "localized string is garbage", "cutscene has no
  audio", "my ECS system never ticks / never runs / silently stopped running", "multiplayer is
  rubber-banding / desyncing", or "how do I get more debug info out of the engine".
  DO NOT TRIGGER for CI job or build configuration failures (use sparkengine-build-ci-and-dependencies),
  for a chronological "what changed and why did it break" narrative (use sparkengine-failure-archaeology),
  for raw compiler/linker error decoding, or for generic C++ crash forensics with no subsystem
  lead (use crash tooling — see routing table).
trilobite_compatible: true
trilobite_role: debugging
---

# SparkEngine Debugging Playbook

> **Live-tree guard (reviewed 2026-08-24).** SparkEngine's `Working` line is
> changing daily. Re-read root `AGENTS.md`, `CLAUDE.md`, the relevant wiki
> pages, and current `git status`/`git log` before acting. Branch labels, counts,
> line numbers, open-gap/status claims, and binary inventories below are dated
> evidence, not current truth. Preserve unrelated dirty work. During read-only
> orientation, do not build, generate, format, update submodules, or run Git
> mutations. On Nathan's Windows setup, route an authorized MSVC build through
> `C:\Users\Nathan\.claude\scripts\build.ps1` so vcvars and `CC`/`CXX`
> pinning are applied.

A **symptomatic lookup table** for SparkEngine subsystems: *"You are seeing X. Look at Y. Here is
how to confirm it's cause Z before you touch code."* Each entry gives a **discriminating
experiment** — a cheap check that distinguishes the suspected cause from a look-alike, so you don't
start editing the wrong file. Plus a first-response toolkit (logging, hooks, dumps, test filters).

All paths are relative to the repository root. Run all `rg` commands from the repo root.

Glossary (defined once):
- **RHI** — Render Hardware Interface, SparkEngine's rendering abstraction (D3D11/D3D12/Vulkan/…).
- **ECS** — Entity Component System (EnTT-backed). A *system* is a per-frame update over entities.
- **UAF** — use-after-free.
- **DI / injected context** — `EngineContext` handed to a game-module DLL via `Initialize(context)`,
  as opposed to a process-global singleton.
- **Fault isolation** — `SPARK_GUARDED_UPDATE` catches exceptions from a subsystem's update and
  auto-disables the subsystem after repeated faults (see First-response toolkit).

## When to use this skill / when NOT to

| Situation | Use |
|-----------|-----|
| A named SparkEngine subsystem misbehaves in a specific way (table below) | **this skill** |
| Engine/console won't start, blank window, "standalone mode" | `TROUBLESHOOTING.md` first, then `sparkengine-run-package-and-release` |
| A CI job fails, build configuration / dependency / preset problems | sibling `sparkengine-build-ci-and-dependencies` |
| Generic segfault / heap corruption / UB with no subsystem lead | crash tooling: `crash_mode on` + `SparkEngine/Source/Utils/CrashHandler.cpp` dumps; the global `cpp-crash-triage` skill if your environment has it |
| "How did we get here / what was the history of this bug" | sibling `sparkengine-failure-archaeology` |
| The architecture rules a fix must respect (service locator, wiring, thresholds) | `sparkengine-architecture-contract` |
| Writing/optimizing ECS queries and views (not "why doesn't it tick") | `sparkengine-ecs-lifecycle-threading-and-memory` |
| Threading/parallel-execution design questions | `sparkengine-ecs-lifecycle-threading-and-memory` |
| Running the validation/QA scripts or interpreting their output | `sparkengine-validation-and-qa` |

Do NOT paste generic C++ advice here. This file only carries SparkEngine-specific symptom→cause maps.

## First-response toolkit (get signal before guessing)

All items below verified against the source on 2026-08-23.

| Tool | Where | How |
|------|-------|-----|
| Startup triage doc | `TROUBLESHOOTING.md` (repo root) | Startup checklist, expected console banner/log lines, memory-footprint table ("under 20 MB → crashed during init"), common failures |
| Log macros | `SparkEngine/Source/Utils/LogMacros.h` | `SPARK_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL(Spark::LogCategory::X, fmt, ...)` — printf-style; also `SPARK_LOG_ONCE` |
| Debug hooks | `SparkEngine/Source/Utils/DebugHookManager.h` | `Register(DebugHookPoint::FrameEnd, "name", callback)` — hook points include `EnginePreInit/PostInit`, `FrameBegin/End`, `SystemPreUpdate/PostUpdate` (see `DebugHookPointToString`) |
| Fault isolation | `SparkEngine/Source/Core/FaultIsolation.h/.cpp` | `SPARK_GUARDED_UPDATE(name, category, {...})` catches exceptions, calls `ReportFault`, and **auto-disables the subsystem after ~3 faults** (default `m_globalMaxRetries = 3`, exponential-backoff auto-recovery). Console commands: `fault.status`, `fault.reset <name>`, `fault.reset_all`, `fault.retries`, `fault.autorecovery` |
| Crash dumps | SparkConsole command `crash_mode on` (`SparkConsole/src/ConsoleApp.cpp`); engine handler in `SparkEngine/Source/Utils/CrashHandler.cpp` | Dumps saved alongside the executable; more in `TROUBLESHOOTING.md` "Getting More Debug Info" |
| Engine console | SparkConsole (launches with the engine) | `help`, `engine_status`, `fps`, `graphics_info`, `memory_info`, `diag` — full table in `TROUBLESHOOTING.md` |
| Test selectors | `Tests/TestMain.cpp` (`--help` prints these) | Single CTest entry `SparkEngineTests` runs the `SparkTests` exe; filter with `SPARK_TEST_FILE=<source file>` / `SPARK_TEST_NAME=<substring>` env vars. The full selector/flag list is owned by `sparkengine-validation-and-qa` §3 |

Example — run only the ECS phase-wiring regression test (bash):

```bash
SPARK_TEST_FILE=Test_lifecycle_ecs_phase_wiring.cpp ./build/bin/SparkTests
```

## Important: most historical bugs below are already FIXED

This table was built from the deferred-issue list in the retired `HARDEN_FLEET_HANDOFF.md` (git history only since 2026-09-03) (git history only since 2026-09-03) and
**re-verified against the live working tree on 2026-08-23** (branch
`claude/whole-nine-yards-20260823`, uncommitted changes ahead of `0e1fe7e7`). Eight of the nine listed
issues are fixed. The value of the table today is twofold:

1. If you observe the symptom, the row tells you the **fix that should be present** so you can spot a
   **regression** (someone reverted or broke the fix).
2. Sequencer audio (row 7) and ECS phase ticking (row 8) now describe their **wired contracts** and
   the discriminating checks for regressions or look-alike failures.

Line numbers drift — grep the named symbol, do not trust any number blindly.

---

## Triage table

### 1. Cloned behavior tree does nothing / has no root (AI)

- **Symptom**: An AI agent whose behavior tree was produced by cloning a template just idles;
  the cloned tree appears empty or its root child is missing.
- **Where to look**: `SparkEngine/Source/Engine/AI/BehaviorTreeTypes.h` (`BTNode::Clone`, pure
  virtual) and `SparkEngine/Source/Engine/AI/BehaviorTreeNodes.h` (per-node `Clone()` overrides).
  The clone site is `AISystem.cpp`: `instance->SetRoot(it->second->GetRoot()->Clone());`.
- **Expected (fixed) state**: `virtual std::unique_ptr<BTNode> Clone() const = 0;` and every node
  type deep-copies: composites (`SequenceNode`, `SelectorNode`, `ParallelNode`) loop their children
  calling `child->Clone()`; decorators clone their child; leaves (`ActionNode`, `ConditionNode`,
  `WaitNode`) copy their payload.
- **Discriminating experiment**:
  ```
  rg -n "std::unique_ptr<BTNode> Clone" SparkEngine/Source/Engine/AI/BehaviorTreeNodes.h
  ```
  If a composite's `Clone()` returns `std::make_unique<SequenceNode>(m_name)` **without** the
  `for (child) cloned->AddChild(child->Clone())` loop, the deep-copy regressed → cloned trees lose
  their children. If the loop is present, the bug is elsewhere (check the *template* was populated
  before cloning, and that `AISystem` actually reaches the `Clone()` call for that agent type).

### 2. Weapon damage/kills attributed to entity 0 (Gameplay/FPS)

- **Symptom**: `WeaponFireEvent.ownerEntity` is always `0`; damage/kill attribution, hit markers, or
  kill feed all credit entity 0.
- **Where to look**: `SparkEngine/Source/Engine/Gameplay/WeaponManager.cpp` — the call chain
  `WeaponSystem::ProcessWeapon` → `HandleFiring` → emit `WeaponFireEvent`.
- **Expected (fixed) state**: the shooter entity id is threaded through the whole chain:
  `ProcessWeapon(uint32_t ownerEntity, …)` receives the entity from the ECS view, passes it to
  `HandleFiring(uint32_t ownerEntity, …)`, which sets `event.ownerEntity = ownerEntity;`.
- **Discriminating experiment**:
  ```
  rg -n "ownerEntity" SparkEngine/Source/Engine/Gameplay/WeaponManager.cpp
  ```
  Confirm `event.ownerEntity = ownerEntity;` and that `ProcessWeapon`/`HandleFiring` take
  `ownerEntity` as a parameter (not a hardcoded `0` or a default-constructed local). If the signature
  still carries the id but the event reads `0`, the loss is at the emit site, not the plumbing.

### 3. Editor crashes / UAF when opening a scene while one is loaded (Editor)

- **Symptom**: The ImGui editor crashes (often a UAF) shortly after `File → Open` while a scene is
  already loaded; panels reference freed entities/components of the old world.
- **Where to look**: `SparkEditor/Source/Core/EditorUI.cpp` — `SwapWorld`, `OpenScene`, and the
  panel-rewire block; `CommandHistory` (undo stack whose commands may close over old entities).
- **Expected (fixed) state**: `EditorUI::SwapWorld(std::unique_ptr<World>)` is the single funnel:
  it (a) clears undo history first — `Spark::Editor::CommandHistory::GetInstance().Clear();` — then
  (b) installs the new world, then (c) rewires every panel. `OpenScene()`, initial world creation,
  and shutdown (`SwapWorld(nullptr)`) all route through it.
- **Discriminating experiment**:
  ```
  rg -n "SwapWorld|OpenScene|CommandHistory::GetInstance\(\)\.Clear" SparkEditor/Source/Core/EditorUI.cpp
  ```
  Confirm `OpenScene()` calls `SwapWorld(...)` and does **not** assign `m_world` directly. If any code
  path sets `m_world = ...` outside `SwapWorld` (bypassing the history-clear + panel-rewire), that
  path is the UAF source — a stale `CommandHistory` command or panel still points at the freed world.

### 4. Editor won't prompt to save despite real changes (Editor / undo-redo)

- **Symptom**: You make edits, undo back to the saved point, then branch to new edits — and the editor
  reports "no unsaved changes" (false negative), so you lose work on close.
- **Where to look**: `SparkEditor/Source/UndoRedo/UndoRedoManager.h` — `HasUnsavedChanges`,
  `m_editSequence`, `m_savedSequence`.
- **Expected (fixed) state**: dirty tracking is a **monotonic counter**, not a stack-size compare.
  `m_editSequence` (a `uint64_t`) increments on every mutating op (execute/merge/undo/redo);
  `MarkSaved` snapshots it into `m_savedSequence`; `HasUnsavedChanges()` returns
  `m_editSequence != m_savedSequence`.
- **Discriminating experiment**:
  ```
  rg -n "m_editSequence|m_savedSequence|HasUnsavedChanges" SparkEditor/Source/UndoRedo/UndoRedoManager.h
  ```
  If `HasUnsavedChanges` compares stack **sizes** or **indices** (e.g. `m_currentIndex != m_savedIndex`)
  instead of the monotonic sequences, the false-negative bug regressed: undo-to-saved-then-branch
  produces an equal size/index while the content differs.

### 5. MMO game module uses the wrong / stale subsystem instance (MMO / DI)

- **Symptom**: An MMO module (`SparkGameMMO`) talks to a different `NetworkManager` (or other
  subsystem) than the one the host injected — messages vanish, or a second stale singleton appears.
- **Where to look**: `GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp` and
  `GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp`.
- **Expected (fixed) state**: the module stores the injected context and resolves subsystems through
  it — e.g. `auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;` — **not** through a
  process-global `SomeManager::GetInstance()`.
- **Discriminating experiment**:
  ```
  rg -n "m_context->Get|::GetInstance\(\)" GameModules/SparkGameMMO/Source
  ```
  `Spark::SimpleConsole::GetInstance()` (log sink) is a **legitimate** shared singleton, and
  `MMOWorldSetup.cpp` deliberately falls back to `SeamlessAreaManager::GetInstance()` only when
  `m_context->GetAreaStreaming()` is null — ignore those. The red flag is resolving *network /
  physics / graphics / the engine context itself* via `GetInstance()` instead of `m_context->GetX()`.
  If you find that, the DI fix regressed and the module is on a stale instance.

### 6. Localized string is intermittent garbage or crashes (Localization)

- **Symptom**: Reading a localized string occasionally returns corrupted text or crashes, especially
  under concurrent access.
- **Where to look**: `SparkEngine/Source/Engine/Localization/LocalizationSystem.h` / `.cpp` —
  `GetString`, and `Format` (which calls `GetString`).
- **Expected (fixed) state**: `std::string GetString(const std::string& key) const;` returns **by
  value**. (Returning `const std::string&` into an internal map after releasing the lock is the
  classic dangling-reference bug.)
- **Discriminating experiment**:
  ```
  rg -n "GetString" SparkEngine/Source/Engine/Localization/LocalizationSystem.h
  ```
  If the signature is `const std::string& GetString(...)` (reference return), the dangling bug is back.
  Value return is safe. Note `Format` binds `const std::string& tmpl = GetString(key);` — that is a
  const-ref to a **temporary** (lifetime-extended) and is fine *only while* `GetString` returns by value.

### 7. Cinematic plays visuals but no audio (Sequencer)

- **Symptom**: A `Sequence` plays animation/property/event tracks correctly, but `AudioCue` tracks
  produce no sound.
- **Where to look**: `SparkEngine/Source/Engine/Cinematic/Sequencer.{h,cpp}`
  (`SetAudioDispatchCallback`, `QueueAudioCue`, `DispatchPendingAudioCues`),
  `Core/Lifecycle/GameplayLifecycleShared.cpp` (game-thread drain after the cinematic worker joins),
  and `Core/SparkEngine{Windows,Linux}{Init,Headless}.cpp` (backend attach/detach).
- **Expected (fixed) state, verified 2026-08-23**: `EngineRuntime` owns the cross-platform
  `IAudioBackend`; `SequencerManager` holds a non-owning pointer and installs a private service
  callback on every manager-created sequence. Timeline evaluation copies crossed cues into a
  mutex-protected queue; the gameplay lifecycle drains it exactly once on the game/update thread.
  `Sequence::SetAudioCallback` is only an observer on the sequencer update thread and cannot
  replace the service wiring. Headless/no-service drains consume cues silently. `SetTime` itself is
  silent; rewinding makes a later forward crossing eligible, and Stop→Play replay is eligible.
- **Discriminating experiment**:
  ```
  rg -n "SetAudioBackend|DispatchPendingAudioCues|SetAudioDispatchCallback" SparkEngine/Source
  SPARK_TEST_FILE=TestSequencerAudioWiring.cpp ./build/bin/SparkTests
  ```
  If wiring and tests are present but runtime sound is absent, distinguish: (a) a `Sequence`
  constructed directly instead of through `SequencerManager::CreateSequence`, (b) no/unavailable
  backend, (c) `AudioCue::soundName` was never loaded into the backend, or (d) time never crossed the
  cue (`cue.time > previousTime && cue.time <= currentTime`). Do **not** add a public
  `SetAudioCallback` call as a substitute for the manager-owned service path.

### 8. A registered ECS system silently never ticks (ECS)

- **Symptom**: You added an ECS system, it compiles, but its `Update` never runs — entities aren't
  processed and no error is raised.
- **Verified current model (2026-08-23, working tree)** — `PhaseSystemManager` is the wired
  canonical manager:
  | Class | File | Role |
  |-------|------|------|
  | `PhaseSystemManager` | `SparkEngine/Source/Engine/ECS/Systems/PhaseSystemManager.h` | **Canonical and WIRED.** `Spark::EngineSetup::CreatePhaseSystemManager(ctx)` (`SparkEngine/Source/Core/EngineSetup.h`) builds it during gameplay init (`GameplayLifecycleShared.cpp`, `InitializeEcsPhaseSystemsImpl`), and the frame loop pumps it via `SPARK_GUARDED_UPDATE("ECS_Phases", "Core", { GetPhaseSystemManagerImpl().UpdateAll(*world, dt); });`. Regression test: `Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp`. |
  | `SystemManager` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` | **Legacy** flat, insertion-order manager. Do not add new systems here. |
  | `StageBasedExecutor` | *(deleted — zero source-file references remain)* | Was a live tick over zero registered systems; removed when `PhaseSystemManager` was wired (2026-07-18). Only docs/wiki still mention it. |
  | `ParallelSystemExecutor` | `SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h` | Access-set batcher, **not wired** into the live tick — `ECSIntegration.h::ConfigureParallelExecution` has no callers, and `PhaseSystemManager::UpdateAll` is serial. See `sparkengine-ecs-lifecycle-threading-and-memory`. |
- **The four ways a system silently doesn't tick**:
  1. **Not added to `CreatePhaseSystemManager`.** New systems must be registered in
     `Spark::EngineSetup::CreatePhaseSystemManager(ctx)` (`SparkEngine/Source/Core/EngineSetup.h`)
     with the correct `Phase::` bucket — that is the set the gameplay lifecycle creates and ticks.
     A system registered into the legacy `SystemManager` or constructed ad hoc will not tick.
  2. **Registered via the flat overload.** `AddSystem<T>(Phase::AI, …)` is phased;
     `AddSystem<T>(…)` (no `Phase`) drops into the **flat list that runs LAST, after all phases**
     (see `PhaseSystemManager::UpdateAll` — "phased first (in phase order), then flat list"). If
     ordering matters (you read another system's output), the flat overload is the cause.
  3. **Its subsystem dependency was null at registration.** In `CreatePhaseSystemManager`, physics,
     audio, and render systems are added **only if** `ctx.GetPhysics()/GetAudio()/GetGraphics()` are
     non-null. In headless / `NullRHIDevice` runs `GetGraphics()` may be null → `RenderSystem` is
     never registered and never ticks (by design).
  4. **Fault isolation disabled it.** The whole phase tick runs under
     `SPARK_GUARDED_UPDATE("ECS_Phases", …)` — if any system's `Update` throws repeatedly (default
     3 faults), `SubsystemFaultIsolator` **disables the entire `ECS_Phases` tick** and only
     exponential-backoff auto-recovery re-enables it. Run `fault.status` in SparkConsole; recover
     with `fault.reset ECS_Phases`.
- **Discriminating experiment**:
  ```
  rg -n "AddSystem<|CreatePhaseSystemManager" SparkEngine/Source/Core/EngineSetup.h
  rg -n "UpdateAll|ECS_Phases" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp
  ```
  Confirm your system appears in `CreatePhaseSystemManager` with a `Phase::` argument and that the
  guarded `UpdateAll` call is present. To prove it ticks, put a one-shot `SPARK_LOG_DEBUG` at the top
  of your `Update`; if it never prints, it's cause 1, 3, or 4 — not a logic bug inside `Update`.

### 9. Multiplayer rubber-banding / desync on the client (Networking / prediction)

- **Symptom**: The local player snaps/teleports back after moving (rubber-banding), or client and
  server positions drift apart.
- **Where to look**: `SparkEngine/Source/Engine/Networking/ClientPrediction.h` / `.cpp` —
  `Reconcile(serverState, deltaTime)` re-applies un-ACKed inputs after a server correction;
  `GetLastCorrectionMagnitude()` reports the size of the last correction (also logged at Debug level
  inside `Reconcile`). Wired into `GameModules/SparkGameFPS/Source/Game/MultiplayerSystem.cpp`,
  `GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp`, and the
  `GameModules/SparkGameMMOFPS/Source/Net/` TF client/server files.
- **Discriminating experiment**: Log `GetLastCorrectionMagnitude()` each time a server state arrives
  (or raise the Network log category to Debug to see `Reconcile: correction magnitude=…`).
  - **Consistently large corrections** → the client's local simulation disagrees with the server's
    (different movement constants, or inputs not recorded into the buffer before `Reconcile`).
  - **Corrections spike only under packet loss/latency** → the input buffer isn't retaining enough
    un-ACKed inputs to replay, or sequence numbers are mismatched. Check every predicted input is
    stamped with a monotonic sequence number and stored before it's sent.
  - **Magnitude ~0 but still visually snapping** → the problem is render interpolation/smoothing, not
    prediction; look outside `ClientPrediction`.

---

## How to use an entry (workflow)

1. Match your symptom to a row. No row? Start with the First-response toolkit, then route via the
   "When to use" table.
2. Run the row's **discriminating experiment** first — do not edit yet.
3. If the experiment shows the fix is **present and correct**, the bug is a *look-alike*; follow the
   "if the loop/signature is present…" branch in that row, or fall back to crash tooling.
4. If the experiment shows the fix **regressed**, restore the expected state described in the row.
   Respect `sparkengine-architecture-contract` (service locator, wiring rules) when doing so.
5. Re-verify with the same grep, and re-run the relevant regression test (see test selectors above).

## Provenance and maintenance

- Built from the deferred-issue list in the retired `HARDEN_FLEET_HANDOFF.md` (git history only since 2026-09-03) (git history only since 2026-09-03) and last re-verified
  on **2026-08-23** against the working tree (uncommitted changes ahead of `0e1fe7e7`) by
  reading source. Rows 1–7 are verified **FIXED**; row 7 additionally has a clean current-source
  `SparkTests` build plus `TestSequencerAudioWiring.cpp` at 3/3 tests, 18/18 assertions. Rows 8–9
  describe the **current** ECS/prediction model.
- Historical note: an earlier revision of this file carried a stale provenance paragraph claiming
  `StageBasedExecutor` was still ticked and `PhaseSystemManager` unwired. That described the
  pre-2026-07-18 state and contradicted row 8; it was removed 2026-08-23 after re-verification —
  `StageBasedExecutor` no longer exists in source, and `PhaseSystemManager` is built by
  `CreatePhaseSystemManager` and ticked every frame under `SPARK_GUARDED_UPDATE("ECS_Phases", …)`.
- Line numbers are advisory; grep the named symbols. One-line re-verification commands (repo root):
  ```
  # Row 1  BT deep-copy present (also: rg -n "GetRoot\(\)->Clone" SparkEngine/Source/Engine/AI)
  rg -n "std::unique_ptr<BTNode> Clone" SparkEngine/Source/Engine/AI/BehaviorTreeNodes.h
  # Row 2  weapon owner threaded
  rg -n "ownerEntity" SparkEngine/Source/Engine/Gameplay/WeaponManager.cpp
  # Row 3  editor world-swap funnel
  rg -n "SwapWorld|OpenScene" SparkEditor/Source/Core/EditorUI.cpp
  # Row 4  monotonic dirty tracking
  rg -n "m_editSequence|m_savedSequence" SparkEditor/Source/UndoRedo/UndoRedoManager.h
  # Row 5  MMO resolves via injected context
  rg -n "m_context->Get|::GetInstance\(\)" GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp
  # Row 6  localization returns by value
  rg -n "GetString" SparkEngine/Source/Engine/Localization/LocalizationSystem.h
  # Row 7  service queue + game-thread drain + startup backend ownership
  rg -n "SetAudioBackend|DispatchPendingAudioCues|SetAudioDispatchCallback" SparkEngine/Source
  rg -n "SequencerAudio_" Tests/TestSequencerAudioWiring.cpp
  # Row 8  phase manager built + ticked; StageBasedExecutor gone from source
  rg -n "AddSystem<|CreatePhaseSystemManager" SparkEngine/Source/Core/EngineSetup.h
  rg -n "ECS_Phases" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp
  rg -l "StageBasedExecutor" SparkEngine SparkEditor GameModules   # expect zero hits
  # Row 9  prediction reconcile + correction magnitude
  rg -n "Reconcile|GetLastCorrectionMagnitude" SparkEngine/Source/Engine/Networking/ClientPrediction.h
  # First-response toolkit
  rg -n "SPARK_TEST_NAME|SPARK_TEST_FILE" Tests/TestMain.cpp
  rg -n "fault\.status|fault\.reset" SparkEngine/Source/Core/FaultIsolation.cpp
  rg -n "crash_mode" SparkConsole/src/ConsoleApp.cpp
  ```
- When you fix a genuinely new subsystem symptom, add a row here (symptom → where → expected state →
  discriminating experiment) and record durable lessons in the project wiki per `CLAUDE.md`.
