/**
 * @file TFHandoffState.h
 * @brief Versioned pawn checkpoint carried by the durable TERRAFRONT handoff reservation.
 *
 * Game-thread value type, owned by the migration participant. Encoding allocates only when a
 * migration starts; decoding uses fixed storage. Bounded to one walking pawn per character.
 * Vehicles and client scene replacement are outside this checkpoint's contract.
 */
#pragma once

#include "Core/TFTypes.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace Terrafront
{
    struct TFHandoffState
    {
        PlayerId player = kInvalidPlayer;
        ClassId cls = ClassId::Ghost;
        float position[3]{};
        float velocity[3]{};
        float yaw = 0.0f;
        float pitch = 0.0f;
        float health = 0.0f;
        float shield = 0.0f;
        uint32_t lastSequence = 0;
        bool grounded = false;

        /// @brief Reject corrupt/non-finite checkpoints before changing either authority.
        bool IsValid() const
        {
            if (player == kInvalidPlayer || cls >= ClassId::COUNT || !std::isfinite(yaw) || !std::isfinite(pitch) ||
                !std::isfinite(health) || !std::isfinite(shield) || health <= 0.0f || shield < 0.0f)
            {
                return false;
            }
            for (size_t i = 0; i < 3; ++i)
            {
                if (!std::isfinite(position[i]) || !std::isfinite(velocity[i]))
                {
                    return false;
                }
            }
            return true;
        }

        /// @brief Fixed hexadecimal encoding preserves every float bit without locale or ABI dependence.
        std::string Encode() const
        {
            if (!IsValid())
            {
                return {};
            }
            const std::array<uint32_t, 14> words = {player,
                                                    static_cast<uint32_t>(cls),
                                                    std::bit_cast<uint32_t>(position[0]),
                                                    std::bit_cast<uint32_t>(position[1]),
                                                    std::bit_cast<uint32_t>(position[2]),
                                                    std::bit_cast<uint32_t>(velocity[0]),
                                                    std::bit_cast<uint32_t>(velocity[1]),
                                                    std::bit_cast<uint32_t>(velocity[2]),
                                                    std::bit_cast<uint32_t>(yaw),
                                                    std::bit_cast<uint32_t>(pitch),
                                                    std::bit_cast<uint32_t>(health),
                                                    std::bit_cast<uint32_t>(shield),
                                                    lastSequence,
                                                    grounded ? 1u : 0u};
            std::string result = "TFH1";
            result.reserve(116);
            constexpr char digits[] = "0123456789abcdef";
            for (uint32_t word : words)
            {
                for (int shift = 28; shift >= 0; shift -= 4)
                {
                    result.push_back(digits[(word >> shift) & 15u]);
                }
            }
            return result;
        }

        /// @brief Decode atomically: invalid input leaves the caller's checkpoint untouched.
        static bool Decode(std::string_view encoded, TFHandoffState& out)
        {
            if (encoded.size() != 116 || !encoded.starts_with("TFH1"))
            {
                return false;
            }
            std::array<uint32_t, 14> words{};
            for (size_t i = 0; i < words.size(); ++i)
            {
                for (size_t j = 0; j < 8; ++j)
                {
                    const char digit = encoded[4 + i * 8 + j];
                    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
                    {
                        return false;
                    }
                    words[i] = (words[i] << 4) | static_cast<uint32_t>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
                }
            }
            if (words[1] >= static_cast<uint32_t>(ClassId::COUNT) || words[13] > 1)
            {
                return false;
            }
            TFHandoffState decoded;
            decoded.player = words[0];
            decoded.cls = static_cast<ClassId>(words[1]);
            for (size_t i = 0; i < 3; ++i)
            {
                decoded.position[i] = std::bit_cast<float>(words[2 + i]);
                decoded.velocity[i] = std::bit_cast<float>(words[5 + i]);
            }
            decoded.yaw = std::bit_cast<float>(words[8]);
            decoded.pitch = std::bit_cast<float>(words[9]);
            decoded.health = std::bit_cast<float>(words[10]);
            decoded.shield = std::bit_cast<float>(words[11]);
            decoded.lastSequence = words[12];
            decoded.grounded = words[13] != 0;
            if (!decoded.IsValid())
            {
                return false;
            }
            out = decoded;
            return true;
        }
    };
} // namespace Terrafront
