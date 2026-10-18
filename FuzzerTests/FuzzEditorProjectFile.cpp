/**
 * @file FuzzEditorProjectFile.cpp
 * @brief Production-entry-point libFuzzer harness for the editor's .sparkproject and
 *        RecentProjects.json field scrape (SparkEditor::ReadProjectDocumentFields and
 *        ReadRecentProjectsDocument).
 */

#include "FuzzEditorProjectFileProduction.h"

#include <cstddef>
#include <cstdint>

// The scrape reads one flat object (plus one array level for scenes/modules); nothing nests.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 2;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == 2);

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
    return SparkFuzzReadEditorProjectFile(data, size);
}
