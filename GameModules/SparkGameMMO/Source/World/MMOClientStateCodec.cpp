/**
 * @file MMOClientStateCodec.cpp
 * @brief Client EntityStateUpdate state request decode and plausibility bounds.
 */

#include "MMOClientStateCodec.h"

#ifdef ENABLE_NETWORKING

#include <cmath>

namespace MMO
{
    namespace
    {
        bool IsFiniteVector(const DirectX::XMFLOAT3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool IsPlausibleCoordinate(const DirectX::XMFLOAT3& value)
        {
            return IsFiniteVector(value) && std::abs(value.x) <= kMaxClientCoordinate &&
                   std::abs(value.y) <= kMaxClientCoordinate && std::abs(value.z) <= kMaxClientCoordinate;
        }

        bool IsPlausibleRotation(const DirectX::XMFLOAT3& value)
        {
            return IsFiniteVector(value) && std::abs(value.x) <= kMaxClientRotationDegrees &&
                   std::abs(value.y) <= kMaxClientRotationDegrees && std::abs(value.z) <= kMaxClientRotationDegrees;
        }

        bool IsPlausibleVelocity(const DirectX::XMFLOAT3& value)
        {
            return IsFiniteVector(value) &&
                   value.x * value.x + value.y * value.y + value.z * value.z <= kMaxClientSpeed * kMaxClientSpeed;
        }
    } // namespace

    std::optional<ClientStateRequest> DecodeClientStateRequest(std::span<const uint8_t> payload)
    {
        if (payload.size() != kClientStateRequestSize)
            return std::nullopt;

        Spark::Net::NetBuffer buffer;
        buffer.WriteBytes(payload.data(), payload.size());
        (void)buffer.ReadUint32(); // Client-chosen network ID: identity comes from the sender, never the wire.
        ClientStateRequest request;
        request.position = buffer.ReadVector3();
        request.rotation = buffer.ReadVector3();
        request.velocity = buffer.ReadVector3();
        const uint16_t propertyCount = buffer.ReadUint16();
        if (buffer.HasError() || buffer.RemainingBytes() != 0 || propertyCount != 0)
            return std::nullopt;
        if (!IsPlausibleCoordinate(request.position) || !IsPlausibleRotation(request.rotation) ||
            !IsPlausibleVelocity(request.velocity))
            return std::nullopt;
        return request;
    }
} // namespace MMO

#endif // ENABLE_NETWORKING
