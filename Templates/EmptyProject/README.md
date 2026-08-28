# EmptyProject

A minimal editable game-module project whose empty authoring scene stays blank while a graphics-enabled module context selects an explicit starter preview.

> **stable-v1 support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified; its exact host is Windows 11 x64. EmptyProject is source and test-fixture evidence, not stable-v1 certification. Its CMake project builds a shared game module, not a bundled host executable.

This is a game-module example for an installed SparkEngine SDK. `Scenes/Default.sparkscene` remains a genuinely
empty authoring canvas. When the supplied engine context includes graphics, the module tries
`Scenes/RuntimePreview.sparkscene` first and creates the preview scene and HUD entities. The focused tests verify scene
selection and entity ownership, not rendered pixels or a launched client window. A null-graphics context with an ECS world
selects the empty authoring scene and owns no entities; the null-context path exercises only the module's lifecycle state.

The module also exposes a lifecycle clock through public accessors. Its state API is independent of graphics and input and
is safe to exercise with a null engine context.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

This CMake target produces `EmptyProject.dll` on Windows. `spark.modules.json` declares that module for a separately
supplied compatible SparkEngine host; neither this template nor its CMake target creates a game executable or launcher.
The installed-template verifier can optionally load EmptyProject headlessly through a separately supplied
`SPARK_ENGINE_EXECUTABLE`, which is evidence of that staged fixture rather than a host bundled by the template.

## Deterministic module states and first edit

- Authoring metadata: `Scenes/Default.sparkscene` is the default empty scene, ready for the first entity.
- Graphics-context fixture: the module selects the authored preview containing the slate floor, origin guides, marker, camera,
  light, and beacon, and cleans up only the entities it owns.
- Null-graphics/headless fixture: zero template-owned entities in the supplied ECS world, plus the deterministic lifecycle counter.

To start a game, add entities to `Scenes/Default.sparkscene` and replace the preview candidate in `Source/GameModule.h`, or
promote your authored scene ahead of `Scenes/RuntimePreview.sparkscene`. `Config/experience.json` records this intended
contract for package/editor consumers; its presence is metadata, not a runtime or certification result.

## License and assets

See the SparkEngine root license for template source terms. `Assets/README.md` and `Assets/manifest.json` document that this starter contains no copied third-party content; the preview uses tiny repository-authored OBJ/MTL markers, one engine-owned sphere primitive, and the existing repository-original runtime sheet.

## Runtime asset sheet

`Assets/empty_project_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine fixed, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
