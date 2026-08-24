# ThirdPersonStarter

A third-person adventure slice with orbit camera, jumping, a pickup, and a goal.

This installed-SDK example keeps movement, jump physics, orbit camera, pickup, goal, and reset rules in a public deterministic state API. The reflected scene provides procedural placeholders; engine-specific adapters can call these methods without coupling the rules to private headers.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `ThirdPersonStarter.dll` through `spark.modules.json`; the scene is `Scenes/Adventure.sparkscene`.

## License and assets

See the SparkEngine root license and the package asset provenance files.

## Runtime asset sheet

`Assets/third_person_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
