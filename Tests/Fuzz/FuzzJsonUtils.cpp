/**
 * @file FuzzJsonUtils.cpp
 * @brief Production-entry-point libFuzzer harness for Spark::Json::ParseBounded.
 */

#include "Utils/JsonUtils.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

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

    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const Spark::Json::JsonLimits limits{
        SPARK_FUZZ_MAX_INPUT_BYTES,
        SPARK_FUZZ_MAX_DEPTH,
        4096,
    };
    Spark::Json::Value value;
    std::string error;
    (void)Spark::Json::ParseBounded(input, limits, &value, &error);
    return 0;
}
