# RPGStarter

A village RPG slice with dialogue, quest, inventory, combat, reward, and save/load.

This installed-SDK example exposes a complete tiny quest flow: talk to the elder, recover a relic, defeat the warden, claim a reward, and exercise in-memory save/load. Movement, dialogue, inventory, combat, quest, and reward state are public and deterministic.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `RPGStarter.dll` through `spark.modules.json`; the scene is `Scenes/Village.sparkscene`.

## Controls

- `W` / `A` / `S` / `D` -- move through the village
- `E` -- talk to the nearby elder, pick up the nearby relic, or close dialogue
- `Space` or left mouse button -- attack the warden while in range
- `Escape` -- close dialogue
- `F5` / `F9` -- save or load the single demo slot
- `R` -- reset the quest after defeat or at any time

The elder, relic, combat, and reward interactions are proximity-gated. The warden retaliates while the hero remains in
combat range; reaching zero health hides the hero until `R` starts a new run. The save slot is intentionally in-process
demo state: it survives `R`/`NewGame()` but not module unload or application exit.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/rpg_runtime_sheet.png` is consumed by the health/XP and save-slot HUD sprites. `Assets/runtime_sheet.json` gives
nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference
art; additional sheet consumers should use the descriptor rather than guessing coordinates.
