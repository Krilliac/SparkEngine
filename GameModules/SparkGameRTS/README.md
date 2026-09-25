# SparkGameRTS

SparkGameRTS is a playable real-time-strategy example built from the module's real unit, building, economy, command,
fog-of-war, and match systems. Loading the module starts a Human-versus-Swarm skirmish and opens the **RTS
Battlefield** panel in editor-enabled runtimes.

## Deterministic skirmish tick

All gameplay advances through `Source/Simulation/RTSSkirmishSimulation`, a fixed 32 Hz tick
(`TICK_SECONDS = 1/32`). Frame time from `OnUpdate` is only accumulated into whole ticks (at most 8 per frame), so
the outcome depends on the starting state and the command stream, never on frame pacing or the host's fixed-step
rate. Each tick runs, in order: AI opponents (once per simulated second) → commands/movement → combat → dead-unit
cleanup → construction/production → economy → fog of war → elimination and win/loss.

Determinism rules the tick relies on:

- Unit, building, resource-node, player-economy, and command-queue containers are id-ordered `std::map`s, so two
  worlds holding the same state iterate identically regardless of insertion order or standard library.
- Simulation math uses only correctly rounded IEEE-754 operations (`std::sqrt`, not `std::hypot`), and the module
  builds with `-ffp-contract=off` on GCC/Clang so no multiply-add is fused into an FMA on some targets only.
- Combat picks every target from start-of-tick state and applies all hits together; ties resolve to the lowest id.
- `ComputeStateHash()` hashes the complete state (units, buildings, queues, economy, nodes, fog grids, match) in
  canonical order. `Tests/TestMOD370SkirmishDeterminismReal.cpp` compares it tick by tick across repeated runs,
  shuffled container insertion, and different frame pacings, and plays a scripted skirmish to Human victory.

Combat is continuous damage (`damage × attackSpeed × tick`) against the nearest enemy unit in template attack range,
falling back to enemy structures. An `Attack` order without a target entity is an attack-move: the unit walks
toward the point but stops to fight whatever enters range. The Swarm AI keeps its barracks producing marines and
sends its idle army at the oldest surviving Human structure once four units are ready. A faction with no units and
no structures is eliminated; the match reports **Victory** or **Defeat** from the local (non-AI) player's side.

## Live controls

| Input | Action |
|---|---|
| Left click | Select a Human unit; Shift-click adds it to the selection |
| Right click | Move selected units; Shift-right-click queues the waypoint |
| `1`, `2`, `3` | Select all Human workers, marines, or tanks |
| `M` | Move the selection through the demonstration waypoint route |
| `H` / `S` | Hold position / stop |
| `R` | Restart the skirmish |

The panel also exposes army selection, production, hold, stop, and restart buttons. Production is authoritative: it
checks faction resources and supply, consumes both when queued, and spawns the completed unit beside its building.
Assigned workers credit their own faction's economy, and fog visibility is rebuilt from the live unit roster every
simulation tick.

## Console controls

- `rts_status`, `rts_units`, `rts_buildings`, `rts_resources` inspect live state.
- `rts_select <workers|marines|tanks|army>` changes selection.
- `rts_move <x> <y> [queue]`, `rts_hold`, and `rts_stop` issue orders.
- `rts_train_marine` queues a marine at the Human barracks.
- `rts_demo_reset` restores the default skirmish.
- `rts_save [slot]` / `rts_load [slot]` save or resume the skirmish (default slot `rts_quicksave`; an autosave is
  written to `rts_autosave` every two minutes).

## Save and resume

Saves go through the engine `SaveSystem` under the custom-state key `SparkGameRTS.match.v2`, encoded by
`Source/Core/RTSPersistence`. A snapshot holds the complete skirmish, so a loaded match continues bit-identically:

- units, buildings (with production queues), player economies, and resource nodes with their worker order;
- the never-reused unit/building/node id counters and the harvest timer;
- every command queue and the current selection;
- match state, players, eliminations, winner, and match time;
- each faction's fog grid, including explored history that cannot be rebuilt from unit positions;
- the simulation tick, which also fixes the AI decision phase. Loading resumes at that tick with the sub-tick
  wall-clock remainder discarded.

Floats are stored as IEEE-754 bit patterns. Loading is all-or-nothing: version 1 slots (records only, from earlier
builds, including old `rts_autosave` files), truncated or trailing data, and out-of-range values are rejected
without changing the running match. `Tests/TestMOD370RTSSaveReal.cpp` saves a scripted skirmish at several ticks,
loads each save into rebuilt systems (directly and through `rts_save`/`rts_load`'s `SaveMatch`/`LoadMatch` on the
real `SaveSystem`), and requires the uninterrupted run's state hash for the next 2000 ticks and at victory.
