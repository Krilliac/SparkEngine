/**
 * @file FuzzMMOEntityEventsProduction.cpp
 * @brief libc++-compiled production adapter for the SparkGameMMO entity event libFuzzer harness.
 *
 * MMOPlayerSystem's EntitySpawn and EntityDestroy handlers run MMO::DecodeEntitySpawn
 * and MMO::DecodeEntityDestroy on every such datagram from the server. The first
 * input byte picks the decoder (even: spawn, odd: destroy); the rest is the payload.
 * A violation of the decoders' contract aborts so libFuzzer records a crash:
 *  - an accepted spawn has a finite position and an entity type no longer than
 *    the NetBuffer string bound (65535 bytes),
 *  - re-encoding an accepted spawn with the NetBuffer writers reproduces the
 *    payload bytes it consumed (network ID, owner, type and position), and the
 *    payload still holds the 12 rotation bytes after them,
 *  - an accepted destroy payload is at least 4 bytes and its ID is the first
 *    4 bytes, little-endian.
 */

#include "FuzzMMOEntityEventsProduction.h"

#include "Engine/Networking/NetworkManager.h"
#include "Player/MMOEntityEventCodec.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 1024;
    constexpr std::size_t kRotationBytes = 12;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzMMOEntityEvents: entity event contract violated: %s\n", what);
        std::abort();
    }

    void CheckSpawn(std::span<const std::uint8_t> payload)
    {
        const std::optional<MMO::EntitySpawnEvent> spawn = MMO::DecodeEntitySpawn(payload);
        if (!spawn)
            return;
        if (!std::isfinite(spawn->position.x) || !std::isfinite(spawn->position.y) || !std::isfinite(spawn->position.z))
            InvariantFailure("a spawn with a non-finite position was accepted");
        if (spawn->entityType.size() > 65535u)
            InvariantFailure("a spawn entity type exceeds the NetBuffer string bound");

        Spark::Net::NetBuffer buffer;
        buffer.WriteUint32(spawn->networkId);
        buffer.WriteUint32(spawn->clientId);
        buffer.WriteString(spawn->entityType);
        buffer.WriteVector3(spawn->position);
        const std::vector<std::uint8_t>& encoded = buffer.GetData();
        if (encoded.size() > payload.size() || std::memcmp(encoded.data(), payload.data(), encoded.size()) != 0)
            InvariantFailure("re-encoding an accepted spawn does not reproduce the bytes it consumed");
        if (payload.size() - encoded.size() < kRotationBytes)
            InvariantFailure("a spawn without its rotation was accepted");
    }

    void CheckDestroy(std::span<const std::uint8_t> payload)
    {
        const std::optional<std::uint32_t> networkId = MMO::DecodeEntityDestroy(payload);
        if (!networkId)
            return;
        if (payload.size() < 4)
            InvariantFailure("a destroy payload under 4 bytes was accepted");
        const std::uint32_t wire =
            static_cast<std::uint32_t>(payload[0]) | (static_cast<std::uint32_t>(payload[1]) << 8) |
            (static_cast<std::uint32_t>(payload[2]) << 16) | (static_cast<std::uint32_t>(payload[3]) << 24);
        if (*networkId != wire)
            InvariantFailure("the destroyed network ID differs from the wire");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoders.
extern "C" int SparkFuzzDecodeMMOEntityEvents(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr || size == 0)
        return 0;

    const std::span<const std::uint8_t> payload(data + 1, size - 1);
    if ((data[0] & 1u) == 0)
        CheckSpawn(payload);
    else
        CheckDestroy(payload);
    return 0;
}
