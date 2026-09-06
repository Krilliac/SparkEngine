# SparkEngine installed-SDK project templates

These nine packages are the physical source for SparkEditor's project templates and compatibility samples. Each is a standalone CMake project that consumes an installed SparkEngine SDK, produces one game-module shared library, loads a current reflected `.sparkscene`, and drives a live input/update/render loop with template-specific gameplay and artwork.

See the root README's [Live Template Showcase](../README.md#live-template-showcase)
for runtime hero, wide, and detail captures of every package.

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
| `MultiplayerArena` | Multiplayer Arena | `Scenes/Arena.sparkscene` |

`MultiplayerArena` remains a legacy compatibility sample outside the built-in registry, but follows the same installed-SDK, reflected-scene, deterministic-gameplay, and asset-provenance contract.

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
- headers from the installed SDK: the ABI-stable surface under `SparkSDK/Include/Spark`
  plus the engine headers the installer stages under `include/SparkEngine`
  (`Game/TemplateRuntime.h`, `Core/Reflection.h`, `Engine/ECS/Components.h`,
  `Graphics/GraphicsEngine.h`, `Graphics/WorldBasicRenderer.h`, `Input/InputManager.h`,
  `SceneManager/ReflectedSceneSerializer.h`, `Utils/LogMacros.h`);
- project-local source, reflected scenes, and metadata.

Only `SparkSDK/Include/Spark` carries the ABI promise. The `include/SparkEngine` headers are
source-level SDK surface for one engine version: a module that includes them is source-compatible
within that version and must be rebuilt against the SDK it ships with. There are no relative
includes of an engine *source* tree - every include above resolves through the installed package's
include directories, which `spark_add_game_module` adds.

Every gameplay module supports a deterministic `OnLoad(nullptr)` test seam. That seam returns
without resolving a scene or touching a world, so it proves construction only; `OnLoad(nullptr)`
succeeding is not evidence that a scene loaded. The eight packages that share
`Spark::Templates::TemplateRuntimeScene` report which happened through its `LastLoadResult()`
(`Deterministic`, `Loaded`, or `Failed`) and `IsActive()`; `FPSStarter` still carries its own copy
of that bridge and is the one package this accessor does not reach. With a real `IEngineContext` a module appends its authored scene to the engine-owned
world, consumes live input, synchronizes entities and camera state, renders, resizes, and removes
only its own entities on unload; `EmptyProject` is the exception and does not read input.

Visual scene composition stays editable and uses built-in procedural primitive paths. A scene
contract requires only the entities a module cannot run without (its camera and, where it has one,
its player); a missing decorative prop degrades that feature and logs one warning instead of
failing the load, so renaming or deleting authored scenery does not stop the module from loading.

Each `Assets/` directory contains original transparent sprite/UI/material artwork plus a validated
3x3 runtime sheet; the live templates use those sheets for camera-relative HUD feedback.
`EmptyProject` ships an empty authoring scene (`Scenes/Default.sparkscene`) plus an explicit
8-entity runtime preview (`Scenes/RuntimePreview.sparkscene`) and one HUD sprite, so it exercises
the same scene/render lifecycle as the gameplay packages without shipping gameplay.

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
|-- Config/
|   |-- gateway.ini
|   `-- server.ini
|-- Package.sparkproject
|-- README.md
|-- spark.modules.json
`-- template.json
```

The package directory name remains a literal source token throughout its text files. SparkEditor can therefore safely materialize a project by replacing that token with the requested project name.

## Local service topology

Every template includes the two project-relative files used by the launcher and editor:

- `Config/server.ini` selects the template's `spark.modules.json`, default scene, dedicated-server port, status file, and local gateway-control endpoint.
- `Config/gateway.ini` defines a matching one-area gateway topology and its status/stop files.

The server and gateway intentionally share `Config/gateway.key`. The editor provisions that project-local file from the operating-system random source the first time **Start gateway** is used, applies owner-only access (mode `0600` on POSIX), and never overwrites an existing key. Command-line deployments must provision the same file themselves with at least 32 cryptographically random bytes. The key is never included in a template or source-control commit. Build the template module before launching its dedicated server so the manifest can resolve the platform's module binary and ABI sidecar.

## Licensing and provenance

Template source, metadata, and repository-original generated artwork are covered by the repository's [Spark Open License 1.0](../LICENSE), not MIT. Every `Assets/README.md` and `Assets/manifest.json` records provenance and a SHA-256. No third-party art, audio, or fonts are bundled. Add verified license and provenance records before adding or redistributing replacement assets.

The 3x3 runtime sheets are regenerated by `Tools/normalize_template_runtime_sheets.py`, a
maintainer-time tool that requires [Pillow](https://pypi.org/project/Pillow/)
(`python -m pip install Pillow`); install it only when regenerating sheets. Runtime and CI
validation use `Tools/validate_template_runtime_sheets.py`, which is standard-library only and is
registered as the `SparkTemplateRuntimeSheets` test. Regenerate a sheet with the normalizer, then
re-run the validator and refresh the `Assets/manifest.json` SHA-256 before committing.
