# SparkGameRTS

SparkGameRTS is a playable real-time-strategy example built from the module's real unit, building, economy, command,
fog-of-war, and match systems. Loading the module starts a Human-versus-Swarm skirmish and opens the **RTS
Battlefield** panel in editor-enabled runtimes.

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
frame.

## Console controls

- `rts_status`, `rts_units`, `rts_buildings`, `rts_resources` inspect live state.
- `rts_select <workers|marines|tanks|army>` changes selection.
- `rts_move <x> <y> [queue]`, `rts_hold`, and `rts_stop` issue orders.
- `rts_train_marine` queues a marine at the Human barracks.
- `rts_demo_reset` restores the default skirmish.
