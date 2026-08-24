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
