# SparkGameRacing

SparkGameRacing is a systems-first circuit-racing example. Loading the module creates one player car and five AI opponents, places them on the active track's starting grid, begins a countdown, and drives the AI toward track waypoints.

## Playable controls

- `W` / `S`: throttle and brake
- `A` / `D`: steer
- `N`: nitro
- `Space`: drift
- `C`: cycle camera mode
- `R`: rebuild the roster and restart the race

## Vehicle physics

Every racer is a Jolt vehicle in the engine's shared `PhysicsSystem`: `RacingVehicleSystem` builds a dynamic chassis body (lowered centre of mass) with a four-wheel `VehicleConstraint` through `PhysicsSystem::CreateVehicle`. The per-type `VehicleStats` set the chassis mass, the engine torque (launch acceleration), the gearing (top gear reaches the redline at 1.3x the governed top speed, leaving nitro/drift boost headroom), the wheel brake torque, and the front steering lock. Player and AI input is latched per frame (nitro drain and drift charge use the frame time, so they do not depend on the input-call rate) and handed to the Jolt controller on every fixed tick; drifting applies a partial rear handbrake, boost adds forward thrust, and the steering lock falls off with speed.

The module is the Racing process's single physics stepping owner: `SparkGameRacingModule::OnFixedUpdate` calls `RacingVehicleSystem::FixedUpdate`, which advances the shared world by exactly one `PhysicsSystem::StepFixed()` tick of the engine fixed timestep and reads the stepped poses back into `VehicleInstance`. There is no kinematic fallback: without a live Jolt world the vehicle system (and the module load) fails.

`RacingTrackSystem` gives every loaded track static colliders in the same world: a road mesh per surface type along the authored centerline, width, and elevation (friction = `RacingVehicleSystem::GetSurfaceGrip`, which Jolt combines with the tyre friction) over a grass run-off slab. Barrier walls stand 2 m beyond the road edge on the outside of every bend, where a car that runs wide leaves the road; the inside of a bend stays open run-off, and a barrier piece that would stand on another stretch of road (the Crossover Arena crossing) is left out. Each checkpoint is a Jolt sensor gate across the road, square to the centerline. The gates report chassis entries through the `PhysicsSystem` trigger callback during the physics tick, and the next race frame validates them in order (`RacingTrackSystem::TakeCheckpointCrossings`). Grass and sand add rolling resistance so a car that runs wide slows down. The AI and the test autopilot brake for corners with `ComputeCornerSpeedLimit` (a braking-point planner over the centerline curvature). A chassis that rolls over, stays pinned under full throttle for three seconds, or falls off the elevated Mountain Pass road is put back on the centerline. Finished and DNF racers are parked: their chassis leaves the physics world so they never block the cars still racing.

## Circuit kit and music

Whenever a track loads with a world, `RacingTrackSystem` dresses it with the Blender-authored Circuit Racing kit in `Assets/Models/Racing/Kit`: the start/finish gantry flanked by tyre stacks at the finish checkpoint, a checkpoint arch flanked by cones at every other checkpoint, a barrier segment on the Crossover Arena's barrier hazard, and warning cones beside the Sunset Circuit oil slick. The props are set dressing only and carry no collider; the barriers and checkpoint gates are the generated Jolt bodies described above. `RacingEngineSystems` registers the seven generated WAV music cues in `Assets/Audio/Racing/Music`. Source, provenance, and preview are in `Art/Blender/SparkGameRacing/`, and `asset-references.json` records every asset path the module source names.

Checkpoint traversal must follow the authored order, and laps complete only at the checkpoint marked as the finish line. This supports both circuit and point-to-point layouts; standings are refreshed after each frame's track-distance synchronization before the HUD and minimap snapshot is published.

The whole race loop (roster/grid setup, race clock, surface and hazard sync, ordered checkpoints, AI, and driving input) lives in `Core/RacingRaceFlow` as `SetupRaceRoster()` and `StepRaceFrame()`; the module only binds engine input to them. AI and a scripted player follow the authored centerline with speed-scaled look-ahead (`RacingTrackSystem::ProjectOntoTrack` / `GetPointAhead`), and AI throttle/brake react to that real corner rather than a synthetic line. Surfaces are resolved against the centerline segments. `Tests/TestMOD380RacingCompleteRaceReal.cpp` (`RacingCompleteRace_*`) binds the systems to a real engine `PhysicsSystem` and runs full races on all three demo tracks through those functions, one shared physics tick per 60 Hz frame. It checks that every racer finishes valid laps, placings follow finish order, a car running wide is stopped by the outside barrier, a checkpoint registers only inside its sensor gate, cutting the infield does not count a lap (and the gates taken afterwards in order still complete exactly one), a restart after results produces a fresh, completable race, every racer is a Jolt chassis advanced one tick per fixed step, the Mountain Pass road colliders carry the cars up to its 20 m summit, and finishers leave the physics world.

Finished and DNF racers remain visible in the presentation state at their last pose.

## Example boundary

The slice demonstrates vehicle state, track surfaces and hazards, checkpoint/lap progression, AI steering, cameras, HUD data, replay/audio integrations, and console tooling. Production projects are expected to replace the procedural/debug presentation with authored vehicles, tracks, materials, and UI.

Known limits: checkpoint gates and barriers are generated boxes rather than authored collision meshes, barrier/jump-ramp hazards are data-only (no collider), vehicle damage is not yet fed by collisions, and ghost/replay persistence is not declared (OD-15).
