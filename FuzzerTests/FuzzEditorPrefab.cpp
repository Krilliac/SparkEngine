/**
 * @file FuzzEditorPrefab.cpp
 * @brief Production-entry-point libFuzzer harness for the .sparkprefab text reader
 *        (SparkEditor::PrefabTextFormat::Parse, the parser behind PrefabAsset::TryLoad).
 */

#include "FuzzEditorPrefabProduction.h"

#include <cstddef>
#include <cstdint>

// A .sparkprefab is a line-oriented list of components, each holding a flat list
// of properties. The nesting is fixed by the grammar and nothing recurses, so the
// depth budget is a single level.
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
    return SparkFuzzParseEditorPrefab(data, size);
}
