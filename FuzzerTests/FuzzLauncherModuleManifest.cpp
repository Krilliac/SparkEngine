/**
 * @file FuzzLauncherModuleManifest.cpp
 * @brief Production-entry-point libFuzzer harness for the launcher spark.modules.json reader
 *        (SparkLauncher::BuildLaunchRequest for LaunchTarget::Game).
 */

#include "FuzzLauncherModuleManifestProduction.h"

#include <cstddef>
#include <cstdint>

// Json::JsonLimits{}.maxDepth: ParseBounded's nesting bound for the manifest.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 128;
// Keep the fuzzer's accepted input range aligned with the production manifest cap.
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 1048576;

static_assert(SPARK_FUZZ_MAX_DEPTH == 128);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }
    if (data == nullptr && size != 0)
    {
        return 0;
    }
    return SparkFuzzResolveLauncherManifest(data, size);
}
