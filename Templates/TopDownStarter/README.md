# TopDownStarter

A top-down action slice with pan/zoom camera, collision bounds, an enemy, a pickup, and restart.

This installed-SDK example exposes bounded movement, pan/zoom camera state, pickup, enemy pursuit/combat, win, health, and restart behavior through a deterministic public API. Its reflected scene uses procedural placeholders only.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `TopDownStarter.dll` through `spark.modules.json`; the scene is `Scenes/Skirmish.sparkscene`.

## License and assets

See the SparkEngine root license and the package asset provenance files.
