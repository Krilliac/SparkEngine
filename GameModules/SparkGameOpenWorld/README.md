# SparkGameOpenWorld

SparkGameOpenWorld is a prototype of open-world survival and exploration systems: player survival state, regions
and points of interest, fast travel, resource gathering and crafting, wildlife, settlements, and dynamic events.

**Release classification:** experimental prototype, outside the stable-v1 release profile
(`tools/module-evidence/manifest.json`). Finishing it as a streamed survival/exploration slice is tracked under
MOD-360.

## What runs

`Source/Core/Main.cpp` creates the world setup, player, exploration, wildlife, settlement, gathering,
dynamic-event, and engine-bridge systems on load. Discovering a point of interest unlocks it as a fast-travel
destination. `OWWorldSetup` registers each region with the engine's seamless area streaming manager.
`ow_save` and `ow_load` write and restore the gameplay state through the module's persistence codec
(`Source/Persistence`). The `ow_*` console commands (`ow_status`, `ow_explore`, `ow_harvest`, `ow_craft`,
`ow_tame`, `ow_fast_travel`, and others) drive the systems.

## Assets

Every asset path the source names is a complete literal, and `asset-references.json` records each one with its
sha256, kind and the `tools/asset-integrity/provenance.json` rule that licenses it.
`python3 tools/check-module-asset-refs.py --module SparkGameOpenWorld` fails if a referenced file is missing,
differs in case, is composed at run time, or disagrees with that record or `Assets/assets.integrity.json`.

- Each region's streaming manifest (`OWWorldSetup::RegisterAreasWithStreaming`) names its ground tile
  `Assets/Models/OpenWorld/Ground/<region>_ground.obj`, the three OpenWorld landmark props in
  `Assets/Models/ModuleKits/OpenWorld/`, a terrain albedo from `Assets/Textures/Terrain/`, the flat normal map
  and `Assets/Audio/ambient_wind.wav`. Regions have no scene file; the manifest is the streamed bundle.
- The nine music tracks are `Assets/Audio/OpenWorld/Music/ow_*.wav`.
- The ground tiles and music loops are repository-original procedural output of
  `tools/generate_default_assets.py` (CC0). They are development content, not authored art or music.

## Known limitations

- There is no input-driven player controller; movement happens through `SetPosition`, fast travel and console
  commands.
- `MusicManager` does not decode audio files, so registering the tracks does not prove audible playback.
- The procedural ground tiles and music loops need owner acceptance, or replacement with authored content, before
  a release-quality claim.
- The module has no networking.

## Tests

`Tests/TestOpenWorldModule.cpp` compiles the module's real system sources into SparkTests. `OW_*` tests cover the
data types; `Gated_OW*` tests (compiled when ImGui is available) cover the player, exploration, gathering,
wildlife, and persistence systems, including rejection of malformed save payloads.
`Tests/TestMOD360OpenWorldPersistenceReal.cpp` runs save, restart and load through the real `SaveSystem`
(`OpenWorldPersistence_*`) and requires every music track and area-manifest path the real systems register to
exist (`OpenWorldAssets_AllRegisteredAssetsExist`). `Tests/Tools/test_check_module_asset_refs.py`
(`OpenWorldAssets_ReferenceCheckFailsClosed`) covers the reference checker.
