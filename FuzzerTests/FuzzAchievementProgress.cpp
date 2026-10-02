/**
 * @file FuzzAchievementProgress.cpp
 * @brief Production-entry-point libFuzzer harness for the achievement progress reader
 *        (Spark::Gameplay::AchievementSystem::LoadFromReader).
 */

#include "FuzzAchievementProgressProduction.h"

#include <cstddef>
#include <cstdint>

// The progress payload is a count followed by flat fixed-size records.
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
    return SparkFuzzLoadAchievementProgress(data, size);
}
