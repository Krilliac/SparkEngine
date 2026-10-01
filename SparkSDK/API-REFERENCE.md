# SparkEngine SDK API reference

This is the installed SDK's short API reference. The complete declarations are
in the public headers under `include/Spark/`.

## Module lifecycle (`Spark/IModule.h`)

An installed game module exports `CreateModule`, `DestroyModule`, and the
compatibility export supplied by `Spark/ModuleABI.h`. The interface exposes
these hooks; optional callbacks depend on the host's scheduling and features:

1. `OnLoad(IEngineContext*)`
2. `OnUpdate(float)` and, when scheduled, `OnFixedUpdate(float)`
3. `OnRender()` and optional event hooks
4. `CanUnload()` to report whether module-owned work is ready for unloading
5. `OnUnload()` to release registrations and owned state

`GetModuleInfo()` returns module-owned name and version strings, the SDK
version, load order, dependencies, and `ModuleKind`. A `Game` module owns the
simulation slot; `Addon` modules can coexist with the game module.

## Engine services (`Spark/IEngineContext.h`)

`IEngineContext` is the service locator passed to `OnLoad`. Core getters expose
graphics, input, timer, and event bus services. Optional subsystem getters may
return `nullptr` when a subsystem is disabled or not initialized; callers must
check before use. Use the public service interfaces in the SDK for logging,
console commands, state validation, networking, and telemetry. The context is
borrowed and does not transfer ownership to the module.

## Compatibility descriptor (`Spark/ModuleABI.h`)

`SparkModuleCompatibilityDescriptor` is a fixed C-layout descriptor read before
the C++ factory is called. The host checks the magic, descriptor format, SDK
and runtime ABI versions, compiler family and version, C++ language level,
runtime library, iterator debug level, and pointer size. The descriptor is
64 bytes for descriptor version 1 and `SPARK_SDK_VERSION` 9.

`Spark::CheckModuleCompatibility()` returns the status. For a diagnostic,
`Spark::GetModuleCompatibilityMismatch()` identifies the sidecar field and
the module and host values. The `.sparkabi` sidecar uses the same field names.
