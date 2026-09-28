/**
 * @file MMOEntityEventCodec.cpp
 * @brief EntitySpawn / EntityDestroy payload decoders for the MMO client.
 */

#include "MMOEntityEventCodec.h"

#ifdef ENABLE_NETWORKING

#include <cmath>

namespace MMO
{
    std::optional<EntitySpawnEvent> DecodeEntitySpawn(std::span<const uint8_t> payload)
    {
        Spark::Net::NetBuffer buf;
        buf.WriteBytes(payload.data(), payload.size());
        EntitySpawnEvent event;
        event.networkId = buf.ReadUint32();
        event.clientId = buf.ReadUint32();
        event.entityType = buf.ReadString();
        event.position = buf.ReadVector3();
        (void)buf.ReadVector3(); // Rotation is not shown in the MMO player summary.
        if (buf.HasError())
            return std::nullopt;
        // The position seeds a remote player's current and target position; a NaN or
        // infinity there would poison every interpolation step that follows.
        if (!std::isfinite(event.position.x) || !std::isfinite(event.position.y) || !std::isfinite(event.position.z))
            return std::nullopt;
        return event;
    }

    std::optional<uint32_t> DecodeEntityDestroy(std::span<const uint8_t> payload)
    {
        Spark::Net::NetBuffer buf;
        buf.WriteBytes(payload.data(), payload.size());
        const uint32_t networkId = buf.ReadUint32();
        if (buf.HasError())
            return std::nullopt;
        return networkId;
    }
} // namespace MMO

#endif // ENABLE_NETWORKING
