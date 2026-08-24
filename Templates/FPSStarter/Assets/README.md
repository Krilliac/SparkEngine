# FPSStarter asset provenance

`fps_starter_atlas.png` is repository-original generated artwork directed specifically for this template. It contains a tactical training rifle, target drone, HUD glyphs, material swatches, decals, and an arena emblem on transparency. The current reflected scene remains renderer-independent and references SparkEngine built-in cube and ground primitives; host rendering/UI adapters can slice the atlas using their normal asset pipeline.

No third-party art, audio, or fonts are bundled. Template source, metadata, and original art remain covered by the SparkEngine repository license. The manifest records the atlas SHA-256 so packaged outputs can be verified.

## Normalized sprite sheet

`fps_starter_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
