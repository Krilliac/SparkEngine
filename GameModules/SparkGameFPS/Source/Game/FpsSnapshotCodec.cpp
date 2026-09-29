/**
 * @file FpsSnapshotCodec.cpp
 * @brief Production FPS player-state and input wire codec.
 */

#include "MultiplayerSystem.h"

#include <cmath>

namespace SparkFPS
{

    namespace
    {
        constexpr size_t kSnapshotHeaderSize = sizeof(uint32_t) + sizeof(uint16_t);
        constexpr size_t kSnapshotScoreSize = 4 * sizeof(uint32_t);
        constexpr size_t kSnapshotRecordSize = NetworkPlayerState::SerializedSize + kSnapshotScoreSize;
        constexpr size_t kActionFlagsOffset = 40;
        constexpr size_t kAliveOffset = 49;
        constexpr size_t kCrouchingOffset = 50;

        bool IsFiniteState(const NetworkPlayerState& state)
        {
            return std::isfinite(state.posX) && std::isfinite(state.posY) && std::isfinite(state.posZ) &&
                   std::isfinite(state.velX) && std::isfinite(state.velY) && std::isfinite(state.velZ) &&
                   std::isfinite(state.yaw) && std::isfinite(state.pitch) && std::isfinite(state.health);
        }
    } // namespace

    std::vector<uint8_t> NetworkPlayerState::Serialize() const
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(SerializedSize);
        Detail::WriteU32(bytes, clientId);
        Detail::WriteFloat(bytes, posX);
        Detail::WriteFloat(bytes, posY);
        Detail::WriteFloat(bytes, posZ);
        Detail::WriteFloat(bytes, velX);
        Detail::WriteFloat(bytes, velY);
        Detail::WriteFloat(bytes, velZ);
        Detail::WriteFloat(bytes, yaw);
        Detail::WriteFloat(bytes, pitch);
        Detail::WriteFloat(bytes, health);
        Detail::WriteU32(bytes, actionFlags);
        Detail::WriteU32(bytes, acknowledgedInputSequence);
        bytes.push_back(currentWeapon);
        bytes.push_back(static_cast<uint8_t>(isAlive));
        bytes.push_back(static_cast<uint8_t>(isCrouching));
        Detail::WriteU32(bytes, sequenceNumber);
        return bytes;
    }

    NetworkPlayerState NetworkPlayerState::Deserialize(const uint8_t* data, size_t size)
    {
        if (!data || size < SerializedSize)
        {
            return {};
        }

        NetworkPlayerState state;
        size_t offset = 0;
        state.clientId = Detail::ReadU32(data, offset);
        state.posX = Detail::ReadFloat(data, offset);
        state.posY = Detail::ReadFloat(data, offset);
        state.posZ = Detail::ReadFloat(data, offset);
        state.velX = Detail::ReadFloat(data, offset);
        state.velY = Detail::ReadFloat(data, offset);
        state.velZ = Detail::ReadFloat(data, offset);
        state.yaw = Detail::ReadFloat(data, offset);
        state.pitch = Detail::ReadFloat(data, offset);
        state.health = Detail::ReadFloat(data, offset);
        state.actionFlags = Detail::ReadU32(data, offset);
        if ((state.actionFlags & ~(ActionJump | ActionFire | ActionReload | ActionCrouch | ActionSprint)) != 0)
        {
            return {};
        }
        state.acknowledgedInputSequence = Detail::ReadU32(data, offset);
        state.currentWeapon = data[offset++];
        const uint8_t alive = data[offset++];
        const uint8_t crouching = data[offset++];
        if (alive > 1 || crouching > 1)
        {
            return {};
        }
        state.isAlive = alive != 0;
        state.isCrouching = crouching != 0;
        state.sequenceNumber = Detail::ReadU32(data, offset);
        return state;
    }

    std::vector<uint8_t> PlayerInput::Serialize() const
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(SerializedSize);
        Detail::WriteFloat(bytes, forward);
        Detail::WriteFloat(bytes, strafe);
        Detail::WriteFloat(bytes, yaw);
        Detail::WriteFloat(bytes, pitch);
        bytes.push_back(static_cast<uint8_t>(jump));
        bytes.push_back(static_cast<uint8_t>(fire));
        bytes.push_back(static_cast<uint8_t>(reload));
        bytes.push_back(static_cast<uint8_t>(crouch));
        Detail::WriteU32(bytes, sequenceNumber);
        return bytes;
    }

    PlayerInput PlayerInput::Deserialize(const uint8_t* data, size_t size)
    {
        if (!data || size < SerializedSize)
        {
            return {};
        }

        PlayerInput input;
        size_t offset = 0;
        input.forward = Detail::ReadFloat(data, offset);
        input.strafe = Detail::ReadFloat(data, offset);
        input.yaw = Detail::ReadFloat(data, offset);
        input.pitch = Detail::ReadFloat(data, offset);
        const uint8_t jump = data[offset++];
        const uint8_t fire = data[offset++];
        const uint8_t reload = data[offset++];
        const uint8_t crouch = data[offset++];
        if (jump > 1 || fire > 1 || reload > 1 || crouch > 1)
        {
            return {};
        }
        input.jump = jump != 0;
        input.fire = fire != 0;
        input.reload = reload != 0;
        input.crouch = crouch != 0;
        input.sequenceNumber = Detail::ReadU32(data, offset);
        return input;
    }

    bool DecodeSnapshotBatch(const uint8_t* data, size_t size, uint32_t& outBatch,
                             std::vector<NetworkPlayerState>& outStates, std::vector<PlayerScore>& outScores)
    {
        outStates.clear();
        outScores.clear();
        if (data == nullptr || size < kSnapshotHeaderSize)
        {
            return false;
        }

        size_t offset = 0;
        const uint32_t batch = Detail::ReadU32(data, offset);
        const uint32_t count = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8u);
        if (count > kMaxPlayers || size != kSnapshotHeaderSize + static_cast<size_t>(count) * kSnapshotRecordSize)
        {
            return false;
        }

        outStates.reserve(count);
        outScores.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            const uint8_t* record = data + kSnapshotHeaderSize + static_cast<size_t>(index) * kSnapshotRecordSize;
            size_t actionOffset = kActionFlagsOffset;
            const uint32_t actionFlags = Detail::ReadU32(record, actionOffset);
            if ((actionFlags & ~(ActionJump | ActionFire | ActionReload | ActionCrouch | ActionSprint)) != 0 ||
                record[kAliveOffset] > 1 || record[kCrouchingOffset] > 1)
            {
                outStates.clear();
                outScores.clear();
                return false;
            }
            const NetworkPlayerState state =
                NetworkPlayerState::Deserialize(record, NetworkPlayerState::SerializedSize);
            if (!IsFiniteState(state) || state.sequenceNumber != batch)
            {
                outStates.clear();
                outScores.clear();
                return false;
            }
            size_t scoreOffset = NetworkPlayerState::SerializedSize;
            PlayerScore score;
            score.clientId = state.clientId;
            score.kills = Detail::ReadU32(record, scoreOffset);
            score.deaths = Detail::ReadU32(record, scoreOffset);
            score.assists = Detail::ReadU32(record, scoreOffset);
            score.score = static_cast<int32_t>(Detail::ReadU32(record, scoreOffset));
            outStates.push_back(state);
            outScores.push_back(score);
        }
        outBatch = batch;
        return true;
    }

} // namespace SparkFPS
