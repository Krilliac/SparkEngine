# FPSStarter

A playable first-person training range with movement, mouse capture, hitscan combat, a damageable target, live HUD state, reloads, and a repeatable round loop.

This standalone installed-SDK example keeps its combat simulation deterministic and testable through public accessors while also providing a complete runtime adapter. It loads the packaged `Startup.sparkscene` (falling back to `Scenes/Arena.sparkscene` when the project root is the working directory), drives the arena through the public installed engine headers, and owns the complete module render frame. The scene now presents a grounded target lane, symmetric cover, an authored rifle display, and perimeter walls that match the runtime's 24-unit movement clamp. A headless context can run the scene and combat simulation with an ECS world alone; graphics and input are optional adapters.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `FPSStarter.dll` through `spark.modules.json`; the starter scene is `Scenes/Arena.sparkscene`. A packaged `LaunchGame.cmd` starts in the package root, allowing the module to load the staged `Startup.sparkscene`, its runtime sheet, and other project-relative assets.

## Controls

- `W`, `A`, `S`, `D`: move with normalized, frame-rate-independent planar motion
- Mouse: first-person yaw and pitch
- Left mouse: recapture the mouse when released; subsequent presses fire, and shots only damage the target when the crosshair ray intersects it
- `R`: reload
- `Enter`: reset the target, player, weapon, and round
- `Escape`: release the mouse

The camera-relative HUD uses `Assets/fps_starter_runtime_sheet.png` for its crosshair, weapon, and reload indicator, with live ammunition and target-health bars. The public basic renderer currently draws these as 2.5D world sprites with read-only depth rather than a depth-disabled screen overlay, so they can be occluded if the camera is moved through geometry. Projects that require a guaranteed overlay should migrate the HUD to `OnImGui` or a dedicated UI pass. The starter intentionally uses bounded kinematic movement; `CharacterControllerComponent` remains available for projects that add a full physics-controller bridge and collision geometry.

## Live playtest checkpoints

1. The initial view faces the orange training target between two cyan-accented cover barriers; no prop should float above the floor.
2. Four crosshair hits destroy the 100-health target and drain the target-health bar in 25-point steps.
3. Eight emitted shots empty the magazine; `R` transfers ammunition from the 24-round reserve after the reload delay.
4. `Escape`, then left mouse, recaptures input without firing that same press. `Enter` restores the target, ammunition, and spawn pose.

The cover and perimeter are presentation geometry for this compact sample, while player motion is deliberately bounded kinematically. Add physics shapes before treating the props as collision-authoritative level geometry.

## License and assets

See the SparkEngine root license. `Assets/README.md` and `Assets/manifest.json` record the repository-original model and sprite provenance; this package bundles no third-party content.

## Runtime asset sheet

`Assets/fps_starter_runtime_sheet.png` is the normalized 3x3 sprite sheet consumed by the live crosshair, ammo, health, target, and victory HUD. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The runtime mirrors that locked version-1 grid through named constants; update those constants together with the descriptor if the sheet contract changes. The larger atlas remains concept and reference art.
