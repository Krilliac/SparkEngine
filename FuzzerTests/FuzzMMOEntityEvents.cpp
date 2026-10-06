/**
 * @file FuzzMMOEntityEvents.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkGameMMO client-side EntitySpawn /
 *        EntityDestroy decoders (MMO::DecodeEntitySpawn, MMO::DecodeEntityDestroy).
 */

#include "FuzzMMOEntityEventsProduction.h"

#include <cstddef>
#include <cstdint>

// A spawn is fixed fields plus one length-prefixed string; a destroy is one integer. No nesting.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 1024;

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
    return SparkFuzzDecodeMMOEntityEvents(data, size);
}
