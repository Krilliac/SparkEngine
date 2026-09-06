# EmptyProject

A minimal editable game-module project whose empty authoring scene stays blank while the module drives an explicit starter preview scene at runtime.

> **stable-v1 support boundary:** [`stable-v1`](../../docs/site/readiness.json) is blocked and uncertified; its exact host is Windows 11 x64. EmptyProject is source and test-fixture evidence, not stable-v1 certification. Its CMake project builds a shared game module, not a bundled host executable.

This is a game-module example for an installed SparkEngine SDK. `Scenes/Default.sparkscene` remains a genuinely
empty authoring canvas: it is the scene the editor opens, and it is where your first entity goes. At runtime the module
prefers `Scenes/RuntimePreview.sparkscene` - eight named preview entities - whether or not the engine context supplies
graphics, so a headless host exercises the same scene, ownership and cleanup path a graphical one does. The HUD sprite is
the one graphics-only addition. The focused tests verify scene selection and entity ownership, not rendered pixels or a
launched client window. The null-context path loads nothing at all and exercises only the module's lifecycle state.
Unlike the eight gameplay packages, this module reads no input.

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

- Authoring metadata: `Scenes/Default.sparkscene` is the default empty scene, ready for the first entity. It is never a
  runtime scene candidate, so no scene contract applies to it while it is empty.
- Runtime fixture (graphical or headless): the module selects the authored preview containing the slate floor, origin
  guides, marker, camera, light, and beacon, and cleans up only the entities it owns. Its scene contract requires only
  an entity named `Preview Camera` carrying a `Transform` and a `Camera`; a removed decorative prop warns and degrades
  the preview. Only the HUD sprite is added when a graphics device is present.
- Null-context fixture: no scene is resolved and no entity is created; `TemplateRuntimeScene::LastLoadResult()` reports
  `Deterministic`, plus the deterministic lifecycle counter.

To start a game, add entities to `Scenes/Default.sparkscene` and replace the preview candidate in `Source/GameModule.h`, or
promote your authored scene ahead of `Scenes/RuntimePreview.sparkscene`. Whichever scene you promote has to satisfy the
same contract - an entity named `Preview Camera` with a `Transform` and a `Camera` - or the module skips it and, with no
other candidate on disk, `OnLoad` fails. `Config/experience.json` records this intended contract for package/editor
consumers; its presence is metadata, not a runtime or certification result.

## License and assets

See the SparkEngine root license for template source terms. `Assets/README.md` and `Assets/manifest.json` document that this starter contains no copied third-party content; the preview uses tiny repository-authored OBJ/MTL markers, one engine-owned sphere primitive, and the existing repository-original runtime sheet.

## Runtime asset sheet

`Assets/empty_project_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine fixed, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
