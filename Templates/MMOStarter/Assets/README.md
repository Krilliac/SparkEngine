# MMOStarter asset provenance

`mmo_starter_atlas.png` is repository-original generated artwork with Azure and Ember characters, a capture beacon, bot/network markers, faction UI, respawn visuals, banners, and materials. The frontier remains renderer-independent and uses built-in primitives. No third-party content is bundled; the manifest records the atlas SHA-256.

## Normalized sprite sheet

`mmo_starter_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
