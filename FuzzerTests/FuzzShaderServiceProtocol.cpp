/**
 * @file FuzzShaderServiceProtocol.cpp
 * @brief Production-entry-point libFuzzer harness for the shader daemon message
 *        decoders (Spark::Daemon::Decode*CacheEntry*, DecodeShaderCacheStats).
 */

#include "FuzzShaderServiceProtocolProduction.h"

#include <cstddef>
#include <cstdint>

// Every shader-service payload is a flat run of fixed-width fields and at most
// one length-prefixed blob, so the depth budget is a single level.
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
    return SparkFuzzDecodeShaderServiceMessage(data, size);
}
