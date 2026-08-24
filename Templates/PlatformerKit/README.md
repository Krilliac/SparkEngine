# PlatformerKit

A complete platformer level with double jump, collectibles, hazards, checkpoint, finish, and restart.

The installed-SDK module exposes deterministic movement physics, jump/double-jump, three collectibles, hazard/lives, checkpoint, finish, and restart behavior through public accessors. Scene visuals are procedural placeholders and the rules use no engine-private headers.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `PlatformerKit.dll` through `spark.modules.json`; the scene is `Scenes/Level01.sparkscene`.

## License and assets

See the SparkEngine root license and package provenance files.
