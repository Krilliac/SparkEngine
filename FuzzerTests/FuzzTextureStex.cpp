/**
 * @file FuzzTextureStex.cpp
 * @brief Production-entry-point libFuzzer harness for the .stex texture container
 *        loader (Spark::Graphics::TextureCompressor::LoadCompressed).
 */

#include "FuzzTextureStexProduction.h"

#include "Graphics/TextureCompressor.h"

#include <cstddef>
#include <cstdint>

// A .stex has no nesting; its only repeated structure is the mip chain, so the
// depth budget is the longest chain the loader accepts.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 15;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == Spark::Graphics::TextureCompressor::kMaxStexMipLevels);

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
    return SparkFuzzLoadStex(data, size);
}
