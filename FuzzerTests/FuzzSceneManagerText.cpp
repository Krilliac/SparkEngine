/**
 * @file FuzzSceneManagerText.cpp
 * @brief Production-entry-point libFuzzer harness for SceneManager's text scene
 *        readers (Spark::ParseVersionedSceneText, ParseIniSceneText and
 *        ParseLegacyObjectLines in SceneManager/SceneTextFormat.cpp).
 */

#include "FuzzSceneManagerTextProduction.h"

#include <cstddef>
#include <cstdint>

// Every dialect is line oriented with no structural nesting (the node
// hierarchy is a flat parent-index table), so the depth budget is one level.
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
    return SparkFuzzParseSceneManagerText(data, size);
}
