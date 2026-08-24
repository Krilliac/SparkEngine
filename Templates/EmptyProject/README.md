# EmptyProject

A truthful empty project with an editable world and no bundled art or gameplay assumptions.

This is a standalone game-module example for an installed SparkEngine SDK. It includes a current reflected scene, an explicit empty/procedural asset manifest, and a lifecycle clock with public accessors. The state API is independent of graphics, input, and private engine headers and is safe to exercise with a null engine context.

## Configure and build

```powershell
cmake -S . -B build -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build --config RelWithDebInfo
```

The host loads `EmptyProject.dll` through `spark.modules.json`. The default reflected scene is `Scenes/Default.sparkscene`.

## License and assets

See the SparkEngine root license for template source terms. `Assets/README.md` and `Assets/manifest.json` document that this starter contains no copied third-party content.
