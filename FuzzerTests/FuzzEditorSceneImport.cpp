/**
 * @file FuzzEditorSceneImport.cpp
 * @brief Production-entry-point libFuzzer harness for the Scene Import panel's game INI
 *        .scene reader (SparkEditor::ParseGameSceneIni).
 */

#include "FuzzEditorSceneImportProduction.h"

#include <cstddef>
#include <cstdint>

// An INI .scene is a flat list of [Section] blocks of key=value lines; nothing nests.
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
    return SparkFuzzParseEditorSceneImport(data, size);
}
