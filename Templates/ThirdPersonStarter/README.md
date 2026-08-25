# ThirdPersonStarter

A polished third-person wayfinding slice with camera-relative movement, sprinting, orbit and reset controls, a crystal objective, and a portal goal.

This installed-SDK example keeps movement, jump physics, orbit camera, pickup, goal, and reset rules in a public deterministic state API while a complete runtime bridge loads and renders the reflected scene. Named scene transforms define the player spawn, interaction locations, and initial camera orbit, so scene edits flow into gameplay.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `ThirdPersonStarter.dll` through `spark.modules.json`; the scene is `Scenes/Adventure.sparkscene`.

## Controls

- `W`, `A`, `S`, `D`: normalized movement relative to the current orbit camera
- Hold `Shift`: sprint
- `Space`: jump
- Hold right mouse and move: orbit the follow camera
- `Z` / `X`: zoom in / out
- `C`: reset only the follow camera to its scene-authored orbit
- `E`: collect the nearby pickup or activate the goal
- `R`: reset the adventure to its scene-authored spawn state

The camera-relative objective HUD advances from the crystal to the portal and completion icons in the runtime sheet as the adventure progresses.

## Expected run and extension seams

The first frame should show the adventurer, the raised wayfinder crystal, and the distant portal in a warmly lit practice
space. The crystal and portal animate subtly so the interaction targets read at a glance. Move near the crystal and press
`E`; the HUD then points to the portal. Reach the portal and press `E` to finish. The completed portal remains visible and
bright rather than disappearing, preserving a clear visual endpoint.

The named player, pickup, goal, and camera transforms in `Scenes/Adventure.sparkscene` are the authoring contract: moving
them changes spawn, interaction locations, and the initial orbit without editing C++. `Config/experience.json` records
those stable names plus live acceptance checks and ships with packaged builds.

## License and assets

See the SparkEngine root license and the package asset provenance files.

## Runtime asset sheet

`Assets/third_person_runtime_sheet.png` is the normalized 3x3 sprite sheet used by the live objective HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
