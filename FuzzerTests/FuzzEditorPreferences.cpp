/**
 * @file FuzzEditorPreferences.cpp
 * @brief Production-entry-point libFuzzer harness for the editor preferences reader
 *        (SparkEditor::ParseEditorPreferences).
 */

#include <cstddef>
#include <cstdint>

// editor-preferences.ini is flat key=value lines: there is no nesting at all.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == 1);

/// Signature of the production adapter.
using SparkFuzzEntry = int(const std::uint8_t* data, std::size_t size);

/// Defined in FuzzEditorPreferencesProduction.cpp, which is compiled against libc++. Declared
/// through the alias so the only call-shaped use of the name is the bounded call below.
extern "C" SparkFuzzEntry SparkFuzzParseEditorPreferences;

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
    return SparkFuzzParseEditorPreferences(data, size);
}
