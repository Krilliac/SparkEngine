/**
 * @file FuzzDaemonFrame.cpp
 * @brief Production-entry-point libFuzzer harness for the daemon IPC frame
 *        receive path (Spark::Daemon::RecvFrame over a Unix socket pair, plus
 *        DecodeDaemonStats on every accepted payload).
 */

#include "FuzzDaemonFrameProduction.h"

#include <cstddef>
#include <cstdint>

// A frame is an 8-byte header and a flat payload; the only nested structure is
// the stats payload's version string and id list, one level deep.
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
    return SparkFuzzRecvDaemonFrames(data, size);
}
