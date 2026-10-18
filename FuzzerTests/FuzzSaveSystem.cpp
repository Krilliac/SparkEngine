/**
 * @file FuzzSaveSystem.cpp
 * @brief Production-entry-point libFuzzer harness for the .spark_save reader
 *        (Spark::DecodeSaveFileBytes, the parser behind SaveSystem::ReadFromFile).
 */

#include "FuzzSaveSystemProduction.h"

#include <cstddef>
#include <cstdint>

// A .spark_save is a flat sequence of length-prefixed records (entities hold
// components, components hold properties) with fixed nesting and no recursion,
// so there is no structural depth to bound beyond a single level.
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
    return SparkFuzzDecodeSaveFile(data, size);
}
