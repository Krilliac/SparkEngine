# FPSStarter

A first-person training-range game module with source paths for movement, mouse capture, hitscan combat, a damageable target, HUD state, reloads, and a repeatable round loop.

> **stable-v1 support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified; its exact host is Windows 11 x64. FPSStarter is source and deterministic test-fixture evidence, not stable-v1 certification. Its CMake project builds a shared game module, not a bundled host executable.

This installed-SDK example keeps its combat rules deterministic and observable through public accessors. When a compatible host supplies an ECS world, the module tries `Startup.sparkscene` and then `Scenes/Arena.sparkscene`, appends the arena's named entities, and removes only its owned entities on unload. Its source includes input, camera, HUD, and render adapters, but the focused evidence is deterministic/headless module testing rather than a certified launched game. The authored scene contains a target lane, symmetric cover, a rifle display, and perimeter walls aligned with the module's 24-unit movement clamp.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

This CMake target produces `FPSStarter.dll` on Windows. `spark.modules.json` declares that module for a separately supplied compatible SparkEngine host; the template itself does not build or bundle a host executable. `spark package` can stage an engine host, generate `LaunchGame.cmd`, and copy `Scenes/Arena.sparkscene` to `Startup.sparkscene`. The CLI fixture verifies that assembly using fabricated module/host files and a mocked subprocess, so it is not live package-execution evidence.

## Module input mappings

These mappings apply when a compatible host supplies the input and graphics adapters:

- `W`, `A`, `S`, `D`: move with normalized, frame-rate-independent planar motion
- Mouse: first-person yaw and pitch
- Left mouse: recapture the mouse when released; subsequent presses fire, and shots only damage the target when the crosshair ray intersects it
- `R`: reload
- `Enter`: reset the target, player, weapon, and round
- `Escape`: release the mouse

The module source configures a camera-relative HUD from `Assets/fps_starter_runtime_sheet.png` for its crosshair, weapon, reload indicator, ammunition, and target-health bars. The public basic-renderer path represents these as 2.5D world sprites with read-only depth rather than a depth-disabled screen overlay, so the source geometry can be occluded if the camera moves through other geometry. This is an implementation description, not pixel-level playtest evidence. Projects that require a guaranteed overlay should migrate the HUD to `OnImGui` or a dedicated UI pass. The starter intentionally uses bounded kinematic movement; `CharacterControllerComponent` remains available for projects that add a physics-controller bridge and collision geometry.

## Deterministic behavior checkpoints

`Tests/TestTemplatesCompile.cpp` covers the combat loop, reload timing, control math, scene-contract fallback, mouse-capture transitions, and headless entity ownership. The following are intended host-level checks, not recorded live-play or packaged-host results:

1. The initial view faces the orange training target between two cyan-accented cover barriers; no prop should float above the floor.
2. Four crosshair hits destroy the 100-health target and drain the target-health bar in 25-point steps.
3. Eight emitted shots empty the magazine; `R` transfers ammunition from the 24-round reserve after the reload delay.
4. `Escape`, then left mouse, recaptures input without firing that same press. `Enter` restores the target, ammunition, and spawn pose.

The cover and perimeter are presentation geometry for this compact sample, while player motion is deliberately bounded kinematically. Add physics shapes before treating the props as collision-authoritative level geometry.

## License and assets

See the SparkEngine root license. `Assets/README.md` and `Assets/manifest.json` record the repository-original model and sprite provenance; the template asset set declares no third-party content.

## Runtime asset sheet

`Assets/fps_starter_runtime_sheet.png` is the normalized 3x3 sprite sheet referenced by the module's crosshair, ammo, health, target, and victory HUD setup. `Assets/runtime_sheet.json` gives nine fixed, named 418x418 source rectangles with transparent gutters. The source mirrors that version-1 grid through named constants; update those constants together with the descriptor if the sheet contract changes. The larger atlas remains concept and reference art.
