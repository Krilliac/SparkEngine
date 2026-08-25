# MultiplayerArena asset provenance

`multiplayer_arena_atlas.png` is repository-original generated artwork with cyan and magenta champions, energy weapons and pickups, spawn pad, matching team flags, scoreboard frame, network/server glyphs, arena materials, and decals. The reflected arena pairs the built-in ground with repository-original `cyan_duelist`, `magenta_duelist`, `shield_pickup`, and `arena_divider` OBJ/MTL sets. Stable gameplay team IDs are `1` for cyan and `2` for magenta. No third-party content is bundled; `manifest.json` records every authored asset SHA-256.

## Normalized sprite sheet

`multiplayer_arena_runtime_sheet.png` contains nine normalized transparent assets on a fixed 3x3 grid. `runtime_sheet.json` is the machine-readable slicing contract. Every 418x418 cell has at least 38 pixels of safe padding; the repository validator checks PNG alpha, geometry, gutters, and locked hashes.
