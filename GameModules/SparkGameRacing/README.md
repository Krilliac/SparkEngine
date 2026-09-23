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

The whole race loop (roster/grid setup, race clock, surface and hazard sync, ordered checkpoints, AI, and driving input) lives in `Core/RacingRaceFlow` as `SetupRaceRoster()` and `StepRaceFrame()`; the module only binds engine input to them. AI and a scripted player follow the authored centerline with speed-scaled look-ahead (`RacingTrackSystem::ProjectOntoTrack` / `GetPointAhead`), and AI throttle/brake react to that real corner rather than a synthetic line. Surfaces are resolved against the centerline segments, and grass/sand bleed speed so a car that runs wide can recover. `Tests/TestMOD380RacingCompleteRaceReal.cpp` (`RacingCompleteRace_*`) runs full races on all three demo tracks through those functions and checks that every racer finishes valid laps, placings follow finish order, cutting the infield does not count a lap, and a restart after results produces a fresh, completable race.

Finished and DNF racers remain visible in the presentation state but receive neutral controls and stop moving while the remaining field continues racing.

## Example boundary

The slice demonstrates vehicle state, track surfaces and hazards, checkpoint/lap progression, AI steering, cameras, HUD data, replay/audio integrations, and console tooling. Production projects are expected to replace the procedural/debug presentation with authored vehicles, tracks, materials, and UI.

Known limits: vehicles still use the module's own fixed-step kinematic model rather than Jolt bodies from the shared `PhysicsSystem`, checkpoints are trigger circles rather than authored collider gates, barrier/jump-ramp hazards are data-only, and ghost/replay persistence is not declared.
