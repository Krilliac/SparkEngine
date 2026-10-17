/**
 * @file FuzzFreezeBufferBlob.cpp
 * @brief Production-entry-point libFuzzer harness for the save-state freeze/thaw blob reader
 *        (Spark::FreezeSystem::ThawAll over Spark::FreezeBuffer).
 */

#include "FuzzFreezeBufferBlobProduction.h"

#include <cstddef>
#include <cstdint>

// The blob is a flat run of length-prefixed tags, strings and fixed records: no
// construct nests, so the reader recurses zero levels.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

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
    return SparkFuzzThawFreezeBlob(data, size);
}
