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
#define SPARK_SDK_VERSION 3

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
