# ThirdPersonStarter asset provenance

`third_person_atlas.png` is repository-original generated artwork with an adventurer, crystal pickup, goal arch, traversal glyphs, and warm environment materials. The scene remains renderer-independent and references built-in primitives. No third-party content is bundled; the manifest records the atlas SHA-256.

## Normalized sprite sheet

`third_person_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
