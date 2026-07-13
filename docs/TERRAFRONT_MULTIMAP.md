# TERRAFRONT Multi-Continent Hosting — Design + Current State

**Lane:** multimap-plumbing (W13). **Owns:** `Game/TFTravelSystem.h/.cpp` (code),
this doc (design). **Status:** design complete; small, safe partial plumbing
shipped; several PREREQUISITES documented below are still open in files this
lane does not own (contended or out-of-scope) — do not treat "the hop button
works" as "multi-continent hosting is done."

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

### 2.2 Travel handshake

Full target (server-authoritative redirect):
1. Client at the sanctuary terminal picks a continent that isn't the one this
   process hosts.
2. Client sends a travel-to-continent request to **the server it's currently
   connected to** (not directly to the target — the server is the trust
   boundary, same as every other TFMsg).
3. The current server looks up the target continent in its own registry and
   replies with either `NoSuchServer` or an endpoint (`host`, `port`).
4. Client disconnects from the current server and connects to the returned
   endpoint (the existing `TFWorldSetup::Connect` path).
5. Client re-runs onboarding against the new server: login (same
   username/password — this only works if accounts are shared, §2.3) →
   character select (same character, if any) → enter world.

**What W13 actually ships is a simplification of step 2-3**: the CLIENT reads
its own local `continents.json` copy directly (steps skipped: no new wire
message, no server-side registry lookup) and self-serves the endpoint. This
is not server-authoritative — a malicious client could point itself anywhere,
which is fine (it's still just connecting-as-a-client to *some* TERRAFRONT
server; the target server is the one that authorizes everything from there).
It keeps this pass code-light: no new `TFMsg` in the frozen wire block, no
edits to `Net/TFNetProtocol.h` or `Net/TFServerSim.*` (both contended).

If a future wave wants the fully server-authoritative version: add a new
reserved wire pair (this lane's block is `0x5434-0x5437`, already full — the
next lane would need a fresh reserved block per the `TFNetProtocol.h`
convention) carrying `{mapId}` request / `{ok, host[64], port}` reply, and
have `TFServerSim` (or a small owned system) hold the same registry
server-side. Low cost, but touches a contended file, so it wasn't done here.

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

## 3. What W13 ships (partial plumbing, `Game/TFTravelSystem.h/.cpp`)

- `TFTravelSystem::ContinentMeta` gained `host`/`port`, parsed from
  `continents.json`'s new optional keys in the existing
  `LoadContinentMeta()`.
- New public method `ClientRequestContinentHop(const ContinentMeta&)`:
  - No-op (sets the status line) unless `ctx.role == NetRole::Client` — only
    a genuine remote client can meaningfully hop; a `ListenHost`/
    `DedicatedServer`/`Standalone` process IS the authority for the active
    continent, so there's nothing to reconnect.
  - No-op with `"No server hosting <name>"` if the target has no configured
    `host`/`port`.
  - Otherwise: best-effort teardown of the current connection
    (`TFClientNet::Disconnect()` + `NetworkManager::Disconnect()`, see §5.2
    for why both are called and what's still missing) then
    `TFWorldSetup::Connect(host, port)` — the exact same public entry point
    `TFLoginFlow::JoinLanServer` already uses for LAN joins.
- The sanctuary terminal menu (`RenderUI`) now renders a live "Travel to
  `<name>` (`host`:`port`)" button for any non-active continent with a
  configured endpoint (only enabled for `NetRole::Client`), and an honest
  disabled "`<name>` - no server hosting this continent" otherwise — no
  fabricated servers, ever.
- Console command `tf_travel_hop <continent key>` mirrors the button for
  headless/scripted testing.
- `RenderDebugUI` (`tf_travel_debug` panel) lists each continent's
  configured endpoint or "no endpoint configured".
- `continents.json`'s `$schema_note` documents the new keys. No continent in
  the shipped file has real `host`/`port` values — there is no second server
  actually running by default, so shipping fake values would violate "keep
  it honest."

This is intentionally the full extent of the code change. It is safe to ship
with zero risk to the single-continent path (every new branch is gated behind
`host`/`port` being present, which is never true today) and gives an operator
who DOES stand up a second process (with a shared `Saves/`, per §2.3) a
working button, immediately, with no further code changes.

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

4. **No server-authoritative redirect** — §2.2. The client self-serves its
   own `continents.json` copy rather than asking its current server to
   resolve the target. Acceptable for a first pass (the target server is
   still the trust boundary for everything downstream) but not the "real"
   design; upgrade path is specced in §2.2 for whoever wants it.

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
  beacon matches. `ClientRequestContinentHop` is unchanged — it's handed the
  resolved (possibly LAN-sourced) endpoint the same way it always took the
  static one.

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

## 6. How to try the partial plumbing today

1. Stand up continent A: `tf_dedicated 27020` (default `tf_continent=
   cindral_wastes`), from working directory `X`.
2. Stand up continent B: `tf_dedicated 27021 -tf_continent veyra_highlands`
   (or however the boot cvar is set), from working directory `Y` where
   `Y/Saves` is the SAME physical location as `X/Saves` (symlink/junction) —
   **required**, see §2.3, or character state won't carry over.
3. In BOTH copies of `continents.json` (client's and both servers' asset
   trees), add to the `veyra_highlands` entry:
   `"host": "127.0.0.1", "port": 27021`.
4. Client: `tf_connect 127.0.0.1:27020`, log in, create a character.
5. Walk to the sanctuary terminal, open it: "Veyra Highlands" now shows a
   live "Travel to Veyra Highlands (127.0.0.1:27021)" button instead of the
   disabled "no server hosting" text.
6. Click it. The client disconnects and connects to :27021; the login screen
   now reappears (gap #3 fixed — see §4 item 3) so the player signs into the
   :27021 server directly. Verify the hop itself worked via the server logs
   (`[TF] continent hop -> Veyra Highlands (127.0.0.1:27021)` on the client,
   a fresh connection log line on the :27021 server) and
   `tf_status`/`tf_travel_debug`.
