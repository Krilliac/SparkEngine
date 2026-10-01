/**
 * @file FuzzMaterialLoader.cpp
 * @brief Production-entry-point libFuzzer harness for the .sparkmat material reader
 *        (Spark::Graphics::ParseSparkMatDefinition).
 */

#include "FuzzMaterialLoaderProduction.h"

#include <cstddef>
#include <cstdint>

// A .sparkmat file is a flat list of key = value lines with no nesting.
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
    return SparkFuzzParseSparkMat(data, size);
}
