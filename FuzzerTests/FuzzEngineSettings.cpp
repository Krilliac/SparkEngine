/**
 * @file FuzzEngineSettings.cpp
 * @brief Production-entry-point libFuzzer harness for the engine settings reader
 *        (EngineSettings::Load over settings.ini and its settings.local.ini override).
 */

#include "FuzzEngineSettingsProduction.h"

#include <cstddef>
#include <cstdint>

// settings.ini holds sections, and sections hold keys: two levels. The local override
// has the same shape.
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
    return SparkFuzzLoadEngineSettings(data, size);
}
