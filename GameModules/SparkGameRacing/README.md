# SparkGameRacing

SparkGameRacing is a systems-first circuit-racing example. Loading the module creates one player car and five AI opponents, places them on the active track's starting grid, begins a countdown, and drives the AI toward track waypoints.

## Playable controls

- `W` / `S`: throttle and brake
- `A` / `D`: steer
- `N`: nitro
- `Space`: drift
- `C`: cycle camera mode
- `R`: rebuild the roster and restart the race

Vehicle input is integrated using the caller's frame time, so acceleration, braking, nitro, and drift do not depend on a fixed 60 Hz input-call rate. The template uses the engine's existing debug HUD and procedural track data instead of duplicating local art assets.

Checkpoint traversal must follow the authored order, and laps complete only at the checkpoint marked as the finish line. This supports both circuit and point-to-point layouts; standings are refreshed after each frame's track-distance synchronization before the HUD and minimap snapshot is published.

Finished and DNF racers remain visible in the presentation state but receive neutral controls and stop moving while the remaining field continues racing.

## Example boundary

The slice demonstrates vehicle state, track surfaces and hazards, checkpoint/lap progression, AI steering, cameras, HUD data, replay/audio integrations, and console tooling. Production projects are expected to replace the procedural/debug presentation with authored vehicles, tracks, materials, and UI.
