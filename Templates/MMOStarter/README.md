# MMOStarter

A bounded local client/server sample with character setup, faction objective, bot, chat, and respawn.

This installed-SDK sample models a bounded local session rather than claiming production MMO scale. Its public deterministic state covers server/client startup, character validation, faction choice, capture progress, a training bot, bounded chat history, death, and respawn. Networking adapters can be added around these rules through public SDK services.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `MMOStarter.dll` through `spark.modules.json`; the scene is `Scenes/Frontier.sparkscene`.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/mmo_starter_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
