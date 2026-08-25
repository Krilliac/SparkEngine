# MMOStarter asset provenance

`mmo_starter_atlas.png` is repository-original generated artwork with Azure and Ember characters, a capture beacon,
bot/network markers, faction UI, respawn visuals, banners, and materials. The reflected frontier scene uses the authored
`astra`, `training_bot`, `capture_beacon`, and `frontier_relay` OBJ/MTL sets plus the built-in ground primitive. No
third-party content is bundled; `manifest.json` records every shipped asset and SHA-256.

## Normalized sprite sheet

`mmo_starter_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
