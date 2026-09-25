# SparkGameVisualScript

This module is a playable “zero C++ gameplay logic” example. The C++ module shell loads the generated AngelScript,
creates the demo entities, binds each script instance to its real ECS entity, and dispatches the script lifecycle.
Movement, jumping, patrol/chase behavior, damage, healing, collection, scoring, and win/lose rules remain in the
generated scripts under `Assets/Scripts/Generated`.

## Play

- `WASD` — move
- `Left Shift` — sprint
- `Space` — jump
- Collect all five glowing sphere pickups to win.
- Avoid the patrol pyramids; walk over the cube health pickup to heal.

Useful console commands:

- `vs_status` — current health, score, remaining pickups, and active-script count
- `vs_restart` — tear down and recreate the complete demo deterministically
- `vs_help` — show controls in the console

Module load is intentionally fail-fast. All five scripts must exist and compile, and all eleven script instances must
attach successfully; otherwise the partial world is rolled back instead of presenting a silently broken example.
Each script must also declare `uint selfEntity = 0;` exactly once. The rejection diagnostic names the script file, and
the line when the fault has one (a compile error, a constructor fault while attaching, a duplicated placeholder).
File paths use forward slashes on every platform, matching the AngelScript builder's compile diagnostics.
This load, spawn and rollback path lives in `Source/Core/VisualScriptDemoWorld.cpp`, which the module shell calls from
`OnLoad` and `vs_restart`. `Tests/TestMOD390VisualScriptDiagnosticsReal.cpp` (the `VisualScriptDiagnostics_*` tests)
runs that file against a real `World` and `AngelScriptEngine`.

## AngelScript build contract

AngelScript is enabled by default when the complete vendored SDK is present. The root build compiles the core runtime
and the `scriptarray`, `scriptbuilder`, and `scriptstdstring` add-ons used by `AngelScriptEngine`. To opt out, configure
with `-DENABLE_ANGELSCRIPT=OFF`.

If support is disabled or the vendored SDK is incomplete, SparkEngine keeps its scripting stub so non-scripted targets
can still compile. This module then rejects `OnLoad` with a clear diagnostic and does not register `vs_status`,
`vs_restart`, or `vs_help`; it never reports a partially working visual-script game.
