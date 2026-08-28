# ThirdPersonStarter

A third-person wayfinding module example with camera-relative movement, sprinting, orbit and reset controls, a crystal objective, and a portal goal.

> **stable-v1 support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified; its exact host is Windows 11 x64. ThirdPersonStarter is an outside-profile example, not stable-v1 release evidence. Its CMake project builds a shared game module, not a bundled host executable, and no current fixture proves a live packaged run.

This installed-SDK example keeps movement, jump, orbit-camera, pickup, goal, and reset rules in a public deterministic state API. When a compatible host supplies the required context, its source adapter loads named entities from `Startup.sparkscene` or `Scenes/Adventure.sparkscene`, updates their transforms and HUD state, and calls the template runtime's render entry point. Focused tests exercise the state rules and a headless ECS scene; they do not establish rendered output or a packaged host launch.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

This CMake target produces `ThirdPersonStarter.dll` on Windows. `spark.modules.json` declares that module for a separately supplied compatible SparkEngine host; the template does not produce a game executable or launcher. The generic CLI packaging fixture copies the declared default scene to `Startup.sparkscene` and preserves `Assets`, `Scenes`, and `Config`, but it uses fabricated module/host files and mocks process execution.

## Module input mappings

These mappings describe the source adapter and require a compatible host's input/runtime context:

- `W`, `A`, `S`, `D`: normalized movement relative to the current orbit camera
- Hold `Shift`: sprint
- `Space`: jump
- Hold right mouse and move: orbit the follow camera
- `Z` / `X`: zoom in / out
- `C`: reset only the follow camera to its scene-authored orbit
- `E`: collect the nearby pickup or activate the goal
- `R`: reset the adventure to its scene-authored spawn state

The source adapter changes the camera-relative objective HUD from crystal to portal to completion cells as the deterministic adventure state progresses. That wiring is not pixel-level playtest evidence.

## Intended behavior and extension seams

The authored scene places the adventurer, raised wayfinder crystal, and distant portal in a practice space. The source
updates crystal and portal transforms and, when the compatible input path is active, `E` advances the objective from
pickup to goal. After the goal transition, the source keeps the portal entity visible. These are code/scene expectations;
the current evidence does not include a recorded live-host visual acceptance run.

The named player, pickup, goal, and camera transforms in `Scenes/Adventure.sparkscene` are the authoring contract: moving
them changes spawn, interaction locations, and the initial orbit without editing C++. `Config/experience.json` records
those fixed names and intended acceptance metadata. Package assembly copies that file, but the metadata is not proof that
the checks ran or passed.

## License and assets

See the SparkEngine root license and the package asset provenance files.

## Runtime asset sheet

`Assets/third_person_runtime_sheet.png` is the normalized 3x3 sprite sheet referenced by the module's objective-HUD setup. `Assets/runtime_sheet.json` gives nine fixed, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
