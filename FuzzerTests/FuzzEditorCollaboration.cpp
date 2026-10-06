/**
 * @file FuzzEditorCollaboration.cpp
 * @brief Production-entry-point libFuzzer harness for the editor collaboration wire decoder.
 */

#include "FuzzEditorCollaborationProduction.h"

#include <cstddef>
#include <cstdint>

constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 1048576;
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 2;

static_assert(SPARK_FUZZ_MAX_DEPTH == 2);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }
    if (data == nullptr && size != 0)
    {
        return 0;
    }
    return SparkFuzzDecodeEditorCollaboration(data, size);
}
