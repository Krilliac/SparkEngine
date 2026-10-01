/**
 * @file FuzzPluginMetadata.cpp
 * @brief Production-entry-point libFuzzer harness for the dynamic-plugin metadata gate
 *        (Spark::ValidatePluginMetadata).
 */

#include "FuzzPluginMetadataProduction.h"

#include <cstddef>
#include <cstdint>

// The schema is one flat JSON object; nested values are parsed by Json::ParseStrict
// (bounded there) and then rejected, so the schema depth is one level.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 70000;

static_assert(SPARK_FUZZ_MAX_DEPTH == 1);

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
    return SparkFuzzValidatePluginMetadata(data, size);
}
