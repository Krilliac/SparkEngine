# RPGStarter asset provenance

`rpg_atlas.png` is repository-original generated artwork with hero, elder, warden, relic,
quest/inventory/dialogue/combat/reward/save visuals, and fantasy materials. The reflected village scene uses six
authored OBJ/MTL sets for its cast and buildings plus the built-in ground primitive. No third-party content is bundled;
`manifest.json` records every shipped asset and SHA-256.

## Normalized sprite sheet

`rpg_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
