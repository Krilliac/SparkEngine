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

## Weather commands (`Spark/IWeatherService.h`)

`GetWeatherService()` returns a borrowed optional `IWeatherService`. Call
`SetWeather(WeatherPreset, intensity, transitionSeconds)` on the game thread.
Presets are Clear=0, Rain=1, Snow=2, Fog=3 and Storm=4. Invalid presets,
nonfinite arguments or unavailable weather return false without mutation.
Finite negative intensity selects the preset default; other intensities clamp
to [0,1], and transitions have a 0.01-second minimum. Success means accepted.

## UI theme (`Spark/UiTheme.h`)

A header-only accessor for module ImGui code (HUDs, menus, overlays) that
should match the host's Ember Signal look. It is not part of `SparkSDK.h`:
include `<Spark/UiTheme.h>` directly, and define `SPARK_HAS_IMGUI` for the
module target, because the whole header compiles to nothing without it. Every
function runs on the thread that owns the current ImGui context.

The engine's game overlay publishes a 64-byte `Spark::Ui::Sdk::SparkUiRuntimeBlock`
through `ImGui::GetIO().UserData` before it injects the ImGui context into
modules. The block carries a colour for each `UiRole` (`Text`, `TextSecondary`,
`Accent`, `TextOnAccent`, `Success`, `Warning`, `Error`, `Info`,
`SurfaceOverlay`), the effective UI `scale`, the logical body, small and H1
font sizes, and the `reduceMotion` and `highContrast` flags. The host owns
the block; a module only reads it and never stores the pointer across frames.

- `Runtime()` returns the block, or `nullptr` when there is no current ImGui
  context, `UserData` is null, or the magic (`kRuntimeBlockMagic`), version
  (`kRuntimeBlockVersion`, 1) or size does not match.
- `Color(role, fallback)` returns the host colour for `role`, or `fallback`
  when there is no valid block. `kFallbackSuccess`, `kFallbackWarning`,
  `kFallbackError` and `kFallbackTextOnAccent` are the Ember Dark values.
- `FindFont(name)` returns the atlas font whose config name is `name`
  (`"Spark.Body"`, `"Spark.Strong"`, `"Spark.Bold"` or `"Spark.Mono"`), or
  the current font when none matches; `HudFont()`, `HudFontBold()` and
  `MonoFont()` are shorthands. It returns `nullptr` only without a context.
- `PushSuccessButton()` pushes four success-tinted button colours with
  `TextOnAccent` text; pair it with `PopSuccessButton()`.

Read `Runtime()` and the colours each frame rather than caching them, so a
module follows the host's scale and High Contrast changes. Animate only when
`reduceMotion` is 0. The header exports no symbols and is not part of the
module binary ABI: the block has its own magic, version and size check, and
`SPARK_SDK_VERSION` did not change when it was added.

## Compatibility descriptor (`Spark/ModuleABI.h`)

`SparkModuleCompatibilityDescriptor` is a fixed C-layout descriptor read before
the C++ factory is called. The host checks the magic, descriptor format, SDK
and runtime ABI versions, compiler family and version, C++ language level,
runtime library, iterator debug level, and pointer size. The descriptor is
64 bytes for descriptor version 1 and `SPARK_SDK_VERSION` 11.

`Spark::CheckModuleCompatibility()` returns the status. For a diagnostic,
`Spark::GetModuleCompatibilityMismatch()` identifies the sidecar field and
the module and host values. The `.sparkabi` sidecar uses the same field names.
