# RPGStarter

A village RPG slice with dialogue, quest, inventory, combat, reward, and save/load.

This installed-SDK example exposes a complete tiny quest flow: talk to the elder, recover a relic, defeat the warden, claim a reward, and exercise in-memory save/load. Movement, dialogue, inventory, combat, quest, and reward state are public and deterministic.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `RPGStarter.dll` through `spark.modules.json`; the scene is `Scenes/Village.sparkscene`.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/rpg_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
