# SparkGame

SparkGame is the base showcase module. It demonstrates how a game module reaches engine subsystems through
`Spark::IEngineContext`; it is not a game.

**Release classification:** experimental showcase, outside the stable-v1 release profile
(`tools/module-evidence/manifest.json`). Completing and repositioning it is tracked under MOD-300.

## What runs

`SparkGameDefaultModule` (`Source/Core/Main.cpp`) registers the base game-state validation rules and owns a
`GameplayShowcase` (`Source/Core/GameplayShowcase.cpp`), which on load:

- subscribes to damage, kill, and weather events on the engine `EventBus`;
- configures the time-of-day cycle and cycles weather types on a timer;
- registers a `TagComponent` save serializer (only when the engine has not already registered one) and spawns
  three ECS entities with transform, health, and tag components.

Console commands: `showcase_status`, `showcase_weather`, `showcase_save`, `showcase_load`, and `showcase_spawn`.
`OnUnload()` removes the module's validation rules and tears the showcase down before the library is unmapped.

## Known limitations

- The coroutine demo is log-only. `StartShowcaseCoroutine()` prints that the spawn, damage, and heal sequence is
  configured, but no coroutine is scheduled because the module does not include `CoroutineScheduler.h`.
- There are no localization resources. `SetupLocalization()` loads no string table; lookups fall back to the
  keys themselves.
- The module ships no assets and has no networking.

## Tests

The only test that reads this module's source is
`ModuleABI_AllValidationRuleOwnersReleaseCallbacksBeforeUnload` (`Tests/TestModuleABI.cpp`). It checks that
`OnUnload()` releases the validation rules this module registers. No test exercises the showcase behavior.
