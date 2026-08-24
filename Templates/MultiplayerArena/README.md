# MultiplayerArena legacy template

MultiplayerArena is a bounded local arena simulation retained for compatibility outside the eight-template editor registry. It demonstrates lobby readiness, two teams, countdown, scoring, respawn timers, and a deterministic match end without claiming a live network transport or dedicated server.

The reflected `Scenes/Arena.sparkscene` uses public built-in primitives. `Assets/multiplayer_arena_atlas.png` supplies original cyan-versus-magenta character, pickup, arena, network, and UI artwork for a host adapter. Configure the generated project against an installed SparkEngine SDK with CMake 3.25 or newer.

All source and repository-original artwork are covered by the SparkEngine repository license. No third-party content is bundled.
