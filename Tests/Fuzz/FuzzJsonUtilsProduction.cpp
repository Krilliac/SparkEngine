/**
 * @file FuzzJsonUtilsProduction.cpp
 * @brief libc++-compiled production adapter for the JSON libFuzzer harness.
 */

#include "Utils/JsonUtils.h"
#include "FuzzJsonUtilsProduction.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 4096;
}

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production parser/logger sources.
extern "C" int SparkFuzzParseJson(const uint8_t* data, size_t size, uint32_t maxDepth)
{
    if (data == nullptr || size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }

    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const Spark::Json::JsonLimits limits{
        SPARK_FUZZ_MAX_INPUT_BYTES,
        maxDepth,
        4096,
    };
    Spark::Json::Value value;
    std::string error;
    (void)Spark::Json::ParseBounded(input, limits, &value, &error);
    return 0;
}
