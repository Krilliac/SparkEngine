# MMOStarter

A bounded local client/server sample with character setup, faction objective, bot, chat, and respawn.

This installed-SDK sample models a bounded local session rather than claiming production MMO scale. Its public deterministic state covers server/client startup, character validation, faction choice, capture progress, a training bot, bounded chat history, death, and respawn. Networking adapters can be added around these rules through public SDK services.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `MMOStarter.dll` through `spark.modules.json`; the scene is `Scenes/Frontier.sparkscene`.

## Controls

With a real engine context the template starts a bounded local session as `Astra` on the Azure faction. Use `WASD` to move, hold `E` near the capture point to claim it, and press `Space` near the training bot to attack. `1`/`2` switch between Azure and Ember, `H` applies 25 self-damage to demonstrate death and the three-second respawn, `T` submits a canned local chat message, and `R` resets the session and scene positions.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/mmo_starter_runtime_sheet.png` is the normalized 3x3 sprite sheet used by the runtime HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; other sheet consumers should use the descriptor rather than guessing coordinates.
