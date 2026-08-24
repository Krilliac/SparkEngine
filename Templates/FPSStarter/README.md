# FPSStarter

A compact first-person game with movement, weapons, a damageable target, HUD state, and a restart loop.

This standalone installed-SDK example keeps its combat simulation deterministic and testable through public accessors: fire cadence, ammunition, reload, target damage, player death, respawn, and round reset. It does not include engine-private graphics or input headers.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `FPSStarter.dll` through `spark.modules.json`; the starter scene is `Scenes/Arena.sparkscene`. Add input/render adapters through public SDK interfaces without changing the pure combat rules.

## License and assets

See the SparkEngine root license. `Assets/README.md` and `Assets/manifest.json` record that this package bundles no third-party content.
