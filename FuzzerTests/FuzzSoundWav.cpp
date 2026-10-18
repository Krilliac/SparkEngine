/**
 * @file FuzzSoundWav.cpp
 * @brief Production-entry-point libFuzzer harness for the SoundEffect WAV parser
 *        (SoundEffect::LoadFromMemory).
 */

#include "FuzzSoundWavProduction.h"

#include <cstddef>
#include <cstdint>

// RIFF/WAVE is a flat list of chunks after the 12-byte header; the parser never
// descends into LIST or any other container chunk.
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
    return SparkFuzzLoadWav(data, size);
}
