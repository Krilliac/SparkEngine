# TERRAFRONT (SparkGameMMOFPS)

A three-faction combined-arms MMOFPS in the spirit of Planetside 2, built as a
SparkEngine game module. Three successor powers wage a persistent territory war
over the flux wells of the shattered colony world **Veyra** — hex-mapped regions,
conduit lattice logistics, vehicles, exosuits, and a server-authoritative
netcode stack (prediction, lag compensation, delta replication) on top of the
engine's WorldServer/AreaServer architecture.

Full engineering spec: [DESIGN.md](DESIGN.md). Asset provenance (all CC0):
[`Assets/MMOFPS/ATTRIBUTION.md`](../../Assets/MMOFPS/ATTRIBUTION.md).

---

## The war at a glance

**Continent 1: Cindral Wastes** — a 4 km × 4 km desert mesa continent of 13
regions. Each faction holds an indestructible **Skyanchor** home bastion;
everything else can be taken — but only along **Conduits**, the lattice links
between regions. Hold territory to earn **Flux** (the vehicle/exosuit currency);
hold *everything* to trigger **Dominion** and lock the continent.

| Faction | Tag | Identity | Doctrine |
|---|---|---|---|
| **Meridian Accord** | MRA | Authoritarian colonial remnant (crimson/gunmetal) | High rate of fire, fast reloads, suppression |
| **Aurum Combine** | AUC | Corporate-secessionist mercenaries (cobalt/gold) | Hard-hitting slow shots, shotguns, heavy armor |
| **Helix Covenant** | HLX | Transhumanist tech-cult (violet/teal) | Energy weapons, no bullet drop, faster shields |

**Classes:** Ghost (recon), Striker (jump-jets), Medtech (heal/revive),
Fabricator (repair/ammo/turrets), Bulwark (heavy), Colossus (flux-purchased
exosuit). **Vehicles:** Drifter (fast quad), Aegis (armored transport and
*deployable mobile spawn* — the core logistics unit), Ravager (light tank).

Core numbers (all data-driven, see [Data tables](#data-tables)): 500 HP +
500 shield, shield regens 80/s after 6 s; TTK ~0.6–1.0 s; friendly fire ON at
50%; respawn 8 s (5 s at a deployed Aegis).

---

## Quickstart

Build the engine as usual (module DLLs under `GameModules/` build automatically):

```powershell
.\generate.bat -g "Visual Studio 17 2022" release
.\build.ps1 -config Release -editor
```

Launch the engine — `SparkGameMMOFPS.dll` loads at loadOrder 1002. To force a
specific module DLL, use the `-game` flag:

```powershell
.\SparkEngine.exe -game .\SparkGameMMOFPS.dll
```

Then drive everything from the console (`~`). A first session:

Press **Enter** in-world to open HUD chat; choose region, faction, squad, or
yell scope before sending.

```
tf_host                    # start an in-process listen server (port 27020)
tf_faction hlx             # join the Helix Covenant
tf_class striker           # pick a class
tf_spawn                   # deploy at your skyanchor
tf_bots 12                 # populate the war (authority only)
tf_map                     # continent map — ownership + lattice
tf_status                  # module/net/session overview at any time
```

### LAN hosting

| Role | Commands |
|---|---|
| Host (plays too) | `tf_host [port]` → pick faction/class → `tf_spawn` |
| Dedicated server | `tf_dedicated [port]` (headless authoritative sim) |
| Client | `tf_connect <ip[:port]>` → `tf_faction …` → `tf_class …` → `tf_spawn` |
| Leave | `tf_disconnect` |

Default port is 27020/UDP. Territory and progression persist across restarts
(see [Save files](#save-files)).

### Console command tour

| Command | What it does |
|---|---|
| `tf_status` | Module, role, session and system status |
| `tf_chat <region\|faction\|squad\|yell> <message>` | Send chat to the selected scope |
| `tf_host [port]` / `tf_dedicated [port]` | Start listen host / headless server |
| `tf_connect <ip[:port]>` / `tf_disconnect` | Join / leave a server |
| `tf_faction <mra\|auc\|hlx>` | Choose your side |
| `tf_class <ghost\|striker\|medtech\|fabricator\|bulwark>` | Choose loadout class |
| `tf_spawn` / `tf_deploy` | Deploy at skyanchor / open the spawn screen |
| `tf_map` | Toggle the continent hex map |
| `tf_regions` | List regions, owners, capture state |
| `tf_flux` | Your rank, XP and flux wallet |
| `tf_vehicle <drifter\|aegis\|ravager>` | Buy a vehicle at a friendly terminal |
| `tf_colossus` | Buy a Colossus exosuit (450 flux) |
| `tf_place <turret\|ammo\|beacon>` | Place a class deployable |
| `tf_give <weaponKey>` | Request a primary weapon (e.g. `hlx_rifle`) |
| `tf_bots <n>` | Spawn server-side bots (authority) |
| `tf_capture <regionId> <faction>` | Debug: force region ownership (authority) |
| `tf_giveflux <n>` | Debug: grant flux (authority) |
| `tf_save` | Force-save territory + progression (authority) |
| `tf_reload_data` | Hot-reload all JSON data tables |
| `tf_pos` / `tf_tp <x> <z>` | Print / teleport pawn position (debug) |
| `tf_debug <system>` | Toggle a per-system debug panel |

---

## Developer map

Module root: `GameModules/SparkGameMMOFPS` → `SparkGameMMOFPS.dll`.
All systems follow the engine subsystem pattern
(`Initialize(ctx, events)` / `Update` / `FixedUpdate` / `Shutdown` /
`RenderDebugUI`) and talk through `TFGameContext` (`Core/TFTypes.h`) and the
`TFEventBus` (`Core/TFEvents.h`) — never through globals.

| Source | Responsibility |
|---|---|
| `Core/Main.cpp` | `SPARK_IMPLEMENT_MODULE`, boots systems, client/server role switch |
| `Core/TFTypes.h`, `Core/TFEvents.h` | Frozen ids/constants, shared context, internal events |
| `Data/TFDataTables.*` | JSON table loaders + validation, hot reload, faction trait resolve |
| `World/TFWorldSetup.*` | Scene/terrain load, WorldServer/AreaServer boot, origin rebasing |
| `World/TFRegionSystem.*` | Hex regions, conduit lattice, capture ticks, Dominion, territory save |
| `Net/TFNetProtocol.h`, `Net/TFRepProtocol.h` | The wire contract (below) |
| `Net/TFServerSim.*` | Authoritative fixed-60 Hz sim: movement validation, hits, lag rewind |
| `Net/TFReplication.*` | EntityReplicator wiring, dirty-check pawn/vehicle broadcasts |
| `Net/TFClientNet.*` | Connection, ClientPrediction reconcile, interpolation buffers |
| `Game/TFPlayerSystem.*` | Spawn/death/respawn, class loadouts, movement tuning |
| `Game/TFWeaponSystem.*` + `TFWeaponMath.h` | Data-driven weapons, hitscan + projectile, spread/recoil |
| `Game/TFDamageSystem.*` | Shield-first pools, friendly fire 50%, kill credit, feedback msgs |
| `Game/TFVehicleSystem.*` | Vehicles, seats, vehicle weapons, Aegis deploy-spawn |
| `Game/TFColossusSystem.*` / `TFDeployableSystem.*` | Exosuit purchase; turrets/ammo/beacons |
| `Game/TFProgressionSystem.*` | XP, ranks 1–30, flux income, persistence |
| `Game/TFSquadSystem.*` / `TFBotSystem.*` | Squads of 6, squad spawn; server-side bots |
| `UI/` | HUD, continent map, spawn screen, scoreboard (ImGui) |
| `Console/TFCommands.cpp` | The `tf_*` command surface above |

### Wire protocol

Server-authoritative everything. Clients send input at 60 Hz; the server
simulates at a fixed 60 Hz, rewinds 200 ms for lag-compensated hits, and
replicates state at 10–20 Hz with client interpolation + owner prediction
reconcile. All structs are packed PODs with `static_assert`ed layouts — drift
breaks the build, not the game.

| Block | Header | Contents |
|---|---|---|
| `0x5400`–`0x5411` | `Net/TFNetProtocol.h` | **Frozen** `TFMsg` events/commands: input, spawn, fire/hit/kill, region state, vehicles, squads, chat, XP |
| `0x54F0`–`0x54F3` | `Net/TFRepProtocol.h` | Pawn replication: create/update/destroy + owner move-state (prediction ack) |
| `0x54F8`–`0x54FB`, `0x54FF` | `Net/TFRepProtocol.h` | Vehicle replication + purchase |
| `0x54FC`–`0x54FE` | `Net/TFRepProtocol.h` | Deployable replication |

Entity *state* rides the replication channels (quantized: positions in cm,
angles in 1/10000 rad); `TFMsg` is for events and commands only.

### Data tables

All balance lives in `Assets/MMOFPS/Data/` — five JSON files validated on load
(unique ids, closed vocabularies, conduit symmetry, complete initial
ownership); any error aborts the load loudly. Hot-reload in-game with
`tf_reload_data`.

| File | Drives |
|---|---|
| `weapons.json` | 23 weapons: damage/RoF/mags/spread/falloff, models + audio |
| `factions.json` | Faction traits — RoF/damage/reload multipliers, HLX zero-drop |
| `classes.json` | Class pools, speeds, abilities, allowed weapon slots |
| `vehicles.json` | Health, speed, seats, weapons, Aegis deploy-spawn |
| `regions.json` | The Cindral Wastes lattice: regions, conduits, initial ownership |

### Save files

Written next to the server executable, atomically (`tmp` + rename):

- `Saves/terrafront_territory.json` — region ownership (survives restart; Dominion resets seed from `regions.json`)
- `Saves/terrafront_state.json` — per-player XP/rank/flux (progression)

### Tests

CI-standalone suites in `Tests/` (no GPU/audio/sockets; module logic is
verified via standalone reimplementation against DESIGN.md, the wire headers
are included directly):

- `TestTFNetProtocolLayout.cpp` — message ids, struct sizes/offsets, memcpy round-trips, quantization precision
- `TestTFDataTables.cpp` — parses the real JSON tables; ids, vocabularies, conduit symmetry, ownership coverage, asset paths exist on disk
- `TestTFDamageModel.cpp` — shield-first absorb, FF 50%, regen timing, TTK contract examples
- `TestTFRegionLattice.cpp` — BFS spawn connectivity + capturability over the real map topology
- `TestTFChatRules.cpp` — text normalization/bounds, channel validation, scope routing, yell range

```powershell
# run just the TERRAFRONT suites (name filter via env var)
$env:SPARK_TEST_NAME = "TF"; .\build\bin\Release\SparkTests.exe
```

---

## Credits

All shipped art and audio is **CC0 1.0** (Kenney, Quaternius, ambientCG,
OpenGameArt contributors). Per-pack table: [`Assets/MMOFPS/ATTRIBUTION.md`](../../Assets/MMOFPS/ATTRIBUTION.md);
per-file provenance: `Assets/MMOFPS/asset_manifest.json`. TERRAFRONT's fiction,
names and design are original to this module.
