/**
 * @file FuzzBinaryReader.cpp
 * @brief Production-entry-point libFuzzer harness for Spark::BinaryReader, the
 *        bounds-checked reader under the asset, shader and daemon codecs.
 */

#include "FuzzBinaryReaderProduction.h"

#include <cstddef>
#include <cstdint>

// The input is a flat op script followed by a flat data buffer.
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
    return SparkFuzzRunBinaryReaderScript(data, size);
}
