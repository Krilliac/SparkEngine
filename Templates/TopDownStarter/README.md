# TopDownStarter

A top-down action slice with pan/zoom camera, collision bounds, an enemy, a pickup, and restart.

This installed-SDK example exposes bounded movement, pan/zoom camera state, pickup, enemy pursuit/combat, win, health, and restart behavior through a deterministic public API. Its reflected scene uses procedural placeholders only.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `TopDownStarter.dll` through `spark.modules.json`; the scene is `Scenes/Skirmish.sparkscene`.

## License and assets

See the SparkEngine root license and the package asset provenance files.

## Runtime asset sheet

`Assets/top_down_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
