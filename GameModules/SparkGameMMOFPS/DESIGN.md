# TERRAFRONT — Design & Engineering Spec (frozen contract v1)

A massively-multiplayer combined-arms FPS in the spirit of Planetside 2, built as a
SparkEngine game module. Three factions fight a persistent territory war over hex-mapped
continents. Original IP — names, factions, and lore are Terrafront's own.

Module: `GameModules/SparkGameMMOFPS` → `SparkGameMMOFPS.dll` (loadOrder 1002)
Assets: `Assets/MMOFPS/**` (OBJ + PNG/JPG + WAV + JSON materials + .scene only — the
Windows-reliable pipeline; no glTF/FBX/DDS/OGG at runtime).

---

## 1. Fiction & naming (FROZEN — use exactly these names)

**Setting:** the shattered colony world **Veyra**. Orbital collapse severed the colony from
Earth; three successor powers war over the planet's flux wells.

**Factions** (Planetside analogs, but original):

| Faction | Identity | Colors | Combat doctrine |
|---|---|---|---|
| **Meridian Accord** (MRA) | Authoritarian remnant of the colonial administration | Crimson / gunmetal | High rate-of-fire ballistics, fast reloads, suppression |
| **Aurum Combine** (AUC) | Corporate-secessionist mercenary alliance | Cobalt / gold | Hard-hitting slow shots, shotguns, heavy armor |
| **Helix Covenant** (HLX) | Transhumanist tech-cult reshaping Veyra's ruins | Violet / teal | Energy weapons, no bullet drop, mobility, faster shield regen |

**Continent 1:** **Cindral Wastes** — desert mesa/canyon continent, 4 km × 4 km.
**Skyanchors** — indestructible faction home bastions (= warpgates). **Conduits** — lattice
links between regions. **Flux** — the territory resource (vehicle/exosuit currency).
**Dominion** — continent lock state when one faction holds all linked territory.

**Classes:** Ghost (recon/sniper, cloak-lite = reduced minimap signature), Striker
(jump-jet light assault), Medtech (heal/revive), Fabricator (repair/ammo/turret),
Bulwark (overshield heavy), **Colossus** (flux-purchased exosuit, MAX analog).

**Vehicles (common pool v1):** Drifter (fast quad), Aegis (armored transport,
**deployable mobile spawn** — the core logistics unit), Ravager (light tank),
Vulture (VTOL gunship, wave-4 stretch).

**Weapons:** per-faction Rifle / Carbine / LMG / Sniper / Sidearm (3×5 = 15) + common
pool Shotgun, Rocket Launcher (anti-vehicle), Repair Tool, Med Applicator, Knife,
Colossus Autocannon, Aegis/ Ravager cannons. All stats data-driven (JSON), not hardcoded.
Faction flavor via stats: MRA high RoF/low dmg, AUC low RoF/high dmg + spread, HLX mid +
zero gravity drop + energy ammo pool.

---

## 2. Engine reality this design is built on (from code audit 2026-07-05)

**Reuse as-is (working code):**
- Module DLL pattern: `SPARK_IMPLEMENT_MODULE`, `IModule` + `IEngineContext` service locator.
- `ClientPrediction` (`Engine/Networking/ClientPrediction.*`) — complete input-buffer
  reconciliation, pluggable movement simulator. WIRE IT (MMO module never did).
- `EntityReplicator` + `ReplicationFields` — dirty-field delta replication, create/update
  packets, visibility filtering. This is the replication backbone.
- `WorldServer` (sessions, area routing, interest scope via `ConnectionScopeFilter`),
  `DedicatedServer` (headless tick loop, UDP, NullRHI), `NetworkManager` handlers.
- `WorldOriginSystem` — origin rebasing; MUST be driven per-frame (MMO configured but
  never called Update).
- EnTT `World` (`Engine/ECS/Components.h`), `ParallelSystemExecutor`, existing components
  (`HealthComponent`, `NetworkIdentity`, Transform, RigidBody).
- FPS module's projectile/weapon/gamemode C++ (`SparkGameFPS/Source/Projectiles`,
  `WeaponStats`, `ProjectilePool`) — adapt, don't rewrite.
- `ClipmapTerrain` + `.scene` INI sections (`[Terrain]`, `[Object]`, `[SpawnPoint]`),
  `MaterialLoader` JSON materials, WIC PNG/JPG textures, XAudio2 WAV.
- MMO module's subsystem pattern (`Initialize(IEngineContext*)/Update/Shutdown/
  RenderDebugUI`) and its Persistence/Account/Chat/Party systems as libraries.

**Engine extension status.** Engine-side extension is explicitly IN SCOPE (owner directive
2026-07-05): when the clean solution is an engine API, extend `SparkEngine/Source` rather
than working around it in the module. Shipped and planned extensions:
- **Shipped:** `AreaServer::SetSimulation(IAreaSimulation*)` provides the engine-level
  authoritative simulation hook, and Terrafront implements it in `TFServerSim`.
- No end-to-end prediction→replication→interpolation wiring exists in any module → Wave 1's
  centerpiece (module side, using existing engine primitives).
- **Shipped:** `Engine/Networking/LagCompensation.{h,cpp}` provides a generic ring buffer
  of entity poses and rewound raycasts for shooter modules; focused unit and integration
  tests cover the implementation.
- **Windows glTF import:** promote `LoadGLTF`/cgltf path out of the Linux-only TU into
  shared `ModelLoading.cpp` compiled on all platforms (keep OBJ as primary content format;
  glTF becomes a supported import convenience).
- **JOINTS_0/WEIGHTS_0 import (stretch, W4):** populate the existing `Vertex.boneIndices/
  boneWeights` from glTF so GPUSkinning gets real data. Not load-bearing for v1 (static
  OBJ + transform animation is the shipping path: turrets yaw, vehicles tilt, infantry =
  capsule + posed model).
- **OGG decode (stretch, W4):** wire stb_vorbis (or miniaudio's built-in decoders) into
  `AudioAsset::Load` so CC0 OGG packs don't need offline conversion. WAV remains primary.
- Engine extensions land as separate commits touching `SparkEngine/Source` with their own
  tests, reviewed against the anti-bloat rules in the repo CLAUDE.md.

**Server topology v1:** one `DedicatedServer` process hosting `WorldServer` + ONE
`AreaServer` covering all of Cindral Wastes (single area, target 32–64 players LAN).
Region hexes are game-logic, not separate AreaServers. Multi-area split is v2; the
area-migration API stays behind an interface (`IWarZoneTopology`) so it can shard later.

---

## 3. Module architecture (FROZEN interfaces)

```
GameModules/SparkGameMMOFPS/
  CMakeLists.txt                  # same SHARED-lib pattern as SparkGameMMO
  DESIGN.md                       # this file
  Source/
    Core/Main.cpp                 # SPARK_IMPLEMENT_MODULE(TerrafrontModule); boots systems
    Core/TerrafrontModule.h       # IModule impl; owns all systems; client/server role switch
    Core/TFTypes.h                # FactionId{MRA,AUC,HLX}, ClassId, TeamColor, ids, constants
    Core/TFEvents.h               # internal event structs (PlayerKilled, RegionCaptured, ...)
    Net/TFNetProtocol.h           # ALL wire messages (enum TFMsg : uint16_t + POD structs)
    Net/TFReplication.{h,cpp}     # EntityReplicator wiring; component<->field mapping
    Net/TFServerSim.{h,cpp}       # authoritative sim: movement validate, hits, capture ticks
    Net/TFClientNet.{h,cpp}       # connection, prediction wiring, interpolation buffers
    World/TFWorldSetup.{h,cpp}    # scene load, terrain, WorldServer/AreaServer boot, origin rebase drive
    World/TFRegionSystem.{h,cpp}  # hex regions, conduits/lattice, capture points, Dominion
    Game/TFPlayerSystem.{h,cpp}   # spawn/death/respawn, class loadouts, movement tuning
    Game/TFWeaponSystem.{h,cpp}   # data-driven weapons, hitscan + projectile, ADS/recoil/spread
    Game/TFDamageSystem.{h,cpp}   # health/shields, TTK model, friendly-fire policy, kill credit
    Game/TFVehicleSystem.{h,cpp}  # Drifter/Aegis/Ravager, seats, vehicle weapons, Aegis deploy-spawn
    Game/TFColossusSystem.{h,cpp} # exosuit purchase (flux), suit stats
    Game/TFDeployableSystem.{h,cpp} # Fabricator turret/ammo pack, Medtech beacon
    Game/TFProgressionSystem.{h,cpp}# XP events, ranks 1-30, flux income, unlock gates
    Game/TFSquadSystem.{h,cpp}    # squads of 6, invites, squad spawn on leader (30s cd)
    UI/TFHUD.{h,cpp}              # ImGui HUD: health/shield/ammo, crosshair, hitmarker, killfeed
    UI/TFMapScreen.{h,cpp}        # continent hex map, region ownership, deploy/redeploy UI
    UI/TFSpawnScreen.{h,cpp}      # death → spawn point chooser (skyanchor/base/Aegis/squad)
    UI/TFScoreboard.{h,cpp}       # per-faction K/D/score/regions tab screen
    Data/TFDataTables.{h,cpp}     # JSON loaders for weapons/vehicles/classes/regions tables
  Console/TFCommands.cpp          # tf_* console commands (tf_status, tf_capture, tf_give, ...)
```

**ECS components (new, in module):** `TFFactionComp{FactionId}`, `TFClassComp{ClassId}`,
`TFShieldComp{cur,max,regenDelay,regenRate}`, `TFWeaponHeldComp{weaponId,ammoMag,ammoPool,state}`,
`TFVehicleComp{vehId,seats[8],fuel}`, `TFSeatComp{vehicle,seatIdx}`, `TFRegionComp{regionId}`,
`TFCapturePointComp{regionId,idx,progress,owner}`, `TFDeployableComp{kind,owner,life}`,
`TFAegisDeployComp{active}`. Replicated state goes through TFReplication field mapping;
everything else is server-local.

**Data tables (Assets/MMOFPS/Data/):** `weapons.json`, `vehicles.json`, `classes.json`,
`regions.json` (hex map: id, name, tier, neighbors[], capturePoints[], spawnPos, flux/min),
`factions.json` (sp4: gained a per-faction `structureMaterial` tint-material path),
`presentation.json` (sp4: skybox/terrain/ambient/viewmodel/muzzle-FX render constants +
the shared pawn mesh path — see `WorldPresentationDef`), `deployables.json` (sp4: per-
`DeployableKind` prop model + scale — see `DeployableVisualDef`), plus
`continents.json`, `regions_highlands.json`, `decor.json`, and `suits.json` consumed by
their owning continent/decor/progression systems. Core schemas are defined in
`TFDataTables.h`; the core set is validated on load and hot-reloadable via
console `tf_reload_data`. Supplemental tables are validated by their owning systems.

**Wire protocol (`TFNetProtocol.h`):** app messages ride `NetworkManager::RegisterHandler`.
`TFMsg` ids start at 0x5400. POD structs, explicit little-endian, `static_assert` sizes:
`TF_ClientInput` (seq, buttons, move, view), `TF_SpawnRequest`, `TF_SpawnReply`,
`TF_FireEvent`, `TF_HitConfirm`, `TF_DamageEvent`, `TF_KillEvent`, `TF_RegionState`,
`TF_CaptureTick`, `TF_VehicleEnter/Exit`, `TF_AegisDeploy`, `TF_SquadMsg`, `TF_ChatMsg`,
`TF_XPEvent`, `TF_LoadoutChange`. Entity transforms/health flow through EntityReplicator,
NOT these messages; TFMsg is for events/commands only.

**Authority model:** server-authoritative everything. Client sends `TF_ClientInput` @ 60Hz;
server simulates in `TFServerSim` (fixed 60Hz), validates movement (speed/accel caps,
terrain clamp), fires lag-compensated hitscan (rewind buffer 200ms), replicates via
EntityReplicator @ 10-20Hz with client-side interpolation; local player uses
ClientPrediction reconciliation.

---

## 4. Game rules v1 (numbers are data-table defaults, tune later)

- **Health:** 500 HP + 500 shield (regen after 6s, 80/s). Headshot ×2. TTK ~0.6-1.0s.
- **Capture:** stand in point radius (10m), 1 contributor cap speed, contested = frozen.
  Outpost 1 point/60s; Fort 2 points/2 caps; Facility A/B/C majority, 4 min.
  Region capturable only if conduit-linked to friendly territory (lattice rule).
- **Spawning:** skyanchor always; owned regions if linked; deployed Aegis within 600m;
  squad leader (30s cooldown). Respawn timer 8s (5s at Aegis).
- **Flux:** +1/min base income +regions bonus; Drifter 50, Ravager 350, Aegis 200,
  Colossus 450. Personal wallet cap 750.
- **XP:** kill 100, assist 50, revive 75, repair tick 5, capture 500/250/100 by tier,
  defend 150. Rank = XP curve table, ranks 1-30, cosmetic + bragging (no power unlocks v1).
- **Dominion:** faction holding every non-skyanchor region → 10-min lock celebration →
  map soft-resets to thirds. Persisted across server restarts.
- **Friendly fire:** ON at 50% damage (PS2 spirit), grief kick at 10 TKs/15min.

## 5. Delivery waves (implementation contract)

- **W0 Contracts:** module skeleton builds & loads; all headers above exist and compile;
  data tables authored + loaders + validation; scene/terrain/materials for Cindral Wastes
  blocked out; asset import from staging. GATE: engine loads module, empty world renders.
- **W1 Core loop:** connect → faction select → spawn at skyanchor → predicted movement →
  shoot (hitscan+projectile) → damage/death → respawn. Server-authoritative with
  ClientPrediction + EntityReplicator wired end-to-end. HUD basics. GATE: 2-client LAN
  kill-each-other session on dedicated server.
- **W2 Territory:** regions/lattice/capture/Dominion, map screen, spawn screen, XP/flux,
  persistence of territory+progression. GATE: capture flow demo, state survives restart.
- **W3 Combined arms:** vehicles + seats + vehicle weapons, Aegis deploy-spawn, Colossus,
  deployables, classes/loadouts complete. GATE: vehicle combat + mobile spawn demo.
- **W4 Flesh & polish:** full weapon tables + audio + killfeed/minimap/scoreboard, squads,
  chat, continent-lock ceremony, balance pass, perf pass (100-entity soak), test suite.
  GATE: full loop soak test 30 min, SparkTests green.

Every wave ends with: incremental build green, new unit tests in `Tests/` green
(`TestTFRegionSystem.cpp`, `TestTFDamage.cpp`, `TestTFDataTables.cpp`, `TestTFNetProtocol.cpp`),
and a console-driven smoke script.

## 6. Non-goals v1

Multiple continents; >64 players; AreaServer sharding (interface kept); rigged character
animation; Steam transport; account security hardening; monetization/cosmetics; voice.

## 7. W5 — Onboarding (Login / Character / World Entry)

Replaces dropping straight into the war with a server-authoritative
login -> character-select -> character-create -> entering-world -> in-world
pipeline. Delivered as six units (Tasks 1-6 below); this section is the
DESIGN.md record for Task 6's additive change to the two FROZEN contract
files (`Core/TFTypes.h`, `Core/Main.cpp`).

### Flow

```
Login/Register -> CharSelect -> CharCreate -> EnteringWorld -> InWorld
(replaces: drop straight into the war at faction-select)
```

### Systems

- **`TFDatabase`** (`Source/Persistence/TFDatabase.{h,cpp}`) — account +
  character persistence. NOT `Spark::Persistence::AsyncDatabasePool` (its
  `SQLiteConnection` fallback is JSON-key-value and does not execute SQL) —
  pivoted to an atomic-JSON-file backing (tmp+rename, the same pattern as
  `TFProgressionSystem.cpp`), storing `accounts[]`/`characters[]` in
  `terrafront.db` under the shared `SavePaths::Root()`, behind the exact CRUD
  interface Tasks 2-6 depend on.
  Plain class, no `Initialize(ctx,events)` — see Boot note below.
- **`TFAccountSystem`** (`Source/Account/TFAccountSystem.{h,cpp}`) —
  register/login core logic over a `TFDatabase*` (unit-testable standalone,
  no `TFGameContext` coupling); self-describing PBKDF2-HMAC-SHA256 hashes
  (150,000 iterations, per-account salt, constant-time derived-key compare;
  self-contained implementation pinned by known-answer tests);
  a `clientId -> accountId` session map (`BindSession`/`AccountForClient`/
  `ClearSession`).
- **`TFCharacterSystem`** (`Source/Account/TFCharacterSystem.{h,cpp}`) —
  character CRUD (list/create/delete), 5-slot cap, name uniqueness/length
  validation, and `EnterWorld(accountId, charId, out)` (ownership-checked,
  returns the authoritative character record). `PersistProgress(charId, xp,
  rank, flux)` routes to `TFDatabase::SaveCharacterProgress`.
- **`TFLoginFlow`** (`Source/UI/TFLoginFlow.{h,cpp}`) — client ImGui state
  machine (`enum class TFFlowState{Login,Register,CharSelect,CharCreate,
  EnteringWorld,InWorld}`), styled like `TFSpawnScreen` (full-viewport
  `NoDecoration|NoBackground` modal, dimmed backdrop, centered panel, `TFUi`
  helpers). Sends requests via `m_ctx->clientNet->SendMsg(...)`; its reply
  sinks (`OnLoginReply`/`OnRegisterReply`/`OnCharList`/`OnCharOpReply`/
  `OnEnteredWorld`) are called directly by `TFClientNet`'s onboarding
  handlers via `m_ctx->loginFlow` (Task 6 — an interim getter-poll fallback
  from Task 5 was removed once this direct wiring landed).

### Character model

Faction (MRA/AUC/HLX) + name + persistent progression (xp/rank/flux),
authoritative from the moment a character enters the world. Class stays
per-spawn (unchanged, free switching at the deploy screen); no appearance
customization (non-goal).

### Net protocol + gating

New `TFMsg` ids after `WorldWelcome = 0x5411`: `LoginRequest/LoginReply`,
`RegisterRequest/RegisterReply`, `CharListRequest/CharListReply`,
`CharCreateReq/CharCreateReply`, `CharDeleteReq/CharDeleteReply`,
`EnterWorldReq` (reply is the now-gated `TF_WorldWelcome`). Packed PODs with
frozen `static_assert` sizes in `Net/TFNetProtocol.h`
(`TF_AuthRequest/TF_AuthReply/TF_CharBrief/TF_CharListReply/
TF_CharCreateRequest/TF_CharOpReply/TF_CharDeleteRequest/
TF_EnterWorldRequest`).

`TF_WorldWelcome` no longer fires from `PollClientJoinsLeaves` on connect; it
is sent ONLY from `TFServerSim::HandleEnterWorld` after
`TFCharacterSystem::EnterWorld` succeeds, and only once per session
(idempotent — a duplicate/late `EnterWorldReq` is ignored while
`m_move.contains(sender) || m_enteredWorld.contains(sender)`).

Every client-originated message — onboarding AND gameplay — is routed through
one function, `TFServerSim::RouteClientMessage(sender, id, data, size)`, used
by both the socket path (`RegisterNetHandlers`) and the listen-host/
standalone loopback path (`TFClientNet::RouteLoopback`), so one dispatcher
runs identical authoritative logic regardless of transport.

### Contract change (additive to the FROZEN `TFGameContext` + `Main.cpp`)

`TFGameContext` (`Core/TFTypes.h`) gains, additive-only (no reorder/removal
of existing members): `TFDatabase* db`, `TFAccountSystem* account`,
`TFCharacterSystem* characters` (added in Task 4, wired in Task 6),
`TFLoginFlow* loginFlow`, `bool inWorld` + `bool InWorld() const` (Task 6).
`Core/Main.cpp` constructs/publishes/boots `TFDatabase -> TFAccountSystem ->
TFCharacterSystem -> TFLoginFlow` after every W1-W4 system, additive to the
existing boot table. `TFDatabase`/`TFAccountSystem`/`TFCharacterSystem` are
plain core-logic classes (unit-tested standalone against a bare
`TFDatabase*`, `Tests/TestTFOnboarding.cpp`) with no uniform
`Initialize(ctx,events)` lifecycle. `Main.cpp` constructs and wires them with
`SetDatabase(...)`; authority roles lazily open
`SavePaths::File("terrafront.db")` in
`TFServerSim::EnsureAuthorityDatabaseOpen()` on the first register/login
request. Pure clients never open or flush the authority database. `OnImGui`
renders `TFLoginFlow::RenderUI()` unconditionally (a no-op once
`InWorld()`), and additionally gates HUD/map/spawn/scoreboard behind
`InWorld()` (they already gated on `HasLocalPlayer()`). `TFSpawnScreen`'s
boot auto-open is gated the same way (`UI/TFSpawnScreen.cpp Update`).

### Security fix — server-verified enter-world gate (T4-review finding #1)

Before this fix, the enter-world gate only withheld `TF_WorldWelcome`; the
gameplay handlers (`HandleClientInput`/`HandleSpawnRequest`/
`HandleFireEvent`/`HandleFactionSelect`) never verified the sender had
logged in and entered the world, so a modified client could send those
messages directly over the socket and play as an unauthenticated "ghost".
Fix: `TFServerSim::RouteClientMessage` now rejects `ClientInput`/
`SpawnRequest`/`FireEvent`/`FactionSelect` from any sender not in
`m_enteredWorld` (populated exactly once, by a successful
`HandleEnterWorld`). Because both the socket route and the loopback route
call through `RouteClientMessage`, the gate applies identically to real
network clients AND the listen-host/standalone local player
(`kTFLocalHostPlayer`) — the local host must complete login -> character
select/create -> enter-world via `TFLoginFlow` exactly like a networked
client before it can move, spawn, fire, or switch factions. Bot-driven
spawns/input (`TFBotSystem`) call `TFPlayerSystem::ServerHandleSpawnRequest`
/ `TFServerSim::EnqueueInput` / `TFWeaponSystem::ServerHandleFire` directly
(never through `RouteClientMessage`), so bots are unaffected by this gate.

### Progression re-keying

`TFProgressionSystem` keeps its existing in-session, `PlayerId`-keyed runtime
state (unchanged — low risk). `TFServerSim` additionally tracks
`PlayerId -> characterId` (`m_activeCharacter`, populated in
`HandleEnterWorld`, exposed as `ActiveCharacterOf(player)`).
`TFProgressionSystem::SaveNow()` — on its existing 2s-dirty-debounce /
30s-safety-net cadence — additionally calls
`TFCharacterSystem::PersistProgress(charId, xp, rank, flux)` for every player
with an active character, making `TFCharacterSystem`/`TFDatabase` the durable
per-character store on top of the session-scoped runtime state. On
disconnect, `TFServerSim`'s client-leave cleanup flushes the departing
session's active character one last time before erasing it, so the final
few seconds of a session are never lost to the debounce window.

### Error handling

Reply `err` byte maps `TFAuthErr`/`TFCharErr` (`Ok, BadCredentials,
UsernameTaken, UsernameTooShort, PasswordTooShort, NotLoggedIn` /
`Ok, SlotsFull, NameTaken, NameInvalid, NoSuchCharacter, NotYourCharacter,
NotLoggedIn, ServerError`) to a human message in `TFLoginFlow`. A rejected
`EnterWorldReq` (unauthenticated, unowned character, or already-in-world) has
no reply message by design — the client stays parked on its current screen,
mirroring how an unauthenticated gameplay message now gets no response
either (see Security fix above).

### Testing

Headless unit tests (`Tests/TestTFOnboarding.cpp`, added to `SparkTests`):
`TFDatabase` account/character round-trip across a `Close()`/reopen; register
-> login with correct/incorrect password; character CRUD + slot cap + name
uniqueness + ownership-checked delete + enter-world. All drive
`TFDatabase`/`TFAccountSystem`/`TFCharacterSystem` directly — `TFServerSim`
(and therefore the security-gate change above) is exercised at the
game-module level (console-driven loopback flow + screenshots), not by
`SparkTests`, since `TFServerSim.cpp` is a full-module file with engine
dependencies outside the minimal-dependency unit-test build.
