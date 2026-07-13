# TERRAFRONT Multi-Continent Hosting — Design + Current State

**Lane:** multimap-plumbing (W13). **Owns:** `Game/TFTravelSystem.h/.cpp` (code),
this doc (design). Server-authoritative follow-up pass additionally touched
`Net/TFNetProtocol.h` and `Net/TFServerSim.h/.cpp` (contended, but the touch
is additive/minimal — see §3). **Status:** design complete; the handshake in
§2.2 is now fully shipped and server-authoritative; several PREREQUISITES
documented below are still open in files this lane does not own (contended or
out-of-scope) — do not treat "the hop button works" as "multi-continent
hosting is done."

## 1. The question

W7 gave every player a per-connection `mapId` (position-isolation: sanctuary
and the continent are reserved coordinate zones of ONE shared sim). W12 added
a second continent's *data* (Veyra Highlands: scene, region lattice,
`continents.json` entry) and the `tf_continent` boot cvar to pick which one
lattice a process loads — but **exactly one continent is still live per server
process**. Real multi-continent hosting means: two (or more) server processes,
each authoritative for one continent, and a player who picks a different
continent at the sanctuary terminal ends up connected to the *other* process
with their character intact.

Two architectures could deliver that:

### Option A — process-per-continent (RECOMMENDED, this is what's specced below)
Each continent is a separate `tf_dedicated`/`tf_host` process (own
`WorldServer`/`AreaServer`, own region-lattice load via `tf_continent=<key>`).
"Travel to a different continent" = disconnect from the current process,
connect to the other process's `host:port`. Account/character data is NOT
in-process state — it lives in `TFDatabase` (see §4) — so a clean reconnect
*can* carry the player's identity across processes, exactly like changing
realms in a realm-list MMO.

- Matches the W7 design note that already called cross-continent travel "a
  different server" and the W12 terminal UI that already renders
  non-active continents as "different server" destinations.
- Zero new server-side simulation surface: `TFServerSim` (contended, owns
  THE `StepFixed`) never has to know about a second continent's entities.
- Bounded blast radius: the only new mechanism is a connect/disconnect
  handshake plus a registry of who's hosting what.

### Option B — in-process multi-world (NOT implemented, do not build)
One process simulates N continents concurrently (N region lattices, N
`AreaServer`s, per-player world-membership routing through replication/
`TFRepProtocol`). This is a full rearchitecture:
- `TFServerSim` owns "the" `StepFixed` for one world; multi-world means
  either N independent fixed-steps (thread/scheduling redesign) or a single
  step that fans out per-world (touches the hottest, most contended file in
  the codebase).
- `TFReplication`/`TFRepProtocol` would need per-world interest filtering
  (exactly the cost the position-isolation design in W7 was chosen to avoid
  — see `TFSanctuaryZone.h`'s architecture note).
- `TFRegionSystem`/`TFDataTables` would need to hold N lattices resident
  simultaneously instead of the current "one process, one lattice" load.
- Every contended file in the current CONTENDED list gets touched.

**Verdict: process-per-continent.** It reuses everything that already works
(connect/disconnect, account-level persistence, the terminal UI's existing
"different server" affordance) and adds no load to the single-world hot path.
In-process multi-world is out of scope for this lane and, on the evidence
above, not obviously worth it even for a future lane — flag it if requirements
change (e.g. a hard "seamless, no-loading-screen" continent transition
requirement would force Option B), but nothing in the current design docs
asks for that.

## 2. Target design (full spec)

### 2.1 Continent registry: name → host:port

`Assets/MMOFPS/Data/continents.json` already lists every continent
(`mapId`, `key`, `name`, `scene`, `regions`, `blurb`) and is loaded directly
by `TFTravelSystem::LoadContinentMeta` (not through `Data/TFDataTables`, so
this lane can extend its schema without touching that contended file). W13
adds two OPTIONAL keys per continent entry:

```jsonc
{
  "mapId": 2,
  "key": "veyra_highlands",
  "name": "Veyra Highlands",
  "host": "192.168.1.50",   // NEW, optional — operator-configured
  "port": 27021,            // NEW, optional
  ...
}
```

Absent `host`/`port` (or `port: 0`) means "no known server for this
continent" — the terminal shows that honestly instead of pretending a hop is
possible. This is a **static, operator-maintained registry**: whoever stands
up the second `tf_dedicated` process edits `continents.json` (ideally the
copy every client/server loads, e.g. a shared asset sync) to say where it
lives. It is deliberately NOT auto-populated by LAN discovery in this pass —
see §5.1 for why and how a follow-up could close that gap.

### 2.2 Travel handshake — FIXED (server-authoritative, follow-up pass)

Target (server-authoritative redirect), now fully shipped:
1. Client at the sanctuary terminal picks a continent that isn't the one this
   process hosts.
2. Client sends a travel-to-continent request to **the server it's currently
   connected to** (not directly to the target — the server is the trust
   boundary, same as every other TFMsg). Wire: `TFMsg::ContinentHopRequest`
   (`0x548C`, C->S `TF_ContinentHopRequest{mapId}`) — a fresh reserved block
   in `Net/TFNetProtocol.h` (`0x548C-0x548F`; this lane's own travel channel,
   `0x5434-0x5437`, was already full). Routed through the same
   `TFServerSim::RouteClientMessage` choke point and enter-world gate as
   every other post-onboarding gameplay id.
3. The current server looks up the target continent in **its own** registry
   — `World/TFTravelSystem.h`'s `LookupContinentEndpoint(mapId)`, sourced
   from THAT process's own `continents.json`, never from anything the
   client asserts — and replies `TFMsg::ContinentHopReply` (`0x548D`, S->C
   `TF_ContinentHopReply{ok, mapId, port, host[64]}`). `ok == 0` means "no
   endpoint configured for that mapId on this server" (unregistered mapId
   and "registered but no host/port" both collapse to the same honest
   refusal — no `NoSuchServer` vs. `BadMap` split; one boolean was enough).
4. Client disconnects from the current server and connects to the endpoint
   **the reply carried** (the existing `TFWorldSetup::Connect` path) — never
   to whatever its own local `continents.json` copy or a LAN beacon guessed.
   Applied on the client's next `Update()` tick after the reply arrives
   (`TFTravelSystem::ApplyPendingContinentHop`), not synchronously inside the
   network message-dispatch callback that received it — doing the
   disconnect/reconnect there would tear down the very socket that dispatch
   loop is iterating (same reentrancy hazard `OnPlayerSpawned` avoids by
   deferring `TeleportPawn` to `ServerPlacePending` on the next fixed tick).
5. Client re-runs onboarding against the new server: login (same
   username/password — this only works if accounts are shared, §2.3) →
   character select (same character, if any) → enter world.

**What changed from the original W13 pass**: previously the CLIENT read its
own local `continents.json` copy directly and self-served the endpoint — not
server-authoritative; a malicious client could point itself at an arbitrary
host by editing that file (still harmless in isolation, since the target
server authorizes everything from there, but not the intended trust model).
The client's local `continents.json` copy (and any LAN-discovered beacon,
§5.1) is now used **only** as a display hint in the terminal menu — whether
to show a continent's "Travel to X (host:port)" button as enabled at all —
never as the actual connect target. `RenderUI`'s button-enable check
(`canHop`/`haveEndpoint`) is unchanged and still reads the local copy for
that hint; only the connect step changed to trust the server's reply
instead.

### 2.3 State carry-over — VERIFIED, with a hard prerequisite

Read the full chain: `TFServerSim::EnsureAuthorityDatabaseOpen()` →
`TFDatabase::Open` → `TFAccountSystem::Login`/`Register` →
`TFCharacterSystem`/`ListCharacters(accountId)`. Confirmed:

- Accounts and characters ARE account-level, not connection-level or
  continent-level. `TFCharacterRecord` carries `xp`, `rank`, `flux`,
  `unlocks`, `loadoutPrimary/Secondary/Tool`, `weaponStats` — everything a
  player has earned. Outfit membership is a parallel `TFOutfitStore`
  (`Saves/outfits.json`) also keyed by character/account, same pattern.
  **If** the two processes share the same backing files, a character that
  logs into continent B after traveling from continent A is bit-for-bit the
  same character: same rank, same unlocks, same loadout, same outfit.

- **BUT**: `TFDatabase::Open` is called with a **hardcoded relative path**,
  `"Saves/terrafront.db"` (`TFServerSim.cpp:1339`, inside
  `EnsureAuthorityDatabaseOpen`), resolved against the process's working
  directory. `TFOutfitStore` follows the identical pattern for
  `"Saves/outfits.json"`. Two separately-launched `tf_dedicated` processes
  each get their OWN local JSON file unless an operator explicitly makes
  `Saves/` the same physical location for both (same machine + same cwd, a
  symlink/junction, or a shared network path both processes mount at
  `Saves/`).

  **This is the load-bearing prerequisite for real multi-continent hosting.**
  Without a shared `Saves/` directory, "travel to Veyra Highlands" logs the
  player into a brand-new, empty account on that server — no characters, no
  progress, indistinguishable from playing on an unrelated server. No code
  in this lane touches `TFDatabase.h/.cpp` (not contended, but also not
  owned here, and the fix is an infrastructure/ops concern — point both
  processes' `Saves/` at the same place — not a code change). Flagging it
  here because it is the single most important fact for whoever stands up a
  second continent server: **the two processes MUST share one `Saves/`
  directory** (junction/symlink/shared mount), or migrate `TFDatabase` off
  local-JSON-file storage entirely onto something inherently shared (a real
  client-server DB, or at minimum a network file share with proper locking —
  the current atomic tmp+rename write pattern is not safe against two
  processes writing the same file over SMB without a locking layer on top).

### 2.4 Boundary (repeated for emphasis)

Do **NOT** build in-process multi-world. If a future requirement demands
seamless cross-continent travel with no reconnect/loading screen, that is a
different, much larger design (see §1 Option B) and deserves its own
dedicated design pass, not an incremental patch on top of this one.

## 3. What W13 ships (`Game/TFTravelSystem.h/.cpp` + `Net/TFNetProtocol.h` + `Net/TFServerSim.*`)

- `TFTravelSystem::ContinentMeta` gained `host`/`port`, parsed from
  `continents.json`'s new optional keys in the existing
  `LoadContinentMeta()`.
- `Net/TFNetProtocol.h` gained a fresh reserved wire block (`0x548C-0x548F`):
  `TFMsg::ContinentHopRequest`/`ContinentHopReply` + packed
  `TF_ContinentHopRequest{mapId}` / `TF_ContinentHopReply{ok, mapId, port,
  host[64]}` — this lane's own travel channel (`0x5434-0x5437`) was full, so
  per that block's own upgrade note this needed a new one.
- `Net/TFServerSim.*` gained `HandleContinentHopRequest`: enter-world-gated
  like every other post-onboarding gameplay id, resolves the request against
  `m_ctx->travel->LookupContinentEndpoint(mapId)` (THIS process's own
  registry) and replies — never trusting anything the client sent beyond the
  `mapId` it's asking about.
- `TFTravelSystem` gained the public `LookupContinentEndpoint(mapId, &host,
  &port)` accessor `TFServerSim` calls above (server-authoritative follow-up,
  §2.2), and `ClientRequestContinentHop(const ContinentMeta&)` was
  rewritten:
  - No-op (sets the status line) unless `ctx.role == NetRole::Client` — only
    a genuine remote client can meaningfully hop; a `ListenHost`/
    `DedicatedServer`/`Standalone` process IS the authority for the active
    continent, so there's nothing to reconnect.
  - No-op with `"Not connected to a server"` if there's no live connection to
    ask.
  - Otherwise: sends `TF_ContinentHopRequest{target.mapId}` to the CURRENT
    server and returns immediately (`"Requesting <name> server address..."`)
    — `target.host`/`target.port` are no longer read for the connect
    decision, only `target.mapId`/`target.name`.
  - `OnNetContinentHopReply` stashes the reply; `ApplyPendingContinentHop`
    (run from the next `Update()` tick, see §2.2 step 4 for why it's
    deferred) does the actual work: `"No server hosting <name>"` if
    `!ok`, otherwise best-effort teardown of the current connection
    (`TFClientNet::Disconnect()` + `NetworkManager::Disconnect()`, see §5.2
    for why both are called and what's still missing) then
    `TFWorldSetup::Connect(reply.host, reply.port)` — the exact same public
    entry point `TFLoginFlow::JoinLanServer` already uses for LAN joins, now
    fed the SERVER-verified endpoint instead of the client's local guess.
- The sanctuary terminal menu (`RenderUI`) still renders a live "Travel to
  `<name>` (`host`:`port`)" button for any non-active continent the CLIENT's
  own `continents.json` copy (or a matching LAN beacon, §5.1) believes has an
  endpoint (only enabled for `NetRole::Client`), and an honest disabled
  "`<name>` - no server hosting this continent" otherwise — no fabricated
  servers, ever. That local knowledge is now a display hint only, per §2.2;
  clicking it always re-resolves against the current server before
  connecting anywhere.
- Console command `tf_travel_hop <continent key>` mirrors the button for
  headless/scripted testing (now also async — the returned status line is
  `"Requesting..."`, not an immediate connect result).
- `RenderDebugUI` (`tf_travel_debug` panel) lists each continent's
  client-locally-configured endpoint or "no endpoint configured".
- `continents.json`'s `$schema_note` documents the new keys. No continent in
  the shipped file has real `host`/`port` values — there is no second server
  actually running by default, so shipping fake values would violate "keep
  it honest."

This is safe to ship with zero risk to the single-continent path (every new
branch is gated behind `host`/`port` being configured on the SERVER side,
which is never true today) and gives an operator who DOES stand up a second
process (with a shared `Saves/`, per §2.3, and `host`/`port` configured in
**each server's own** `continents.json` for the OTHER continent) a working,
server-verified button with no further code changes.

## 4. Known gaps (found during investigation, NOT fixed by this lane)

These live in files this lane doesn't own (contended or simply out of the
`Game/TFTravelSystem.*` scope) and are called out for whichever lane picks
this up next:

1. **Shared `Saves/` prerequisite** — §2.3. Infra/ops fix, or a `TFDatabase`
   redesign; not a `TFTravelSystem` change.

2. **PARTIALLY FIXED (follow-up pass).** `TFClientNet::Disconnect()` doesn't
   touch the socket. It resets TF-level client state (`m_connected`,
   prediction/interp buffers, chat history) and flips `ctx.role` back to
   `Standalone`, but never calls `NetworkManager::Disconnect()`. The existing
   `tf_disconnect` console command (`Console/TFCommands.cpp`) calls the raw
   `NetworkManager` Disconnect/StopServer directly instead, and is tagged
   with its own `TF-W2` comment admitting this should "route through a
   TFWorldSetup stop/teardown API so world state (role, servers, scene)
   resets cleanly alongside the socket." `ClientRequestContinentHop` calls
   BOTH (`TFClientNet::Disconnect()` for TF state + `NetworkManager::
   Disconnect()` for the socket) as the best available combination.
   `TFWorldSetup::Connect()` (`World/TFWorldSetup.cpp`) now closes the
   networking half of the gap directly: it calls the same `StopNetworking()`
   that `Shutdown()` uses whenever `m_netBooted` is already true, so a second
   `Connect()` on an already-used instance (exactly what a continent hop
   does) no longer leaks the old `m_worldServer`/`m_areaServer`/
   `m_knownClients` or races the old socket against the new one — those are
   torn down before the new `NetworkManager::Connect()` is attempted. What
   this does **NOT** do: reload scene/collision. `TFWorldSetup` still loads
   exactly one continent's scene/terrain/collision at `Initialize()`
   (single-continent-per-process, §1), so a hopped-to client keeps rendering
   the OLD continent's geometry — that remains a real gap for anyone who
   wants the client's *visuals* to follow the hop, not just its network
   session, and is a much larger change (co-owned by the scene-load path,
   `LoadSceneAndTerrain`/`LoadSanctuaryScene`) than this pass's scope.

3. **FIXED (follow-up pass).** `TFLoginFlow`'s state machine previously did
   not reset on disconnect: `TFFlowState m_state` only advanced via the
   onboarding reply sinks, and nothing set it back to `TFFlowState::Login`
   when the connection dropped, so a manual `tf_disconnect` (or this lane's
   hop) left `m_state == InWorld` — `IsOpen()` stayed `false`, the rest of
   the game UI kept rendering as if still connected, and the login screen
   the player needed to sign into the NEW server never reappeared. Fixed
   exactly as recommended above: `TFLoginFlow::ResetToLogin()`
   (`UI/TFLoginFlow.h/.cpp`) clears the flow back to `Login` (password,
   char list/selection, error, pending-op, account id — username is kept
   for convenience), and `TFClientNet::Disconnect()` calls it whenever
   `loginFlow->State() != Login`. Both the manual-disconnect and
   continent-hop paths route through `TFClientNet::Disconnect()`, so this
   closes the gap for both. Step 6 in the walkthrough below is now stale —
   the login screen DOES reappear after a hop.

4. **FIXED (this pass).** No server-authoritative redirect — §2.2. The
   client used to self-serve its own `continents.json` copy rather than
   asking its current server to resolve the target. Fixed exactly as
   specced: `TFMsg::ContinentHopRequest`/`ContinentHopReply`
   (`Net/TFNetProtocol.h`, `0x548C-0x548F`) round-trips the hop through
   `Net/TFServerSim.cpp`'s `HandleContinentHopRequest`, which answers from
   `TFTravelSystem::LookupContinentEndpoint` — THIS server's own
   `continents.json`-sourced registry — never the requesting client's. The
   client's local copy (and LAN discovery, §5.1) is now a display hint only;
   see §2.2 and §3 for the full mechanics, including why the reply is applied
   a tick late (`ApplyPendingContinentHop`) rather than synchronously inside
   the network dispatch callback that received it.

## 5. Deferred / explicitly not done

### 5.1 LAN auto-discovery integration — FIXED (follow-up pass)

W11's `TFLanDiscovery` (owned by `UI/TFLoginFlow.h/.cpp`) already broadcasts
`mapName` in its beacon (`TF_LanBeacon::mapName`) and the scanner already
dedupes by source IP + port — in principle it's a ready-made live registry:
match a discovered beacon's `mapName` against a continent's `name`/`key` and
you have an auto-populated `host`/`port` with zero operator config. The
original W13 pass did NOT wire that in, on purpose (see the original
reasoning below), and recommended a small, specific follow-up. That
follow-up has now landed, exactly as recommended:

- `TFLoginFlow::LanServers()` (`UI/TFLoginFlow.h`) is a new read-only
  accessor onto `m_lan.Servers()`.
- `TFLoginFlow::Update()` (`UI/TFLoginFlow.cpp`) keeps the scanner armed past
  the login screen: once `m_state == TFFlowState::InWorld` on
  `NetRole::Client` it calls `m_lan.StartScanning()` itself (idempotent) —
  every other state/role still calls `StopScanning()`, so headless runs and
  non-client roles never bind the extra socket, matching the original
  design's safety story.
- `TFTravelSystem::RenderUI` (`World/TFTravelSystem.cpp`) reads
  `m_ctx->loginFlow->LanServers()` for each non-active continent and, when a
  fresh beacon's `mapName` matches that continent's `name` or `key`, prefers
  the beacon's `ip`/`gamePort` over the static `continents.json`
  `host`/`port` for both the button's enabled state and the label (suffixed
  `[LAN]` so it's visually distinguishable from an operator-configured
  static entry). Falls back to the static entry exactly as before when no
  beacon matches. **Post server-authoritative follow-up (§2.2):** this
  LAN-vs-static resolution is now display-only — it decides what the button
  says and whether it's enabled, not where the client actually connects.
  `ClientRequestContinentHop` reads only `target.mapId`/`target.name` off
  whichever `ContinentMeta` `RenderUI` passes it; the real endpoint always
  comes back from the current server's `TF_ContinentHopReply`.

Original reasoning (kept for context): reading `m_lan` from `TFTravelSystem`
needed either (a) a new accessor on `TFLoginFlow` plus a `TFGameContext`
member to reach it (both files outside the original lane's OWNS list), or
(b) `TFTravelSystem` standing up its OWN second UDP listener on port 27025
— (a) is what got built; `TFGameContext` already exposed `loginFlow`, so it
was a small addition, not new wiring.

### 5.2 In-process multi-world

Not implemented. See §1 Option B and §2.4. Do not build without a fresh
design pass — this doc explicitly recommends against it for the requirements
as understood today.

## 6. How to try the plumbing today

1. Stand up continent A: `tf_dedicated 27020` (default `tf_continent=
   cindral_wastes`), from working directory `X`.
2. Stand up continent B: `tf_dedicated 27021 -tf_continent veyra_highlands`
   (or however the boot cvar is set), from working directory `Y` where
   `Y/Saves` is the SAME physical location as `X/Saves` (symlink/junction) —
   **required**, see §2.3, or character state won't carry over.
3. In BOTH copies of `continents.json` (client's and continent A's asset
   tree — continent B's own copy is never consulted by this handshake) add
   to the `veyra_highlands` entry: `"host": "127.0.0.1", "port": 27021`.
   - **Continent A's copy is load-bearing** since the server-authoritative
     follow-up (§2.2): continent A's server (working directory `X`) is the
     one that actually answers the hop request, so ITS `continents.json` is
     the one that decides whether the reply is `ok`. Miss this one and the
     hop is refused ("No server hosting Veyra Highlands") even though
     continent B is really up.
   - **The client's copy is display-only now**: it only decides whether the
     terminal button shows as enabled and what host:port it *claims* (step 5)
     — omit it there and the button stays honestly disabled even though
     clicking it would have worked, but it no longer affects where a
     successful hop actually connects (that's always continent A's answer).
     Same for a matching LAN beacon (§5.1) and for continent B's own copy —
     neither is read by this handshake at all.
4. Client: `tf_connect 127.0.0.1:27020`, log in, create a character.
5. Walk to the sanctuary terminal, open it: "Veyra Highlands" now shows a
   live "Travel to Veyra Highlands (127.0.0.1:27021)" button instead of the
   disabled "no server hosting" text (this label is still the CLIENT's local
   guess — step 6 is where the server confirms it).
6. Click it. The client sends `TF_ContinentHopRequest` to :27020 and shows
   "Requesting Veyra Highlands server address..."; :27020 resolves it from
   its own registry and replies `TF_ContinentHopReply`; on the client's next
   tick it disconnects from :27020 and connects to the endpoint THAT REPLY
   carried. The login screen reappears (gap #3 fixed — see §4 item 3) so the
   player signs into the :27021 server directly. Verify the hop itself
   worked via the server logs (`[TF] player <id> continent-hop request:
   mapId 2 -> 127.0.0.1:27021` on :27020, `[TF] continent hop -> Veyra
   Highlands (127.0.0.1:27021, server-verified)` on the client, a fresh
   connection log line on the :27021 server) and `tf_status`/`tf_travel_debug`.
