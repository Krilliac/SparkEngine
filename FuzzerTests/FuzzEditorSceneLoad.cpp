/**
 * @file FuzzEditorSceneLoad.cpp
 * @brief Production-entry-point libFuzzer harness for the editor scene load entry
 *        (SparkEditor::SceneSerializer::LoadScene): extension dispatch, the bounded file read,
 *        the JSON decoder and SceneFile::Validate, plus the save/reload round trip.
 */

#include "FuzzEditorSceneLoadProduction.h"

#include <cstddef>
#include <cstdint>

// The first byte only picks the file name; the decoder's parser refuses nesting deeper than 128.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 128;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == 128);

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
    return SparkFuzzLoadEditorScene(data, size);
}
