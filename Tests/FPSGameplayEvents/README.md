# Public FPS event consumer

This consumer compiles the production `FPSGameplayEvents.cpp` adapter with only
the FPS source tree and a staged copy of `Spark/EventBus.h`, `Spark/EventSystem.h`
and `Spark/EventTypes.h`. It does not add `SparkEngine/Source` or link
`SparkEngineLib`. The event declarations and implementation are canonical in the
SDK; the old engine headers forward to them.

Configure this standalone consumer against an installed SDK include root:

```powershell
cmake -S Tests/FPSGameplayEvents -B <consumer-build> -G "Ninja Multi-Config" -DSPARK_ENGINE_INCLUDE_DIR=<sdk>/include
cmake --build <consumer-build> --config Release --parallel 1
ctest --test-dir <consumer-build> -C Release --output-on-failure
```

The standalone project can also locate the include root through
`-DSparkEngine_DIR=<sdk>/lib/cmake/SparkEngine`. Use the same compiler, C++ runtime
and configuration ABI as the host. The three required event headers must be
present in the installed SDK; an older SDK fails configuration.

The normal engine test build also adds this directory, staging the headers from
the source SDK. That mode proves the source boundary, while running the standalone
project against a fresh CMake install additionally proves package inclusion.

The registered tests are:

- `SparkFPSGameplayEvents`: real inventory/quest mutations and publication,
  including rejected additions, null buses and repeated quest completion.
- `SparkPublicEventContract`: real public synchronous dispatch, deferred priority
  and FIFO order, queue overflow, callback-time enqueue, clear, exception isolation,
  handle moves, unsubscribe and a handle that outlives its bus.
- `SparkFPSPublicEventsModuleLifecycle` (Windows and Unix): loads a test shared
  image containing the real FPS adapter, passes a host-owned bus, observes events
  in both directions, destroys module subscriptions, clears queued callbacks and
  channels, and performs actual unload/reload three times. Fresh module static
  state on every load checks that the image was released between cycles.

The module test uses an injected `EventBus*`, the same borrowed object passed to
the production adapter. It does not instantiate the full FPS `IModule` or prove
the engine's entire shutdown path. A module's `EventBus::Global()` is not a
substitute for `IEngineContext::GetEventBus()` across shared-library boundaries.

Unsubscription alone does not release the bus's channel objects. Their RTTI,
virtual destructors and erased callbacks can reside in the module image. The
host must stop publishers and in-flight dispatch, release module subscriptions,
drain or clear module-created queued work, and destroy channels with `ClearAll()`
while the image is still mapped. A module must not clear other host subscribers
on its own. These lifecycle and same-toolchain C++ ABI requirements are preserved
by moving the headers; this extraction changes no type layout or virtual slot.

The complete production FPS DLL still uses private concrete engine services and
links `SparkEngineLib`. This test proves the event migration slice only.
