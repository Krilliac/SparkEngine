# EmptyProject asset provenance

`empty_project_atlas.png` is repository-original generated artwork with lifecycle controls, timing/module glyphs, neutral materials, and a blank-project thumbnail frame. The editable default scene remains intentionally empty; graphical runs use a deliberately small repository-authored runtime preview. Its slate stage, crossed origin guides, tilted color marker, and beacon sphere make a successful 3D launch obvious while preserving the visual character of an empty starting point.

No third-party art, audio, or fonts are bundled. The manifest records every shipped preview asset and its SHA-256 for packaged-output verification.

## Normalized sprite sheet

`empty_project_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
