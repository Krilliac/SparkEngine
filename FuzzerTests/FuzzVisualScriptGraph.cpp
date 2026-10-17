/**
 * @file FuzzVisualScriptGraph.cpp
 * @brief Production-entry-point libFuzzer harness for the .vscript visual-script
 *        graph decoder (Spark::Scripting::VisualScriptGraphIO::Parse).
 */

#include "FuzzVisualScriptGraphProduction.h"

#include <cstddef>
#include <cstdint>

// The decoder parses with a JSON depth limit of 16 (the format nests at most
// six levels); the budget of 32 lets mutated inputs probe well past that limit.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 32;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH >= 16);

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
    return SparkFuzzParseVisualScriptGraph(data, size);
}
