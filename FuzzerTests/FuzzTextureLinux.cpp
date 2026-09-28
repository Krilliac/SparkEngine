/**
 * @file FuzzTextureLinux.cpp
 * @brief Production-entry-point libFuzzer harness for the non-Windows texture
 *        file loader (Texture::CreateFromFile: stb_image BMP/TGA and the bounded
 *        EXR route).
 */

#include "FuzzTextureLinuxProduction.h"

#include <cstddef>
#include <cstdint>

// BMP and TGA headers are flat and the EXR route accepts single-part scanline
// files only, so there is no nesting to bound.
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
    return SparkFuzzCreateTextureFromFile(data, size);
}
