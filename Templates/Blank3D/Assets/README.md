# Blank3D asset provenance

`blank3d_atlas.png` is repository-original generated artwork with transform gizmos, cameras, lights, primitive previews, technical materials, and viewport glyphs. `Scenes/Default.sparkscene` builds its hero, scale studies, modular arch, sphere, and cylinder from built-in primitives plus the tiny repository-authored `Models/spectrum_block.obj` and `Models/studio_block.obj` transform blocks. Their shared MTL palette makes material grouping visible in a live engine run without external textures. No third-party content is bundled; the manifest records every shipped showcase asset and its SHA-256 value.

## Normalized sprite sheet

`blank3d_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
