# RPGStarter

A complete village RPG quest with bounded exploration, dialogue, relic inventory, reactive combat, objective HUD,
reward, and save/load.

This installed-SDK example exposes a complete tiny quest flow: talk to the elder, recover a relic, defeat the warden,
claim a reward, and exercise in-memory save/load. Movement is normalized and village-bounded; the hero and warden face
their actions while a following camera keeps the authored village composition visible. Dialogue, inventory, combat,
quest, reward, and save state remain public and deterministic.

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

## Quest walkthrough

1. Walk northeast to the elder and press `E` to accept the relic quest; press `E` again to close dialogue.
2. Travel northwest to the glowing relic and press `E` to add it to the inventory.
3. Cross to the northeast training yard and use `Space` or left mouse while near the warden. Do not linger in range:
   the warden retaliates once per second.
4. Return to the elder and press `E` to complete the quest and claim 50 gold plus 100 experience.
5. Use `F5`, move or restart, and then use `F9` to verify the bounded demo save slot.

The center HUD card changes from elder to relic, warden, return-scroll, and completion art. The left card tints with
health, and the right card changes when a save slot exists, so every major state transition has visible feedback.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/rpg_runtime_sheet.png` is consumed by the health/XP and save-slot HUD sprites. `Assets/runtime_sheet.json` gives
nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference
art; additional sheet consumers should use the descriptor rather than guessing coordinates.
