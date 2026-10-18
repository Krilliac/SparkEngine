/**
 * @file FuzzExr.cpp
 * @brief Production-entry-point libFuzzer harness for the bounded OpenEXR loader
 *        (Spark::Graphics::EXRLoader::Load over tinyexr).
 */

#include "FuzzExrProduction.h"

#include <cstddef>
#include <cstdint>

// An EXR header is a flat attribute list and the loader accepts single-part
// scanline files only, so there is no nesting to bound.
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
    return SparkFuzzLoadExr(data, size);
}
