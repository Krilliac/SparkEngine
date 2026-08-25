# TopDownStarter asset provenance

`top_down_atlas.png` is repository-original generated artwork with player/enemy units, tactical rings and arrows,
camera glyphs, grid materials, boundary corners, and a victory beacon. The reflected skirmish scene uses authored
`tactician`, `hunter_drone`, `energy_cell`, and `skirmish_wall` OBJ/MTL sets plus the built-in ground primitive. No
third-party content is bundled; `manifest.json` records every shipped asset and SHA-256.

## Normalized sprite sheet

`top_down_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
