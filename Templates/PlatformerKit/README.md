# PlatformerKit

A polished side-view platformer challenge with double jump, collectibles, hazards, checkpoint recovery, sprinting, and a gated finish.

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
- Hold `Shift` -- sprint (useful for the long jump after the hazard)
- `Space` -- jump; press again in the air for the double jump
- `R` -- restart the level, including coins, lives, checkpoint, and timer

Coins, the checkpoint, hazards, and the exit activate on contact. The exit completes the level only after all three
coins are collected. The camera follows the player while authored platform heights and trigger volumes remain the source
of truth for gameplay placement.

## Expected run and extension seams

The first frame should show the runner at the left edge of a readable obstacle course, with the coin objective in the
camera-relative HUD. Coins spin in place, the checkpoint becomes the current HUD objective when reached, and the finish
gate brightens only after the three coins are collected. Falling below the course or touching the spike strip consumes a
life and respawns at the latest checkpoint.

Edit `Scenes/Level01.sparkscene` to move the two named platforms and trigger models; the module derives collision surfaces
and trigger extents from those transforms. Keep the stable entity names documented in `Config/experience.json` when
replacing art. The public state and interaction methods remain deterministic for automated rules tests.

`Config/experience.json` is copied into packaged builds and records the objective, controls, stable scene contract, and
live acceptance checks for editor automation or downstream projects.

## License and assets

See the SparkEngine root license and package provenance files.

## Runtime asset sheet

`Assets/platformer_runtime_sheet.png` is consumed by the runtime status HUD. `Assets/runtime_sheet.json` gives nine
stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art;
additional sheet consumers should use the descriptor rather than guessing coordinates.
