/**
 * @file FuzzEditorRecovery.cpp
 * @brief Production-entry-point libFuzzer harness for the editor recovery snapshot reader
 *        (SparkEditor::EditorRecoveryStore::LoadForProject).
 */

#include "FuzzEditorRecoveryProduction.h"

#include <cstddef>
#include <cstdint>

// RecoveryJsonLimits caps the envelope at depth 64; the nested world is read by
// Spark::DeserializeInto, whose own depth budget the reflected-scene target covers.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 64;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == 64);

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
    return SparkFuzzLoadEditorRecovery(data, size, SPARK_FUZZ_MAX_DEPTH);
}
