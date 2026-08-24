# MultiplayerArena legacy template

MultiplayerArena is a bounded local arena simulation retained for compatibility outside the eight-template editor registry. It demonstrates lobby readiness, cyan-versus-magenta teams, countdown, scoring, respawn timers, and a deterministic match end without claiming a live network transport or dedicated server. Team IDs remain stable for host adapters: `1` is cyan and `2` is magenta.

The reflected `Scenes/Arena.sparkscene` uses public built-in primitives. `Assets/multiplayer_arena_atlas.png` supplies original cyan-versus-magenta character, pickup, arena, network, and UI artwork for a host adapter. Configure the generated project against an installed SparkEngine SDK with CMake 3.25 or newer.

All source and repository-original artwork are covered by the SparkEngine repository license. No third-party content is bundled.

## Runtime asset sheet

`Assets/multiplayer_arena_runtime_sheet.png` is the normalized 3x3 sprite sheet prepared for host adapters. `Assets/runtime_sheet.json` gives nine stable, named 418x418 source rectangles with transparent gutters. The larger atlas remains concept and reference art; sheet consumers should use the descriptor rather than guessing coordinates.
