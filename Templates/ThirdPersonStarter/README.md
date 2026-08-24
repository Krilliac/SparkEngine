# ThirdPersonStarter

A third-person adventure slice with orbit camera, jumping, a pickup, and a goal.

This installed-SDK example keeps movement, jump physics, orbit camera, pickup, goal, and reset rules in a public deterministic state API while a complete runtime bridge loads and renders the reflected scene. Named scene transforms define the player spawn, interaction locations, and initial camera orbit, so scene edits flow into gameplay.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `ThirdPersonStarter.dll` through `spark.modules.json`; the scene is `Scenes/Adventure.sparkscene`.

## Controls

- `W`, `A`, `S`, `D`: normalized movement relative to the current orbit camera
- `Space`: jump
- Hold right mouse and move: orbit the follow camera
- `Z` / `X`: zoom in / out
- `E`: collect the nearby pickup or activate the goal
- `R`: reset the adventure to its scene-authored spawn state

The camera-relative objective HUD advances from the crystal to the portal and completion icons in the runtime sheet as the adventure progresses.

## License and assets

See the SparkEngine root license and the package asset provenance files.

## Runtime asset sheet

`Assets/third_person_runtime_sheet.png` is the normalized 3x3 sprite sheet used by the live objective HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
