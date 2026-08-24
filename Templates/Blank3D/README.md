# Blank3D

A ready-to-edit 3D scene with a visible primitive, ground, lighting, and fly camera.

This standalone installed-SDK example provides a pure fly-camera state with public movement, look, speed, reset, and timing methods. The reflected scene supplies the visible primitive; no engine-private headers are required.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `Blank3D.dll` through `spark.modules.json`; the starter scene is `Scenes/Default.sparkscene`.

## License and assets

See the SparkEngine root license. The asset manifest records only built-in procedural primitives.

## Runtime asset sheet

`Assets/blank3d_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
