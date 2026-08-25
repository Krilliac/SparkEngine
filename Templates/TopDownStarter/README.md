# TopDownStarter

A top-down arena skirmish with bounded movement, pan/zoom follow camera, pursuing enemy combat, an energy upgrade,
live feedback, and restart.

This installed-SDK example keeps bounded movement, pickup, enemy pursuit/combat, win, health, and restart rules in a
deterministic public API while a complete runtime bridge loads and renders the reflected scene. The hunter drone attacks
on a readable cadence rather than applying continuous contact damage, and player/enemy emissive flashes make hits
visible. Named scene transforms define the player, enemy, pickup, and camera spawn state, so scene edits remain
authoritative.

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

## Playable loop

1. Move northwest and press `F` near the hovering energy cell. It restores 25 health and raises attack damage from 20
   to 30, reducing the hits needed to destroy the drone.
2. Keep moving as the hunter drone pursues. It deals 12 damage at most once every 0.8 seconds while in contact range.
3. Press `Space` within attack range until the drone is defeated, then use `R` to replay the encounter.

The camera-relative HUD uses three cards: round state on the left, enemy health/visibility in the center, and energy
upgrade state on the right. It switches to objective and restart art for victory or defeat.

## License and assets

See the SparkEngine root license and the package asset provenance files.

## Runtime asset sheet

`Assets/top_down_runtime_sheet.png` is the normalized 3x3 sprite sheet used by the live status HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
