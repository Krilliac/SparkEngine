# Blank3D asset provenance

`blank3d_atlas.png` is repository-original generated artwork with transform gizmos, cameras, lights, primitive previews, technical materials, and viewport glyphs. `Scenes/Default.sparkscene` remains renderer-independent and references built-in primitives. No third-party content is bundled; the manifest records the atlas SHA-256.

## Normalized sprite sheet

`blank3d_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
