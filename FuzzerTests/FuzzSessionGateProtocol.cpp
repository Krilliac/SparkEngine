/**
 * @file FuzzSessionGateProtocol.cpp
 * @brief SEC-120 libFuzzer entry point for the MMO session-gate parser.
 */
#include "GameModules/SparkGameMMO/Source/Session/MMOSessionGateProtocol.h"

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    MMO::SessionGateWire::Packet packet{};
    const std::span<const uint8_t> bytes(data, size);
    if (MMO::SessionGateWire::Decode(bytes, packet))
    {
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
