/**
 * @file FuzzMMOChatWire.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkGameMMO chat wire decoder and server
 *        relay policy (MMO::MMOChatSystem::DecodeWirePayload,
 *        MMO::MMOChatSystem::BuildServerRelayPayload).
 */

#include "FuzzMMOChatWireProduction.h"

#include <cstddef>
#include <cstdint>

// A chat payload is a channel byte and two length-prefixed strings with no nesting.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 1024;

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
    return SparkFuzzDecodeMMOChat(data, size);
}
