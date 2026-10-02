/**
 * @file FuzzModManifest.cpp
 * @brief Production-entry-point libFuzzer harness for mod discovery
 *        (Spark::ModSystem::ScanForMods over untrusted mod.json manifests).
 */

#include "FuzzModManifestProduction.h"

#include <cstddef>
#include <cstdint>

// ModSystemIO parses every manifest with Json::ParseBounded at maxDepth 16.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 16;
// Two 64 KiB manifests and the 0x00 byte that separates them.
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 131073;

static_assert(SPARK_FUZZ_MAX_DEPTH == 16);

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
    return SparkFuzzScanMods(data, size);
}
