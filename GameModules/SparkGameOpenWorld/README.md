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

## Known limitations

- There is no content. The region scenes (`Assets/Scenes/OpenWorld/<Region>.scene`), the per-region streaming
  manifests under `Assets/OpenWorld/<Region>/`, and the `Assets/Audio/Music/ow_*.ogg` tracks that the source
  registers do not exist in the repository, so streaming and music have nothing to load.
- The module ships no asset root and has no networking.

## Tests

`Tests/TestOpenWorldModule.cpp` compiles the module's real system sources into SparkTests. `OW_*` tests cover the
data types; `Gated_OW*` tests (compiled when ImGui is available) cover the player, exploration, gathering,
wildlife, and persistence systems, including rejection of malformed save payloads.
