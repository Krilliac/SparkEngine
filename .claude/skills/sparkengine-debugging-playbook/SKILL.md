---
name: sparkengine-debugging-playbook
description: >-
  SparkEngine subsystem-specific symptom lookup table: you observed a concrete wrong behavior in
  a SparkEngine subsystem (AI behavior trees, weapons, the ImGui editor, undo/redo, MMO modules,
  localization, cinematic sequencer, ECS system ticking, or client-side network prediction) and
  need to know where to look, what the correct code should look like, and a discriminating
  experiment to confirm the cause before changing anything.
  TRIGGER when the user says things like "cloned behavior tree does nothing", "weapon damage is
  attributed to entity 0", "editor crashes when I open a scene", "editor won't prompt to save",
  "MMO module uses the wrong NetworkManager", "localized string is garbage", "cutscene has no
  audio", "my ECS system never ticks / never runs", or "multiplayer is rubber-banding / desyncing".
  DO NOT TRIGGER for generic C++ crashes, undefined-behavior hunts, or compiler/linker error
  messages (use the global cpp-crash-triage and msvc-error-decoder skills), nor for a chronological
  "what changed and why" narrative (use the sibling sparkengine-failure-archaeology skill).
trilobite_compatible: true
trilobite_role: debugging
---

# SparkEngine Debugging Playbook

A **symptomatic lookup table** for SparkEngine subsystems: *"You are seeing X. Look at Y. Here is
how to confirm it's the cause Z before you touch code."* Each entry gives a **discriminating
experiment** — a cheap check that distinguishes the suspected cause from a look-alike, so you don't
start editing the wrong file.

Glossary (defined once):
- **RHI** — Render Hardware Interface, SparkEngine's rendering abstraction (D3D11/D3D12/Vulkan/…).
- **ECS** — Entity Component System (EnTT-backed). A *system* is a per-frame update over entities.
- **UAF** — use-after-free.
- **DI / injected context** — `EngineContext` handed to a game-module DLL via `Initialize(context)`,
  as opposed to a process-global singleton.

## When to use this skill / when NOT to

| Situation | Use |
|-----------|-----|
| A named SparkEngine subsystem misbehaves in a specific way (table below) | **this skill** |
| Generic segfault / heap corruption / UB with no subsystem lead | global `cpp-crash-triage` |
| A raw MSVC/clang compile or link error message | global `msvc-error-decoder` |
| "How did we get here / what was the history of this bug" | sibling `sparkengine-failure-archaeology` |
| The architecture rules a fix must respect (service locator, wiring, thresholds) | `sparkengine-architecture-contract` |

Do NOT paste generic C++ advice here. This file only carries SparkEngine-specific symptom→cause maps.

## Important: most historical bugs below are already FIXED

This table was built from `HARDEN_FLEET_HANDOFF.md`'s deferred-issue list and then **re-verified
against the live code on 2026-07-07**. Eight of the nine listed issues have since been fixed. The
value of the table today is twofold:

1. If you observe the symptom, the row tells you the **fix that should be present** so you can spot a
   **regression** (someone reverted or broke the fix).
2. The one **partially-fixed** issue (Sequencer audio) and the ECS ticking model are live gotchas.

Every file/line reference was checked on 2026-07-07 (branch `Working`). Line numbers drift — grep the
named symbol, do not trust the number blindly.

---

## Triage table

### 1. Cloned behavior tree does nothing / has no root (AI)

- **Symptom**: An AI agent whose behavior tree was produced by cloning a template just idles;
  the cloned tree appears empty or its root child is missing.
- **Where to look**: `SparkEngine/Source/Engine/AI/BehaviorTreeTypes.h` (`BTNode::Clone`) and
  `SparkEngine/Source/Engine/AI/BehaviorTreeNodes.h` (per-node `Clone()` overrides).
- **Expected (fixed) state**: `Clone()` is **pure virtual** (`virtual std::unique_ptr<BTNode> Clone() const = 0;`).
  Every node type deep-copies: composites (`SequenceNode`, `SelectorNode`, `ParallelNode`) loop their
  children calling `child->Clone()`; decorators (`InverterNode`, `RepeaterNode`) clone `m_child`;
  leaves (`ActionNode`, `ConditionNode`, `WaitNode`) copy their payload.
- **Discriminating experiment**: Grep the node header for the `Clone()` bodies:
  ```
  rg -n "std::unique_ptr<BTNode> Clone" SparkEngine/Source/Engine/AI/BehaviorTreeNodes.h
  ```
  If a composite's `Clone()` returns `std::make_unique<SequenceNode>(m_name)` **without** the
  `for (child) cloned->AddChild(child->Clone())` loop, the deep-copy regressed → cloned trees lose
  their children. If the loop is present, the bug is elsewhere (check the *template* was populated
  before cloning, and that `AISystem` actually calls `Clone()` for unseen agent types).

### 2. Weapon damage/kills attributed to entity 0 (Gameplay/FPS)

- **Symptom**: `WeaponFireEvent.ownerEntity` is always `0`; damage/kill attribution, hit markers, or
  kill feed all credit entity 0.
- **Where to look**: `SparkEngine/Source/Engine/Gameplay/WeaponManager.cpp` — the call chain
  `Update` → `ProcessWeapon` → `HandleFiring` → emit `WeaponFireEvent`.
- **Expected (fixed) state**: the shooter entity id is threaded through the whole chain:
  `ProcessWeapon(uint32_t ownerEntity, …)` receives the entity from the ECS view
  (`view.each()` → `ProcessWeapon(static_cast<uint32_t>(entity), …)`), passes it to
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
  panel-rewire block; `CommandHistory` (undo stack that may close over old entities).
- **Expected (fixed) state**: `EditorUI::SwapWorld(std::unique_ptr<World>)` is the single funnel:
  it (a) clears undo history first — `Spark::Editor::CommandHistory::GetInstance().Clear();` — then
  (b) `m_world = std::move(newWorld);` then (c) rewires every panel's `SetWorld(m_world.get())`.
  `OpenScene()` and the initial world creation both route through `SwapWorld()`.
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
  `m_editSequence` increments on every mutating op (execute/merge/undo/redo); `MarkSaved` snapshots it
  into `m_savedSequence`; `HasUnsavedChanges()` returns `m_editSequence != m_savedSequence`.
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
- **Expected (fixed) state**: the module stores the injected context (`m_context = context;` in
  `Initialize`) and resolves subsystems through it — e.g. `auto* netMgr = m_context ? m_context->GetNetwork() : nullptr;`
  — **not** through a process-global `SomeManager::GetInstance()`.
- **Discriminating experiment**:
  ```
  rg -n "m_context->Get|::GetInstance\(\)" GameModules/SparkGameMMO/Source
  ```
  `SimpleConsole::GetInstance()` and `SeamlessAreaManager::GetInstance()` are **legitimate** shared
  singletons — ignore those. The red flag is resolving *network / physics / graphics / the engine
  context itself* via `GetInstance()` instead of `m_context->GetX()`. If you find that, the DI fix
  regressed and the module is on a stale instance.

### 6. Localized string is intermittent garbage or crashes (Localization)

- **Symptom**: Reading a localized string occasionally returns corrupted text or crashes, especially
  under concurrent access.
- **Where to look**: `SparkEngine/Source/Engine/Localization/LocalizationSystem.h` / `.cpp` —
  `GetString`, and `Format` (which calls `GetString`).
- **Expected (fixed) state**: `std::string GetString(const std::string& key) const;` returns **by
  value**. (Returning `const std::string&` into an internal map after releasing the lock is the classic
  dangling-reference bug.)
- **Discriminating experiment**:
  ```
  rg -n "GetString" SparkEngine/Source/Engine/Localization/LocalizationSystem.h
  ```
  If the signature is `const std::string& GetString(...)` (reference return), the dangling bug is back.
  Value return is safe. Note `Format` binds `const std::string& tmpl = GetString(key);` — that is a
  const-ref to a **temporary** (lifetime-extended) and is fine *only while* `GetString` returns by value.

### 7. Cinematic plays visuals but no audio (Sequencer) — PARTIALLY WIRED

- **Symptom**: A `Sequence` plays animation/property/event tracks correctly, but `AudioCue` tracks
  produce no sound.
- **Where to look**: `SparkEngine/Source/Engine/Cinematic/Sequencer.h` (`AudioCallback` typedef,
  `SetAudioCallback`) and `Sequencer.cpp` (the per-frame dispatch loop and `SetAudioCallback`).
- **Verified current state (2026-07-07)**: the **mechanism exists** — `using AudioCallback = std::function<void(const AudioCue&)>;`,
  `void SetAudioCallback(AudioCallback)`, and a dispatch loop that fires
  `m_audioCallback(*cue)` for each triggered cue **only when `m_audioCallback` is set**. BUT: grep
  finds **no caller** of `SetAudioCallback` anywhere in the engine or game modules. So by default the
  callback is null and cues silently no-op. This is the one issue that is only *partially* resolved.
- **Discriminating experiment**:
  ```
  rg -n "SetAudioCallback" SparkEngine GameModules SparkEditor
  ```
  If the only hits are the declaration (`Sequencer.h`) and definition (`Sequencer.cpp`), then **nothing
  wires audio** — the fix is to have the cinematic owner call
  `sequence.SetAudioCallback([audio](const AudioCue& c){ /* play c */ });` at setup. If a caller does
  exist and audio still fails, check `AudioCueTrack::GetTriggeredCues(prevTime, currentTime)` and that
  `m_previousTime`/`m_currentTime` actually advance (a zero-length step triggers nothing).

### 8. A registered ECS system silently never ticks (ECS)

- **Symptom**: You added an ECS system, it compiles, but its `Update` never runs — entities aren't
  processed and no error is raised.
- **Where to look**: three names historically collided; here is the **verified current model
  (2026-07-18)** — `PhaseSystemManager` is now the wired canonical manager:
  | Class | File | Role |
  |-------|------|------|
  | `PhaseSystemManager` | `SparkEngine/Source/Engine/ECS/Systems/PhaseSystemManager.h` | **Canonical and WIRED (2026-07-18).** `GameplayLifecycleShared.cpp` calls `CreatePhaseSystemManager(ctx)` during gameplay init and pumps `UpdateAll(*world, dt)` every frame in `Phase` enum order. Regression test: `Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp`. |
  | `SystemManager` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` | **Legacy** flat, insertion-order manager. Do not add new systems here. |
  | `StageBasedExecutor` | *(deleted 2026-07-18)* | Was a live tick over zero registered systems; removed when `PhaseSystemManager` was wired. |
  | `ParallelSystemExecutor` | `ParallelSystemExecutor.h` | A separate access-set batcher, **not wired** into the live tick; `PhaseSystemManager::UpdateAll` is serial (see its `@note`). See `sparkengine-job-system-threading §1c/§1d`. |
- **The three ways a system silently doesn't tick**:
  1. **Not added to `CreatePhaseSystemManager`.** New systems must be registered in
     `Spark::CreatePhaseSystemManager(ctx)` (`SparkEngine/Source/Core/EngineSetup.h`) with the correct
     `Phase::` bucket — that is the set the gameplay lifecycle creates and ticks. A system registered
     into the legacy `SystemManager` or constructed ad hoc will not tick.
  2. **Registered via the flat overload.** `AddSystem<T>(Phase::AI, …)` is phased;
     `AddSystem<T>(…)` (no `Phase`) drops into the **flat list that runs LAST, after all phases**. If
     ordering matters (you read another system's output), the flat overload is the cause.
  3. **Its subsystem dependency was null at registration.** In `CreatePhaseSystemManager`, physics,
     audio, and render systems are added **only if** `ctx.GetPhysics()/GetAudio()/GetGraphics()` are
     non-null. In headless / `NullRHIDevice` runs `GetGraphics()` may be null → `RenderSystem` is never
     registered and never ticks (by design).
- **Discriminating experiment**:
  ```
  rg -n "AddSystem<|UpdateAll" SparkEngine/Source/Core/EngineSetup.h
  ```
  Then confirm your system appears with a `Phase::` argument and that the owner of that manager calls
  `UpdateAll` every frame. To prove it ticks, put a one-shot log at the top of your `Update`; if it
  never prints, it's cause 1 or 3, not a logic bug inside `Update`.

### 9. Multiplayer rubber-banding / desync on the client (Networking / prediction)

- **Symptom**: The local player snaps/teleports back after moving (rubber-banding), or client and
  server positions drift apart.
- **Where to look**: `SparkEngine/Source/Engine/Networking/ClientPrediction.h` / `.cpp` —
  `Reconcile(serverState, dt)` re-applies un-ACKed inputs after a server correction;
  `GetLastCorrectionMagnitude()` reports the size of the last correction. It is wired into
  `GameModules/SparkGameFPS/Source/Game/MultiplayerSystem.cpp` and the MMO/TF nets.
- **Discriminating experiment**: Log `GetLastCorrectionMagnitude()` each time a server state arrives.
  - **Consistently large corrections** → the client's local simulation disagrees with the server's
    (different movement constants, or inputs not being recorded into the buffer before `Reconcile`).
  - **Corrections spike only under packet loss/latency** → the input buffer isn't retaining enough
    un-ACKed inputs to replay, or sequence numbers are mismatched. Check that every predicted input is
    stamped with a monotonic sequence number and stored before it's sent.
  - **Magnitude ~0 but still visually snapping** → the problem is render interpolation/smoothing, not
    prediction; look outside `ClientPrediction`.

---

## How to use an entry (workflow)

1. Match your symptom to a row.
2. Run the row's **discriminating experiment** first — do not edit yet.
3. If the experiment shows the fix is **present and correct**, the bug is a *look-alike*; follow the
   "if the loop/signature is present…" branch in that row, or fall back to `cpp-crash-triage`.
4. If the experiment shows the fix **regressed**, restore the expected state described in the row.
   Respect `sparkengine-architecture-contract` (service locator, wiring rules) when doing so.
5. Re-verify with the same grep.

## Provenance and maintenance

- Built from `D:\SparkEngine\HARDEN_FLEET_HANDOFF.md` (handoff deferred-issue list) and re-verified
  against the live `Working` branch on **2026-07-07**. Result: rows 1–6 verified **FIXED**, row 7
  (Sequencer audio) verified **partially wired** (mechanism present, no caller), rows 8–9 describe the
  **current** ECS/prediction model. Note the ECS-manager reality (row 8): `StageBasedExecutor`
  **exists** (`ParallelSystemExecutor.h:378`) and its `ExecuteAll(dt)` is ticked every frame from
  `GameplayLifecycleShared.cpp:1062`, but has **no `RegisterSystem` caller** (dead tick);
  `PhaseSystemManager` is defined-not-wired (zero callers). None of the three managers is a
  cleanly-wired home — an OPEN architectural weak point (see `sparkengine-architecture-contract`
  Invariant 3 and `sparkengine-job-system-threading §1c`).
- Line numbers are advisory; grep the named symbols. One-line re-verification commands:
  ```
  # Row 1  BT deep-copy present
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
  # Row 7  is SetAudioCallback ever called? (only decl+def == not wired)
  rg -n "SetAudioCallback" SparkEngine GameModules SparkEditor
  # Row 8  ECS managers: intended-but-unwired vs the live-but-empty tick
  rg -n "AddSystem<|UpdateAll|CreatePhaseSystemManager" SparkEngine/Source/Core/EngineSetup.h
  rg -n "StageBasedExecutor::GetInstance\(\).ExecuteAll" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp
  rg -rn "RegisterSystem\(" SparkEngine/Source | rg -i stage   # expect only the definition (dead tick)
  # Row 9  prediction reconcile + correction magnitude
  rg -n "Reconcile|GetLastCorrectionMagnitude" SparkEngine/Source/Engine/Networking/ClientPrediction.h
  ```
- When you fix a genuinely new subsystem symptom, add a row here (symptom → where → expected state →
  discriminating experiment) and record durable lessons in the project wiki per `CLAUDE.md`.
- Validate edits to this file by running the global skill-linter (`~/.claude/skills/skill-linter/`)
  against this folder: pass `-Path .claude/skills/sparkengine-debugging-playbook`.
