/**
 * @file FuzzFpsSnapshotProduction.cpp
 * @brief libc++ production adapter for the FPS player-state wire codec.
 */

#include "FuzzFpsSnapshotProduction.h"

#include "Game/MultiplayerSystem.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr std::size_t kHeaderSize = sizeof(std::uint32_t) + sizeof(std::uint16_t);
    constexpr std::size_t kScoreSize = 4u * sizeof(std::uint32_t);
    constexpr std::size_t kRecordSize = SparkFPS::NetworkPlayerState::SerializedSize + kScoreSize;
    constexpr std::size_t kMaxInputBytes = 65536;
    // clientId (4) + nine floats (36) + actionFlags (4) + ack (4) + weapon (1) = 49.
    constexpr std::size_t kAliveOffset = 49;
    constexpr std::size_t kCrouchingOffset = 50;

    [[noreturn]] void InvariantFailure(const char* message)
    {
        std::fprintf(stderr, "SparkFuzzFpsSnapshot: %s\n", message);
        std::abort();
    }

    std::uint32_t ReadRawU32(const std::uint8_t* data)
    {
        return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8u) |
               (static_cast<std::uint32_t>(data[2]) << 16u) | (static_cast<std::uint32_t>(data[3]) << 24u);
    }
} // namespace

extern "C" int SparkFuzzProcessFpsSnapshot(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    std::uint32_t batch = 0;
    std::vector<SparkFPS::NetworkPlayerState> states;
    std::vector<SparkFPS::PlayerScore> scores;
    const bool accepted = SparkFPS::DecodeSnapshotBatch(data, size, batch, states, scores);

    // Independent framing model: header, bounded count, exact record payload length.
    if (size < kHeaderSize)
    {
        if (accepted)
        {
            InvariantFailure("decoder accepted a payload shorter than the header");
        }
        return 0;
    }
    const std::uint32_t rawBatch = ReadRawU32(data);
    const std::uint32_t rawCount = static_cast<std::uint32_t>(data[4]) | (static_cast<std::uint32_t>(data[5]) << 8u);
    if (rawCount > SparkFPS::kMaxPlayers || size != kHeaderSize + rawCount * kRecordSize)
    {
        if (accepted)
        {
            InvariantFailure("decoder accepted a payload whose framing the model rejects");
        }
        return 0;
    }
    if (!accepted)
    {
        if (!states.empty() || !scores.empty())
        {
            InvariantFailure("a rejected batch left decoded records in the outputs");
        }
        return 0;
    }
    if (batch != rawBatch || states.size() != rawCount || scores.size() != rawCount)
    {
        InvariantFailure("decoded batch disagrees with the independent header model");
    }
    for (std::size_t index = 0; index < states.size(); ++index)
    {
        const SparkFPS::NetworkPlayerState& state = states[index];
        if (state.sequenceNumber != batch)
        {
            InvariantFailure("accepted snapshot state has a mismatched sequence");
        }
        if (!std::isfinite(state.posX) || !std::isfinite(state.posY) || !std::isfinite(state.posZ) ||
            !std::isfinite(state.velX) || !std::isfinite(state.velY) || !std::isfinite(state.velZ) ||
            !std::isfinite(state.yaw) || !std::isfinite(state.pitch) || !std::isfinite(state.health))
        {
            InvariantFailure("accepted snapshot state contains a non-finite value");
        }
        const std::uint8_t* record = data + kHeaderSize + index * kRecordSize;
        // Independent wire-contract model: only defined action bits, canonical 0/1 booleans.
        constexpr std::uint32_t kKnownActions = SparkFPS::ActionJump | SparkFPS::ActionFire | SparkFPS::ActionReload |
                                                SparkFPS::ActionCrouch | SparkFPS::ActionSprint;
        if ((state.actionFlags & ~kKnownActions) != 0 || record[kAliveOffset] > 1 || record[kCrouchingOffset] > 1)
        {
            InvariantFailure("accepted snapshot state carries undefined action bits or a non-canonical boolean");
        }
        if (state.Serialize() !=
            std::vector<std::uint8_t>(record, record + SparkFPS::NetworkPlayerState::SerializedSize))
        {
            InvariantFailure("decoded state does not reproduce its source record");
        }
        const std::uint8_t* score = record + SparkFPS::NetworkPlayerState::SerializedSize;
        if (scores[index].clientId != state.clientId || scores[index].kills != ReadRawU32(score) ||
            scores[index].deaths != ReadRawU32(score + 4) || scores[index].assists != ReadRawU32(score + 8) ||
            static_cast<std::uint32_t>(scores[index].score) != ReadRawU32(score + 12))
        {
            InvariantFailure("decoded score does not reproduce its source record");
        }
    }
    return 0;
}
