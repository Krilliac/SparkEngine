# SparkEngine Fix Apply-Plan — 2026-07-06

Synthesis of the fix-authoring + feature-design fan-out. Fix bodies (exact
`exactOldString`/`exactNewString`) live in the raw fan-out JSON, referenced here
by **lane**. This document is the ordering, counts, architectural backlog,
feature designs, and execution sequence.

**Totals:** 68 directly-appliable fixes across 12 fix-lanes; 4 complex/architectural
items deferred; 3 feature-design streams.

---

## 1. Apply order

Ordered so remote/trivially-exploitable security-criticals land first, then
memory-safety/crash criticals, then data-integrity + auth hardening, then
defensive robustness. Each lane's hunks are internally non-overlapping (per each
lane's `complexPlan`); apply header hunks before their `.cpp` companions.

### Batch A — remote-exploitable, security-critical (do first)

**Lane `script-security`** — VisualScript codegen injection + AngelScript ctor sandbox bypass (16 hunks, all in `SparkEngine/Source/Engine/Scripting/`)
- `VisualScriptCompiler.cpp` — add `#include <cctype>` (enables the new helpers).
- `VisualScriptCompiler.cpp:161` — add `EscapeAngelScriptString` + `SanitizeIdentifier` helpers (both primitives every other hunk depends on).
- `VisualScriptCompiler.cpp:161` (ConstString `PinKind::String`) — escape spliced default string.
- `VisualScriptCompiler.cpp:386-395` (GetKeyDown/GetKey) — escape `key` property.
- `VisualScriptCompiler.cpp:416-422` (GetEntityByName) — escape `name` property.
- `VisualScriptCompiler.cpp:440-448` (PlaySound) — escape `sound` property.
- `VisualScriptCompiler.cpp:449-457` (PlayAnimation) — escape `animation` property.
- `VisualScriptCompiler.cpp:458-466` (SpawnEntity) — escape `name` property.
- `VisualScriptCompiler.cpp:470-478` (FireEvent) — escape `event` property.
- `VisualScriptCompiler.cpp:528-542` (GetVariable/SetVariable) — sanitize var name to bare identifier (no-quote injection class).
- `VisualScriptCompiler.cpp:547-550` (CallFunction) — sanitize call-target name.
- `VisualScriptCompiler.cpp:707-712` (Compile, OnKeyPress guard) — escape `key`.
- `VisualScriptCompiler.cpp:838-853` (Compile, FunctionGraph) — sanitize method + param names.
- `VisualScriptCompiler.cpp:876-889` (Compile, CustomEventDef) — sanitize handler + param names.
- `VisualScriptCompiler.cpp:636-650` (Compile, class header) — sanitize `className` + member `var.name`. (Residual: `VariableDecl.defaultValue` still raw — see §3.)
- `AngelScriptEngine.cpp:684` (AttachScript) — install `ScriptSandbox::LineCallback` around the factory/ctor `Execute()` so ctor obeys instruction/time/memory limits.

**Lane `combat-cheats`** — client-trusted WeaponId + unlimited-fire-rate (5 hunks, `GameModules/SparkGameMMOFPS/Source/Game/`)
- `TFWeaponSystem.h` — declare `IsWeaponInLoadout`; split `ShooterState` into per-weapon `WeaponFireState` map. (Apply both header hunks first.)
- `TFWeaponServer.cpp` — implement `IsWeaponInLoadout` (after `ServerNow()`).
- `TFWeaponServer.cpp:72` (ServerHandleFire) — reject fire whose `weaponId` isn't in the shooter's loadout; seat-weapon path exempt.
- `TFWeaponServer.cpp:124` (ValidateFire) — per-weapon-id RoF token bucket via `try_emplace`, closing the alternate-weapon bucket refill.

**Lane `net-auth`** — auth-bypass / replication break (1 appliable hunk, `SparkEngine/Source/Engine/Networking/`)
- `NetworkConnection.cpp:789` (ProcessIncoming validation gateway) — derive `isAuthenticated` from server-trusted `m_clientAddresses` (server) / `m_connectionState` (client), never the wire `senderID`. (Encryption pipeline is deferred — §3.)

**Lane `proc-inject`** — command-line argument injection (2 hunks, `SparkEngine/Source/Utils/`)
- `ProcessWin32.cpp` — add `AppendQuotedArg` (CommandLineToArgvW-compatible quoting) in the anonymous namespace.
- `ProcessWin32.cpp` (Launch cmdLine build) — use `AppendQuotedArg` for exe + every arg. Fixes all callers (ConsoleProcessManager, CrashHandler, DaemonLifecycle) transparently.

**Lane `game-net-safety`** — NaN/Inf tick-hang DoS, faction cheat, vehicle eject scaling (4 hunks, `GameModules/SparkGameMMOFPS/Source/`)
- `Net/TFServerSim.cpp:287` (StepPlayer) — `std::isfinite` guard on client viewYaw/viewPitch before WrapPi/clamp (WrapPi never terminates on ±Inf → tick hang). (Sibling seated-passenger path at `TFServerSim.cpp:219-220` left for fast-follow.)
- `Net/TFServerSim.h` — add public `IsPlayerAlive(PlayerId)` accessor.
- `Net/TFClientNetHandlers.cpp:199` (RouteLoopback FactionSelect) — gate on `IsPlayerAlive` so listen-host can't team-swap mid-life.
- `Game/TFVehicleSystem.cpp:874` (DestroyVehicle) — scale eject damage by the occupant's own `ClassDef.health+shield`, not a hardcoded 1000 (Colossus 2200/0 was under-punished).

### Batch B — memory-safety / crash criticals

**Lane `audio-3d`** — stack OOB read (1 hunk, `SparkEngine/Source/Audio/`)
- `AudioEngine.cpp` (Apply3DAudioToSource) — size the SetOutputMatrix buffer by actual (clamped) src×dst channel counts; a stereo source overran a 2-float array.

**Lane `editor-uaf`** — undo use-after-free + orphaned children + dirty-flag under-report (6 hunks, `SparkEditor/Source/`)
- `Panels/HierarchyPanel.cpp:1259` (ResetToDefault) — `CommandHistory::Clear()` before freeing `m_ownedScene` (stale command Ctrl+Z = UAF).
- `Panels/HierarchyPanel.cpp:981` (DeleteObject) — recursively delete subtree so no orphan keeps a dead `parentID`.
- `Core/EditorUI.h` — add `#include "../CommandHistory.h"`; then `IsSceneModified()` ORs in `CommandHistory::IsModified()`.
- `Core/EditorUI.cpp` (SaveCurrentScene) — `MarkSaved()` after save.
- `Core/EditorUI.cpp` (OpenScene) — `MarkSaved()` after open. (Residual: OpenScene World-backed path doesn't Clear() — §3.)

**Lane `asyncdb`** — SyncQuery/worker data race + `?N` placeholder + getline truncation (6 hunks, `SparkEngine/Source/Engine/Persistence/`)
- `AsyncDatabase.cpp` (Open) — create + prepare a dedicated `m_syncConnection`.
- `AsyncDatabase.cpp` (PrepareStatement) — also register late statements on `m_syncConnection`.
- `AsyncDatabase.cpp` (Close) — close/reset `m_syncConnection`.
- `AsyncDatabase.cpp` (SyncQuery) — execute against `m_syncConnection`, not `m_connections[0]` (the actual race fix).
- `AsyncDatabase.cpp` (`?N` substitution) — reject digit-suffixed false-prefix matches so `?1` doesn't match inside `?10`/`?11`.
- `AsyncDatabase.cpp` (ExecuteRaw SET) — drain `stream.rdbuf()` instead of `getline` so embedded-newline values aren't truncated.

**Lane `module-mgr`** — DLL probe crash + legacy module leak (2 hunks, `Source/Core/`)
- `ModuleManager.cpp` (DiscoverModules) — load probe DLLs with flags `0`, not `DONT_RESOLVE_DLL_REFERENCES` (calling exports on an uninitialized image is UB/crash).
- `ModuleManager.cpp` (LegacyModuleAdapter dtor) — call `m_legacyDestroyFn(m_legacy)` so legacy modules aren't leaked on unload/reload.

### Batch C — data-integrity criticals + auth hardening

**Lane `onboarding-auth`** — password strength, corrupt-db wipe, silent save failures (10 hunks, `GameModules/SparkGameMMOFPS/Source/`) — **in-flight lane, see §5**
- `Account/TFAccountSystem.cpp:29` (HashPassword) — 100k-round iterated salted mix (interim; real KDF in §3).
- `Account/TFAccountSystem.h` — add `TFAuthErr::PasswordTooShort` (appended, ordinals preserved).
- `Account/TFAccountSystem.cpp:51` (Register) — reject passwords `< 8`.
- `Persistence/TFDatabase.cpp` — add `#include "Utils/LogMacros.h"`.
- `Persistence/TFDatabase.cpp:68` (Open) — on parse-fail of an existing file, quarantine to `.corrupt-<ms>.bak` and refuse (prevents silent empty-db overwrite).
- `Persistence/TFDatabase.cpp:219/241/263/304/319` — log `SPARK_LOG_ERROR` on discarded `SaveToDisk()` failures (CreateAccount, TouchLogin, CreateCharacter, DeleteCharacter, SaveCharacterProgress).

**Lane `reflection-serial`** — enum/mask fields dropped on scene round-trip + archetype dup-component abort (5 hunks, `SparkEngine/Source/`)
- `Core/ComponentReflection.cpp` (SetFieldFromString) — add `FieldType::Enum` case (enum-as-int).
- `Core/ComponentReflection.cpp` (GetFieldAsString) — symmetric `FieldType::Enum` read.
- `Core/ComponentReflection.cpp` — register RigidBody `type`/`motionQuality` + Collider `shape`.
- `Core/ComponentReflection.cpp` — register CollisionMask/VisibilityMask/CameraDrawMask uint32 fields.
- `Engine/ECS/EntityArchetypeLoader.cpp` (ApplyComponentViaReflection) — `HasComponent` guard so a duplicate archetype line doesn't `SPARK_REQUIRE`-abort the loader.

### Batch D — defensive robustness

**Lane `tests-gates`** — RHIBridge null-device deref guards (10 hunks, `Graphics/RHI/RHIBridge.cpp`)
- `:456` CreateVertexBuffer, CreateIndexBuffer, CreateConstantBuffer, CreateTexture2D, CreateDepthBuffer, CreateRenderTarget, CreateSamplerLinearWrap, CreateSamplerLinearClamp+PointClamp, CreateSamplerAnisotropic — early `if (!m_device) return nullptr;`.
- `:606` GetCapabilities/GetFrameStatistics — ternary-guard to a function-local static default (reference-returning, can't return null). (2 architectural test-coverage items are deferred — §3.)

---

## 2. Directly-appliable fix count per lane

| Lane | Appliable fixes | Highest severity | Target (build unit) |
|---|---:|---|---|
| script-security | 16 | critical | SparkEngine (Scripting) |
| onboarding-auth | 10 | critical | SparkGameMMOFPS |
| tests-gates | 10 | high | SparkEngine (Graphics/RHI) |
| editor-uaf | 6 | critical | SparkEditor |
| asyncdb | 6 | critical | SparkEngine (Persistence) |
| combat-cheats | 5 | critical | SparkGameMMOFPS |
| reflection-serial | 5 | critical | SparkEngine (Core/ECS) |
| game-net-safety | 4 | critical | SparkGameMMOFPS |
| module-mgr | 2 | critical | Source/Core (editor/app) |
| proc-inject | 2 | critical | SparkEngine (Utils) |
| net-auth | 1 | critical | SparkEngine (Networking) |
| audio-3d | 1 | critical | SparkEngine (Audio) |
| **Total** | **68** | | |

---

## 3. Complex / architectural (non-appliable — needs design work)

### `net-auth` — wire encryption / rate-limit / replay pipeline (`NetworkManager.cpp`, critical)
The live send/recv path is fully plaintext, unauthenticated beyond the (now-fixed)
IP:port table, replayable, and floodable. `NetworkEncryption.h` primitives
(SessionKey, EncryptPacket/DecryptPacket, RateLimiter, ReplayProtection,
ConnectionToken) exist but are wired only into the unused `NetworkStack` scaffolding.
**Plan:** per-connection SessionKey/ReplayProtection + server RateLimiter (Step 1);
handshake in HandleConnect/ConnectAccepted carrying token+key (Step 2); encrypt at
the SendRawTo call sites, decrypt+rate-limit+replay-check before Deserialize in
ProcessIncoming (Step 3); ordered pipeline RateLimit→Decrypt→Deserialize→Replay→auth
gateway (Step 4); `m_encryptionEnabled` config + LAN fallback (Step 5); integration
test asserting no plaintext on the wire, replay/HMAC/flood rejection (Step 6). Prefer
`NetworkEncryption.h` over the `NetworkSecurity.h` XOR toy; flag NetworkStack for
removal once wired. Changes wire format → needs live end-to-end test; ship as its own PR.

### `script-security` — ScriptSandbox function whitelist is inert (`ScriptSandbox.cpp:111`, high)
`IsFunctionAllowed`/AddAllowed/AddBlocked/SetSecurityLevel(Strict) have zero call sites;
every engine-native function is registered as a direct `asCALL_CDECL` with no gate, so
even "Strict" scripts get the full API. **Plan:** gate at *registration* time, not per-call.
Reorder Initialize() so `m_sandbox` is constructed before `RegisterEngineAPI()`; add
`ConfigureSandboxSecurity(...)` pre-init hook; add `RegisterGuardedFunction(decl, scriptName, fn)`
that skips registration when `!m_sandbox->IsFunctionAllowed(scriptName)`; convert all
~20 `RegisterGlobalFunction` call sites; warn when Strict + empty whitelist; document that
post-Initialize whitelist changes don't take effect (AngelScript can't unregister globals);
do NOT gate RegisterObjectType/Property. Acceptance: Strict + allow-only `print` → a
`destroyEntity(0)` script fails to compile.
- **Residual (in-lane, not fixed):** `VisualScriptCompiler.cpp` `VariableDecl.defaultValue`
  is still spliced as a raw literal expression — needs a type-aware literal validator, not a
  one-line escape (naive escaping corrupts legit `Vector3(1,2,3)` values).

### `tests-gates` — SparkDaemon Windows test coverage gap (`Tests/CMakeLists.txt:472`, architectural)
7 of 9 SparkDaemon test files are `#if __linux__||__APPLE__`-gated, and `DaemonClient::Connect()`
is a hard Windows stub — so widening the guards alone would assert-fail, not skip. **Plan:**
(1) implement `DaemonClient::Connect/Request/Disconnect` for `_WIN32` via CreateFileA/WaitNamedPipe
against DaemonServer's existing `\\.\pipe\` naming; (2) widen the 7 test guards + replace POSIX
readiness helpers (stat/unlink/sun_path) with a retry-connect loop; (3) update the stale
DaemonClient.h doc comment. This is a real feature (Windows named-pipe client transport) — its
own PR; pilot on TestDaemonFoundation.cpp first.

### `tests-gates` — TestAsyncDatabase.cpp tests a mock, not the real code (architectural)
All 23 TEST() cases (383 lines) reimplement a local `Spark::Persistence` mock whose shapes
diverge from the real header (public fields + throwing accessors vs. private state + safe
getters; real `AsyncDatabasePool`/`IDatabaseConnection`/SQLite worker pool unmodeled). **Plan:**
full rewrite against the real header — real pool over in-memory/temp SQLite, real async API,
assertions rewritten to real field/throw semantics (EXPECT_THROW for type-mismatch). ~300+ lines,
needs the real IDatabaseConnection/SQLiteConnection surface read first + build/run verification;
dedicated follow-up, not a blind patch.

---

## 4. Feature designs

### Stream `sp4-dehardcode` — data-drive the remaining hardcoded literals (SparkGameMMOFPS)

**Approach.** Extend the frozen `TFDataTables` JSON-loader with new fields/tables, author the
JSON, swap each C++ literal for a lookup — the same pattern TERRAFRONT already uses for
weapons/vehicles. Two plumbing pieces carry everything: (1) add `structureMaterial` to
`FactionDef` + one shared `FactionStructureMaterial(ctx, FactionId)` helper replacing the
4×-duplicated faction→material switch (TFDeployableSystem, TFVehicleSystem, TFVehicleNet,
TFPlayerSystemClient); (2) two new tables `deployables.json` (per-kind model+scale) and
`presentation.json` (skybox/terrain/ambient/viewmodel/muzzle-FX/pawnMesh constants) consumed
via a `WorldPresentationDef` whose member-initializer defaults == today's magic numbers, so
behavior is byte-identical with zero scattered fallbacks. Vehicle meshes are already data-driven.
Explicitly out of scope: TFDeployableSystem `kStats` (W4 balance pass).

**Files.** Modify (careful, hand-written): `Data/TFDataTables.h` (new structs/accessors, `FactionDef`
field), `Data/TFDataTables.cpp` (ParsePresentation/ParseDeployables, wire into LoadAllInternal/ReloadAll).
New (careful): `Game/TFVisualUtils.h` (the shared helper — single point of failure). New (mechanical):
`Assets/MMOFPS/Data/deployables.json`, `presentation.json`. Modify (mechanical): `factions.json` (+field
×3), the 4 call-site files, `World/TFWorldSetup.{h,cpp}` (`Pres()` accessor + literal→field swaps),
`Tests/TestTFDataTables.cpp`, `DESIGN.md`.

**Units.** (1) FactionDef field + ParseFactions + JSON. (2) WorldPresentationDef + presentation.json +
ParsePresentation. (3) DeployableVisualDef + deployables.json + ParseDeployables. (4) TFVisualUtils
helper — **review before touching call sites** (a wrong fallback silently miscolors every
pawn/vehicle/deployable). (5) Swap the 4 call sites. (6) TFWorldSetup literal swaps, one block at a
time (skybox→terrain→wind→viewmodel→muzzle-FX). (7) Test coverage. (8) DESIGN.md addendum. (9) Build +
screenshot.

**Verification.** Two layers, matching the repo. (a) Headless: `TestTFDataTables.cpp` runs standalone
against the real Data files — both new JSON parse, closed-vocabulary/uniqueness, and `AssetExists()`
on every new path. (b) Screenshot: launch SparkGameMMOFPS, load a scene with each faction's
pawn/vehicle/deployable, before/after diff should show **zero visual change** (every default == old
constant). Risks flagged: FactionDef is a frozen contract (needs DESIGN.md addendum), presentation.json
is a 6th data file where DESIGN enumerates 5, render-path regressions are invisible to the headless
test (screenshot is mandatory), and hot-reload must include both new files in ReloadAll.

**Offloadable to local 7B** (opt-in `ToolSearch "select:mcp__local-llm__offload"`, then review):
authoring `deployables.json`/`presentation.json` (all values quoted verbatim in the design — pure
transcription, few-shot with vehicles.json), the `ParsePresentation`/`ParseDeployables` loader
functions (copy-adapt ParseFactions), the 4 call-site literal→lookup replacements + TFWorldSetup
constant→`Pres().*` swaps (rote find/replace), the TestTFDataTables additions (structural copy of
existing weapon test pairs), the DESIGN.md line. **NOT offloadable:** the `TFVisualUtils` helper
(single failure point across 4 owners — verify fallback at runtime), and the `WorldPresentationDef`/
`DeployableVisualDef` struct field lists (frozen public surface — a miscopied field name propagates).

### Stream `collab-server` — reflected per-field edit deltas over the existing collab substrate (SparkEditor)

**Approach.** The host-mediated TCP star (`CollaborativeEditSession`), the `EditMessage` wire fields
(componentType/propertyName/newValue/oldValue), the headless `--collab-server` mode, and the engine
reflection apply-path (`ComponentFactory::GetComponentRaw` + `SetField`) all already exist and are
already compiled into SparkTests. The real gap is: nothing emits a per-field `ComponentModified`
message from an inspector edit, and nothing applies one on receipt. Split into two tracks.
**Track A (hand, small, headless):** add `CollabWorldSync` with `MakeFieldEditMessage(...)` and
`ApplyFieldEdit(World&, EditMessage)` (parse nodeId→entity, GetComponentRaw, SetField, reject
unknown entity/component/field); wire `RunCollabServer` to own a `World` and apply
`ComponentModified` edits — making the headless server the authoritative merged peer. One headless
test proves the full delta→wire→apply loop across two Worlds. **Track B (follow-on, UI-touching):**
`ComponentFieldRegistry` extracting the ~40 inline Inspector field lists, per-field diff in
`RENDER_REFLECTED_COMPONENT` → `BroadcastEdit`, `ApplyRemoteEditToScene` for the SceneFile document model.

**Files.** New: `Communication/CollabWorldSync.{h,cpp}`, `Tests/TestCollabWorldSync.cpp` (+CMakeLists line).
Modify: `main.cpp` (RunCollabServer owns a World, apply in the receive callback). Track B: new
`Panels/ComponentFieldRegistry.{h,cpp}`; modify `InspectorComponentRenderers_Reflected.cpp`,
`CollaborationPanel.cpp`, `EditorUI.cpp` (SetGraphicsDevice wiring). No wire-format changes needed.

**Units.** (1) CollabWorldSync — `ApplyFieldEdit` is the one real-logic function (3 failure modes).
(2) RunCollabServer World ownership. (3) Headless round-trip test (reuse the `CollabEdit_HostAndConnect`
host/connect/Update rhythm) + a negative case (unknown component → false, World untouched).
(4) CMakeLists. (5-8) Track B registry extraction, per-field broadcast, ApplyRemoteEditToScene, editor test.

**Verification.** Headless — build via `build.ps1`, run the new `TestCollabWorldSync` cases: happy path
(host mutates Transform.position → client World matches after one Update pump) and negative path (malformed
edit rejected, no crash). Track B: two-instance manual run with screenshot of both Inspectors converging.
Risks flagged: ID-space mismatch (entt::entity uint32 vs editor ObjectID uint64 — keep the two tracks as
distinct modes, don't unify without a discriminator), keep all apply calls on the message-drain thread
(no network-thread writes vs ImGui), surface apply failures via NotificationManager (silent divergence is
worse multi-user), throttle Track B per-field broadcast to `IsItemDeactivatedAfterEdit()` (drag emits
dozens/sec), and keep exactly one authoritative World (no P2P mesh without ID reconciliation).

**Offloadable to local 7B** (review before commit): Track B's `ComponentFieldRegistry` extraction
(1:1 transcription of existing `FIELD_*` lists — **then a careful diff-review pass**, since a wrong
offsetof/type silently corrupts memory), enum-to-string switch/array maintenance, and the 3 near-identical
negative-path test bodies. **NOT offloadable:** `ApplyFieldEdit` itself (untrusted-wire→raw-`void*`
write boundary — memory-safety-critical, the exact P/Invoke-marshalling class the 7B botched before),
the RunCollabServer World/threading wiring (single-thread-apply invariant), and the Track B broadcast-throttle UX decision.

### Stream `editor-b2b3` — `--open-scene` flag, fly-camera, gizmo overlay (SparkEditor)

**Approach.** Three independent slices; ship **flag → B2 → B3** (flag first makes B2/B3 screenshot-testable).
**Flag:** `EditorApplication::Initialize()` already seeds the World and `EditorUI::OpenScene(path)` already
loads+rewires; the flag is just parse `--open-scene`, force `config.testMode=true`, call `OpenScene` post-init
(same shape as `--save-scene`, minus the exit) — genuinely free. **B2 fly-camera:** `RenderSceneContent()`
hardcodes eye/at and discards the already-maintained yaw/pitch/distance. Ship 2a+2b together: (2a) derive orbit
eye from yaw/pitch/distance via the forward-vector formula `UpdateCamera()` already computes; (2b) add
`m_cameraTarget` and rewrite the WASD block to translate it (the current code collapses WASD into a
distance-scalar hack that becomes a *visible* zoom-instead-of-strafe bug the moment 2a lands). **B3 gizmo:**
three gaps beyond "call Render()" — (1) GizmoSystem operates on `SparkEditor::Transform` (quaternion,
dormant SceneFile model) while the live doc is ECS `::Transform` (Euler degrees); (2) SceneViewPanel's
`SelectionManager` subscription is dead — the live source is `EditorUI::GetSelectedEntity()`; (3) GizmoSystem's
6 render/hit methods ignore the passed viewport and rebuild it from `DisplaySize` (only correct fullscreen/undocked).
Design: a per-frame `m_gizmoProxy` (`SparkEditor::Transform`) bridged to/from ECS — position/scale direct,
rotation via `XMQuaternionRotationRollPitchYaw` matching `GetLocalMatrix`'s RPY order, re-derived from Euler
**only when not interacting** (mid-drag the proxy quaternion must accumulate). Fix the viewport cache
(`m_cachedViewport`) at all 6 sites.

**Files.** `main.cpp` (`--open-scene`). B2: `Panels/SceneViewPanel.{h,cpp}` (`m_cameraTarget`, eye derivation,
WASD rewrite). B3: `Gizmos/GizmoSystem.{h,cpp}` (`m_cachedViewport` + 6 replacements), `Panels/SceneViewPanel.{h,cpp}`
(SetEditorUI/SetGizmoSystem, proxy, sync helpers, Render/HandleInput call sites, MapMode), `Core/EditorUI.cpp`
(SetGraphicsDevice wiring).

**Units.** (1) `--open-scene` (ships first). (2) B2a orbit eye. (3) B2b camera target + WASD (ship with 2a).
(4) GizmoSystem viewport-cache fix (independently testable: project a known point, assert pixel in-rect).
(5) SceneViewPanel↔EditorUI/GizmoSystem plumbing. (6) Euler↔quaternion proxy bridge — **trickiest; standalone
round-trip check first**. (7) Render+input call sites (Left button for gizmo, gate camera-orbit while interacting).
(8) Remove dead SelectionManager subscription.

**Verification.** Screenshot-based via the new flag — **but the harness doesn't exist yet on Windows**:
`TakeScreenshot()` is a stub and `testFrameLimit` isn't honored in the Windows run loop. So a paired sub-unit is
required: honor `config.testFrameLimit` in `EditorApplicationWindows::Run()` (mirror the Linux frameCount check)
and add `--screenshot <path>` that copies the backbuffer to a STAGING texture + encodes via WIC (already linked).
Then `SparkEditor.exe --open-scene s.json --test-mode --test-frames 5 --screenshot out.png` and diff per unit.
Unit checks without screenshots: WorldToScreen-in-rect for the viewport fix, Euler↔quat round-trip epsilon
(avoid ±90° pitch). Risks flagged: WASD regression must ship with 2a; gimbal lock near ±90° (clamp/no-op);
dead SelectionManager (grep other readers before deleting); multi-select out of scope; Left-button gizmo must
suppress click-select that frame; no existing GizmoSystem unit tests.

**Offloadable to local 7B** (mechanical, one-glance review): the 6-site `vp = {0,0,DisplaySize...}` →
`m_cachedViewport` find/replace in GizmoSystem.cpp, the `--open-scene` argv branch (copies the existing strcmp
chain), the WIC backbuffer→PNG boilerplate (standard, though it ships — review), and the 3-way `MapMode` switch.
**NOT offloadable:** the B2 camera math (axis/sign errors "look plausible"), the Euler↔quaternion proxy bridge
(the correctness-critical transform class the standing guidance warns off), and the EditorUI/SceneViewPanel/GizmoSystem
wiring (high blast radius on shared `EditorUI` + the two-selection-system reasoning).

---

## 5. Recommended execution sequence

Constraints: **≤2 concurrent MSVC builds** on the 16 GB box; RAM preflight
(`fleet-preflight.ps1`) before each concurrent pair; prefer incremental rebuilds. Build via the
pinned-vcvars flow (`build.ps1`). The **`onboarding-auth` lane is in-flight** — it and `combat-cheats`/
`game-net-safety` all rebuild the **SparkGameMMOFPS** target, so they must be serialized against each
other, not run concurrently.

Two independent build targets dominate: **SparkEngineLib** (script-security, net-auth, proc-inject,
audio-3d, asyncdb, reflection-serial, tests-gates/RHIBridge) and **SparkGameMMOFPS** (combat-cheats,
onboarding-auth, game-net-safety). **SparkEditor** (editor-uaf, module-mgr) is a third. The safe
concurrent pair is one SparkEngineLib build + one SparkGameMMOFPS build.

**Step 1 — land Batch A criticals on their two targets, in parallel (2 builds).**
- Concurrent pair: **(engine)** apply all `script-security` (16) + `net-auth` (1) + `proc-inject` (2)
  hunks → one SparkEngineLib rebuild; **(game)** apply `combat-cheats` (5), then `game-net-safety` (4)
  → one SparkGameMMOFPS rebuild. Serialize combat-cheats/game-net-safety within the game target
  (same TU set). Do **not** also kick a third build.

**Step 2 — finish the in-flight onboarding lane on the game target (1 build).**
- After Step 1's game build is green, apply `onboarding-auth` (10) → SparkGameMMOFPS rebuild.
  Keeping it after combat-cheats/game-net-safety avoids three serialized game rebuilds racing; it's the
  same target so it cannot run concurrently with them anyway. Run its regression specs (password strength,
  corrupt-db quarantine, SaveToDisk-failure logging).

**Step 3 — Batch B criticals: engine + editor in parallel (2 builds).**
- Concurrent pair: **(engine)** apply `audio-3d` (1) + `asyncdb` (6) → SparkEngineLib rebuild (can piggyback
  on a Step-1 incremental if not yet linked); **(editor)** apply `editor-uaf` (6) + `module-mgr` (2) →
  SparkEditor rebuild. These targets don't share TUs, so this pair is RAM-safe after preflight.

**Step 4 — Batch C data-integrity on the engine target (1 build).**
- Apply `reflection-serial` (5) → SparkEngineLib/Core rebuild; run the enum/mask round-trip + duplicate-archetype
  regression tests. (`onboarding-auth` already landed in Step 2.)

**Step 5 — Batch D robustness (1 build, can overlap a feature build).**
- Apply `tests-gates` RHIBridge guards (10) → Graphics/RHI rebuild; run the null-device accessor test. This may
  run concurrently with **one** feature-stream build below (both are engine-side but separately linkable — verify
  preflight GO first).

**Step 6 — feature streams, sequenced by target to respect ≤2 concurrency.**
- `sp4-dehardcode` (SparkGameMMOFPS) and `collab-server`/`editor-b2b3` (SparkEditor) hit different targets, so at
  most one game + one editor feature build run concurrently. Recommended order:
  1. **`sp4-dehardcode`** first among features — it's the most self-contained, offload-heavy, and unblocks nothing
     else; land it on the (now-quiet) game target while an editor feature builds.
  2. **`editor-b2b3`** — do the `--screenshot`/`testFrameLimit` harness sub-unit early (it's the only way to
     verify B2/B3), then flag → B2 → B3.
  3. **`collab-server`** Track A (headless, cheap) can interleave with editor-b2b3 since its only build touch is
     SparkEditor + SparkTests; sequence its Track B after editor-b2b3's Inspector work settles to avoid two agents
     editing `InspectorComponentRenderers_*`/`EditorUI` concurrently.

**Throughout:** the two deferred architectural PRs (`net-auth` encryption pipeline, `script-security` sandbox
registration gate) and the two `tests-gates` test-coverage rewrites (SparkDaemon Windows transport, TestAsyncDatabase)
are **separate PRs**, not part of this apply loop — schedule after the 68 appliable fixes are green, each with its
own live/build verification.
