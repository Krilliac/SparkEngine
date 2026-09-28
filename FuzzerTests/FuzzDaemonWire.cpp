/**
 * @file FuzzDaemonWire.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkDaemon bounded
 *        wire decoders (collaboration broker and orchestration control plane).
 */

#include "FuzzDaemonWireProduction.h"

#include <cstddef>
#include <cstdint>

// The deepest message is a snapshot: a list of records whose fields are
// length-prefixed strings, two levels below the message.
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
    return SparkFuzzDecodeDaemonWireMessage(data, size);
}
