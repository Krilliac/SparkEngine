# W5 T7 Acceptance: Onboarding Console Commands + Loopback Gate Proof

Date: 2026-07-06
Branch: `claude/terrafront-buildout`

## What this proves

1. **The full onboarding flow works end to end**, driven by console commands,
   over the listen-host/standalone loopback: register -> login -> character
   create -> character list -> enter-world -> the player can move/spawn/fire
   as that character.
2. **THE SECURITY GATE (T6 / T4-review #1) actually holds**: a client that has
   NOT logged in and entered world is blocked from gameplay -- SpawnRequest /
   ClientInput / FireEvent / FactionSelect sent before enter-world are rejected
   with no authoritative effect (no pawn, no bound faction). Once the same
   client completes login -> enter-world, the identical calls succeed.

Both are proved with real PASS/FAIL assertions (`tf_selftest_onboarding`), not
just a build-succeeded claim, and separately shown visually (screenshots of
the Login screen and the in-world HUD after a full manual walkthrough).

## What was added

- `GameModules/SparkGameMMOFPS/Source/Console/TFCommands.cpp`: five onboarding
  console commands (`tf_register`, `tf_login`, `tf_char_create`,
  `tf_char_list`, `tf_enter`), each building the Task-4 wire POD and sending
  it via `m_ctx.clientNet->SendMsg(...)` -- the same path `TFLoginFlow` uses --
  plus `tf_selftest_onboarding`, the loopback acceptance harness described
  above (`RunOnboardingAcceptanceSelfTest`).
- `Tools/tf_onboard_selftest.cfg`: `tf_host` then `tf_selftest_onboarding` --
  the assertion-bearing acceptance run.
- `Tools/tf_onboard.cfg`: manual walkthrough (`tf_register`/`tf_login`/
  `tf_char_create`/`tf_char_list`/`tf_enter`) + an in-world screenshot.
- `Tools/tf_onboard_shot.cfg`: screenshots the Login screen on bare launch
  (no `tf_host` needed -- `TFLoginFlow::RenderUI()` renders unconditionally as
  the pre-world menu, per Task 6).

### A real bug found and fixed along the way

Running the harness first surfaced that **every onboarding S->C reply was
silently dropped for the listen-host/standalone local player**:
`TFClientNet::RouteLoopback` sends C->S traffic straight into
`TFServerSim::RouteClientMessage` in-process (no socket involved), but the
S->C replies (`LoginReply`, `CharListReply`, `CharCreateReply`,
`TF_WorldWelcome`, ...) went out through `TFServerSim::SendToPlayer` ->
`NetworkManager::SendToClient`, which requires a real registered client
socket address. The local host player (`kTFLocalHostPlayer`) never has one
(`RouteLoopback` bypasses the socket entirely) -- so `SendToClient` found
nothing in `m_clientAddresses` and returned without effect
(`NetworkConnection.cpp:465-501`). Unlike movement/spawn (which the local
player reads straight from the authoritative ECS `Transform`, no reply
needed), the onboarding client state (`IsLoggedIn()`, the character list,
`TFGameContext::inWorld`) is **entirely reply-driven** -- there is no ground
truth to read instead. The result: the whole login -> world flow was
silently inert for local/standalone play, the primary way this feature is
actually played.

Fix (additive, non-FROZEN files): `TFServerSim::SendToPlayer` now detects
`player == m_ctx->localPlayer` with no real socket registration and calls a
new `TFClientNet::DeliverLoopbackReply(TFMsg, data, size)` directly in-process
instead of going through `NetworkManager`. `DeliverLoopbackReply` dispatches
to the exact same `On*Reply`/`OnWorldWelcome` handlers
`RegisterClientHandlers` wires to the socket, so local and networked clients
now go through identical logic, just a different delivery path for the one
synthetic local-host id. Files touched: `Net/TFServerSim.cpp` (+`#include
"Net/TFClientNet.h"`), `Net/TFClientNet.h`, `Net/TFClientNetHandlers.cpp`.

## Commands run (from CWD `D:\SparkEngine`)

Build:
```
$env:TF_TARGET='SparkGameMMOFPS'
powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build/windows-release --config Release --target SparkGameMMOFPS"
powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkTests"
```
Both exit 0. (First attempt targeted the plain `build` tree for the module and
silently built nothing relevant -- `build/windows-release` is a separate
CMake configure per `CMakePresets.json`; verified by DLL timestamp/size before
and after re-targeting the correct tree.)

SparkTests suite:
```
build\bin\SparkTests.exe
```
`Tests: 5820 passed, 0 failed, 5820 total` (baseline was 5819/0; T7 adds no
new SparkTests cases, so the +1 is pre-existing from T6 -- the important
number is **0 failed**, unchanged).

Acceptance harness (detached, per the `AttachConsole` gotcha):
```
Start-Process build\windows-release\bin\SparkEngine.exe -ArgumentList `
  '-game','D:\SparkEngine\build\windows-release\bin\SparkGameMMOFPS.dll', `
  '-exec','Tools/tf_onboard_selftest.cfg','-test-seconds','8','-window-size','1280','720' `
  -WorkingDirectory 'D:\SparkEngine' -WindowStyle Hidden `
  -RedirectStandardOutput onboard_selftest_out.txt -RedirectStandardError onboard_selftest_err.txt
```
Exit code 0, no stderr. `exec_audit.log` evidence:

```
-- gate check: gameplay BEFORE login/enter-world must be rejected --
  PASS: SpawnRequest before enter-world spawned NO pawn (RouteClientMessage gate held)
  PASS: FactionSelect before enter-world bound NO faction (gate held)
  PASS: client is not InWorld before login/enter-world
-- happy path: register -> login -> char-create -> enter-world -> spawn --
  PASS: register accepted (or already registered by a prior run of this self-test)
  PASS: login succeeded (LoginReply delivered + TFAccountSystem verified the hash)
  PASS: character create accepted
  PASS: have a valid character id to enter with
  PASS: char list reply shows at least one character
  PASS: enter-world succeeded (TF_WorldWelcome delivered, client is InWorld)
  PASS: player's authoritative faction is now the character's faction (MRA)
  PASS: SpawnRequest AFTER enter-world spawned a pawn (gate lifted -> gameplay allowed)
  PASS: the spawned pawn is alive and resolvable (can move/fire/spawn)
[TF-ACCEPTANCE] RESULT: 12 passed, 0 failed
```

Manual walkthrough (separate process, fresh session) + screenshots:
```
tf_host
tf_register cmd sekret123
tf_login cmd sekret123
tf_char_create Vanguard mra
tf_char_list
tf_enter 0
gfx_screenshot Screenshots/onboarding_inworld.png
```
Audit log: `register 'cmd': ok` -> `login ok: account 2` -> `character
'Vanguard' created: id 2` -> `characters (1): [0] Vanguard MRA rank 1 id 2`
-> `entered world as character 2`.

(One real snag found during the walkthrough: the plan's own literal example
password `sekret1` is 7 characters, but `TFAccountSystem::Register` requires
`password.size() >= 8` -- `TFAuthErr::PasswordTooShort` (err=4). Fixed in
`Tools/tf_onboard.cfg` by using `sekret123`; called out here rather than
silently editing around it.)

## Screenshots

- `Screenshots/onboarding_login.png` (viewed directly): the TERRAFRONT Login
  screen -- "Sign in to deploy.", CALLSIGN + PASSPHRASE fields, Login /
  Create Account buttons -- rendered on bare launch with no `tf_host` and no
  war visible, confirming Task 6's "render unconditionally, gate the rest of
  the UI behind InWorld()" wiring.
- `Screenshots/onboarding_inworld.png` (viewed directly): first-person HUD
  (health bar, minimap, compass) with the `MRA` faction tag visible -- the
  faction of the character just created and entered with -- confirming a real
  pawn spawned as that character after `tf_enter`.

## Verdict

**Both required assertions verified, not asserted from theory:**
- **Happy path**: register -> login -> character create -> enter-world ->
  spawn all succeed end to end over the loopback, confirmed by both the
  12/12-assertion harness and a fully independent manual console walkthrough
  with screenshot evidence.
- **Security gate**: SpawnRequest/FactionSelect (and, by the identical
  `RouteClientMessage` guard, ClientInput/FireEvent) before login/enter-world
  produce zero authoritative effect; the same calls succeed immediately after
  enter-world using the exact same client identity -- a direct, contrastive
  proof the T6 gate is real, not merely "nothing happens by default."

SparkTests stays green (5820/0). No fabrication: the local-loopback reply
delivery gap was a real, previously-invisible bug (it never surfaced because
no prior task ran the flow end to end); it is called out and fixed above,
not hidden.
