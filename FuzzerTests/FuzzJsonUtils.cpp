/**
 * @file FuzzJsonUtils.cpp
 * @brief Production-entry-point libFuzzer harness for Spark::Json::ParseBounded.
 */

#include "FuzzJsonUtilsProduction.h"

constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 32;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 4096;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }
    if (data == nullptr)
    {
        return 0;
    }
    return SparkFuzzParseJson(data, size, SPARK_FUZZ_MAX_DEPTH);
}
