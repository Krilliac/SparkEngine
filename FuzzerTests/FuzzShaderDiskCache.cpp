/**
 * @file FuzzShaderDiskCache.cpp
 * @brief Production-entry-point libFuzzer harness for the shader disk-cache blob reader
 *        (Spark::Graphics::ShaderDiskCache::Lookup).
 */

#include "FuzzShaderDiskCacheProduction.h"

#include <cstddef>
#include <cstdint>

// A cached blob is raw bytecode with no structure the reader walks.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 1048576;

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
    return SparkFuzzLookupShaderDiskCache(data, size);
}
