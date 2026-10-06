# SparkEngine SDK migration notes

The SDK ABI is an exact-match contract. `SPARK_SDK_VERSION` is defined in
`include/Spark/Version.h`; a module built against another ABI version must be
rebuilt against the installed SDK before loading. There is no N-1 module load
or automatic ABI migration.

## Current version: SDK ABI v10

Version 10 appends `IEngineContext::GetWeatherService()` and adds the public
`Spark/IWeatherService.h` interface with explicit `WeatherPreset` values.
Rebuild every module against SDK10 and regenerate its sidecar; SDK9 modules
are rejected before loading. Do not edit a sidecar to disguise an old binary.

Use the borrowed service on the game thread for weather commands. Check for
a null getter and a false command result. The service is host-owned; accepted
commands start a transition rather than reporting its completion. The old
private `GetWeather()` getters remain in their original slots for migration;
new module code should use the public command interface.

The descriptor stays version1/64 bytes and runtime ABI1. This SDK bump does
not change the separate plugin ABI or enable N-1 module loading. Prior SDK9
qualification evidence remains attached to its original binaries.

## Previous version: SDK ABI v9

Version 9 appended `IEngineContext::GetStateValidation()` and the public
`IStateValidation` interface. Modules that used the engine's private
`Utils/InvalidStateDetector.h` must include the SDK interface and register
their rules through `IEngineContext::GetStateValidation()` instead.

The version history in `include/Spark/Version.h` records the earlier ABI
changes. In particular, v8 added `IConsole::Print()` and v7 added
`IEngineContext::GetConsole()`. Modules migrating from those versions should
rebuild against the current headers and replace private console macros with
the public console interface.

## Migration procedure

1. Install the matching `sdk` component and point `SparkEngine_DIR` at its
   installed CMake package.
2. Rebuild the module with the installed public headers and the same supported
   toolchain/runtime settings as the host.
3. Ensure the generated `.sparkabi` sidecar is shipped beside the module.
4. If loading is rejected, inspect the named sidecar field and rebuild; do not
   copy private engine headers or libraries into the consumer project.

