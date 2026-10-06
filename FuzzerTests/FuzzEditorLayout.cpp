/**
 * @file FuzzEditorLayout.cpp
 * @brief Production-entry-point libFuzzer harness for the editor panel-layout reader
 *        (SparkEditor::EditorLayoutManager::LoadLayout).
 */

#include "FuzzEditorLayoutProduction.h"

#include <cstddef>
#include <cstdint>

// {"layout": {..., "panels": [{...}]}}: three levels; the reader matches panel braces itself.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 3;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == 3);

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
    return SparkFuzzLoadEditorLayout(data, size);
}
