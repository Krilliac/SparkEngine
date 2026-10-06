/**
 * @file FuzzRTSPersistence.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkGameRTS save snapshot
 *        (RTS::RTSPersistence::Deserialize).
 */

#include "FuzzRTSPersistenceProduction.h"

#include <cstddef>
#include <cstdint>

// Sections hold records; a building record holds its production queue and a command
// queue holds commands with their waypoint paths.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 3;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 262144;

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
    return SparkFuzzDeserializeRTSSnapshot(data, size);
}
