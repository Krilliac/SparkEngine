/**
 * @file FuzzSessionGateProtocol.cpp
 * @brief SEC-120 libFuzzer entry point for the MMO session-gate parser (MMO::SessionGateWire::Decode).
 */
#include "GameModules/SparkGameMMO/Source/Session/MMOSessionGateProtocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

// A session-gate packet is a fixed header plus bounded length-prefixed text fields with no nesting,
// capped at MMO::SessionGateWire::MaxPacketBytes.
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 512;
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;

static_assert(SPARK_FUZZ_MAX_DEPTH == 1);
static_assert(SPARK_FUZZ_MAX_INPUT_BYTES == MMO::SessionGateWire::MaxPacketBytes);

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
    MMO::SessionGateWire::Packet packet{};
    const std::span<const uint8_t> bytes(data, size);
    if (MMO::SessionGateWire::Decode(bytes, packet))
    {
        // An accepted packet must be canonical: re-encoding reproduces the input byte for byte.
        const auto encoded = MMO::SessionGateWire::Encode(packet);
        MMO::SessionGateWire::Packet roundTrip{};
        if (!MMO::SessionGateWire::Decode(encoded, roundTrip) || encoded.size() != bytes.size() ||
            !std::equal(encoded.begin(), encoded.end(), bytes.begin()))
        {
            std::abort();
        }
    }
    return 0;
}
