# PlatformerKit

A complete platformer level with double jump, collectibles, hazards, checkpoint, finish, and restart.

The installed-SDK module exposes deterministic movement physics, jump/double-jump, three collectibles, hazard/lives,
checkpoint, finish, and restart behavior through public accessors. Its shared runtime bridge loads the authored scene and
adds a sheet-backed status HUD while keeping the same headless rules for tests.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `PlatformerKit.dll` through `spark.modules.json`; the scene is `Scenes/Level01.sparkscene`.

## Controls

- `A` / `D` -- move left or right
- `Space` -- jump; press again in the air for the double jump
- `R` -- restart the level, including coins, lives, checkpoint, and timer

Coins, the checkpoint, hazards, and the exit activate on contact. The exit completes the level only after all three
coins are collected. The camera follows the player while authored platform heights and trigger volumes remain the source
of truth for gameplay placement.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/platformer_runtime_sheet.png` is consumed by the runtime status HUD. `Assets/runtime_sheet.json` gives nine
stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art;
additional sheet consumers should use the descriptor rather than guessing coordinates.
