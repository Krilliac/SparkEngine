/**
 * @file FuzzReflectedScene.cpp
 * @brief Production-entry-point libFuzzer harness for the reflected JSON scene
 *        reader (Spark::DeserializeInto), which LoadWorld and editor crash
 *        recovery both call.
 */

#include "FuzzReflectedSceneProduction.h"

#include <cstddef>
#include <cstdint>

// DeserializeInto parses through the vendored JSON reader, which throws on
// nesting deeper than json::max_parse_depth (256); the adapter asserts that the
// shipped constant still matches this budget.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 256;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

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
    return SparkFuzzDeserializeReflectedScene(data, size, SPARK_FUZZ_MAX_DEPTH);
}
