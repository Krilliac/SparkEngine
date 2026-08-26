# EmptyProject

A minimal editable project whose empty authoring scene stays blank while graphical runs use an explicit starter preview.

This is a standalone game-module example for an installed SparkEngine SDK. `Scenes/Default.sparkscene` remains a genuinely
empty canvas for the editor. A graphical runtime selects `Scenes/RuntimePreview.sparkscene` first and shows a slate
stage, crossed origin guides, color marker, camera, light, and beacon, so a successful launch is visibly distinct from a failed or
misconfigured renderer. Null-graphics and null-context runs still load the empty authoring scene and keep the world empty.

The module also exposes a lifecycle clock through public accessors. Its state API is independent of graphics and input and
is safe to exercise with a null engine context.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

The host loads `EmptyProject.dll` through `spark.modules.json`. The editor default is `Scenes/Default.sparkscene`; a
graphical game run selects `Scenes/RuntimePreview.sparkscene` as its explicit success-state preview.

## Expected run and first edit

- Editor: an empty hierarchy and empty reflected scene, ready for the first entity.
- Graphical runtime: a slate floor, crossed origin guides, lit color marker, and beacon sphere. A completely blank white client area
  is not the intended result.
- Null-graphics/headless test runtime: zero template-owned entities, plus the deterministic lifecycle counter.

To start a game, add entities to `Scenes/Default.sparkscene` and replace the preview candidate in `Source/GameModule.h`, or
promote your authored scene ahead of `Scenes/RuntimePreview.sparkscene`. `Config/experience.json` captures this contract for
packaging checks and editor automation.

## License and assets

See the SparkEngine root license for template source terms. `Assets/README.md` and `Assets/manifest.json` document that this starter contains no copied third-party content; the preview uses tiny repository-authored OBJ/MTL markers, one engine-owned sphere primitive, and the existing repository-original runtime sheet.

## Runtime asset sheet

`Assets/empty_project_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
