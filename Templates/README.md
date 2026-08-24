# SparkEngine installed-SDK project templates

These eight packages are the physical source for SparkEditor's built-in project templates. Each is a standalone CMake project that consumes an installed SparkEngine SDK, produces one game-module shared library, loads a current reflected `.sparkscene`, and drives a live input/update/render loop with template-specific gameplay and artwork.

| Package | Editor template | Default scene |
|---|---|---|
| `EmptyProject` | Empty | `Scenes/Default.sparkscene` |
| `FPSStarter` | First Person | `Scenes/Arena.sparkscene` |
| `ThirdPersonStarter` | Third Person | `Scenes/Adventure.sparkscene` |
| `TopDownStarter` | Top Down | `Scenes/Skirmish.sparkscene` |
| `Blank3D` | Blank 3D | `Scenes/Default.sparkscene` |
| `MMOStarter` | MMO | `Scenes/Frontier.sparkscene` |
| `PlatformerKit` | Platformer | `Scenes/Level01.sparkscene` |
| `RPGStarter` | RPG | `Scenes/Village.sparkscene` |

`MultiplayerArena` is a complete legacy compatibility sample outside the built-in registry. It now follows the same installed-SDK, reflected-scene, deterministic-gameplay, and asset-provenance contract.

## SDK requirement

Install SparkEngine before configuring a template. A raw engine build tree is not a standalone SDK package.

```powershell
cmake --install <engine-build> --prefix <sdk> --config Release
cmake -S Templates/Blank3D -B build/blank-3d -DSparkEngine_DIR="<sdk>/lib/cmake/SparkEngine"
cmake --build build/blank-3d --config RelWithDebInfo
```

Every package uses only:

- `find_package(SparkEngine CONFIG REQUIRED)`;
- `spark_add_game_module(...)` from the installed package;
- public headers under `SparkSDK/Include/Spark`;
- project-local source, reflected scenes, and metadata.

There are no relative includes of the engine source tree. Every gameplay module supports a deterministic `OnLoad(nullptr)` test seam and, with a real `IEngineContext`, appends its authored scene to the engine-owned world, consumes live input, synchronizes entities and camera state, renders, resizes, and removes only its own entities on unload. Visual scene composition stays editable and uses built-in procedural primitive paths. Each `Assets/` directory contains original transparent sprite/UI/material artwork plus a validated 3x3 runtime sheet; the live templates use those sheets for camera-relative HUD feedback. `EmptyProject` intentionally keeps its world and HUD empty while retaining the same scene/render lifecycle.

## Package layout

```text
Package/
|-- Assets/
|   |-- <package>_atlas.png
|   |-- README.md
|   `-- manifest.json
|-- Scenes/
|   `-- <Default>.sparkscene
|-- Source/
|   |-- GameModule.cpp
|   `-- GameModule.h
|-- CMakeLists.txt
|-- Package.sparkproject
|-- README.md
|-- spark.modules.json
`-- template.json
```

The package directory name remains a literal source token throughout its text files. SparkEditor can therefore safely materialize a project by replacing that token with the requested project name.

## Licensing and provenance

Template source, metadata, and repository-original generated artwork are covered by the repository's [Spark Open License 1.0](../LICENSE), not MIT. Every `Assets/README.md` and `Assets/manifest.json` records provenance and a SHA-256. No third-party art, audio, or fonts are bundled. Add verified license and provenance records before adding or redistributing replacement assets.
