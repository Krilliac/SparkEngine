/**
 * @file FuzzCrashManifest.cpp
 * @brief Production-entry-point libFuzzer harness for crash-manifest JSON.
 */

#include "FuzzCrashManifestProduction.h"

#include <cstddef>
#include <cstdint>

namespace
{
    constexpr std::size_t SPARK_FUZZ_MAX_DEPTH = 16;
    constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;
    static_assert(SPARK_FUZZ_MAX_DEPTH == 16);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
        return 0;
    if (data == nullptr || size == 0)
        return 0;
    return SparkFuzzParseCrashManifest(data, size);
}
