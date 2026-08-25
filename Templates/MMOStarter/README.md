# MMOStarter

A bounded MMO frontier slice with character setup, faction choice, a capture objective, pursuing bot combat, chat,
respawn, and spawn protection.

This installed-SDK sample models a bounded local session rather than claiming production MMO scale. Its deterministic
state covers server/client startup, character validation, faction choice, capture progress, a pursuing training bot,
bounded chat history, death, timed bot/player respawns, and fair spawn protection. Networking adapters can be added
around these rules through public SDK services.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `MMOStarter.dll` through `spark.modules.json`; the scene is `Scenes/Frontier.sparkscene`.

## Controls

With a real engine context the template starts a bounded local session as `Astra` on the Azure faction.

- `W`, `A`, `S`, `D` -- move Astra while the chase camera follows
- `Space` -- strike the training bot while in range; it respawns after four seconds until the objective is secured
- `E` (hold) -- capture the frontier beacon while standing nearby
- `1` / `2` -- switch between the Azure and Ember factions
- `H` -- apply 25 self-damage to demonstrate death, a three-second respawn, and 1.5 seconds of spawn protection
- `T` -- submit the canned local faction-chat message
- `R` -- restart the entire session from scene-authored spawn transforms

## Playable loop

1. Move toward the bot and practice the edge-triggered melee attack. Its HUD indicator reacts to damage and respawn.
2. Reach the relay at the far side of the frontier and hold `E` inside the capture radius.
3. Secure the objective to stop the bot encounter and show the result state, or press `R` to replay with another faction.

The three camera-relative HUD cards show faction, capture state, and contextual chat/respawn/combat status. The template
keeps all rules local on purpose; `local-client-server` demonstrates lifecycle boundaries, not a production transport.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/mmo_starter_runtime_sheet.png` is the normalized 3x3 sprite sheet used by the runtime HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; other sheet consumers should use the descriptor rather than guessing coordinates.
