/**
 * @file Version.h
 * @brief SparkEngine SDK version information and compatibility checks
 *
 * Defines the engine and SDK version constants used for module compatibility
 * verification. When the engine loads a module, it checks the module's
 * SPARK_SDK_VERSION against its own to ensure ABI compatibility.
 */

#pragma once

#include <Spark/GeneratedVersion.h>

#include <cstdint>

// SDK ABI version — increment when IModule, IEngineContext, or any SDK
// interface changes in a binary-incompatible way.
// v2: Added 7 subsystem getters, IModule lifecycle hooks, ILogger, math/input/event types
// v3: Added IModule::CanUnload() as a non-destructive unload/hot-reload veto.
// v4: Appended IEngineContext::GetInvalidStateDetector()/GetComponentSerializers()
//     (four vtable slots). IsSDKCompatible is exact equality, so a module built
//     against v4 must not be accepted by a v3 host: the host's vtable is shorter
//     and the call would run off the end of it.
// v5: Removed IEngineContext::InitializeAll()/ShutdownAll() (two vtable slots,
//     OD-01): EngineRuntime owns subsystem lifecycle, so every later slot moved.
// v6: Appended IEngineContext::GetLogger() (one vtable slot, MOD-295) so modules
//     log through the host's ILogger instead of the private console headers.
// v7: Appended IEngineContext::GetConsole() (one vtable slot, MOD-295) and the
//     IConsole interface, so modules register console commands through the host
//     instead of the private Utils/SparkConsole.h.
// v8: Appended IConsole::Print() (one IConsole vtable slot, MOD-310) so modules
//     write to the host's in-game console instead of the private LOG_TO_CONSOLE
//     macros. Migration: rebuild every module against v8; a module that
//     implements IConsole itself must add Print.
// v9: Appended IEngineContext::GetStateValidation() (one vtable slot, MOD-295)
//     and the IStateValidation interface, so modules register ECS invalid-state
//     rules through the host instead of the private Utils/InvalidStateDetector.h.
//     StateViolationSeverity, StateViolation and StateCheckFn moved into the SDK
//     unchanged.
// v10: Appended IEngineContext::GetWeatherService() (one vtable slot, MOD-310)
//      and public IWeatherService/WeatherPreset, so modules issue weather commands
//      without including the private WeatherSystem. Rebuild every module against
//      v10; SDK compatibility remains exact-match and old GetWeather slots remain.
#define SPARK_SDK_VERSION 10

// Packed engine version for runtime comparisons: 0xMMmmpp
#define SPARK_ENGINE_VERSION_PACKED                                                                                    \
    ((SPARK_ENGINE_VERSION_MAJOR << 16) | (SPARK_ENGINE_VERSION_MINOR << 8) | SPARK_ENGINE_VERSION_PATCH)

namespace Spark
{

    /** @brief Get the packed engine version at runtime */
    inline constexpr uint32_t GetEngineVersion()
    {
        return SPARK_ENGINE_VERSION_PACKED;
    }

    /** @brief Get the SDK ABI version at runtime */
    inline constexpr uint32_t GetSDKVersion()
    {
        return SPARK_SDK_VERSION;
    }

    /**
 * @brief Check if a module's SDK version is compatible with this engine
 * @param moduleSDKVersion The SDK version the module was compiled against
 * @return true if compatible
 */
    inline constexpr bool IsSDKCompatible(uint32_t moduleSDKVersion)
    {
        return moduleSDKVersion == SPARK_SDK_VERSION;
    }

} // namespace Spark
