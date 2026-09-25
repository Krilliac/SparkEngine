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
- schedules the `SparkGame.ShowcaseLifecycle` coroutine on the host's scheduler
  (`IEngineContext::GetCoroutineScheduler()`): it spawns a `CoroutineTarget` entity, waits 3 s, deals 25 damage
  and publishes an `EntityDamagedEvent`, waits 2 s, then heals the target back to 100 HP. `showcase_status` reports
  the step reached on its `Coroutine sequence:` line. If a step loses its target the sequence is cancelled and the
  line keeps the first failure (`failed: ...`). A host without a scheduler gets a logged warning and the line reads
  `unavailable (host exposes no CoroutineScheduler)`.
- places the Engine Showcase exhibit: four `MeshRenderer` props (`display_pedestal`, `info_signpost`,
  `supply_crate`, `light_pylon` from `Assets/Models/Showcase/Kit/`) in a row 4 m behind the spawned entities,
  facing them. They are removed with the other showcase entities on unload.

## Assets

The exhibit props are an original Blender 4.0.2 kit: `tools/blender/author_showcase_kit.py` (built on
`tools/blender/spark_kit.py`) writes the editable source `Art/Blender/SparkGame/showcase_kit.blend`, the OBJ/MTL
exports (each prop also has `_lod1` and `_collision` variants) and `Art/Blender/SparkGame/provenance.json`.
Colours come from the MTL base colours (graphite, brushed steel, porcelain, one Spark-amber accent); there are no
textures. `Art/Blender/SparkGame/preview.png` is a Workbench render of the kit.

```sh
PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
  --python tools/blender/author_showcase_kit.py -- --repo .
python3 tools/blender/validate_kit.py Art/Blender/SparkGame/provenance.json
```

`xvfb-run` is only needed for the preview render on a host without an EGL/GPU context. Every mesh path the source
names is recorded in `asset-references.json`.

Console commands: `showcase_status`, `showcase_weather`, `showcase_save`, `showcase_load`, and `showcase_spawn`.
`OnUnload()` removes the module's validation rules and tears the showcase down before the library is unmapped.
Teardown stops the lifecycle coroutine first; `StopCoroutine()` outside a scheduler tick destroys it immediately, so
no step callable that lives in this image is left in the scheduler.

## Known limitations

- There are no localization resources. `SetupLocalization()` loads no string table; lookups fall back to the
  keys themselves.
- The module has no networking. The exhibit props are placed but not yet reviewed in a rendered run, and the
  `_lod1`/`_collision` variants are exported but not used by the module.
- Still outstanding under MOD-300: rendered showcase output (`OnRender`), exact-state quickload evidence, and a
  packaged smoke run.

## Tests

`SparkGameShowcase_*` (`Tests/TestSparkGameShowcase.cpp`) loads the built SparkGame image through
`ModuleManager` with a host context that supplies a real `World`, `EventBus`, and the engine `CoroutineScheduler`,
then steps the scheduler with a fixed 1/64 s delta. The tests cover the full spawn, damage, and heal sequence;
stopping and destroying the coroutine at shutdown before unload; cancelling the sequence when its target is lost;
and the missing-scheduler warning. They are registered on Linux only (the test loads the `.so`); there is no Windows
lane for them yet.

`ModuleABI_AllValidationRuleOwnersReleaseCallbacksBeforeUnload` (`Tests/TestModuleABI.cpp`) checks that
`OnUnload()` releases the validation rules this module registers.
