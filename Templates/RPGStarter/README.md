# RPGStarter

A village RPG slice with dialogue, quest, inventory, combat, reward, and save/load.

This installed-SDK example exposes a complete tiny quest flow: talk to the elder, recover a relic, defeat the warden, claim a reward, and exercise in-memory save/load. Movement, dialogue, inventory, combat, quest, and reward state are public and deterministic.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

Load `RPGStarter.dll` through `spark.modules.json`; the scene is `Scenes/Village.sparkscene`.

## License and assets

See the SparkEngine root license and package provenance files.
