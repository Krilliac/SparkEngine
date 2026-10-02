/**
 * @file MMOEntityEventCodec.h
 * @brief Network-free decoders for the EntitySpawn and EntityDestroy payloads a
 *        SparkGameMMO client receives from its server.
 *
 * MMOPlayerSystem's network handlers call these and keep the game-side policy
 * (only "MMOPlayer" spawns for remote clients are shown). They are split out so
 * the SEC-120 fuzz target (FuzzerTests/FuzzMMOEntityEvents.cpp) drives exactly
 * the decoders the client runs. Thread affinity: none (pure functions).
 */

#pragma once

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/NetworkManager.h" // DirectX::XMFLOAT3 and the NetBuffer wire types

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace MMO
{
    /// The fields of NetworkManager's canonical EntitySpawn payload the MMO client uses.
    struct EntitySpawnEvent
    {
        uint32_t networkId = 0;
        uint32_t clientId = 0;
        std::string entityType;
        DirectX::XMFLOAT3 position{};
    };

    /**
     * @brief Decode an EntitySpawn payload: networkId, ownerId, entityType string,
     *        position, rotation (rotation is read for length but not returned).
     * @return The event, or nullopt when the payload is truncated or the position is
     *         not finite. Bytes after the rotation are ignored.
     */
    std::optional<EntitySpawnEvent> DecodeEntitySpawn(std::span<const uint8_t> payload);

    /// @return The destroyed entity's network ID, or nullopt when the payload is shorter than 4 bytes.
    std::optional<uint32_t> DecodeEntityDestroy(std::span<const uint8_t> payload);
} // namespace MMO

#endif // ENABLE_NETWORKING
