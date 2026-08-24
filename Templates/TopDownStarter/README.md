# TopDownStarter

A top-down action slice with pan/zoom camera, collision bounds, an enemy, a pickup, and restart.

This installed-SDK example keeps bounded movement, pickup, enemy pursuit/combat, win, health, and restart rules in a deterministic public API while a complete runtime bridge loads and renders the reflected scene. Named scene transforms define the player, enemy, pickup, and camera spawn state, so edits made in the scene remain authoritative.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `TopDownStarter.dll` through `spark.modules.json`; the scene is `Scenes/Skirmish.sparkscene`.

## Controls

- `W`, `A`, `S`, `D`: normalized player movement; the camera follows the player
- Arrow keys: pan the camera around its follow target
- `Q` / `E`: zoom in / out
- `Space`: attack the enemy while in range
- `F`: collect the energy pickup while nearby
- `R`: restart after victory or defeat, restoring the scene-authored spawn state

The camera-relative status HUD switches between the status bars, objective badge, and restart icon from the runtime sheet as the round changes.

## License and assets

See the SparkEngine root license and the package asset provenance files.

## Runtime asset sheet

`Assets/top_down_runtime_sheet.png` is the normalized 3x3 sprite sheet used by the live status HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
