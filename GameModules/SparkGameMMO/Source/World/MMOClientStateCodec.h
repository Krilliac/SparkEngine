/**
 * @file MMOClientStateCodec.h
 * @brief Network-free decoder for the client EntityStateUpdate state request.
 *
 * MMOWorldSetup::ApplyClientStateRequest keeps the role and sender checks and
 * the entity binding; the byte-level decode and the plausibility bounds live
 * here so the SEC-120 fuzz target (FuzzerTests/FuzzMMOClientState.cpp) drives
 * exactly the decoder the server runs. Thread affinity: none (pure function).
 */

#pragma once

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/NetworkManager.h" // DirectX::XMFLOAT3 and the NetBuffer wire types

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace MMO
{
    /// networkId (4) + position, rotation, velocity (3 x 12) + property count (2).
    inline constexpr size_t kClientStateRequestSize = 42;
    /// Any coordinate past this is garbage or an exploit, not a world position (1000 km).
    inline constexpr float kMaxClientCoordinate = 1.0e6f;
    /// Sprinting players move at 10.5 m/s; this leaves headroom for knockback and lag.
    inline constexpr float kMaxClientSpeed = 100.0f;
    /// ReplicatedEntity rotation is Euler degrees, republished to every other client. Any
    /// finite value used to pass, so a peer could push values near FLT_MAX that overflow to
    /// inf/NaN in the first lerp or matrix built from them. One full turn either way covers
    /// every orientation a client can mean.
    inline constexpr float kMaxClientRotationDegrees = 360.0f;

    /// A decoded, plausibility-checked client state request. The client-chosen network ID on
    /// the wire is not part of it: identity comes from the sender, never the payload.
    struct ClientStateRequest
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 rotation{};
        DirectX::XMFLOAT3 velocity{};
    };

    /**
     * @brief Decode one client state request payload.
     * @return The request, or nullopt unless @p payload is exactly kClientStateRequestSize
     *         bytes with a zero property count, every component finite, position within
     *         kMaxClientCoordinate per axis, rotation within kMaxClientRotationDegrees per
     *         axis and speed at most kMaxClientSpeed.
     */
    std::optional<ClientStateRequest> DecodeClientStateRequest(std::span<const uint8_t> payload);
} // namespace MMO

#endif // ENABLE_NETWORKING
