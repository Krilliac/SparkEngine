# SparkGameVisualScript

This module is a playable “zero C++ gameplay logic” example. The C++ module shell loads the generated AngelScript,
creates the demo entities, binds each script instance to its real ECS entity, and dispatches the script lifecycle.
Movement, jumping, patrol/chase behavior, damage, healing, collection, scoring, and win/lose rules remain in the
generated scripts under `Assets/Scripts/Generated`.

Those scripts are compiled from visual-script graphs checked in under `Assets/Graphs` (`PlayerController.vscript`,
`Collectible.vscript`, `EnemyPatrol.vscript`, `GameManager.vscript`, `HealthPickup.vscript`). The engine's
`VisualScriptGraphIO` reads them, and they open in the editor's Visual Script panel. The graphs are the source of
truth: `VisualScriptGraphs_CheckedInGraphsRegenerateShippedScripts` (`Tests/TestMOD390VisualScriptGraphsReal.cpp`)
compiles each one with `VisualScriptCompiler` and fails unless the result matches the shipped `.as` byte for byte. To
change gameplay, edit a graph and regenerate: run that test with `SPARK_VSCRIPT_OUTPUT_DIR=<dir>`, copy the `.as` it
writes over `Assets/Scripts/Generated` (and the canonical `.vscript` over `Assets/Graphs`), then update the SHA-256
values in `Assets/manifest.json`. Only the generated `.as` files and the cue audio (below) are staged into builds.

## Play

- `WASD` — move
- `Left Shift` — sprint
- `Space` — jump
- Collect all five glowing sphere pickups to win.
- Avoid the patrol pyramids; walk over the cube health pickup to heal.

Useful console commands:

- `vs_status` — current health, score, remaining pickups, and active-script count
- `vs_restart` — tear down and recreate the complete demo deterministically
- `vs_reload` — hot-reload the five scripts into the running demo, keeping its state (see below)
- `vs_help` — show controls in the console

Module load is intentionally fail-fast. All five scripts must exist and compile, and all eleven script instances must
attach successfully; otherwise the partial world is rolled back instead of presenting a silently broken example.
Each script must also declare `uint selfEntity = 0;` exactly once. The rejection diagnostic names the script file, and
the line when the fault has one (a compile error, a constructor fault while attaching, a duplicated placeholder).
File paths use forward slashes on every platform, matching the AngelScript builder's compile diagnostics.
This load, spawn and rollback path lives in `Source/Core/VisualScriptDemoWorld.cpp`, which the module shell calls from
`OnLoad` and `vs_restart`. `Tests/TestMOD390VisualScriptDiagnosticsReal.cpp` (the `VisualScriptDiagnostics_*` tests)
runs that file against a real `World` and `AngelScriptEngine`.

## Hot reload

Edit a graph, regenerate its `.as` (above), copy it into the script root the module loaded from (the load log names
it: `Validated 5 visual scripts from <root>`; a build runs the copy staged beside the executable), then run
`vs_reload`. `DemoWorld::ReloadScripts()` re-reads all five files and validates them exactly as a load does (compile,
declared class, one `selfEntity` placeholder) before it touches the running demo. Any failure prints the
`<file>:<line>` diagnostic and changes nothing, including scripts validated before the broken one. It then recompiles
each entity's per-entity module from its re-bound source through `AngelScriptEngine::HotReloadModuleFromSource()`.
Script fields carry over by the engine's hot-reload state rules (same name and type keep their value; see
`wiki/subsystems/Scripting-with-AngelScript.md`), `Start()` does not run again, and the ECS state the scripts own
(positions, health, the score kept in the game manager's health) is untouched. The command prints, per class, how
many instances reloaded and how many fields were carried, defaulted (new) or dropped (removed or retyped). A new
constructor that faults leaves that one entity without a script and is reported; `vs_restart` recovers.
A later `vs_restart` uses the reloaded sources. `Tests/TestMOD390VisualScriptHotReloadReal.cpp` (the
`VisualScriptHotReload_*` tests) reloads mid-game and plays on to the win, applies an edited speed constant, and
rejects a compile error, a lost placeholder and a renamed class.

`Tests/TestMOD390VisualScriptGameplayReal.cpp` (the `VisualScriptGameplay_*` tests) plays the shipped scripts headless.
It holds W/A/S/D through a real `InputManager` in the injected `EngineContext`, where the scripts' `getKey` reads it,
and ticks every script at the module's sanitized frame delta. The player collects all five coins and `GameManager`
announces the win with a score of 500. Enemy contact costs 10 HP per strike, and the health pack heals 30, hides,
and respawns after 10 seconds. The test reads outcomes only from state the scripts write: positions, health, and
their `print` output.

## Sound and animation cues

The scripts request media by name; the engine systems act on the request, so the C++ shell holds no cue logic.

- `playSound(entity, name)` queues a `ScriptAudioCues` cue on the entity (positioned at the entity's `Transform` when
  it has one). The engine's `AudioUpdateSystem` starts each queued cue through `AudioEngine` in the Audio phase and
  clears the queue; a cue whose sound is not loaded is counted as dropped. Collectible plays `coin_pickup`,
  EnemyPatrol `enemy_attack`, GameManager `victory_fanfare`, and HealthPickup `health_pickup` and `pickup_respawn`.
- `playAnimation(entity, clip)` switches the entity's `AnimationController` to a clip it lists and never creates a
  controller, so `DemoWorld::Spawn` gives every entity whose script requests a clip its controller: coins list
  `idle` and `collect_burst`, enemies `idle`, `walk` and `attack_swing`. Re-requesting the playing clip (EnemyPatrol
  asks for `walk` every patrol frame) does not restart it, and `AnimationUpdateSystem` advances it.

Each cue's audio ships in `Assets/Audio/VisualScript/<cue>.wav`, placeholder stingers composed by
`tools/audio/compose_visualscript_cues.py` (deterministic, CC0; rerun it and update the SHA-256 values in
`Assets/manifest.json` to change them). The build stages that directory beside the scripts, and `OnLoad` registers
every cue in `SoundCues` (`Source/Core/VisualScriptDemoRuntime.h`) with the engine's `AudioEngine` from the content root
the scripts were loaded from, and unloads them in `OnUnload`. Audio is optional: without an `AudioEngine` (headless,
server) or with a missing file the module still loads, logs a warning naming the file, and that cue is dropped.
`VisualScriptDemo_EverySoundCueShipsItsAudio` fails when a script's `playSound` names a cue that is not in `SoundCues`
or has no shipped WAV.

`VisualScriptGameplay_ScriptCuesReachAudioQueueAndAnimationControllers` plays the coin route to the win and checks
each collected coin's `coin_pickup` cue and `collect_burst` clip, the GameManager's `victory_fanfare` cue, and that a
patrolling enemy's `walk` clip keeps advancing under `AnimationUpdateSystem`.

## Blueprint-lab kit

After a successful spawn, `DemoWorld::PlaceKitProps` dresses the demo with the Blender-authored kit in
`Assets/Models/VisualScript/Kit`: a `pressure_plate` under the player spawn with a `lever` beside it, and a
`sliding_door` behind the coin row flanked by two `signal_lamp`s. The five `VSKit_*` entities carry only a transform
and a mesh, no script, so the eleven-script-entity contract is unchanged; `GetKitProps()` lists them, and
`DestroyEntities()` removes them on restart, rollback and unload. They are set dressing: the lever, plate and door do
not drive gameplay, and nobody has reviewed them in a running engine yet. Source, provenance, and preview are in
`Art/Blender/SparkGameVisualScript/`, and `asset-references.json` records every asset path the module source names.

## AngelScript build contract

AngelScript is enabled by default when the complete vendored SDK is present. The root build compiles the core runtime
and the `scriptarray`, `scriptbuilder`, and `scriptstdstring` add-ons used by `AngelScriptEngine`. To opt out, configure
with `-DENABLE_ANGELSCRIPT=OFF`.

If support is disabled or the vendored SDK is incomplete, SparkEngine keeps its scripting stub so non-scripted targets
can still compile. This module then rejects `OnLoad` with a clear diagnostic and does not register `vs_status`,
`vs_restart`, `vs_reload`, or `vs_help`; it never reports a partially working visual-script game.
