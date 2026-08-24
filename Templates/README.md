# SparkEngine installed-SDK project templates

These eight packages are the physical source for SparkEditor's built-in project templates. Each is a standalone CMake project that consumes an installed SparkEngine SDK, produces one game-module shared library, opens a current reflected `.sparkscene`, and bundles deterministic gameplay source plus an original, template-specific visual atlas.

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

There are no relative includes of the engine source tree. Gameplay behavior is represented by deterministic state machines with public accessors, allowing a package to be tested with `OnLoad(nullptr)`. Visual scene composition stays editable and uses built-in procedural primitive paths; each `Assets/` directory also contains an original transparent sprite/UI/material atlas ready for a host rendering adapter.

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
